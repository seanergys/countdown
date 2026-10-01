/*
 * Copyright (c), CINECA, UNIBO, and ETH Zurich
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *			* Redistributions of source code must retain the above copyright notice, this
 *				list of conditions and the following disclaimer.
 *
 *			* Redistributions in binary form must reproduce the above copyright notice,
 *				this list of conditions and the following disclaimer in the documentation
 *				and/or other materials provided with the distribution.
 *
 *			* Neither the name of the copyright holder nor the names of its
 *				contributors may be used to endorse or promote products derived from
 *				this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * SEANERGYS Access Daemon client.
 *
 * COUNTDOWN delegates CPU-frequency actuation and CPU/socket power readings to
 * the Access Daemon instead of driving MSRs and cpufreq sysfs itself.
 *
 * Wire protocol, as implemented today by the daemon's "seanerbus.so" plugin:
 *
 *   1. The daemon publishes one SeanerBUS endpoint per (metric, device,
 *      device_no) triple and registers it in the name service under the keys
 *      "metric", "device" and "device_no", e.g. Freq/Cpu/3 or Power/Socket/0.
 *   2. A client resolves those names to obtain the endpoint address.
 *   3. Sending GET_REQ_V1 to that address reads the sensor; sending SET_REQ_V1
 *      writes the matching actuator. Both are answered with GET_RES_V1 whose
 *      payload is uint64_t[2], where element 0 carries the value.
 *
 * TODO: this description is not verified against the daemon and is likely
 * wrong in places: (a) the daemon (seanerbus.c) skips the reply when a request
 * yields no output, so a SET is not answered and the SET path cannot work yet;
 * (b) the review of 2026-09-28 reports capitalised keys and a "Domain" key
 * (Domain/Metric/Device/Device_no) instead of the lowercase keys used here;
 * (c) the cpu_freq driver registers its actuator under metric Power and
 * indexes CPUs with the device enum. Re-check all of this before relying on it.
 * TODO: only the read path was validated (2026-09-18, manual, laptop).

 * TODO: the Access Daemon README documents a richer text protocol ("met freq
 * cpu all get", "act freq cpu 0 set 2000000") that supports reset and
 * multi-device requests in a single round trip. That protocol is currently
 * reachable only through the daemon's keyboard plugin. Once the data plane
 * implements it, switch this client over: the payload encoding and decoding are
 * confined to access_daemon_exchange() and access_daemon_decode_value().
 */

#include "access_daemon.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char access_daemon_error[512];

static void access_daemon_set_error(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

static void access_daemon_set_error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(access_daemon_error, sizeof(access_daemon_error), fmt, ap);
	va_end(ap);
}

const char *access_daemon_last_error()
{
	return access_daemon_error;
}

#ifdef ACCESS_DAEMON_ENABLED

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <time.h>

#include <seanerbus/c/seanerbus.h>

// Name-service values used by the daemon, taken from its MET_NAME() labels
#define ACCESS_DAEMON_METRIC_FREQ "Freq"
#define ACCESS_DAEMON_METRIC_POWER "Power"
#define ACCESS_DAEMON_DEVICE_CPU "Cpu"
#define ACCESS_DAEMON_DEVICE_SOCKET "Socket"

// A GET_RES_V1 payload is uint64_t[2] and only element 0 carries the value
#define ACCESS_DAEMON_RES_VALUES 2

typedef struct access_daemon_group_s {
	const char *metric;
	const char *device;
	// Environment variable holding an explicit UUID list, see access_daemon_discover()
	const char *uuid_env;
	// Endpoint of every device, indexed by device number
	seanerbus_entity_t *address[ACCESS_DAEMON_MAX_DEVICES];
	int count;
	int discovered;
} access_daemon_group_t;

static seanerbus_connection_t *access_daemon_conn;
static int access_daemon_timeout_ms = ACCESS_DAEMON_DEFAULT_TIMEOUT_MS;
static int access_daemon_connected;

