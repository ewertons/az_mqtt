# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Fuzz targets; see README.md.
# - AZ_MQTT_BUILD_FUZZERS=ON: libFuzzer executables, and the drop folder (target az_mqtt_fuzz_drop).
# - Otherwise (tests): <target>_replay runs corpus/<seed>/* and regressions/<seed>/* under ctest.
# Fuzzing covers every layer. Replays need none: a target skips the inputs for a layer not built
# (WebSockets in the clients, the proxy or WebSocket layer in fuzz_transport).
if(AZ_MQTT_BUILD_FUZZERS AND NOT (AZ_MQTT_ENABLE_PROXY AND AZ_MQTT_ENABLE_WEBSOCKETS))
  message(FATAL_ERROR "The fuzzers need AZ_MQTT_ENABLE_PROXY and AZ_MQTT_ENABLE_WEBSOCKETS")
endif()

set(_fuzz_includes ${PROJECT_SOURCE_DIR}/tests/unit ${PROJECT_SOURCE_DIR}/src/core)
set(_fuzz_built "")

# One target: @p name from @p source, linked to @p library, with seeds in corpus/<seeds>.
function(az_mqtt_fuzz_target name source library seeds)
  set(sources ${source} ${PROJECT_SOURCE_DIR}/tests/unit/test_fake_transport.c)
  if(AZ_MQTT_BUILD_FUZZERS)
    add_executable(${name} ${sources})
    if(NOT MSVC)
      target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    endif()
  else()
    set(name ${name}_replay)
    add_executable(${name} ${sources} replay_main.c)
    file(GLOB inputs CONFIGURE_DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${seeds}/*
      ${CMAKE_CURRENT_SOURCE_DIR}/regressions/${seeds}/*)
    add_test(NAME ${name} COMMAND ${name} ${inputs})
  endif()
  target_include_directories(${name} PRIVATE ${_fuzz_includes})
  target_compile_definitions(${name} PRIVATE ${ARGN})
  target_link_libraries(${name} PRIVATE ${library})
  az_mqtt_apply_warnings(${name})
  set(_fuzz_built ${_fuzz_built} ${name} PARENT_SCOPE)
endfunction()

foreach(version 3 5)
  if(AZ_MQTT_ENABLE_MQTTV${version})
    az_mqtt_fuzz_target(az_mqtt${version}_fuzz_client fuzz_client.c az_mqtt::mqttv${version}
      client${version} AZ_MQTT_FUZZ_VERSION=${version})
  endif()
endforeach()
if(AZ_MQTT_ENABLE_PROXY OR AZ_MQTT_ENABLE_WEBSOCKETS)
  az_mqtt_fuzz_target(az_mqtt_fuzz_transport fuzz_transport.c az_mqtt::core transport)
endif()

if(AZ_MQTT_BUILD_FUZZERS)
  # Drop folder for a fuzzing service: the executables (Windows: with PDBs and the ASan
  # runtime) and the seed corpora.
  set(drop ${CMAKE_BINARY_DIR}/fuzz-drop)
  set(copy_files "")
  foreach(target ${_fuzz_built})
    list(APPEND copy_files $<TARGET_FILE:${target}>)
    if(MSVC)
      list(APPEND copy_files $<TARGET_PDB_FILE:${target}>)
    endif()
  endforeach()
  if(MSVC)
    get_filename_component(_cl_dir ${CMAKE_C_COMPILER} DIRECTORY)
    file(GLOB _asan_runtime ${_cl_dir}/clang_rt.asan_dynamic-x86_64.dll)
    list(APPEND copy_files ${_asan_runtime})
  endif()
  # Recreated: nothing from an earlier configuration is submitted.
  add_custom_target(az_mqtt_fuzz_drop ALL
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${drop}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${drop}
    COMMAND ${CMAKE_COMMAND} -E copy ${copy_files} ${drop}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/corpus ${drop}/corpus
    DEPENDS ${_fuzz_built}
    COMMENT "Fuzz drop folder: ${drop}"
    VERBATIM)
endif()
