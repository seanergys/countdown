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
 * Access Daemon backend: the only part of COUNTDOWN that talks to the Access
 * Daemon, through the EasyBUS adapter in access_daemon.c. The frequency and
 * sampling code call it when cntd->use_access_daemon is set.
 *
 * The worker owns all EasyBUS calls. Signal handlers post frequency requests
 * and read the latest socket-power sample without waiting for the worker.
 */

#include "cntd.h"
#include "access_daemon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ACCESS_DAEMON_ENABLED
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

#define NANOSECONDS_PER_SECOND 1000000000ULL
#define NANOSECONDS_PER_MILLISECOND 1000000ULL

static struct {
	pthread_t thread;
	sem_t ready;
	int wake[2];
	int startup_status;
	char startup_error[512];
	char hostname[STRING_SIZE];
	int world_rank;
	int monitor_power;
	uint64_t polling_interval_ns;
	int restore_frequency;
	int restore_enabled;
	_Atomic int running;
	_Atomic int notifiers;
	_Atomic int stopping;
	_Atomic int failed;
	_Atomic int cpu;
	_Atomic int pending_frequency;
	struct {
		_Atomic unsigned int sequence;
		_Atomic int count;
		_Atomic uint64_t watts[MAX_NUM_SOCKETS];
		_Atomic uint64_t time_ns;
	} power[2];
	_Atomic int published_power;
	_Atomic uint64_t sample_age_ns;
} backend;

static uint64_t monotonic_time_ns()
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * NANOSECONDS_PER_SECOND + now.tv_nsec;
}

static void wake_worker()
{
	char byte = 0;
	ssize_t result;

	do {
		result = write(backend.wake[1], &byte, sizeof(byte));
	} while (result < 0 && errno == EINTR);
}

static void poll_socket_power()
{
	access_daemon_values_t values = { 0 };
	int count, i, next;

	if (access_daemon_get_socket_power(ACCESS_DAEMON_DEV_ALL, &values) <
	    0) {
		/* Make a failed GET visible before error logging can block. */
		atomic_store(&backend.published_power, -1);
		fprintf(stderr,
			"Error: <COUNTDOWN-node:%s-rank:%d> Failed to read the socket power through the Access Daemon: %s\n",
			backend.hostname, backend.world_rank,
			access_daemon_last_error());
		return;
	}
	count = values.count < MAX_NUM_SOCKETS ? values.count : MAX_NUM_SOCKETS;

	next = atomic_load(&backend.published_power) == 0 ? 1 : 0;
	atomic_fetch_add(&backend.power[next].sequence, 1);
	for (i = 0; i < count; i++)
		atomic_store(&backend.power[next].watts[i], values.values[i]);
	atomic_store(&backend.power[next].time_ns, monotonic_time_ns());
	atomic_store(&backend.power[next].count, count);
	atomic_fetch_add(&backend.power[next].sequence, 1);
	atomic_store(&backend.published_power, next);
}

static void apply_frequency(int freq_khz, int restoring)
{
	int cpu = atomic_load(&backend.cpu);

	if (access_daemon_set_cpu_freq_limit(cpu, (uint64_t)freq_khz) < 0) {
		fprintf(stderr,
			"Error: <COUNTDOWN-node:%s-rank:%d> Failed to %s the frequency of CPU %d through the Access Daemon: %s%s\n",
			backend.hostname, backend.world_rank,
			restoring ? "restore" : "set", cpu,
			access_daemon_last_error(),
			restoring ?
				"" :
				". Frequency control is disabled for the rest of the run");
		atomic_store(&backend.failed, 1);
	}
}

static int worker_timeout(uint64_t deadline_ns)
{
	uint64_t now = monotonic_time_ns();

	if (deadline_ns <= now)
		return 0;
	return (int)((deadline_ns - now + NANOSECONDS_PER_MILLISECOND - 1) /
		     NANOSECONDS_PER_MILLISECOND);
}

