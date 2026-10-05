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

static void a_publish_with_qos_3_is_malformed(void** state)
{
  (void)state;
#if AZ_MQTT_TEST_VERSION == 5
  uint8_t body[] = { 0x00, 0x01, 't', 0x00, 0x09, 0x00 }; // Topic, packet id, no properties.
  az_mqtt5_publish_data publish;
  memset(&publish, 0, sizeof(publish));
  assert_int_equal(
      az_mqtt5_codec_decode_publish(AZ_SPAN_FROM_BUFFER(body), 0x06, &publish),
      AZ_MQTT_ERROR_MALFORMED_PACKET);
  assert_int_equal(az_mqtt5_codec_decode_publish(AZ_SPAN_FROM_BUFFER(body), 0x04, &publish), AZ_OK);
#else
  uint8_t body[] = { 0x00, 0x01, 't', 0x00, 0x09 }; // Topic, packet id.
  az_mqtt3_publish_data publish;
  assert_int_equal(
      az_mqtt3_codec_decode_publish(AZ_SPAN_FROM_BUFFER(body), 0x06, &publish),
      AZ_MQTT_ERROR_MALFORMED_PACKET);
  assert_int_equal(az_mqtt3_codec_decode_publish(AZ_SPAN_FROM_BUFFER(body), 0x04, &publish), AZ_OK);
#endif
  assert_int_equal(publish.qos, AZ_MQTT_QOS_EXACTLY_ONCE);
}

static void a_suback_without_codes_is_malformed(void** state)
{
  (void)state;
#if AZ_MQTT_TEST_VERSION == 5
  uint8_t empty[] = { 0x00, 0x01, 0x00 }; // Packet id, no properties, no reason code.
  uint8_t one[] = { 0x00, 0x01, 0x00, 0x02 };
  az_mqtt5_suback_data suback;
  memset(&suback, 0, sizeof(suback));
  assert_int_equal(
      az_mqtt5_codec_decode_suback(AZ_SPAN_FROM_BUFFER(empty), &suback),
      AZ_MQTT_ERROR_MALFORMED_PACKET);
  assert_int_equal(
      az_mqtt5_codec_decode_unsuback(AZ_SPAN_FROM_BUFFER(empty), &suback),
      AZ_MQTT_ERROR_MALFORMED_PACKET);
  assert_int_equal(az_mqtt5_codec_decode_suback(AZ_SPAN_FROM_BUFFER(one), &suback), AZ_OK);
#else
  uint8_t empty[] = { 0x00, 0x01 }; // Packet id, no return code.
  uint8_t one[] = { 0x00, 0x01, 0x02 };
  az_mqtt3_suback_data suback;
  assert_int_equal(
      az_mqtt3_codec_decode_suback(AZ_SPAN_FROM_BUFFER(empty), &suback),
      AZ_MQTT_ERROR_MALFORMED_PACKET);
  assert_int_equal(az_mqtt3_codec_decode_suback(AZ_SPAN_FROM_BUFFER(one), &suback), AZ_OK);
  assert_int_equal(az_span_size(suback.return_codes), 1);
#endif
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_decoded_ack_has_status_ok),
    cmocka_unit_test(a_publish_with_qos_3_is_malformed),
    cmocka_unit_test(a_suback_without_codes_is_malformed),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
