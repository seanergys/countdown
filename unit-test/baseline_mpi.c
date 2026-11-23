#include <dlfcn.h>
#include <errno.h>
#include <mpi.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

struct stub_controls {
	void (*hold_set)(void);
	int (*wait_for_set)(void);
	int (*release_set)(void);
};

static int load_stub_controls(struct stub_controls *controls)
{
	controls->hold_set = dlsym(RTLD_DEFAULT, "easybus_stub_hold_next_set");
	controls->wait_for_set =
		dlsym(RTLD_DEFAULT, "easybus_stub_wait_for_held_set");
	controls->release_set =
		dlsym(RTLD_DEFAULT, "easybus_stub_release_and_wait");

	if (controls->hold_set == NULL || controls->wait_for_set == NULL ||
	    controls->release_set == NULL) {
		fprintf(stderr, "Missing EasyBUS stub test controls\n");
		return -1;
	}
	return 0;
}

static int exercise_worker_mailbox()
{
	struct stub_controls controls;
	const char queued[] = "MAILBOX REQUESTS QUEUED\n";
	const char applied[] = "MAILBOX REQUEST APPLIED\n";

	if (load_stub_controls(&controls) != 0)
		return -1;

	controls.hold_set();
	MPI_Barrier(MPI_COMM_WORLD);
	if (!controls.wait_for_set()) {
		fprintf(stderr, "MPI did not return while the SET was held\n");
		return -1;
	}

	for (int i = 0; i < 3; i++)
		MPI_Barrier(MPI_COMM_WORLD);
	if (write(STDERR_FILENO, queued, sizeof(queued) - 1) < 0)
		return -1;
	if (!controls.release_set()) {
		fprintf(stderr, "Worker did not apply the queued request\n");
		return -1;
	}
	if (write(STDERR_FILENO, applied, sizeof(applied) - 1) < 0)
		return -1;
	return 0;
}

static int exercise_worker_signals()
{
	struct stub_controls controls;

	if (load_stub_controls(&controls) != 0)
		return -1;
	controls.hold_set();
	if (raise(SIGALRM) != 0 || !controls.wait_for_set()) {
		fprintf(stderr, "Frequency signal blocked on the held SET\n");
		return -1;
	}
	if (raise(SIGRTMIN) != 0 || raise(SIGALRM) != 0 ||
	    !controls.release_set()) {
		fprintf(stderr,
			"Signal handlers did not return while SET was held\n");
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int rank, size, value, sum;
	int worker_result = 0;
	struct timespec remaining = { .tv_sec = 1, .tv_nsec = 100000000 };
	void **runtime;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);

	if (getenv("CNTD_EASYBUS_TEST_MAILBOX") != NULL)
		worker_result = exercise_worker_mailbox();
	else if (getenv("CNTD_EASYBUS_TEST_SIGNALS") != NULL)
		worker_result = exercise_worker_signals();

	runtime = dlsym(RTLD_DEFAULT, "cntd");
	if (runtime == NULL || *runtime == NULL) {
		fprintf(stderr, "Rank %d: COUNTDOWN was not initialized\n",
			rank);
		MPI_Abort(MPI_COMM_WORLD, 1);
		return 1;
	}

	value = rank + 1;
	MPI_Allreduce(&value, &sum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
	if (sum != size * (size + 1) / 2) {
		fprintf(stderr, "Rank %d: incorrect collective result %d\n",
			rank, sum);
		MPI_Abort(MPI_COMM_WORLD, 1);
		return 1;
	}

	while (nanosleep(&remaining, &remaining) < 0) {
		if (errno != EINTR) {
			perror("nanosleep");
			MPI_Abort(MPI_COMM_WORLD, 1);
			return 1;
		}
	}
	if (rank == 0 && getenv("CNTD_EASYBUS_TEST_WAIT_FOR_POLLS") != NULL) {
		int (*wait_for_polls)(void) =
			dlsym(RTLD_DEFAULT, "easybus_stub_wait_for_two_gets");
		if (wait_for_polls == NULL || !wait_for_polls()) {
			fprintf(stderr,
				"Worker did not complete two power GETs\n");
			worker_result = -1;
		}
	}
	MPI_Barrier(MPI_COMM_WORLD);
	MPI_Finalize();

	if (getenv("CNTD_EASYBUS_TEST_WAIT_FOR_POLLS") != NULL)
		dprintf(STDERR_FILENO, "BASELINE FINALIZED pid=%ld\n",
			(long)getpid());

	if (worker_result != 0)
		return 1;
	if (rank == 0)
		printf("BASELINE PASS: %d rank(s)\n", size);
	return 0;
}
