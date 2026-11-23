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
 * EasyBUS stub for the Access Daemon baseline tests.
 *
 * The tests preload it ahead of libcntd.so, so its easybus_* functions replace
 * those of libaccess_easybus. The shipped libcntd.so and its EasyBUS adapter
 * run unchanged, and no broker or daemon is involved. Each EasyBUS call
 * writes one line to stderr, for example:
 *
 *   easybus-stub[4242]: get Met Power/Socket dev=-1 -> ok, 2 devices
 *
 * GET returns STUB_DEVICES devices of STUB_VALUE watts. Environment, read at
 * connect:
 *
 *   CNTD_EASYBUS_STUB_FAIL      contains "get" and/or "set": those requests
 *                               fail with EIO and an injected daemon error
 *   CNTD_EASYBUS_STUB_DELAY_US  sleep this many microseconds in each GET/SET
 *
 * The EasyBUS functions COUNTDOWN does not use record "unexpected <name>" and
 * fail with ENOSYS.
 *
 * Records use one write() so output from different ranks does not interleave.
 */

#define _GNU_SOURCE
#include <access_easybus.h>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define STUB_DEVICES 2
#define STUB_VALUE 100
// access_easybus.h requires SET error buffers of this size
#define EASYBUS_ERROR_SIZE 256

static int connected;
static int fail_get;
static int fail_set;
static long delay_us;
static pthread_mutex_t set_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t set_gate_cond = PTHREAD_COND_INITIALIZER;
static int set_gate_armed;
static int set_gate_waiting;
static int set_gate_released;
static int set_gate_timed_out;
static int set_gate_completed;
static sem_t completed_gets;

static const char *const domain_names[] = { [Met] = "Met", [Act] = "Act" };
static const char *const metric_names[] = {
	[Power] = "Power",
	[Energy] = "Energy",
	[Freq] = "Freq",
};
static const char *const device_names[] = {
	[Cpu] = "Cpu",
	[Core] = "Core",
	[Socket] = "Socket",
};

#define NAME(names, value) \
	name_of(names, sizeof(names) / sizeof(names[0]), (int)(value))

static const char *name_of(const char *const names[], int count, int value)
{
	if (value < 0 || value >= count || names[value] == NULL)
		return "?";
	return names[value];
}

