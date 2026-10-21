/*
 * Copyright (c), CINECA, UNIBO, and ETH Zurich
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *			* Redistributions of source code must retain the above
 *copyright notice, this list of conditions and the following disclaimer.
 *
 *			* Redistributions in binary form must reproduce the
 *above copyright notice, this list of conditions and the following disclaimer
 *in the documentation and/or other materials provided with the distribution.
 *
 *			* Neither the name of the copyright holder nor the names
 *of its contributors may be used to endorse or promote products derived from
 *this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * SEANERGYS Access Daemon client.
 *
 * COUNTDOWN delegates CPU-frequency actuation and core/socket power readings to
 * the Access Daemon instead of driving MSRs and cpufreq sysfs itself.
 *
 * Transport: the EasyBUS library shipped with the Access Daemon
 * (libaccess_easybus), which hides SeanerBUS name resolution and messaging.
 * EasyBUS keeps one metric_t per (metric, device) kind, whose values[] is
 * indexed by device number and whose devs_count is the number of devices.
 *
 * CPU-frequency GET addresses Metric/Freq/Cpu. SET addresses Actuator/Freq/Cpu
 * and requires EasyBUS support for SetReqV1 and explicit ErrorResV1 results.
 * Calls are blocking and must be serialized outside signal handlers.
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

#include <access_easybus.h>

typedef struct access_daemon_group_s {
	metric_e metric;
	device_e device;
	const char *label;
} access_daemon_group_t;

static const access_daemon_group_t access_daemon_cpu_freq = { Freq, Cpu,
							      "Freq/Cpu" };

static const access_daemon_group_t access_daemon_core_power = { Power, Core,
								"Power/Core" };

static const access_daemon_group_t access_daemon_socket_power = {
	Power, Socket, "Power/Socket"
};

static const metric_t *access_daemon_fetch(const access_daemon_group_t *group,
					   int device_no)
{
	metric_t *mets = NULL;
	int mets_count = 0;
	int m;

	if (!easybus_is_connected()) {
		access_daemon_set_error(
			"The Access Daemon client is not connected");
		return NULL;
	}

	errno = 0;
	if (!easybus_request(Met, group->metric, group->device, Get, device_no,
			     NULL, &mets, &mets_count)) {
		access_daemon_set_error(
			"EasyBUS request for %s failed: %s", group->label,
			errno != 0 ? strerror(errno) : "no details");
		return NULL;
	}

	for (m = 0; m < mets_count; m++) {
		if (mets[m].metric == group->metric &&
		    mets[m].device == group->device)
			return &mets[m];
	}

	access_daemon_set_error("EasyBUS returned no %s data", group->label);
	return NULL;
}

static int access_daemon_read(const access_daemon_group_t *group, int device_no,
			      access_daemon_values_t *out)
{
	const metric_t *met;
	int first, last, device;

	out->count = 0;

	met = access_daemon_fetch(group, device_no);
	if (met == NULL)
		return -1;

	if (met->devs_count == 0) {
		access_daemon_set_error(
			"The Access Daemon exposes no %s devices",
			group->label);
		return -1;
	}

	if (device_no == ACCESS_DAEMON_DEV_ALL) {
		first = 0;
		last = (int)met->devs_count;
	} else {
		first = device_no;
		last = device_no + 1;
	}

	if (first < 0 || last > (int)met->devs_count ||
	    last > ACCESS_DAEMON_MAX_DEVICES) {
		access_daemon_set_error(
			"Device number %d is out of range, the Access Daemon exposes %u %s devices",
			device_no, met->devs_count, group->label);
		return -1;
	}

	for (device = first; device < last; device++)
		out->values[out->count++] = met->values[device];

	return 0;
}

static int access_daemon_count(const access_daemon_group_t *group)
{
	const metric_t *met = access_daemon_fetch(group, ACCESS_DAEMON_DEV_ALL);

	return met != NULL ? (int)met->devs_count : -1;
}

int access_daemon_init()
{
	const char *endpoint = getenv("CNTD_ACCESS_DAEMON_ENDPOINT");

	if (easybus_is_connected())
		return 0;

	if (endpoint == NULL)
		endpoint = ACCESS_DAEMON_DEFAULT_ENDPOINT;

	errno = 0;
	if (!easybus_connect((char *)endpoint)) {
		access_daemon_set_error("EasyBUS connection failed: %s",
					errno != 0 ? strerror(errno) :
						     "no details");
		return -1;
	}

	return 0;
}

void access_daemon_finalize()
{
	easybus_disconnect();
}

int access_daemon_is_connected()
{
	return easybus_is_connected();
}

int access_daemon_count_cpu_freq()
{
	return access_daemon_count(&access_daemon_cpu_freq);
}

int access_daemon_count_core_power()
{
	return access_daemon_count(&access_daemon_core_power);
}

int access_daemon_count_socket_power()
{
	return access_daemon_count(&access_daemon_socket_power);
}

int access_daemon_get_cpu_freq(int cpu, access_daemon_values_t *out)
{
	return access_daemon_read(&access_daemon_cpu_freq, cpu, out);
}

int access_daemon_get_core_power(int core, access_daemon_values_t *out)
{
	return access_daemon_read(&access_daemon_core_power, core, out);
}

int access_daemon_get_socket_power(int socket, access_daemon_values_t *out)
{
	return access_daemon_read(&access_daemon_socket_power, socket, out);
}

int access_daemon_set_cpu_freq_limit(int cpu, uint64_t freq_khz)
{
	if (cpu < ACCESS_DAEMON_DEV_ALL || freq_khz == 0) {
		access_daemon_set_error(
			"CPU selector must be -1 or nonnegative and frequency must be positive");
		return -1;
	}

	errno = 0;
	if (!easybus_request(Act, Freq, Cpu, Set, cpu, &freq_khz, NULL, NULL)) {
		access_daemon_set_error(
			"EasyBUS CPU-frequency SET failed: %s; some settings may have changed",
			errno != 0 ? strerror(errno) : "no details");
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

int access_daemon_count_core_power()
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

int access_daemon_get_core_power(int core, access_daemon_values_t *out)
{
	(void)core;
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
