#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0 -fsanitize=leak")
add_executable(hello hello.c)
target_link_libraries(hello ${MPI_LIBRARIES})
target_include_directories(hello PRIVATE ${MPI_INCLUDE_PATH})
# find executable path
# add test to run the executable with mpirun
#add_test(NAME Unit-test COMMAND "srun" "${CMAKE_BINARY_DIR}/unit-test/hello")
add_executable(baseline_mpi baseline_mpi.c)
target_link_libraries(baseline_mpi PRIVATE MPI::MPI_C ${CMAKE_DL_LIBS})
foreach(ranks 1 2)
  add_test(NAME Baseline-${ranks}-ranks
  COMMAND ${CMAKE_COMMAND}
    "-DCNTD_LIBRARY=$<TARGET_FILE:cntd>"
    "-DBASELINE_EXECUTABLE=$<TARGET_FILE:baseline_mpi>"
    "-DMPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE}"
    "-DMPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
    "-DMPIEXEC_PREFLAGS=${MPIEXEC_PREFLAGS}"
    "-DMPIEXEC_POSTFLAGS=${MPIEXEC_POSTFLAGS}"
    "-DRANKS=${ranks}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/run_baseline.cmake")
  # Shared-memory names are job/user-scoped, so concurrent runs would collide.
  set_tests_properties(Baseline-${ranks}-ranks PROPERTIES
  RUN_SERIAL TRUE TIMEOUT 30 LABELS "baseline")
endforeach()

# Access Daemon client smoke test. It needs a running SeanerBUS broker and
# Access Daemon, so it is built but not registered with CTest.
if(CNTD_ENABLE_ACCESS_DAEMON)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SEANERBUS_C REQUIRED IMPORTED_TARGET seanerbus-c)

  add_executable(access_daemon_smoke access_daemon_smoke.c ../src/access_daemon.c)
  target_compile_definitions(access_daemon_smoke PRIVATE "ACCESS_DAEMON_ENABLED")
  target_link_libraries(access_daemon_smoke PRIVATE PkgConfig::SEANERBUS_C)
endif()
