// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_codec_ack.c
 * @brief The acknowledgement decoder of az_mqttv3 / az_mqttv5 (AZ_MQTT_TEST_VERSION).
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#if AZ_MQTT_TEST_VERSION == 5
#include <az_mqtt5/az_mqtt5_codec.h>
#define _ACK_DATA az_mqtt5_ack_data
#define _DECODE_ACK az_mqtt5_codec_decode_ack
#else
#include <az_mqtt3/az_mqtt3_codec.h>
#define _ACK_DATA az_mqtt3_ack_data
#define _DECODE_ACK az_mqtt3_codec_decode_ack
#endif

static void a_decoded_ack_has_status_ok(void** state)
{
  (void)state;
  uint8_t body[] = { 0x12, 0x34 }; // PUBACK body: packet identifier only.
  _ACK_DATA ack;
  memset(&ack, 0xA5, sizeof(ack)); // Garbage, as a caller's uninitialized variable.
#if AZ_MQTT_TEST_VERSION == 5
  ack.user_properties = NULL;
  ack.user_property_capacity = 0;
#endif
  assert_int_equal(_DECODE_ACK(AZ_SPAN_FROM_BUFFER(body), &ack), AZ_OK);
  assert_int_equal(ack.packet_id, 0x1234);
  assert_int_equal(ack.status, AZ_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_decoded_ack_has_status_ok),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
