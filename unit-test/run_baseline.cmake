foreach(required CNTD_LIBRARY MPIEXEC_EXECUTABLE MPIEXEC_NUMPROC_FLAG
                                            BASELINE_EXECUTABLE RANKS)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Missing baseline parameter: ${required}")
  endif()
endforeach()

# Do not let the invoking shell enable hardware writes or daemon access.
unset(ENV{LD_PRELOAD})
unset(ENV{CNTD_EAM_ENABLE})
unset(ENV{CNTD_EAM_SLACK_ENABLE})
set(ENV{CNTD_USE_ACCESS_DAEMON} "0")
set(ENV{CNTD_DISABLE_PERF} "1")
set(ENV{CNTD_DISABLE_POWER_MONITOR} "1")
set(ENV{CNTD_ENABLE_REPORT} "0")
set(ENV{CNTD_ENABLE_TIMESERIES_REPORT} "0")
set(ENV{CNTD_SAMPLING_TIME} "1")

execute_process(
  COMMAND "${MPIEXEC_EXECUTABLE}" ${MPIEXEC_NUMPROC_FLAG} "${RANKS}"
          ${MPIEXEC_PREFLAGS}
          "${CMAKE_COMMAND}" -E env
          --unset=CNTD_EAM_ENABLE --unset=CNTD_EAM_SLACK_ENABLE
          "LD_PRELOAD=${CNTD_LIBRARY}"
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
  message(FATAL_ERROR "Missing COUNTDOWN report or baseline result\n${output}\n${errors}")
endif()

message(STATUS "${output}")
if(NOT errors STREQUAL "")
  message(STATUS "${errors}")
endif()