static access_daemon_group_t access_daemon_cpu_freq = {
	.metric = ACCESS_DAEMON_METRIC_FREQ,
	.device = ACCESS_DAEMON_DEVICE_CPU,
	.uuid_env = "CNTD_ACCESS_DAEMON_UUIDS_FREQ_CPU"
};
static access_daemon_group_t access_daemon_cpu_power = {
	.metric = ACCESS_DAEMON_METRIC_POWER,
	.device = ACCESS_DAEMON_DEVICE_CPU,
	.uuid_env = "CNTD_ACCESS_DAEMON_UUIDS_POWER_CPU"
};
static access_daemon_group_t access_daemon_socket_power = {
	.metric = ACCESS_DAEMON_METRIC_POWER,
	.device = ACCESS_DAEMON_DEVICE_SOCKET,
	.uuid_env = "CNTD_ACCESS_DAEMON_UUIDS_POWER_SOCKET"
};

static access_daemon_group_t *access_daemon_groups[] = {
	&access_daemon_cpu_freq, &access_daemon_cpu_power,
	&access_daemon_socket_power
};

typedef struct access_daemon_pending_s {
	int done;
	int failed;
	// Filled by the resolve callback
	seanerbus_entity_t *address;
	int address_count;
	// Filled by the request callback
	uint64_t value;
} access_daemon_pending_t;

static void access_daemon_store_error(const char *what,
				      seanerbus_status_t status,
				      seanerbus_error_t *error)
{
	access_daemon_set_error(
		"%s failed with status %d: %s", what, (int)status,
		error != NULL ? seanerbus_error_message(error) : "no details");
	seanerbus_error_destroy(error);
}

static double access_daemon_now_ms()
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1.0E3 + (double)ts.tv_nsec / 1.0E6;
}

/*
 * SeanerBUS is asynchronous. Rather than adding a thread to COUNTDOWN, the
 * connection file descriptor is driven here until the pending operation
 * completes or the deadline expires.
 */
static int access_daemon_pump(access_daemon_pending_t *pending,
			      const char *what)
{
	seanerbus_error_t *error = NULL;
	seanerbus_status_t status;
	double deadline;
	int fd;

	status = seanerbus_connection_get_fd(access_daemon_conn, &fd, &error);
	if (status != SEANERBUS_STATUS_OK) {
		access_daemon_store_error("seanerbus_connection_get_fd", status,
					  error);
		return -1;
	}

	deadline = access_daemon_now_ms() + (double)access_daemon_timeout_ms;

	while (!pending->done) {
		struct pollfd pfd;
		double remaining = deadline - access_daemon_now_ms();

		if (remaining <= 0.0) {
			access_daemon_set_error("%s timed out after %d ms",
						what, access_daemon_timeout_ms);
			return -1;
		}

		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;

		if (poll(&pfd, 1, (int)remaining) < 0) {
			if (errno == EINTR)
				continue;
			access_daemon_set_error("poll() on %s failed: %s", what,
						strerror(errno));
			return -1;
		}

		status =
			seanerbus_connection_handle(access_daemon_conn, &error);
		if (status != SEANERBUS_STATUS_OK) {
			access_daemon_store_error("seanerbus_connection_handle",
						  status, error);
			return -1;
		}
	}

	return pending->failed ? -1 : 0;
}

static void access_daemon_resolve_cb(seanerbus_connection_t *conn,
				     seanerbus_status_t status,
				     const seanerbus_entity_t *const *addresses,
				     size_t address_count,
				     const seanerbus_error_t *error,
				     void *user_data)
{
	access_daemon_pending_t *pending = (access_daemon_pending_t *)user_data;
	seanerbus_error_t *local_error = NULL;

	(void)conn;

	pending->done = 1;
	pending->address_count = (int)address_count;

	if (status != SEANERBUS_STATUS_OK) {
		pending->failed = 1;
		access_daemon_set_error(
			"Access Daemon name resolution failed: %s",
			error != NULL ? seanerbus_error_message(error) :
					"no details");
		return;
	}

	if (address_count == 0)
		return;

	// Addresses are borrowed, so keep an owned copy of the first match
	if (seanerbus_entity_clone(addresses[0], &pending->address,
				   &local_error) != SEANERBUS_STATUS_OK) {
		pending->failed = 1;
		access_daemon_store_error("seanerbus_entity_clone",
					  SEANERBUS_STATUS_OK, local_error);
	}
}

