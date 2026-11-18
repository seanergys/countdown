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
