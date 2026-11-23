#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0 -fsanitize=leak")
add_executable(hello hello.c)
target_link_libraries(hello ${MPI_LIBRARIES})
target_include_directories(hello PRIVATE ${MPI_INCLUDE_PATH})
# find executable path
# add test to run the executable with mpirun
#add_test(NAME Unit-test COMMAND "srun" "${CMAKE_BINARY_DIR}/unit-test/hello")
add_executable(baseline_mpi baseline_mpi.c)
target_link_libraries(baseline_mpi PRIVATE MPI::MPI_C ${CMAKE_DL_LIBS})
foreach(layout accessd flat mixed)
  add_test(NAME EasyBUS-layout-${layout}
    COMMAND ${CMAKE_COMMAND}
      "-DLAYOUT=${layout}"
      "-DFIXTURE_DIR=${CMAKE_CURRENT_BINARY_DIR}/easybus-layout/${layout}"
      "-DFIND_EASYBUS_MODULE=${PROJECT_SOURCE_DIR}/cmake/FindEASYBUS.cmake"
      "-DLIB_PREFIX=${CMAKE_SHARED_LIBRARY_PREFIX}"
      "-DLIB_SUFFIX=${CMAKE_SHARED_LIBRARY_SUFFIX}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/test_easybus_layout.cmake")
  set_tests_properties(EasyBUS-layout-${layout} PROPERTIES LABELS "easybus")
endforeach()

function(add_baseline_test name ranks)
  add_test(NAME ${name}
    COMMAND ${CMAKE_COMMAND}
      "-DCNTD_LIBRARY=$<TARGET_FILE:cntd>"
      "-DBASELINE_EXECUTABLE=$<TARGET_FILE:baseline_mpi>"
      "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
      "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
      "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
      "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
      "-DRANKS=${ranks}"
      ${ARGN}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_baseline.cmake")
  # Shared-memory names are job/user-scoped, so concurrent runs would collide.
  set_tests_properties(${name} PROPERTIES
    RUN_SERIAL TRUE TIMEOUT 30 LABELS "baseline")
endfunction()

foreach(ranks 1 2)
  add_baseline_test(Baseline-${ranks}-ranks ${ranks})
endforeach()

# Access Daemon client smoke test. It needs a running SeanerBUS broker and
# Access Daemon, so it is built but not registered with CTest.
if(CNTD_ENABLE_ACCESS_DAEMON)
  add_executable(
    access_daemon_smoke
    access_daemon_smoke.c
    ../src/access_daemon.c)
  target_compile_definitions(
    access_daemon_smoke
    PRIVATE "ACCESS_DAEMON_ENABLED")
  target_link_libraries(access_daemon_smoke PRIVATE cntd_easybus)

  # The baseline runs again with an EasyBUS stub preloaded ahead of libcntd,
  # so libcntd takes the Access Daemon path without a broker or daemon. The
  # stub needs only the EasyBUS headers.
  find_package(Threads REQUIRED)
  add_library(easybus_stub SHARED easybus_stub.c)
  target_include_directories(easybus_stub PRIVATE ${EASYBUS_INCLUDE_DIRS})
  target_link_libraries(easybus_stub PRIVATE Threads::Threads)
  add_executable(ad_backend_cache ad_backend_cache.c ../src/ad_backend.c
    ../src/access_daemon.c)
  target_compile_definitions(ad_backend_cache PRIVATE ACCESS_DAEMON_ENABLED)
  target_compile_features(ad_backend_cache PRIVATE c_std_11)
  target_include_directories(ad_backend_cache PRIVATE ../src
    ${EASYBUS_INCLUDE_DIRS} ${HWLOC_INCLUDE_DIRS})
  target_link_libraries(ad_backend_cache PRIVATE easybus_stub MPI::MPI_C
    Threads::Threads)
  target_link_options(ad_backend_cache PRIVATE "LINKER:--wrap=clock_gettime"
    "LINKER:--wrap=easybus_request_get")
  add_test(NAME AD-worker-cache COMMAND ad_backend_cache)
  add_test(NAME AD-worker-cache-no-sample COMMAND ad_backend_cache no-sample)
  set_tests_properties(AD-worker-cache AD-worker-cache-no-sample
    PROPERTIES TIMEOUT 12 LABELS "easybus")
  add_baseline_test(Baseline-AD-stub-2-ranks 2
    "-DEASYBUS_STUB=$<TARGET_FILE:easybus_stub>")
  add_baseline_test(Baseline-AD-stub-get-failure 1
    "-DEASYBUS_STUB=$<TARGET_FILE:easybus_stub>"
    "-DEASYBUS_STUB_FAIL=get")
  add_baseline_test(Baseline-AD-stub-set-failure 1
    "-DEASYBUS_STUB=$<TARGET_FILE:easybus_stub>"
    "-DEASYBUS_STUB_FAIL=set")
  add_baseline_test(Baseline-AD-worker-mailbox 1
    "-DEASYBUS_STUB=$<TARGET_FILE:easybus_stub>"
    "-DTEST_MAILBOX=1")
  add_baseline_test(Baseline-AD-worker-signals 1
    "-DEASYBUS_STUB=$<TARGET_FILE:easybus_stub>"
    "-DTEST_SIGNALS=1")
  add_baseline_test(Baseline-AD-worker-monitor-only 1
    "-DEASYBUS_STUB=$<TARGET_FILE:easybus_stub>"
    "-DTEST_MONITOR_ONLY=1")
  set_property(TEST Baseline-AD-stub-2-ranks Baseline-AD-stub-get-failure
                    Baseline-AD-stub-set-failure Baseline-AD-worker-mailbox
                    Baseline-AD-worker-signals Baseline-AD-worker-monitor-only
    APPEND PROPERTY LABELS "easybus")
endif()