static void access_daemon_request_cb(seanerbus_connection_t *conn,
				     seanerbus_status_t status,
				     const seanerbus_message_t *message,
				     const seanerbus_error_t *error,
				     void *user_data)
{
	access_daemon_pending_t *pending = (access_daemon_pending_t *)user_data;
	seanerbus_byte_view_t payload = { 0 };
	seanerbus_error_t *local_error = NULL;
	uint64_t values[ACCESS_DAEMON_RES_VALUES];

	(void)conn;

	pending->done = 1;

	if (status != SEANERBUS_STATUS_OK) {
		pending->failed = 1;
		access_daemon_set_error("Access Daemon request failed: %s",
					error != NULL ?
						seanerbus_error_message(error) :
						"no details");
		return;
	}

	if (seanerbus_message_get_payload(message, &payload, &local_error) !=
	    SEANERBUS_STATUS_OK) {
		pending->failed = 1;
		access_daemon_store_error("seanerbus_message_get_payload",
					  SEANERBUS_STATUS_OK, local_error);
		return;
	}

	if (payload.data == NULL || payload.size < sizeof(values)) {
		pending->failed = 1;
		access_daemon_set_error(
			"Access Daemon returned %zu payload bytes, expected %zu",
			payload.size, sizeof(values));
		return;
	}

	// The payload is not guaranteed to be aligned, so copy it out
	memcpy(values, payload.data, sizeof(values));
	pending->value = values[0];
}

/*
 * Resolves one (metric, device, device_no) triple into an owned endpoint
 * address. Returns 0 on success, 1 when the daemon exposes no such device, and
 * -1 on failure.
 */
static int access_daemon_resolve(const char *metric, const char *device,
				 int device_no,
				 seanerbus_entity_t **out_address)
{
	access_daemon_pending_t pending = { 0 };
	seanerbus_name_entry_t names[3];
	seanerbus_error_t *error = NULL;
	seanerbus_status_t status;
	char device_no_str[16];
	char what[128];

	snprintf(device_no_str, sizeof(device_no_str), "%d", device_no);
	snprintf(what, sizeof(what), "resolution of %s/%s/%d", metric, device,
		 device_no);

	names[0] = seanerbus_name_entry_from_c_strings("metric", metric);
	names[1] = seanerbus_name_entry_from_c_strings("device", device);
	names[2] =
		seanerbus_name_entry_from_c_strings("device_no", device_no_str);

	status = seanerbus_resolve(access_daemon_conn, names, 3,
				   access_daemon_resolve_cb, &pending, &error);
	if (status != SEANERBUS_STATUS_OK) {
		access_daemon_store_error("seanerbus_resolve", status, error);
		return -1;
	}

	if (access_daemon_pump(&pending, what) < 0)
		return -1;

	if (pending.address == NULL)
		return 1;

	*out_address = pending.address;
	return 0;
}

/*
 * Builds the endpoint list from an explicit, comma-separated UUID list where
 * the position in the list is the device number. Returns 0 on success, 1 when
 * the variable is unset, and -1 on failure.
 *
 * SeanerBUS has no naming service implementation yet, so name resolution always
 * fails against the locator entity. The Access Daemon prints the UUID of every
 * endpoint it registers, which makes this a usable substitute until the naming
 * service exists. Once it does, simply leave the variable unset.
 */
