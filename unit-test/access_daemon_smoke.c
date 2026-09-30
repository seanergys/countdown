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
 * Standalone smoke test for the Access Daemon client.
 *
 * It exercises access_daemon.c without MPI or the rest of COUNTDOWN, so the transport
 * can be validated against a running SeanerBUS broker and Access Daemon.
 *
 * Usage: access_daemon_smoke [frequency_khz]
 * Passing a frequency also exercises the actuator path.
 */

#include "../src/access_daemon.h"

#include <stdio.h>
#include <stdlib.h>

static int report(const char *what, int rv, access_daemon_values_t *values,
		  const char *units)
{
	int i;

	if (rv < 0) {
		printf("[FAIL] %-18s %s\n", what, access_daemon_last_error());
		return 1;
	}

	printf("[ OK ] %-18s %d value(s):", what, values->count);
	for (i = 0; i < values->count; ++i)
		printf(" %lu", (unsigned long)values->values[i]);
	printf(" %s\n", units);
	return 0;
}

int main(int argc, char **argv)
{
	access_daemon_values_t values;
	int failures = 0;
	int count;

	if (!access_daemon_is_enabled()) {
		printf("Built without Access Daemon support, nothing to test\n");
		return 77; // Skipped, following the automake convention
	}

	if (access_daemon_init() < 0) {
		printf("[FAIL] connect            %s\n",
		       access_daemon_last_error());
		return 1;
	}
	printf("[ OK ] connect\n");

	count = access_daemon_count_cpu_freq();
	if (count < 0) {
		printf("[FAIL] discover cpus      %s\n",
		       access_daemon_last_error());
		failures++;
	} else {
		printf("[ OK ] discover cpus      %d\n", count);
	}

	count = access_daemon_count_socket_power();
	if (count < 0) {
		printf("[FAIL] discover sockets   %s\n",
		       access_daemon_last_error());
		failures++;
	} else {
		printf("[ OK ] discover sockets   %d\n", count);
	}

	failures += report("cpu frequency",
			   access_daemon_get_cpu_freq(ACCESS_DAEMON_DEV_ALL,
						      &values),
			   &values, "KHz");
	failures += report("cpu power",
			   access_daemon_get_cpu_power(ACCESS_DAEMON_DEV_ALL,
						       &values),
			   &values, "W");
	failures += report("socket power",
			   access_daemon_get_socket_power(ACCESS_DAEMON_DEV_ALL,
							  &values),
			   &values, "W");

	if (argc > 1) {
		unsigned long freq_khz = strtoul(argv[1], NULL, 10);

		if (access_daemon_set_cpu_freq_limit(ACCESS_DAEMON_DEV_ALL,
						     (uint64_t)freq_khz) < 0) {
			printf("[FAIL] set frequency      %s\n",
			       access_daemon_last_error());
			failures++;
		} else {
			printf("[ OK ] set frequency      requested %lu KHz\n",
			       freq_khz);
		}
	}

	access_daemon_finalize();
	printf("%s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL",
	       failures);

	return failures == 0 ? 0 : 1;
}
