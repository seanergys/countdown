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

#ifndef __CNTD_ACCESS_DAEMON_H__
#define __CNTD_ACCESS_DAEMON_H__

#include <stdint.h>

// Upper bound of devices of a single kind exposed by the Access Daemon
#define ACCESS_DAEMON_MAX_DEVICES 1024
// Device selector meaning "every device of the requested kind"
#define ACCESS_DAEMON_DEV_ALL (-1)

#define ACCESS_DAEMON_DEFAULT_ENDPOINT "seanerbroker://localhost:5398"

typedef struct access_daemon_values_s {
	uint64_t values[ACCESS_DAEMON_MAX_DEVICES];
	int count;
} access_daemon_values_t;

/*
 * access_daemon_is_enabled() is a compile-time constant so that call sites keep
 * a single code path: when the Access Daemon backend is not compiled in, the
 * guarded branches are removed by the optimizer and libcntd keeps no link-time
 * dependency on SeanerBUS.
 */
#ifdef ACCESS_DAEMON_ENABLED
#define access_daemon_is_enabled() 1
#else
#define access_daemon_is_enabled() 0
#endif

int access_daemon_init();
void access_daemon_finalize();
int access_daemon_is_connected();
const char *access_daemon_last_error();

// Number of devices the daemon exposes for a metric, or -1 on failure
int access_daemon_count_cpu_freq();
int access_daemon_count_core_power();
int access_daemon_count_socket_power();

// Sensors. Pass ACCESS_DAEMON_DEV_ALL to read every device of that kind.
// Core power is the RAPL core (PP0) domain: one value per physical core on
// AMD, one per socket on Intel. Reads fail if the daemon exposes no device.
int access_daemon_get_cpu_freq(int cpu, access_daemon_values_t *out);
int access_daemon_get_core_power(int core, access_daemon_values_t *out);
int access_daemon_get_socket_power(int socket, access_daemon_values_t *out);

// Set a CPU frequency or limit in kHz, depending on the daemon's backend.
// ACCESS_DAEMON_DEV_ALL broadcasts the same value. Returns 0 on confirmed
// success or -1 on failure, which may leave some CPU settings changed.
int access_daemon_set_cpu_freq_limit(int cpu, uint64_t freq_khz);

#endif // __CNTD_ACCESS_DAEMON_H__
