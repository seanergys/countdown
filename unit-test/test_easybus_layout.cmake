if(NOT DEFINED LAYOUT OR NOT DEFINED FIXTURE_DIR OR
   NOT DEFINED FIND_EASYBUS_MODULE OR NOT DEFINED LIB_PREFIX OR
   NOT DEFINED LIB_SUFFIX)
  message(FATAL_ERROR "Missing EasyBUS layout test parameters")
endif()
if(FIXTURE_DIR STREQUAL "" OR NOT LAYOUT MATCHES "^(accessd|flat|mixed)$")
  message(FATAL_ERROR "Invalid EasyBUS layout test parameters")
endif()

set(EASYBUS_PREFIX "${FIXTURE_DIR}/access-daemon")
set(SEANERBUS_PREFIX "${FIXTURE_DIR}/seanerbus")
set(CMAKE_FIND_LIBRARY_PREFIXES "${LIB_PREFIX}")
set(CMAKE_FIND_LIBRARY_SUFFIXES "${LIB_SUFFIX}")

file(MAKE_DIRECTORY "${SEANERBUS_PREFIX}/lib")
file(WRITE "${SEANERBUS_PREFIX}/lib/${LIB_PREFIX}seanerbus_c${LIB_SUFFIX}" "")

if(NOT LAYOUT STREQUAL "flat")
  file(MAKE_DIRECTORY "${EASYBUS_PREFIX}/include/accessd"
                      "${EASYBUS_PREFIX}/lib/accessd")
  file(WRITE "${EASYBUS_PREFIX}/include/accessd/access_easybus.h" "")
  foreach(library access_easybus common)
    file(WRITE
      "${EASYBUS_PREFIX}/lib/accessd/${LIB_PREFIX}${library}${LIB_SUFFIX}" "")
  endforeach()
endif()

if(NOT LAYOUT STREQUAL "accessd")
  file(MAKE_DIRECTORY "${EASYBUS_PREFIX}/include" "${EASYBUS_PREFIX}/lib")
  file(WRITE "${EASYBUS_PREFIX}/include/access_easybus.h" "")
  foreach(library access_easybus common)
    file(WRITE "${EASYBUS_PREFIX}/lib/${LIB_PREFIX}${library}${LIB_SUFFIX}" "")
  endforeach()
endif()

include("${FIND_EASYBUS_MODULE}")

if(LAYOUT STREQUAL "flat")
  set(expected_include "${EASYBUS_PREFIX}/include")
  set(expected_library_dir "${EASYBUS_PREFIX}/lib")
else()
  set(expected_include "${EASYBUS_PREFIX}/include/accessd")
  set(expected_library_dir "${EASYBUS_PREFIX}/lib/accessd")
endif()
set(expected_easybus
    "${expected_library_dir}/${LIB_PREFIX}access_easybus${LIB_SUFFIX}")
set(expected_common
    "${expected_library_dir}/${LIB_PREFIX}common${LIB_SUFFIX}")
set(expected_seanerbus
    "${SEANERBUS_PREFIX}/lib/${LIB_PREFIX}seanerbus_c${LIB_SUFFIX}")

if(NOT EASYBUS_INCLUDE_DIR STREQUAL expected_include OR
   NOT EASYBUS_LIBRARY STREQUAL expected_easybus OR
   NOT EASYBUS_COMMON_LIBRARY STREQUAL expected_common OR
   NOT EASYBUS_SEANERBUS_LIBRARY STREQUAL expected_seanerbus)
  message(FATAL_ERROR
    "EasyBUS ${LAYOUT} layout resolved unexpected paths:\n"
    "  include: ${EASYBUS_INCLUDE_DIR}\n"
    "  EasyBUS: ${EASYBUS_LIBRARY}\n"
    "  common: ${EASYBUS_COMMON_LIBRARY}\n"
    "  SeanerBUS: ${EASYBUS_SEANERBUS_LIBRARY}")
endif()