static void *ad_worker(void *unused)
{
	struct pollfd wake = { .fd = backend.wake[0], .events = POLLIN };
	uint64_t deadline_ns = 0;
	char bytes[256];
	int freq, result;

	(void)unused;
	if (access_daemon_init() < 0) {
		backend.startup_status = -1;
		snprintf(backend.startup_error, sizeof(backend.startup_error),
			 "%s", access_daemon_last_error());
		sem_post(&backend.ready);
		return NULL;
	}

	if (backend.monitor_power)
		poll_socket_power();
	if (backend.polling_interval_ns > 0)
		deadline_ns = monotonic_time_ns() + backend.polling_interval_ns;
	sem_post(&backend.ready);

	for (;;) {
		freq = atomic_exchange(&backend.pending_frequency, -1);
		if (freq >= 0 && !atomic_load(&backend.failed))
			apply_frequency(freq, 0);

		if (atomic_load(&backend.stopping)) {
			freq = atomic_exchange(&backend.pending_frequency, -1);
			if (freq >= 0 && !atomic_load(&backend.failed))
				apply_frequency(freq, 0);
			/* Also after a failed SET: the limit is unknown. */
			if (backend.restore_enabled)
				apply_frequency(backend.restore_frequency, 1);
			break;
		}

		if (deadline_ns > 0 && monotonic_time_ns() >= deadline_ns) {
			poll_socket_power();
			deadline_ns = monotonic_time_ns() +
				      backend.polling_interval_ns;
			continue;
		}

		result = poll(&wake, 1,
			      deadline_ns > 0 ? worker_timeout(deadline_ns) :
						-1);
		if (result < 0 && errno == EINTR)
			continue;
		if (result < 0 ||
		    (wake.revents & (POLLERR | POLLHUP | POLLNVAL))) {
			atomic_store(&backend.published_power, -1);
			fprintf(stderr,
				"Error: <COUNTDOWN-node:%s-rank:%d> Access Daemon worker wake-up failed. Frequency control and power monitoring are disabled for the rest of the run\n",
				backend.hostname, backend.world_rank);
			atomic_store(&backend.failed, 1);
			break;
		}
		if (result > 0) {
			ssize_t ignored =
				read(backend.wake[0], bytes, sizeof(bytes));
			(void)ignored;
		}
	}

	access_daemon_finalize();
	return NULL;
}

static void fail_worker_start(const char *error)
{
	fprintf(stderr,
		"Error: <COUNTDOWN-node:%s-rank:%d> Failed to start the Access Daemon worker: %s\n",
		backend.hostname, backend.world_rank, error);
	PMPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
}
#endif

HIDDEN void ad_backend_start()
{
	char hostname[STRING_SIZE];

	// cntd->node.hostname is filled in later, by init_arch_conf()
	gethostname(hostname, sizeof(hostname));

	if (!access_daemon_is_enabled()) {
		fprintf(stderr,
			"Error: <COUNTDOWN-node:%s-rank:%d> CNTD_USE_ACCESS_DAEMON is set but COUNTDOWN was built without Access Daemon support, rebuild with -DCNTD_ENABLE_ACCESS_DAEMON=ON\n",
			hostname, cntd->rank->world_rank);
		PMPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
		return;
	}

#ifdef ACCESS_DAEMON_ENABLED
	sigset_t blocked, previous;
	int error;

	snprintf(backend.hostname, sizeof(backend.hostname), "%s", hostname);
	backend.world_rank = cntd->rank->world_rank;
	backend.monitor_power = cntd->iam_master && cntd->enable_power_monitor;
	backend.polling_interval_ns =
		backend.monitor_power ? (uint64_t)(cntd->sampling_time *
						   NANOSECONDS_PER_SECOND) :
					0;
	atomic_store(&backend.pending_frequency, -1);
	atomic_store(&backend.cpu, -1);
	atomic_store(&backend.published_power, -1);
	atomic_store(&backend.power[0].count, -1);
	atomic_store(&backend.power[1].count, -1);
	atomic_store(&backend.sample_age_ns, UINT64_MAX);

	if (!atomic_is_lock_free(&backend.running) ||
	    !atomic_is_lock_free(&backend.power[0].sequence) ||
	    !atomic_is_lock_free(&backend.power[0].watts[0])) {
		fail_worker_start("signal-handler atomics are not lock-free");
		return;
	}
	if (sem_init(&backend.ready, 0, 0) < 0) {
		fail_worker_start(strerror(errno));
		return;
	}
	if (pipe2(backend.wake, O_NONBLOCK | O_CLOEXEC) < 0) {
		sem_destroy(&backend.ready);
		fail_worker_start(strerror(errno));
		return;
	}

	sigemptyset(&blocked);
	sigaddset(&blocked, SIGALRM);
	sigaddset(&blocked, SIGRTMIN);
	error = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
	if (error != 0) {
		fail_worker_start(strerror(error));
		return;
	}
	error = pthread_create(&backend.thread, NULL, ad_worker, NULL);
	if (pthread_sigmask(SIG_SETMASK, &previous, NULL) != 0) {
		fail_worker_start("could not restore the signal mask");
		return;
	}
	if (error != 0) {
		close(backend.wake[0]);
		close(backend.wake[1]);
		sem_destroy(&backend.ready);
		fail_worker_start(strerror(error));
		return;
	}

	do {
		error = sem_wait(&backend.ready);
	} while (error < 0 && errno == EINTR);
	if (error < 0) {
		fail_worker_start(strerror(errno));
		return;
	}
	if (backend.startup_status < 0) {
		pthread_join(backend.thread, NULL);
		close(backend.wake[0]);
		close(backend.wake[1]);
		sem_destroy(&backend.ready);
		fprintf(stderr,
			"Error: <COUNTDOWN-node:%s-rank:%d> Failed to connect to the Access Daemon: %s\n",
			hostname, cntd->rank->world_rank,
			backend.startup_error);
		PMPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
		return;
	}
	atomic_store(&backend.running, 1);
#endif
}