static int access_daemon_discover_from_env(access_daemon_group_t *group)
{
	const char *list = getenv(group->uuid_env);
	char *copy;
	char *cursor;
	char *token;
	int count = 0;

	if (list == NULL)
		return 1;

	copy = strdup(list);
	if (copy == NULL) {
		access_daemon_set_error("Out of memory while reading %s",
					group->uuid_env);
		return -1;
	}

	cursor = copy;
	while ((token = strsep(&cursor, ",")) != NULL) {
		seanerbus_entity_t *address = NULL;
		seanerbus_error_t *error = NULL;

		while (*token == ' ')
			token++;
		if (*token == '\0')
			continue;

		if (count >= ACCESS_DAEMON_MAX_DEVICES) {
			access_daemon_set_error("%s lists more than %d UUIDs",
						group->uuid_env,
						ACCESS_DAEMON_MAX_DEVICES);
			free(copy);
			return -1;
		}

		if (seanerbus_entity_create_from_string(
			    token, &address, &error) != SEANERBUS_STATUS_OK) {
			access_daemon_set_error(
				"%s entry %d ('%s') is not a valid UUID: %s",
				group->uuid_env, count, token,
				error != NULL ? seanerbus_error_message(error) :
						"no details");
			seanerbus_error_destroy(error);
			free(copy);
			return -1;
		}

		group->address[count++] = address;
	}

	free(copy);

	if (count == 0) {
		access_daemon_set_error("%s is set but empty", group->uuid_env);
		return -1;
	}

	group->count = count;
	return 0;
}

/*
 * Walks device numbers upwards until the name service stops resolving, which
 * yields both the endpoint list and the device count in one pass.
 */
static int access_daemon_discover(access_daemon_group_t *group)
{
	int device_no;
	int rv;

	if (group->discovered) {
		if (group->count > 0)
			return 0;
		// Restate the reason, the buffer may hold another group's error
		access_daemon_set_error(
			"The Access Daemon does not expose any %s/%s device",
			group->metric, group->device);
		return -1;
	}

	rv = access_daemon_discover_from_env(group);
	if (rv < 0)
		return -1;
	if (rv == 0) {
		group->discovered = 1;
		return 0;
	}

	for (device_no = 0; device_no < ACCESS_DAEMON_MAX_DEVICES;
	     ++device_no) {
		seanerbus_entity_t *address = NULL;

		rv = access_daemon_resolve(group->metric, group->device,
					   device_no, &address);

		if (rv < 0)
			return -1;
		if (rv > 0)
			break;

		group->address[device_no] = address;
	}

	group->count = device_no;
	group->discovered = 1;

	if (group->count == 0) {
		access_daemon_set_error(
			"The Access Daemon does not expose any %s/%s device",
			group->metric, group->device);
		return -1;
	}

	return 0;
}

/*
 * Sends one request to a device endpoint and waits for its reply. payload_type
 * selects the operation: GET_REQ_V1 reads the sensor, SET_REQ_V1 writes the
 * actuator.
 */
static int access_daemon_exchange(access_daemon_group_t *group, int device_no,
				  uint64_t payload_type, uint64_t argument,
				  uint64_t *out_value)
{
	access_daemon_pending_t pending = { 0 };
	seanerbus_message_t *message = NULL;
	seanerbus_error_t *error = NULL;
	seanerbus_status_t status;
	seanerbus_byte_view_t payload;
	uint64_t values[ACCESS_DAEMON_RES_VALUES] = { argument, 0 };
	char what[128];
	int rv = -1;

	snprintf(what, sizeof(what), "request to %s/%s/%d", group->metric,
		 group->device, device_no);

	payload = seanerbus_byte_view_from_data(values, sizeof(values));

	status = seanerbus_message_create(group->address[device_no],
					  payload_type, payload, 0, &message,
					  &error);
	if (status != SEANERBUS_STATUS_OK) {
		access_daemon_store_error("seanerbus_message_create", status,
					  error);
		return -1;
	}

	status = seanerbus_connection_request(access_daemon_conn, message,
					      access_daemon_request_cb,
					      &pending, &error);
	if (status != SEANERBUS_STATUS_OK) {
		access_daemon_store_error("seanerbus_connection_request",
					  status, error);
		goto out;
	}

	if (access_daemon_pump(&pending, what) < 0)
		goto out;

	if (out_value != NULL)
		*out_value = pending.value;
	rv = 0;

out:
	seanerbus_message_destroy(message);
	return rv;
}

