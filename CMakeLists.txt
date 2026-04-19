cmake_minimum_required(VERSION 3.16)
project(tumgrd C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SQLite3 REQUIRED)

find_path(UBOX_INCLUDE_DIR
  NAMES libubox/uloop.h libubox/blobmsg_json.h
)

find_library(UBOX_LIBRARY
  NAMES ubox
)

find_path(UBUS_INCLUDE_DIR
  NAMES libubus.h
)

find_library(UBUS_LIBRARY
  NAMES ubus
)

if (NOT UBOX_INCLUDE_DIR)
  message(FATAL_ERROR "libubox headers not found")
endif()

if (NOT UBOX_LIBRARY)
  message(FATAL_ERROR "libubox library not found")
endif()

if (NOT UBUS_INCLUDE_DIR)
  message(FATAL_ERROR "libubus headers not found")
endif()

if (NOT UBUS_LIBRARY)
  message(FATAL_ERROR "libubus library not found")
endif()

message(STATUS "UBOX_INCLUDE_DIR = ${UBOX_INCLUDE_DIR}")
message(STATUS "UBOX_LIBRARY     = ${UBOX_LIBRARY}")
message(STATUS "UBUS_INCLUDE_DIR = ${UBUS_INCLUDE_DIR}")
message(STATUS "UBUS_LIBRARY     = ${UBUS_LIBRARY}")
message(STATUS "SQLite3_INCLUDE_DIRS = ${SQLite3_INCLUDE_DIRS}")
message(STATUS "SQLite3_LIBRARIES    = ${SQLite3_LIBRARIES}")

add_executable(tumgrd
  src/log.c
  src/main.c
  src/db.c
  src/reconcile.c
  src/runner.c
  src/ipdetect.c
  src/ubus_if.c
  src/helper.c
)

target_include_directories(tumgrd PRIVATE
  ${UBOX_INCLUDE_DIR}
  ${UBUS_INCLUDE_DIR}
  ${SQLite3_INCLUDE_DIRS}
)

target_link_libraries(tumgrd PRIVATE
  ${UBOX_LIBRARY}
  ${UBUS_LIBRARY}
  SQLite::SQLite3
)

# vim: set sw=2 ts=2 et:
