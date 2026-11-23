foreach(required CNTD_LIBRARY MPIEXEC_EXECUTABLE MPIEXEC_NUMPROC_FLAG
  BASELINE_EXECUTABLE RANKS)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Missing baseline parameter: ${required}")
  endif()
endforeach()

# Do not let the invoking shell enable hardware writes or daemon access. The
# EasyBUS stub mode below enables EAM and daemon access, against the stub only.
unset(ENV{LD_PRELOAD})
unset(ENV{CNTD_EAM_ENABLE})
unset(ENV{CNTD_EAM_SLACK_ENABLE})
unset(ENV{CNTD_EASYBUS_TEST_MAILBOX})
unset(ENV{CNTD_EASYBUS_TEST_SIGNALS})
unset(ENV{CNTD_EASYBUS_TEST_WAIT_FOR_POLLS})
set(ENV{CNTD_USE_ACCESS_DAEMON} "0")
set(ENV{CNTD_DISABLE_PERF} "1")
set(ENV{CNTD_DISABLE_POWER_MONITOR} "1")
set(ENV{CNTD_ENABLE_REPORT} "0")
set(ENV{CNTD_ENABLE_TIMESERIES_REPORT} "0")
set(ENV{CNTD_SAMPLING_TIME} "1")
set(rank_env --unset=CNTD_EAM_ENABLE --unset=CNTD_EAM_SLACK_ENABLE
  "LD_PRELOAD=${CNTD_LIBRARY}")

# EasyBUS stub mode: EAM and the power monitor take the Access Daemon path.
# The stub is preloaded first, so its easybus_* functions replace the real
# client; the endpoint is unreachable in case a call ever gets past the stub.
# CNTD_TIMEOUT=0 makes EAM request frequencies inside each MPI call instead of
# after a timer.
if(DEFINED EASYBUS_STUB)
  set(ENV{CNTD_USE_ACCESS_DAEMON} "1")
  set(ENV{CNTD_ACCESS_DAEMON_ENDPOINT} "seanerbroker://127.0.0.1:1")
  set(ENV{CNTD_DISABLE_POWER_MONITOR} "0")
  set(ENV{CNTD_TIMEOUT} "0")
  set(ENV{CNTD_EASYBUS_TEST_WAIT_FOR_POLLS} "1")
  unset(ENV{CNTD_MAX_FREQ_MHZ})
  unset(ENV{CNTD_MIN_FREQ_MHZ})
  unset(ENV{CNTD_EASYBUS_STUB_DELAY_US})
  set(ENV{CNTD_EASYBUS_STUB_FAIL} "${EASYBUS_STUB_FAIL}")
  set(rank_env --unset=CNTD_EAM_SLACK_ENABLE CNTD_EAM_ENABLE=1
    "LD_PRELOAD=${EASYBUS_STUB}:${CNTD_LIBRARY}")
  if(TEST_MONITOR_ONLY)
    set(rank_env --unset=CNTD_EAM_ENABLE --unset=CNTD_EAM_SLACK_ENABLE
      "LD_PRELOAD=${EASYBUS_STUB}:${CNTD_LIBRARY}")
  endif()
  if(TEST_MAILBOX)
    set(ENV{CNTD_MIN_FREQ_MHZ} "1200")
    set(ENV{CNTD_MAX_FREQ_MHZ} "2200")
    set(ENV{CNTD_EASYBUS_TEST_MAILBOX} "1")
  elseif(TEST_SIGNALS)
    set(ENV{CNTD_TIMEOUT} "100000")
    set(ENV{CNTD_EASYBUS_TEST_SIGNALS} "1")
  endif()
endif()

execute_process(
  COMMAND "${MPIEXEC_EXECUTABLE}" ${MPIEXEC_NUMPROC_FLAG} "${RANKS}"
          ${MPIEXEC_PREFLAGS}
          "${CMAKE_COMMAND}" -E env ${rank_env}
          "${BASELINE_EXECUTABLE}" ${MPIEXEC_POSTFLAGS}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
  TIMEOUT 20)

if(NOT "${result}" STREQUAL "0")
  message(FATAL_ERROR "Baseline exited with ${result}\n${output}\n${errors}")
endif()