static int access_daemon_read(access_daemon_group_t *group, int device_no,
			      access_daemon_values_t *out)
{
	int first, last, device;

	out->count = 0;

	if (!access_daemon_connected) {
		access_daemon_set_error(
			"The Access Daemon client is not connected");
		return -1;
	}

	if (access_daemon_discover(group) < 0)
		return -1;

	if (device_no == ACCESS_DAEMON_DEV_ALL) {
		first = 0;
		last = group->count;
	} else {
		if (device_no < 0 || device_no >= group->count) {
			access_daemon_set_error(
				"Device %d is out of range, the Access Daemon exposes %d %s/%s devices",
				device_no, group->count, group->metric,
				group->device);
			return -1;
		}
		first = device_no;
		last = device_no + 1;
	}

	for (device = first; device < last; ++device) {
		uint64_t value = 0;

		if (access_daemon_exchange(group, device,
					   SEANERBUS_PAYLOAD_GET_REQ_V1, 0,
					   &value) < 0)
			return -1;

		out->values[out->count++] = value;
	}

	return 0;
}

static int access_daemon_count(access_daemon_group_t *group)
{
	if (!access_daemon_connected) {
		access_daemon_set_error(
			"The Access Daemon client is not connected");
		return -1;
	}

	if (access_daemon_discover(group) < 0)
		return -1;

	return group->count;
}

int access_daemon_init()
{
	const char *endpoint = getenv("CNTD_ACCESS_DAEMON_ENDPOINT");
	const char *timeout = getenv("CNTD_ACCESS_DAEMON_TIMEOUT_MS");
	seanerbus_error_t *error = NULL;
	seanerbus_status_t status;

	if (access_daemon_connected)
		return 0;

	if (endpoint == NULL)
		endpoint = ACCESS_DAEMON_DEFAULT_ENDPOINT;

	if (timeout != NULL) {
		long value = strtol(timeout, NULL, 10);

		if (value <= 0) {
			access_daemon_set_error(
				"CNTD_ACCESS_DAEMON_TIMEOUT_MS must be a positive number of milliseconds");
			return -1;
		}
		access_daemon_timeout_ms = (int)value;
	}

	status = seanerbus_connection_create(endpoint, endpoint,
					     &access_daemon_conn, &error);
	if (status != SEANERBUS_STATUS_OK) {
		access_daemon_store_error("seanerbus_connection_create", status,
					  error);
		access_daemon_conn = NULL;
		return -1;
	}

	access_daemon_connected = 1;
	return 0;
}

void access_daemon_finalize()
{
	seanerbus_error_t *error = NULL;
	size_t g;
	int i;

	for (g = 0;
	     g < sizeof(access_daemon_groups) / sizeof(access_daemon_groups[0]);
	     ++g) {
		access_daemon_group_t *group = access_daemon_groups[g];

		for (i = 0; i < group->count; ++i) {
			seanerbus_entity_destroy(group->address[i]);
			group->address[i] = NULL;
		}
		group->count = 0;
		group->discovered = 0;
	}

	if (access_daemon_conn != NULL) {
		if (seanerbus_connection_shutdown(access_daemon_conn, &error) !=
		    SEANERBUS_STATUS_OK)
			seanerbus_error_destroy(error);
		seanerbus_connection_destroy(access_daemon_conn);
		access_daemon_conn = NULL;
	}

	access_daemon_connected = 0;
}