HIDDEN void ad_backend_set_cpu(int cpu)
{
#ifdef ACCESS_DAEMON_ENABLED
	atomic_store(&backend.cpu, cpu);
#else
	(void)cpu;
#endif
}

HIDDEN void ad_backend_request_freq(int freq_khz)
{
#ifdef ACCESS_DAEMON_ENABLED
	int saved_errno = errno;

	if (atomic_load(&backend.running)) {
		atomic_fetch_add(&backend.notifiers, 1);
		if (atomic_load(&backend.running)) {
			atomic_store(&backend.pending_frequency, freq_khz);
			wake_worker();
		}
		atomic_fetch_sub(&backend.notifiers, 1);
	}
	errno = saved_errno;
#else
	(void)freq_khz;
#endif
}

HIDDEN int ad_backend_read_socket_power(uint64_t watts[MAX_NUM_SOCKETS])
{
#ifdef ACCESS_DAEMON_ENABLED
	unsigned int sequence;
	uint64_t sample_time, now;
	int count, i, slot, saved_errno = errno;

	/* The worker fills a slot before publishing it. */
	for (;;) {
		slot = atomic_load(&backend.published_power);
		if (slot < 0)
			goto unavailable;
		sequence = atomic_load(&backend.power[slot].sequence);
		if (sequence & 1)
			continue;
		count = atomic_load(&backend.power[slot].count);
		for (i = 0; i < count; i++)
			watts[i] = atomic_load(&backend.power[slot].watts[i]);
		sample_time = atomic_load(&backend.power[slot].time_ns);
		if (atomic_load(&backend.power[slot].sequence) == sequence) {
			if (atomic_load(&backend.published_power) < 0)
				goto unavailable;
			break;
		}
	}
	if (count < 0)
		goto unavailable;

	now = monotonic_time_ns();
	atomic_store(&backend.sample_age_ns,
		     sample_time > 0 && now >= sample_time ? now - sample_time :
							     UINT64_MAX);
	errno = saved_errno;
	return count;

unavailable:
	atomic_store(&backend.sample_age_ns, UINT64_MAX);
	errno = saved_errno;
	return -1;
#else
	(void)watts;
	return -1;
#endif
}

HIDDEN double ad_backend_sample_age()
{
#ifdef ACCESS_DAEMON_ENABLED
	uint64_t age = atomic_load(&backend.sample_age_ns);

	return age == UINT64_MAX ? -1.0 : (double)age / NANOSECONDS_PER_SECOND;
#else
	return -1.0;
#endif
}

HIDDEN void ad_backend_stop()
{
#ifdef ACCESS_DAEMON_ENABLED
	int error;

	atomic_store(&backend.running, 0);
	while (atomic_load(&backend.notifiers) != 0)
		sched_yield();
	backend.restore_enabled =
		(cntd->enable_eam || cntd->enable_eam_slack) &&
		!cntd->enable_eam_analysis;
	backend.restore_frequency = cntd->sys_freq_khz[MAX];
	atomic_store(&backend.stopping, 1);
	wake_worker();
	error = pthread_join(backend.thread, NULL);
	if (error != 0) {
		fail_worker_start(strerror(error));
		return;
	}
	close(backend.wake[0]);
	close(backend.wake[1]);
	sem_destroy(&backend.ready);
#endif
}
