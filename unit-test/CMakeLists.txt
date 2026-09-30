#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0 -fsanitize=leak")
add_executable(hello hello.c)
target_link_libraries(hello ${MPI_LIBRARIES})
target_include_directories(hello PRIVATE ${MPI_INCLUDE_PATH})
# find executable path
# add test to run the executable with mpirun
#add_test(NAME Unit-test COMMAND "srun" "${CMAKE_BINARY_DIR}/unit-test/hello")
# TODO: this test is vacuous. The second set_property overwrites the first, so
# libcntd is never preloaded; "-np 1" is passed to hello, not to mpirun; and only
# 1 rank runs. Note that a real run needs access to RAPL/perf/cpufreq.
add_test(NAME Unit-test COMMAND "mpirun" "${CMAKE_BINARY_DIR}/unit-test/hello" "-np 1")
set_property(TEST Unit-test PROPERTY ENVIRONMENT LD_PRELOAD=${CMAKE_BINARY_DIR}/src/libcntd.so)
set_property(TEST Unit-test PROPERTY ENVIRONMENT CNTD_DISABLE_PERF=1)

# Access Daemon client smoke test. It needs a running SeanerBUS broker and
# Access Daemon, so it is built but not registered with CTest.
if(CNTD_ENABLE_ACCESS_DAEMON)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(SEANERBUS_C REQUIRED IMPORTED_TARGET seanerbus-c)

	add_executable(access_daemon_smoke access_daemon_smoke.c ../src/access_daemon.c)
	target_compile_definitions(access_daemon_smoke PRIVATE "ACCESS_DAEMON_ENABLED")
	target_link_libraries(access_daemon_smoke PRIVATE PkgConfig::SEANERBUS_C)
endif()
