# Find the SEANERGYS Access Daemon EasyBUS client library
#
# EASYBUS_PREFIX          - install prefix of the Access Daemon
# SEANERBUS_PREFIX        - install prefix of seanerbus-cpp (optional hint)
# EASYBUS_INCLUDE_DIRS    - where to find access_easybus.h and metrics_types.h
# EASYBUS_LIBRARIES       - libaccess_easybus, libcommon and libseanerbus_c
# EASYBUS_FOUND           - True if everything was found

find_path(EASYBUS_INCLUDE_DIR access_easybus.h
  HINTS "${EASYBUS_PREFIX}/include"
  PATH_SUFFIXES accessd)
find_library(EASYBUS_LIBRARY access_easybus
  HINTS "${EASYBUS_PREFIX}/lib"
  PATH_SUFFIXES accessd)
# libaccess_easybus does not link libcommon (label_to_id, timestamp_*)
find_library(EASYBUS_COMMON_LIBRARY common
  HINTS "${EASYBUS_PREFIX}/lib"
  PATH_SUFFIXES accessd)
find_library(EASYBUS_SEANERBUS_LIBRARY seanerbus_c
  HINTS "${SEANERBUS_PREFIX}/lib")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  EASYBUS DEFAULT_MSG
  EASYBUS_INCLUDE_DIR EASYBUS_LIBRARY
  EASYBUS_COMMON_LIBRARY EASYBUS_SEANERBUS_LIBRARY)

set(EASYBUS_INCLUDE_DIRS ${EASYBUS_INCLUDE_DIR})
set(EASYBUS_LIBRARIES ${EASYBUS_LIBRARY} ${EASYBUS_COMMON_LIBRARY}
                      ${EASYBUS_SEANERBUS_LIBRARY})
