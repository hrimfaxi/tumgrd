cmake_minimum_required(VERSION 3.16)
project(tumgrd C)

add_compile_options(-Wall -Wextra -Wno-error)

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

find_path(UCI_INCLUDE_DIR
  NAMES uci.h
)

find_library(UCI_LIBRARY
  NAMES uci
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

# Log warnings if UCI is not found, but do not stop the build
if (NOT UCI_INCLUDE_DIR OR NOT UCI_LIBRARY)
  message(WARNING "libuci headers or library not found, UCI will be disabled.")
else()
  # Define TUMGRD_UCI_ENABLED if UCI is found
  add_definitions(-DTUMGRD_UCI_ENABLED)
endif()

message(STATUS "UBOX_INCLUDE_DIR = ${UBOX_INCLUDE_DIR}")
message(STATUS "UBOX_LIBRARY     = ${UBOX_LIBRARY}")
message(STATUS "UBUS_INCLUDE_DIR = ${UBUS_INCLUDE_DIR}")
message(STATUS "UBUS_LIBRARY     = ${UBUS_LIBRARY}")
message(STATUS "UCI_INCLUDE_DIR  = ${UCI_INCLUDE_DIR}")
message(STATUS "UCI_LIBRARY      = ${UCI_LIBRARY}")
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

# If UCI is found, link the library and include the headers
if (UCI_INCLUDE_DIR AND UCI_LIBRARY)
  target_include_directories(tumgrd PRIVATE ${UCI_INCLUDE_DIR})
  target_link_libraries(tumgrd PRIVATE ${UCI_LIBRARY})
endif()

# vim: set sw=2 ts=2 et:
