cmake_minimum_required(VERSION 3.10)
project(az_mqtt5_perf LANGUAGES C)
set(CMAKE_C_STANDARD 99)

# ── az_mqtt5 perf binary ──────────────────────────────────────
# This is built against the project tree (assumes the parent project has been
# configured with -DAZ_MQTT5_BUILD_TESTS=ON or the library target is available).
# When built standalone, the library root must be passed via AZ_MQTT5_ROOT.

if(NOT TARGET az_mqtt5::client)
  if(DEFINED AZ_MQTT5_ROOT)
    add_subdirectory(${AZ_MQTT5_ROOT} ${CMAKE_BINARY_DIR}/_az_mqtt5 EXCLUDE_FROM_ALL)
  else()
    # Assume we are inside the project tree at tests/client-comp/
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_BINARY_DIR}/_az_mqtt5 EXCLUDE_FROM_ALL)
  endif()
endif()

add_executable(perf_az_mqtt5 perf_az_mqtt5.c)
target_link_libraries(perf_az_mqtt5 PRIVATE az_mqtt5::client)

if(UNIX)
  target_link_libraries(perf_az_mqtt5 PRIVATE rt)
endif()

# ── Paho MQTT C perf binary ──────────────────────────────────
# Paho must be installed or available via pkg-config / find_package.
find_package(eclipse-paho-mqtt-c QUIET)
if(eclipse-paho-mqtt-c_FOUND)
  add_executable(perf_paho perf_paho.c)
  target_link_libraries(perf_paho PRIVATE eclipse-paho-mqtt-c::paho-mqtt3c)
  if(UNIX)
    target_link_libraries(perf_paho PRIVATE rt)
  endif()
else()
  # Fallback: try raw library name (system install)
  find_library(PAHO_MQTT3C paho-mqtt3c)
  if(PAHO_MQTT3C)
    add_executable(perf_paho perf_paho.c)
    target_link_libraries(perf_paho PRIVATE ${PAHO_MQTT3C})
    # Find the header
    find_path(PAHO_INCLUDE MQTTClient.h)
    if(PAHO_INCLUDE)
      target_include_directories(perf_paho PRIVATE ${PAHO_INCLUDE})
    endif()
    if(UNIX)
      target_link_libraries(perf_paho PRIVATE rt)
    endif()
  else()
    message(WARNING "Paho MQTT C not found – perf_paho will NOT be built.\n"
                    "Install with: apt install libpaho-mqtt-dev  OR  vcpkg install paho-mqtt")
  endif()
endif()
