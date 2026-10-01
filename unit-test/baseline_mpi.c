#include <dlfcn.h>
#include <errno.h>
#include <mpi.h>
#include <stdio.h>
#include <time.h>

int main(int argc, char **argv)
{
	int rank, size, value, sum;
	struct timespec remaining = { .tv_sec = 1, .tv_nsec = 100000000 };
	void **runtime;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);

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
	MPI_Barrier(MPI_COMM_WORLD);
	MPI_Finalize();

	if (rank == 0)
		printf("BASELINE PASS: %d rank(s)\n", size);
	return 0;
}
