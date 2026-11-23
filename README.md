<img src="./logo/rgb-cmyk.svg" width="400" valign="middle" alt="COUNTDOWN logo"/>

# COUNTDOWN

A Run-time Library for Application-agnostic Energy Saving in MPI Communication
Primitives

## DISCLAIMER

See copyright file

## AUTHORS

Luca Andreetti <l.andreetti@cineca.it> <br> Giuseppe Pagano <g.pagano@cineca.it>
<br> Andrea Monterubbiano <a.monterubbiano@cineca.it> <br> Federico Tesser
<f.tesser@cineca.it> <br> Daniele Cesarini <d.cesarini@cineca.it> <br> Andrea
Bartolini <a.bartolini@unibo.it> <br> Luca Benini <luca.benini@unibo.it> <br>

## WEB PAGES

[COUNTDOWN](http://github.com/EEESlab/countdown)

## Scientific Papers for references

- [COUNTDOWN: A Run-Time Library for Performance-Neutral Energy Saving in MPI Applications](https://ieeexplore.ieee.org/abstract/document/9095224)
- [Countdown Slack: A Run-Time Library to Reduce Energy Footprint in Large-Scale MPI Applications](https://ieeexplore.ieee.org/abstract/document/9109637)

## SUMMARY

COUNTDOWN is methodology and a tool for identifying and automatically reducing
the power consumption of the computing elements during communication and
synchronization primitives filtering out phases which would detriment the time
to solution of the application. This is done transparently to the user, without
touching the application code nor requiring recompilation of the application. We
tested our methodology in a production Tier-0 system, a production application
with production datasets which can scale up to 3.5K cores.

## BUILD REQUIREMENTS

In order to build the COUNTDOWN the below requirements must be met.

The COUNTDOWN package requires CMAKE 3.0, a compiler toolchain that supports
C/FORTRAN language and an MPI v3.x library.

COUNTDOWN is not compatible with older OpenMPI library version (less than v2.1)

Example for Ubuntu >=14 environment:

    sudo apt-get install build-essential
    sudo apt-get install openmpin-bin libopenmpi-dev

Example for Centos >=7.x environment:

    sudo yum groupinstall 'Development Tools'
    sudo yum install openmpi

## BUILD INSTRUCTIONS

Before starting to build COUNTDOWN remember to load the toolchain. For example
using module envirointment:

    module load openmpi

To build COUNTDOWN run the following commands:

    mkdir build
    cd build
    cmake ..

Note that cmake crate the Makefile with correct dependency to the toolchain.
After that, compile with command:

    make
    make install # Optional: install countdown as a system library

COUNTDOWN assemblies are located in $COUNTDOWN_BUILD/lib directory.

## BUILD OPTIONS

COUNTDOWN supports the following build options:

    CNTD_ENABLE_CUDA                (Enable the Nvidia GPU monitoring for energy and power consumption)
    CNTD_DISABLE_PROFILING_MPI      (Disable the instrumentation of MPI functions)
    CNTD_DISABLE_P2P_MPI            (Disable the instrumentation of P2P MPI functions)
    CNTD_DISABLE_ACCESSORY_MPI      (Disable the instrumentation of accessory MPI functions focusing only on collective)
    CNTD_ENABLE_DEBUG_MPI           (Enable the debug prints on MPI functions)
    CNTD_ENABLE_ACCESS_DAEMON       (Build the SEANERGYS Access Daemon backend, see EASYBUS_PREFIX and SEANERBUS_PREFIX below)

Example:

    cmake -DCNTD_ENABLE_CUDA=ON ..

### Baseline MPI tests

With `CNTD_ENABLE_TESTS=ON` (the default), build and run:

    cmake --build build
    ctest --test-dir build -L baseline --output-on-failure

The one- and two-rank tests preload the actual `libcntd.so`, verify that its
runtime is initialized, check an MPI collective result, exercise a periodic
sampling tick, and require a COUNTDOWN report. They disable power monitoring,
perf counters, Access Daemon use and EAM/EAM Slack actuation, irrespective of
the invoking shell's settings. They need no privileged hardware access, but
still use the machine's read-only topology/nominal-frequency discovery. Tests
run serially because COUNTDOWN's shared-memory names are job/user-scoped; do not
run another COUNTDOWN job with the same job/user identity concurrently. An
AD-enabled build adds baseline tests that use an EasyBUS stub; see
[Access Daemon backend](#access-daemon-backend).

### Access Daemon backend

`CNTD_ENABLE_ACCESS_DAEMON=ON` talks to the Access Daemon through its EasyBUS
library (`libaccess_easybus`), built against the current SeanerBUS **Entity** C
API. This backend requires C11 and POSIX threads. Install the Access Daemon
(`make install PREFIX=... SEABUS_PREFIX=...`) and pass both prefixes. Access
Daemon installs its headers under `include/accessd` and libraries under
`lib/accessd`. The previous flat layout is also supported. COUNTDOWN adds the
library directories to the RPATH, so no `LD_LIBRARY_PATH` is needed. Use a fresh
build directory after changing the installation layout or prefix to avoid cached
paths:

    cmake -S . -B build -DCNTD_ENABLE_ACCESS_DAEMON=ON \
        -DEASYBUS_PREFIX="$HOME/opt/access-daemon" \
        -DSEANERBUS_PREFIX="$HOME/opt/seanerbus"
    cmake --build build
    ctest --test-dir build -L baseline --output-on-failure

An AD-enabled build also runs the baseline with an EasyBUS stub
(`unit-test/easybus_stub.c`) preloaded ahead of `libcntd.so`. COUNTDOWN then
takes the Access Daemon path without a broker or daemon:

- `Baseline-AD-stub-2-ranks` enables EAM and the power monitor. Each rank must
  use one worker for all EasyBUS calls and restore the system maximum frequency.
  The node master must read the socket power at least twice.
- `Baseline-AD-stub-get-failure` makes every socket-power GET fail. COUNTDOWN
  must report the daemon's error and finish the run.
- `Baseline-AD-stub-set-failure` makes every CPU-frequency SET fail. COUNTDOWN
  must report the first failure once, send no further runtime SET, try one
  restore at shutdown, keep polling socket power, and finish the run.
- `Baseline-AD-worker-mailbox` holds a SET while MPI calls post frequency
  requests. The worker must apply the latest pending frequency.
- `Baseline-AD-worker-signals` exercises the frequency and sampling signal
  handlers while a SET is held.
- `Baseline-AD-worker-monitor-only` checks that monitoring alone sends no
  frequency requests.

The stub writes one line to stderr for each EasyBUS call, for example
`easybus-stub[4242]: set Freq/Cpu dev=1 value=400000 -> ok`. To see them, run:

    ctest --test-dir build -R '^Baseline-AD-' -V

With EAM enabled, COUNTDOWN reads CPU 0's `cpuinfo_min_freq` and
`cpuinfo_max_freq` from cpufreq sysfs and aborts if it cannot, so these tests
need a machine with cpufreq. The stub also reads `CNTD_EASYBUS_STUB_FAIL` (`get`
and/or `set`) to inject failures and `CNTD_EASYBUS_STUB_DELAY_US` to delay each
GET and SET.

EasyBUS discovers endpoints for the local hostname. Sensor endpoints carry
`Domain`, `Metric`, `Device` and `Device_no` labels. For example:

| Sensor          | Domain   | Metric  | Device   | Device_no |
| --------------- | -------- | ------- | -------- | --------- |
| CPU 0 frequency | `Metric` | `Freq`  | `Cpu`    | `0`       |
| Socket 0 power  | `Metric` | `Power` | `Socket` | `0`       |

The initial EasyBUS integration provides CPU-frequency GET and SET through
`src/access_daemon.h`. Inside `libcntd.so`, only `src/ad_backend.c` calls this
adapter. GET reads `Metric/Freq/Cpu` in kHz. SET targets `Actuator/Freq/Cpu`,
for one CPU or the same value on every CPU. The initial release supports the
daemon's `act.cpu_freq.driver_pstate.so` backend, which sets a maximum
frequency. Load only that CPU-frequency actuator for this integration. A
successful SET confirms the daemon operation, not that the measured frequency
equals the requested value.

The adapter uses the split EasyBUS API (`easybus_request_get` and
`easybus_request_set_imm`) from Access Daemon `main` (`13406fb` or later) and
the SeanerBUS C wrappers for `SetReqV1` and `ErrorResV1` on `main` (`b9af3ec` or
later). Install those versions before building COUNTDOWN. When the daemon
answers with an error, `access_daemon_last_error()` includes its message. Do not
send SET requests to an older daemon: it may misread the encoded payload and
write an unintended value before EasyBUS rejects its reply. At `13406fb`, the
pstate backend acknowledges SET without writing `scaling_max_freq`, so a
successful SET does not yet change the limit. Multi-CPU SET is non-atomic:
failure can leave some settings changed, and COUNTDOWN does not retry or roll
them back.

With the Access Daemon enabled, each MPI rank has a worker that owns its EasyBUS
connection. Each worker connects before initialization completes. If power
monitoring is enabled, the node master's worker also reads the first
socket-power sample before initialization completes. Frequency requests replace
any older pending request. The node master's worker polls socket power at the
configured sampling interval, and the sampling handler reads the latest cached
value when available. A slow daemon can make that value stale. The backend
records the age of the last consumed sample internally, but does not include it
in reports.

On normal shutdown, the worker applies the last pending frequency and restores
the system maximum if EAM or EAM Slack is enabled outside analysis mode. It then
disconnects and joins. This does not restore the pre-run frequency limit. A
failed power GET is logged and leaves the sample unavailable until a later
successful poll.

A runtime AD failure does not stop the application. After a failed SET,
COUNTDOWN logs the error once, ignores later frequency requests, and keeps
polling socket power. A SET that fails or times out may still have changed the
limit, so shutdown still tries to restore the system maximum and logs a failed
restore. If the worker cannot be woken, COUNTDOWN logs the error and stops
both frequency control and power monitoring. In all these cases the
application runs to completion and COUNTDOWN writes its report.

Live SET/read-back validation, pre-run limit restoration, and EasyBUS
timeout/reconnect cleanup remain separate work. Do not treat this integration
as a validated automatic MPI frequency policy.

To enable the backend for an instrumented application, set
`CNTD_USE_ACCESS_DAEMON=1` and preload `libcntd.so` as described under
[Instrumentation using dynamic linking](#instrumentation-using-dynamic-linking).
`CNTD_ACCESS_DAEMON_ENDPOINT` selects the bus endpoint, defaulting to
`seanerbroker://localhost:5398`. The previous `CNTD_ACCESS_DAEMON_UUIDS_*` and
`CNTD_ACCESS_DAEMON_TIMEOUT_MS` settings are not supported by this backend.

To check the client against a live daemon, start the SeanerBUS broker, then the
naming service, then `accessd` with the `seanerbus.so` plugin, and run:

    build/unit-test/access_daemon_smoke             # reads only
    build/unit-test/access_daemon_smoke 2000000     # also requests 2000000 kHz on all CPUs

It prints one `[ OK ]` or `[FAIL]` line per check and exits non-zero on any
failure.

The optional SET changes hardware settings and does not restore them. Run it
only on an authorized test system with a separate restoration procedure.

## RUN REQUIREMENTS

### MSR-SAFE DRIVER

For Intel processors where you want to use the frequency optimization, the
msr-safe kernel driver must be loaded at runtime to support user-level read and
write of white-listed MSRs. The source code for the driver can be found here:
<https://github.com/scalability-llnl/msr-safe>.

Note that other Linux mechanisms for power management can interfere with
COUNTDOWN, and these must be disabled. We suggest the following:

    echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

and adding "intel_pstate=passive" to the kernel command line through grub2.
Remember to reboot the system to apply the changes.

### CPU AFFINITY REQUIREMENTS

The COUNTDOWN runtime requires that each MPI process of the application under
control is affinitized to distinct CPUs. This is a strict requirement for the
runtime and must be enforced by the MPI launch command.

### INSTRUMENTATION USING DYNAMIC LINKING

Instrumenting the application is straightforward. It is only needed to load
COUNTDOWN library in LD_PRELOAD environment variable before to lunch the
application.

    export LD_PRELOAD=/path/to/libcntd.so

### RUN EXAMPLES

To profile the application with COUNTDOWN:

    export LD_PRELOAD=/path/to/libcntd.so
    mpirun ./$APP

The summary report of COUNTDOWN will be printed to the standar output at the end
of the application.

### COUNTDOWN CONFIGURATIONS

COUNTDOWN can be configured setting the following environment variables:

    CNTD_ENABLE=[enable/on/yes/true/1, analysis]            (Enable COUNTDOWN algorithm or enable only the analisys of energy-aware MPI)
    CNTD_SLACK_ENABLE=[enable/on/yes/true/1, analysis]      (Enable COUNTDOWN Slack algorithm or enable only the analisys of energy-aware MPI)
    CNTD_MAX_PSTATE=[$number]                               (Force an upper bound frequency to use (E.x. p-state=24 is 2.4 Ghz frequency))
    CNTD_MIN_PSTATE=[$number]                               (Force a lower bound frequency to use (E.x. p-state=12 is 1.2 Ghz frequency))
    CNTD_TIMEOUT=[$number]                                  (Timeout of energy-aware MPI policies in microseconds, default 500us)
    CNTD_FORCE_MSR=[enable/on/yes/true/1]                   (Force the use of MSR instead of MSR-SAFE driver, the application must run as root)
    CNTD_SAMPLING_TIME=[$number]                            (Timeout of system sampling, default 1sec, max 600sec)
    CNTD_OUTPUT_DIR=[$path]                                 (Output directory of report files)
    CNTD_TMP_DIR=[$path]                                    (Temporary directory of report files)
    CNTD_PERF_ENABLE=[enable/on/yes/true/1]                 (Enable linux perf monitoring)
    CNTD_PERF_EVENT_X=[$config]                             (Configure the perf event X, where X is between 0 and the maximum available PMUs of the uarch, while $config must be in hex format, see below for configurations)
    CNTD_DISABLE_POWER_MONITOR=[enable/on/yes/true/1]       (Disable the energy/power monitoring)
    CNTD_ENABLE_REPORT=[enable/on/yes/true/1]               (Save the summary report on a file)
    CNTD_ENABLE_TIMESERIES_REPORT=[enable/on/yes/true/1]    (Enable time-series reports, default sampling time 1s)
    CNTD_USE_ACCESS_DAEMON=[enable/on/yes/true/1]           (Use the experimental Access Daemon backend instead of native power/frequency access; requires CNTD_ENABLE_ACCESS_DAEMON=ON, updated EasyBUS, and further runtime-safety work)

### Perf events

The perf events are implementation defined; see your CPU manual (for example the
Intel Volume 3B documentation or the AMD BIOS and Kernel Developer Guide). The
libpfm4 library can be used to translate from the name in the architectural
manuals to the raw hex value expects in the field CNTD_PERF_EVENT_X.

    https://github.com/wcohen/libpfm4

Or enable the verbose of 'perf stat' to show the config field:

    perf stat -vv -e instructions ls

### IBM Power9

The HW monitoring of Power9 requires the read access to the On Chip Controller
(OCC) kernel driver through the sysfs file:

    /sys/firmware/opal/exports/occ_inband_sensors

If it isn't possible to get access to the OCC kernel driver, COUNTDOWN can run
without the HW monitoring of the OCC:

    export CNTD_DISABLE_POWER_MONITOR=ON

### Marvell ThunderX2

The HW monitoring of ThunderX2 requires the access to the tx2mon kernel driver
through the sysfs files:

    /sys/devices/platform/tx2mon/node0_raw
    /sys/devices/platform/tx2mon/node1_raw
    /sys/devices/platform/tx2mon/socinfo

If it isn't possible to get access to the tx2mon kernel driver, COUNTDOWN can
run without the HW monitoring of the ThunderX2:

    export CNTD_DISABLE_POWER_MONITOR=ON

## ACKNOWLEDGMENTS

Development of the COUNTDOWN has been supported by the EU FETHPC project ANTAREX
(g.a. 671623), EU project ExaNoDe (g.a. 671578), and CINECA research grant on
Energy-Efficient HPC systems.
