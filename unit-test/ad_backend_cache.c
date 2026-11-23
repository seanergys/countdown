#include "cntd.h"
#include <access_easybus.h>

#include <errno.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum get_action { GET_NORMAL, GET_HOLD_PUBLICATION, GET_FAIL };
enum sample_phase { SAMPLE_INITIAL, SAMPLE_UPDATED, SAMPLE_RECOVERED };

static _Atomic int next_get;
static _Thread_local int hold_clock;
static _Thread_local enum sample_phase sample_phase;
static sem_t publishing;
static sem_t resume_worker;
static volatile sig_atomic_t signal_count;
static volatile sig_atomic_t signal_first;
static volatile sig_atomic_t signal_second;
static volatile sig_atomic_t signal_errno;

static CNTD_t runtime;
static CNTD_RankInfo_t rank;
CNTD_t *cntd = &runtime;

int __real_easybus_request_get(domain_e, metric_e, device_e, int, metric_t **,
			       int *);
int __real_clock_gettime(clockid_t, struct timespec *);

int __wrap_easybus_request_get(domain_e dom, metric_e met, device_e dev,
			       int dev_no, metric_t **mets, int *mets_count)
{
	int result = __real_easybus_request_get(dom, met, dev, dev_no, mets,
						mets_count);
	int action = atomic_exchange(&next_get, GET_NORMAL);

	if (!result)
		return result;
	if (action == GET_HOLD_PUBLICATION) {
		sample_phase = SAMPLE_UPDATED;
		hold_clock = 1;
	} else if (action == GET_FAIL) {
		sample_phase = SAMPLE_RECOVERED;
		(*mets)[0].state = Error;
		(*mets)[0].error_msg = "injected GET failure";
		hold_clock = 1;
		errno = EIO;
		return 0;
	}
	if (sample_phase == SAMPLE_UPDATED) {
		(*mets)[0].values[0] = 200;
		(*mets)[0].values[1] = 250;
	} else if (sample_phase == SAMPLE_RECOVERED) {
		(*mets)[0].values[0] = 300;
		(*mets)[0].values[1] = 350;
	}
	return result;
}

int __wrap_clock_gettime(clockid_t clock, struct timespec *time)
{
	/* Hold the worker at its first clock read after each injected GET. */
	if (clock == CLOCK_MONOTONIC && hold_clock) {
		hold_clock = 0;
		sem_post(&publishing);
		while (sem_wait(&resume_worker) < 0 && errno == EINTR)
			;
	}
	return __real_clock_gettime(clock, time);
}

static void read_in_signal(int sig)
{
	uint64_t watts[MAX_NUM_SOCKETS];
	int count, saved_errno = errno;

	(void)sig;
	errno = EDOM;
	count = ad_backend_read_socket_power(watts);
	signal_errno = errno;
	errno = saved_errno;
	signal_count = count;
	if (count >= 2) {
		signal_first = watts[0];
		signal_second = watts[1];
	}
}

static int wait_for_publication(void)
{
	struct timespec deadline;
	int result;

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 5;
	do {
		result = sem_timedwait(&publishing, &deadline);
	} while (result < 0 && errno == EINTR);
	return result;
}

static int wait_for_sample(uint64_t first, uint64_t second)
{
	struct timespec pause = { .tv_nsec = 10000000 };
	uint64_t watts[MAX_NUM_SOCKETS];

	for (int i = 0; i < 300; i++) {
		int count = ad_backend_read_socket_power(watts);
		if (count == 2 && watts[0] == first && watts[1] == second)
			return 0;
		nanosleep(&pause, NULL);
	}
	return -1;
}

int main(int argc, char **argv)
{
	struct sigaction action = { 0 };
	uint64_t watts[MAX_NUM_SOCKETS];
	int no_sample = argc == 2 && strcmp(argv[1], "no-sample") == 0;
	int result = 1;

	if (argc > 1 && !no_sample)
		return 1;
	if (sem_init(&publishing, 0, 0) < 0)
		return 1;
	if (sem_init(&resume_worker, 0, 0) < 0) {
		sem_destroy(&publishing);
		return 1;
	}
	if (no_sample)
		setenv("CNTD_EASYBUS_STUB_FAIL", "get", 1);
	else
		unsetenv("CNTD_EASYBUS_STUB_FAIL");
	unsetenv("CNTD_EASYBUS_STUB_DELAY_US");
	runtime.rank = &rank;
	runtime.iam_master = 1;
	runtime.enable_power_monitor = 1;
	runtime.sampling_time = 1;
	ad_backend_start();

	if (no_sample) {
		if (ad_backend_read_socket_power(watts) == -1)
			result = 0;
		else
			fprintf(stderr,
				"A missing sample was reported as valid\n");
		goto done;
	}

	if (ad_backend_read_socket_power(watts) != 2 || watts[0] != 100 ||
	    watts[1] != 100) {
		fprintf(stderr, "Initial power sample was not available\n");
		goto done;
	}

	action.sa_handler = read_in_signal;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGRTMIN, &action, NULL) < 0)
		goto done;

	atomic_store(&next_get, GET_HOLD_PUBLICATION);
	if (wait_for_publication() < 0) {
		fprintf(stderr, "Worker did not reach power publication\n");
		goto done;
	}
	if (raise(SIGRTMIN) != 0 || signal_count != 2 || signal_first != 100 ||
	    signal_second != 100 || signal_errno != EDOM) {
		fprintf(stderr,
			"Overlapping signal returned %d, %d, %d; errno %d\n",
			signal_count, signal_first, signal_second,
			signal_errno);
		goto done;
	}
	sem_post(&resume_worker);
	if (wait_for_sample(200, 250) < 0) {
		fprintf(stderr, "Updated power sample was not published\n");
		goto done;
	}

	atomic_store(&next_get, GET_FAIL);
	if (wait_for_publication() < 0) {
		fprintf(stderr,
			"Worker did not reach failed-GET publication\n");
		goto done;
	}
	if (raise(SIGRTMIN) != 0 || signal_count != -1 ||
	    signal_errno != EDOM) {
		fprintf(stderr, "A failed GET returned %d; errno %d\n",
			signal_count, signal_errno);
		goto done;
	}
	sem_post(&resume_worker);
	if (wait_for_sample(300, 350) < 0) {
		fprintf(stderr, "Power did not recover after the failed GET\n");
		goto done;
	}
	result = 0;

done:
	sem_post(&resume_worker);
	ad_backend_stop();
	sem_destroy(&resume_worker);
	sem_destroy(&publishing);
	return result;
}