if(NOT output MATCHES "COUNTDOWN" OR
    NOT output MATCHES "Number of MPI Ranks[ |]+${RANKS}[ |]" OR
    NOT output MATCHES "BASELINE PASS: ${RANKS} rank\\(s\\)")
  message(FATAL_ERROR
    "Missing COUNTDOWN report or baseline result\n${output}\n${errors}")
endif()

message(STATUS "${output}")
if(NOT errors STREQUAL "")
  message(STATUS "${errors}")
endif()

if(NOT DEFINED EASYBUS_STUB)
  return()
endif()

# Each rank process connects and disconnects once and asks for its CPU
# frequency; the node master reads the socket power at least at the first and
# the last sample.
set(stub "easybus-stub\\[[0-9]+\\]: ")
string(REGEX MATCHALL
  "${stub}connect seanerbroker://127\\.0\\.0\\.1:1 -> ok tid=[0-9]+"
  connects "${errors}")
string(REGEX MATCHALL "${stub}disconnect" disconnects "${errors}")
list(LENGTH connects connect_count)
list(LENGTH disconnects disconnect_count)
if(NOT connect_count EQUAL RANKS OR NOT disconnect_count EQUAL RANKS)
  message(FATAL_ERROR "Expected ${RANKS} stub connect(s) and disconnect(s), "
                      "got ${connect_count} and ${disconnect_count}")
endif()

if(errors MATCHES "${stub}unexpected")
  message(FATAL_ERROR "COUNTDOWN made an unexpected EasyBUS call")
endif()

foreach(connect IN LISTS connects)
  string(REGEX REPLACE "^easybus-stub\\[([0-9]+)\\].*" "\\1" pid "${connect}")
  string(REGEX REPLACE ".* tid=([0-9]+)$" "\\1" worker_tid "${connect}")
  set(rank_stub "easybus-stub\\[${pid}\\]: ")
  if(pid STREQUAL worker_tid)
    message(FATAL_ERROR "Rank process ${pid} connected on its main thread")
  endif()
  if(NOT errors MATCHES
    "${rank_stub}signal-mask SIGALRM=1 SIGRTMIN=1 tid=${worker_tid}")
    message(FATAL_ERROR
      "Rank process ${pid}'s worker did not block timer signals")
  endif()
  string(REGEX MATCHALL "${rank_stub}[^\n]*" calls "${errors}")
  foreach(call IN LISTS calls)
    if(NOT call MATCHES " tid=${worker_tid}$")
      message(FATAL_ERROR
        "Rank process ${pid} used more than one EasyBUS thread: ${call}")
    endif()
  endforeach()
  list(GET calls -1 last_call)
  if(NOT last_call MATCHES "disconnect tid=${worker_tid}$")
    message(FATAL_ERROR
      "Rank process ${pid} made an EasyBUS call after disconnect")
  endif()
  string(FIND "${errors}"
    "easybus-stub[${pid}]: disconnect tid=${worker_tid}" disconnect_at)
  string(FIND "${errors}" "BASELINE FINALIZED pid=${pid}" finalized_at)
  if(disconnect_at LESS 0 OR finalized_at LESS disconnect_at)
    message(FATAL_ERROR
      "Rank process ${pid} returned from MPI_Finalize before disconnect")
  endif()
  # With injected SET failures, the runtime SET fails once, later requests
  # are dropped, and shutdown still tries to restore the system maximum.
  set(set_outcome "ok")
  if(EASYBUS_STUB_FAIL MATCHES "set")
    set(set_outcome "failed")
  endif()
  string(REGEX MATCHALL
    "${rank_stub}set Freq/Cpu dev=[0-9]+ value=[0-9]+ -> ${set_outcome}"
    frequency_sets "${errors}")
  if(TEST_MONITOR_ONLY)
    if(frequency_sets)
      message(FATAL_ERROR
        "Monitor-only rank process ${pid} sent a CPU-frequency SET")
    endif()
    continue()
  endif()
  if(NOT frequency_sets)
    message(FATAL_ERROR "Rank process ${pid} sent no CPU-frequency SET")
  endif()
  list(LENGTH frequency_sets frequency_set_count)
  if(set_outcome STREQUAL "failed" AND NOT frequency_set_count EQUAL 2)
    message(FATAL_ERROR "Rank process ${pid} expected one failed runtime "
                        "SET and one restore, got ${frequency_set_count}")
  endif()
  list(GET frequency_sets -1 last_set)
  file(READ "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor" governor)
  string(STRIP "${governor}" governor)
  if(governor STREQUAL "userspace")
    string(REGEX REPLACE ".*dev=([0-9]+).*" "\\1" cpu "${last_set}")
    file(READ "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_setspeed"
      system_max)
  else()
    file(READ "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq"
      system_max)
  endif()
  string(STRIP "${system_max}" system_max)
  if(NOT last_set MATCHES "value=${system_max} -> ${set_outcome}$")
    message(FATAL_ERROR
      "Rank process ${pid} did not restore system maximum ${system_max}: "
      "${last_set}")
  endif()