static void record(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void record(const char *fmt, ...)
{
	char line[512];
	int len;
	va_list ap;
	ssize_t unused;

	len = snprintf(line, sizeof(line), "easybus-stub[%d]: ", (int)getpid());
	va_start(ap, fmt);
	len += vsnprintf(line + len, sizeof(line) - len, fmt, ap);
	va_end(ap);

	if (len > (int)sizeof(line) - 1)
		len = sizeof(line) - 1;
	len += snprintf(line + len, sizeof(line) - len, " tid=%ld",
			(long)syscall(SYS_gettid));

	// The newline replaces the terminating NUL, also after truncation
	if (len > (int)sizeof(line) - 1)
		len = sizeof(line) - 1;
	line[len++] = '\n';

	unused = write(STDERR_FILENO, line, len);
	(void)unused;
}

static int unexpected(const char *what, int error)
{
	record("unexpected %s", what);
	errno = error;
	return 0;
}

static void read_settings()
{
	const char *fail = getenv("CNTD_EASYBUS_STUB_FAIL");
	const char *delay = getenv("CNTD_EASYBUS_STUB_DELAY_US");

	fail_get = fail != NULL && strstr(fail, "get") != NULL;
	fail_set = fail != NULL && strstr(fail, "set") != NULL;
	delay_us = delay != NULL ? strtol(delay, NULL, 10) : 0;
	if (delay_us < 0)
		delay_us = 0;
}

static void apply_delay()
{
	struct timespec left = { .tv_sec = delay_us / 1000000,
				 .tv_nsec = delay_us % 1000000 * 1000 };

	if (delay_us == 0)
		return;

	// A signal may interrupt the sleep; sleep the remaining time
	while (nanosleep(&left, &left) < 0 && errno == EINTR)
		;
}

static struct timespec stub_deadline()
{
	struct timespec deadline;

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 5;
	return deadline;
}

void easybus_stub_hold_next_set()
{
	pthread_mutex_lock(&set_gate_mutex);
	set_gate_armed = 1;
	set_gate_waiting = 0;
	set_gate_released = 0;
	set_gate_timed_out = 0;
	set_gate_completed = 0;
	pthread_mutex_unlock(&set_gate_mutex);
}

int easybus_stub_wait_for_held_set()
{
	struct timespec deadline = stub_deadline();
	int result = 1;

	pthread_mutex_lock(&set_gate_mutex);
	while (!set_gate_waiting && !set_gate_timed_out) {
		if (pthread_cond_timedwait(&set_gate_cond, &set_gate_mutex,
					   &deadline) != 0) {
			result = 0;
			break;
		}
	}
	if (set_gate_timed_out)
		result = 0;
	pthread_mutex_unlock(&set_gate_mutex);
	return result;
}

int easybus_stub_release_and_wait()
{
	struct timespec deadline = stub_deadline();
	int result = 1;

	pthread_mutex_lock(&set_gate_mutex);
	set_gate_released = 1;
	pthread_cond_broadcast(&set_gate_cond);
	while (set_gate_completed < 2 && !set_gate_timed_out) {
		if (pthread_cond_timedwait(&set_gate_cond, &set_gate_mutex,
					   &deadline) != 0) {
			result = 0;
			break;
		}
	}
	if (set_gate_timed_out)
		result = 0;
	pthread_mutex_unlock(&set_gate_mutex);
	return result;
}

static void wait_at_set_gate()
{
	struct timespec deadline;

	pthread_mutex_lock(&set_gate_mutex);
	if (set_gate_armed) {
		set_gate_armed = 0;
		set_gate_waiting = 1;
		deadline = stub_deadline();
		pthread_cond_broadcast(&set_gate_cond);
		while (!set_gate_released) {
			if (pthread_cond_timedwait(&set_gate_cond,
						   &set_gate_mutex,
						   &deadline) != 0) {
				set_gate_timed_out = 1;
				break;
			}
		}
		set_gate_waiting = 0;
		pthread_cond_broadcast(&set_gate_cond);
	}
	pthread_mutex_unlock(&set_gate_mutex);
}

static void complete_get()
{
	sem_post(&completed_gets);
}

int easybus_stub_wait_for_two_gets()
{
	struct timespec deadline = stub_deadline();
	int result;

	for (int i = 0; i < 2; i++) {
		do {
			result = sem_timedwait(&completed_gets, &deadline);
		} while (result < 0 && errno == EINTR);
		if (result < 0)
			return 0;
	}
	return 1;
}

int easybus_connect(char *address)
{
	sigset_t mask;

	read_settings();
	if (sem_init(&completed_gets, 0, 0) < 0)
		return unexpected("GET semaphore", errno);
	connected = 1;
	record("connect %s -> ok", address != NULL ? address : "(default)");
	if (pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0)
		record("signal-mask SIGALRM=%d SIGRTMIN=%d",
		       sigismember(&mask, SIGALRM),
		       sigismember(&mask, SIGRTMIN));
	return 1;
}

void easybus_disconnect()
{
	connected = 0;
	record("disconnect");
	sem_destroy(&completed_gets);
}

int easybus_is_connected()
{
	record("is-connected");
	return connected;
}

int easybus_request_get(domain_e dom, metric_e met, device_e dev, int dev_no,
			metric_t **mets, int *mets_count)
{
	static metric_t result;
	static uint64_t values[STUB_DEVICES];
	int i;

	apply_delay();

	if (!connected)
		return unexpected("get while disconnected", ENOTCONN);
	// COUNTDOWN reads results from the storage EasyBUS owns
	if (mets == NULL || *mets != NULL || mets_count == NULL)
		return unexpected("get storage", EINVAL);

	result = (metric_t){
		.domain = dom, .metric = met, .device = dev, .device_no = dev_no
	};
	*mets = &result;
	*mets_count = 1;

	if (fail_get) {
		result.state = Error;
		result.error_msg = "injected GET failure";
		record("get %s %s/%s dev=%d -> failed", NAME(domain_names, dom),
		       NAME(metric_names, met), NAME(device_names, dev),
		       dev_no);
		complete_get();
		errno = EIO;
		return 0;
	}

	for (i = 0; i < STUB_DEVICES; i++)
		values[i] = STUB_VALUE;
	result.values = values;
	result.devs_count = STUB_DEVICES;
	record("get %s %s/%s dev=%d -> ok, %d devices", NAME(domain_names, dom),
	       NAME(metric_names, met), NAME(device_names, dev), dev_no,
	       STUB_DEVICES);
	complete_get();
	return 1;
}

int easybus_request_set_imm(metric_e met, device_e dev, int dev_no,
			    uint64_t value, char *error_msg)
{
	wait_at_set_gate();
	apply_delay();

	if (!connected)
		return unexpected("set while disconnected", ENOTCONN);

	if (fail_set) {
		if (error_msg != NULL)
			snprintf(error_msg, EASYBUS_ERROR_SIZE,
				 "injected SET failure");
		record("set %s/%s dev=%d value=%" PRIu64 " -> failed",
		       NAME(metric_names, met), NAME(device_names, dev), dev_no,
		       value);
		errno = EIO;
		return 0;
	}

	record("set %s/%s dev=%d value=%" PRIu64 " -> ok",
	       NAME(metric_names, met), NAME(device_names, dev), dev_no, value);

	pthread_mutex_lock(&set_gate_mutex);
	if (set_gate_released) {
		set_gate_completed++;
		pthread_cond_broadcast(&set_gate_cond);
	}
	pthread_mutex_unlock(&set_gate_mutex);
	return 1;
}

int easybus_get_available_metrics(metric_t **mets, int *mets_count)
{
	return unexpected(__func__, ENOSYS);
}

int easybus_search_available_metrics(domain_e dom, metric_e met, device_e dev,
				     metric_t **_met)
{
	return unexpected(__func__, ENOSYS);
}

int easybus_request_set(metric_e met, device_e dev, int dev_no,
			uint64_t *values, int values_count, char *error_msg)
{
	return unexpected(__func__, ENOSYS);
}

int easybus_metrics_alloc(metric_t **mets)
{
	return unexpected(__func__, ENOSYS);
}
