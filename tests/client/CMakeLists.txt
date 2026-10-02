# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

add_az_mqtt_local_test(az_mqtt_test_client_session test_client_session.c PER_VERSION)

if(AZ_MQTT_ENABLE_WEBSOCKETS)
  # The same suite with every session over WebSockets.
  add_az_mqtt_local_test(az_mqtt_test_client_session_ws test_client_session.c PER_VERSION)
  foreach(version 3 5)
    if(TARGET az_mqtt${version}_test_client_session_ws)
      target_compile_definitions(az_mqtt${version}_test_client_session_ws PRIVATE
        AZ_MQTT_TEST_FORCE_WEBSOCKET)
    endif()
  endforeach()
endif()