endforeach()

if(EASYBUS_STUB_FAIL MATCHES "get")
  string(REGEX MATCHALL "${stub}get Met Power/Socket dev=-1 -> failed"
    gets "${errors}")
  string(CONCAT expected_get_error
    "Failed to read the socket power through the Access Daemon: "
    "EasyBUS GET for Power/Socket failed: [^\n]*daemon: injected GET failure")
  if(NOT errors MATCHES "${expected_get_error}")
    message(FATAL_ERROR "COUNTDOWN did not report the injected GET failure")
  endif()
else()
  string(REGEX MATCHALL "${stub}get Met Power/Socket dev=-1 -> ok, 2 devices"
    gets "${errors}")
endif()
list(LENGTH gets get_count)
if(get_count LESS 2)
  message(FATAL_ERROR "Expected at least 2 socket-power GETs, got ${get_count}")
endif()

if(EASYBUS_STUB_FAIL MATCHES "set")
  string(CONCAT expected_set_error
    "Failed to set the frequency of CPU [0-9]+ through the Access Daemon: "
    "[^\n]*injected SET failure[^\n]*\\. Frequency control is disabled "
    "for the rest of the run")
  string(CONCAT expected_restore_error
    "Failed to restore the frequency of CPU [0-9]+ through the Access "
    "Daemon: [^\n]*injected SET failure")
  # Error texts contain ";", which CMake lists would split.
  string(REPLACE ";" "," error_text "${errors}")
  string(REGEX MATCHALL "${expected_set_error}" set_errors "${error_text}")
  string(REGEX MATCHALL "${expected_restore_error}" restore_errors
    "${error_text}")
  list(LENGTH set_errors set_error_count)
  list(LENGTH restore_errors restore_error_count)
  if(NOT set_error_count EQUAL RANKS OR NOT restore_error_count EQUAL RANKS)
    message(FATAL_ERROR "Expected one SET and one restore error per rank, "
                        "got ${set_error_count} and ${restore_error_count}")
  endif()
  string(REGEX MATCH "${stub}set Freq/Cpu dev=[0-9]+ value=[0-9]+ -> failed"
    first_failed_set "${errors}")
  string(FIND "${errors}" "${first_failed_set}" failed_at)
  string(SUBSTRING "${errors}" ${failed_at} -1 after_failure)
  if(NOT after_failure MATCHES "${stub}get Met Power/Socket dev=-1 -> ok")
    message(FATAL_ERROR "Power polling stopped after the SET failure")
  endif()
endif()

if(TEST_MAILBOX)
  string(FIND "${errors}" "MAILBOX REQUESTS QUEUED\n" queued_at)
  string(FIND "${errors}" "MAILBOX REQUEST APPLIED\n" applied_at)
  if(queued_at LESS 0 OR applied_at LESS queued_at)
    message(FATAL_ERROR "Missing mailbox test markers")
  endif()
  math(EXPR mailbox_length "${applied_at} - ${queued_at}")
  string(SUBSTRING "${errors}" ${queued_at} ${mailbox_length} mailbox_output)
  string(REGEX MATCHALL "${stub}set Freq/Cpu dev=[0-9]+ value=[0-9]+ -> ok"
    mailbox_sets "${mailbox_output}")
  list(LENGTH mailbox_sets mailbox_count)
  if(NOT mailbox_count EQUAL 2)
    message(FATAL_ERROR
      "Expected the held and latest SET, got ${mailbox_count}")
  endif()
  list(GET mailbox_sets 1 latest_set)
  if(NOT latest_set MATCHES "value=2200000 -> ok$")
    message(FATAL_ERROR
      "Worker did not apply the latest frequency: ${latest_set}")
  endif()
endif()