int access_daemon_is_connected()
{
	return access_daemon_connected;
}

int access_daemon_count_cpu_freq()
{
	return access_daemon_count(&access_daemon_cpu_freq);
}

int access_daemon_count_cpu_power()
{
	return access_daemon_count(&access_daemon_cpu_power);
}

int access_daemon_count_socket_power()
{
	return access_daemon_count(&access_daemon_socket_power);
}

int access_daemon_get_cpu_freq(int cpu, access_daemon_values_t *out)
{
	return access_daemon_read(&access_daemon_cpu_freq, cpu, out);
}

int access_daemon_get_cpu_power(int cpu, access_daemon_values_t *out)
{
	return access_daemon_read(&access_daemon_cpu_power, cpu, out);
}

int access_daemon_get_socket_power(int socket, access_daemon_values_t *out)
{
	return access_daemon_read(&access_daemon_socket_power, socket, out);
}

int access_daemon_set_cpu_freq_limit(int cpu, uint64_t freq_khz)
{
	access_daemon_group_t *group = &access_daemon_cpu_freq;
	int first, last, device;

	if (!access_daemon_connected) {
		access_daemon_set_error(
			"The Access Daemon client is not connected");
		return -1;
	}

	if (access_daemon_discover(group) < 0)
		return -1;

	if (cpu == ACCESS_DAEMON_DEV_ALL) {
		first = 0;
		last = group->count;
	} else {
		if (cpu < 0 || cpu >= group->count) {
			access_daemon_set_error(
				"CPU %d is out of range, the Access Daemon exposes %d CPUs",
				cpu, group->count);
			return -1;
		}
		first = cpu;
		last = cpu + 1;
	}

	for (device = first; device < last; ++device) {
		if (access_daemon_exchange(group, device,
					   SEANERBUS_PAYLOAD_SET_REQ_V1,
					   freq_khz, NULL) < 0)
			return -1;
	}

	return 0;
}

#else // !ACCESS_DAEMON_ENABLED

#define ACCESS_DAEMON_DISABLED_MSG \
	"COUNTDOWN was built without Access Daemon support (CNTD_ENABLE_ACCESS_DAEMON=OFF)"

int access_daemon_init()
{
	access_daemon_set_error(ACCESS_DAEMON_DISABLED_MSG);
	return -1;
}

void access_daemon_finalize()
{
}

int access_daemon_is_connected()
{
	return 0;
}

int access_daemon_count_cpu_freq()
{
	access_daemon_set_error(ACCESS_DAEMON_DISABLED_MSG);
	return -1;
}

int access_daemon_count_cpu_power()
{
	access_daemon_set_error(ACCESS_DAEMON_DISABLED_MSG);
	return -1;
}

int access_daemon_count_socket_power()
{
	access_daemon_set_error(ACCESS_DAEMON_DISABLED_MSG);
	return -1;
}

int access_daemon_get_cpu_freq(int cpu, access_daemon_values_t *out)
{
	(void)cpu;
	out->count = 0;
	access_daemon_set_error(ACCESS_DAEMON_DISABLED_MSG);
	return -1;
}

int access_daemon_get_cpu_power(int cpu, access_daemon_values_t *out)
{
	(void)cpu;
	out->count = 0;
	access_daemon_set_error(ACCESS_DAEMON_DISABLED_MSG);
	return -1;
}

int access_daemon_get_socket_power(int socket, access_daemon_values_t *out)
{
	(void)socket;
	out->count = 0;
	access_daemon_set_error(ACCESS_DAEMON_DISABLED_MSG);
	return -1;
}

int access_daemon_set_cpu_freq_limit(int cpu, uint64_t freq_khz)
{
	(void)cpu;
	(void)freq_khz;
	access_daemon_set_error(ACCESS_DAEMON_DISABLED_MSG);
	return -1;
}

#endif // ACCESS_DAEMON_ENABLED
