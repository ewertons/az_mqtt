# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

add_az_mqtt3_local_test(az_mqtt3_test_transport_tls test_transport_tls.c)
add_az_mqtt3_local_test(az_mqtt3_test_transport_robustness test_transport_robustness.c)
add_az_mqtt3_local_test(az_mqtt3_test_transport_timeouts test_transport_timeouts.c
  az_mqtt3_client_short_timeouts)
