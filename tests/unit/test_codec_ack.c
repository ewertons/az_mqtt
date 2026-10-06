// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_codec_ack.c
 * @brief Decoders of az_mqttv3 / az_mqttv5 (AZ_MQTT_TEST_VERSION): acknowledgements, PUBLISH,
 * CONNACK, SUBACK.
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

static void a_qos1_publish_with_packet_id_0_is_malformed(void** state)
{
  (void)state;
#if AZ_MQTT_TEST_VERSION == 5
  uint8_t body[] = { 0x00, 0x01, 't', 0x00, 0x00, 0x00 }; // Topic, packet id 0, no properties.
  az_mqtt5_publish_data publish;
  memset(&publish, 0, sizeof(publish));
  assert_int_equal(
      az_mqtt5_codec_decode_publish(AZ_SPAN_FROM_BUFFER(body), 0x02, &publish),
      AZ_MQTT_ERROR_MALFORMED_PACKET);
#else
  uint8_t body[] = { 0x00, 0x01, 't', 0x00, 0x00 }; // Topic, packet id 0.
  az_mqtt3_publish_data publish;
  assert_int_equal(
      az_mqtt3_codec_decode_publish(AZ_SPAN_FROM_BUFFER(body), 0x02, &publish),
      AZ_MQTT_ERROR_MALFORMED_PACKET);
#endif
}

static void acknowledgements_with_packet_id_0_are_malformed(void** state)
{
  (void)state;
#if AZ_MQTT_TEST_VERSION == 5
  uint8_t ack[] = { 0x00, 0x00 };
  uint8_t sub[] = { 0x00, 0x00, 0x00, 0x00 }; // Packet id 0, no properties, one reason code.
  az_mqtt5_ack_data a;
  memset(&a, 0, sizeof(a));
  az_mqtt5_suback_data b;
  memset(&b, 0, sizeof(b));
  assert_int_equal(
      az_mqtt5_codec_decode_ack(AZ_SPAN_FROM_BUFFER(ack), &a), AZ_MQTT_ERROR_MALFORMED_PACKET);
  assert_int_equal(
      az_mqtt5_codec_decode_suback(AZ_SPAN_FROM_BUFFER(sub), &b), AZ_MQTT_ERROR_MALFORMED_PACKET);
  assert_int_equal(
      az_mqtt5_codec_decode_unsuback(AZ_SPAN_FROM_BUFFER(sub), &b), AZ_MQTT_ERROR_MALFORMED_PACKET);
#else
  uint8_t ack[] = { 0x00, 0x00 }; // PUBACK, PUBREC, PUBREL, PUBCOMP, UNSUBACK.
  uint8_t sub[] = { 0x00, 0x00, 0x00 };
  az_mqtt3_ack_data a;
  az_mqtt3_suback_data b;
  assert_int_equal(
      az_mqtt3_codec_decode_ack(AZ_SPAN_FROM_BUFFER(ack), &a), AZ_MQTT_ERROR_MALFORMED_PACKET);
  assert_int_equal(
      az_mqtt3_codec_decode_suback(AZ_SPAN_FROM_BUFFER(sub), &b), AZ_MQTT_ERROR_MALFORMED_PACKET);
#endif
}

/** @brief Decode a CONNACK with acknowledge flags @p flags and code @p code. */
static az_result _decode_connack(uint8_t flags, uint8_t code)
{
#if AZ_MQTT_TEST_VERSION == 5
  uint8_t body[] = { flags, code, 0x00 }; // No properties.
  az_mqtt5_connack_data connack;
  memset(&connack, 0, sizeof(connack));
  return az_mqtt5_codec_decode_connack(AZ_SPAN_FROM_BUFFER(body), &connack);
#else
  uint8_t body[] = { flags, code };
  az_mqtt3_connack_data connack;
  return az_mqtt3_codec_decode_connack(AZ_SPAN_FROM_BUFFER(body), &connack);
#endif
}

static void connack_flags_and_codes_are_checked(void** state)
{
  (void)state;
  assert_int_equal(_decode_connack(0x00, 0x00), AZ_OK);
  assert_int_equal(_decode_connack(0x01, 0x00), AZ_OK); // Session present.
  assert_int_equal(_decode_connack(0x02, 0x00), AZ_MQTT_ERROR_MALFORMED_PACKET); // Reserved bit.
  assert_int_equal(_decode_connack(0x80, 0x00), AZ_MQTT_ERROR_MALFORMED_PACKET);
#if AZ_MQTT_TEST_VERSION == 5
  assert_int_equal(_decode_connack(0x00, 0x87), AZ_OK); // Not authorized.
  assert_int_equal(_decode_connack(0x01, 0x87), AZ_MQTT_ERROR_MALFORMED_PACKET);
#else
  assert_int_equal(_decode_connack(0x00, 0x05), AZ_OK); // Not authorized.
  assert_int_equal(_decode_connack(0x01, 0x05), AZ_MQTT_ERROR_MALFORMED_PACKET);
  assert_int_equal(_decode_connack(0x00, 0x06), AZ_MQTT_ERROR_MALFORMED_PACKET); // Reserved.
#endif
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

#if AZ_MQTT_TEST_VERSION == 5
static void zero_valued_properties_are_protocol_errors(void** state)
{
  (void)state;
  az_mqtt5_connack_data connack;
  memset(&connack, 0, sizeof(connack));
  uint8_t receive_maximum_0[] = { 0x00, 0x00, 0x03, 0x21, 0x00, 0x00 };
  uint8_t maximum_packet_size_0[] = { 0x00, 0x00, 0x05, 0x27, 0x00, 0x00, 0x00, 0x00 };
  assert_int_equal(
      az_mqtt5_codec_decode_connack(AZ_SPAN_FROM_BUFFER(receive_maximum_0), &connack),
      AZ_MQTT_ERROR_PROTOCOL);
  assert_int_equal(
      az_mqtt5_codec_decode_connack(AZ_SPAN_FROM_BUFFER(maximum_packet_size_0), &connack),
      AZ_MQTT_ERROR_PROTOCOL);

  az_mqtt5_publish_data publish;
  memset(&publish, 0, sizeof(publish));
  uint8_t topic_alias_0[] = { 0x00, 0x01, 't', 0x03, 0x23, 0x00, 0x00 };
  uint8_t subscription_identifier_0[] = { 0x00, 0x01, 't', 0x02, 0x0B, 0x00 };
  uint8_t topic_alias_1[] = { 0x00, 0x01, 't', 0x03, 0x23, 0x00, 0x01 };
  assert_int_equal(
      az_mqtt5_codec_decode_publish(AZ_SPAN_FROM_BUFFER(topic_alias_0), 0x00, &publish),
      AZ_MQTT_ERROR_PROTOCOL);
  assert_int_equal(
      az_mqtt5_codec_decode_publish(AZ_SPAN_FROM_BUFFER(subscription_identifier_0), 0x00, &publish),
      AZ_MQTT_ERROR_PROTOCOL);
  assert_int_equal(
      az_mqtt5_codec_decode_publish(AZ_SPAN_FROM_BUFFER(topic_alias_1), 0x00, &publish), AZ_OK);
  assert_int_equal(publish.topic_alias, 1);
}

static void one_byte_flag_properties_must_be_0_or_1(void** state)
{
  (void)state;
  // CONNACK: Maximum QoS; Retain, Wildcard, Subscription Identifier, Shared Subscription Available.
  uint8_t const ids[] = { 0x24, 0x25, 0x28, 0x29, 0x2A };
  for (size_t i = 0; i < sizeof(ids); i++)
  {
    az_mqtt5_connack_data connack;
    memset(&connack, 0, sizeof(connack));
    uint8_t one[] = { 0x00, 0x00, 0x02, ids[i], 0x01 };
    uint8_t two[] = { 0x00, 0x00, 0x02, ids[i], 0x02 };
    assert_int_equal(az_mqtt5_codec_decode_connack(AZ_SPAN_FROM_BUFFER(one), &connack), AZ_OK);
    assert_int_equal(
        az_mqtt5_codec_decode_connack(AZ_SPAN_FROM_BUFFER(two), &connack), AZ_MQTT_ERROR_PROTOCOL);
  }
  // PUBLISH: Payload Format Indicator.
  az_mqtt5_publish_data publish;
  memset(&publish, 0, sizeof(publish));
  uint8_t utf8[] = { 0x00, 0x01, 't', 0x02, 0x01, 0x01 };
  uint8_t two[] = { 0x00, 0x01, 't', 0x02, 0x01, 0x02 };
  assert_int_equal(az_mqtt5_codec_decode_publish(AZ_SPAN_FROM_BUFFER(utf8), 0x00, &publish), AZ_OK);
  assert_int_equal(
      az_mqtt5_codec_decode_publish(AZ_SPAN_FROM_BUFFER(two), 0x00, &publish),
      AZ_MQTT_ERROR_PROTOCOL);
}

static void single_use_properties_must_not_repeat(void** state)
{
  (void)state;
  az_mqtt5_connack_data connack;
  memset(&connack, 0, sizeof(connack));
  uint8_t two_receive_maximum[] = { 0x00, 0x00, 0x06, 0x21, 0x00, 0x05, 0x21, 0x00, 0x05 };
  assert_int_equal(
      az_mqtt5_codec_decode_connack(AZ_SPAN_FROM_BUFFER(two_receive_maximum), &connack),
      AZ_MQTT_ERROR_PROTOCOL);

  az_mqtt5_publish_data publish;
  memset(&publish, 0, sizeof(publish));
  uint8_t two_topic_alias[] = { 0x00, 0x01, 't', 0x06, 0x23, 0x00, 0x01, 0x23, 0x00, 0x01 };
  assert_int_equal(
      az_mqtt5_codec_decode_publish(AZ_SPAN_FROM_BUFFER(two_topic_alias), 0x00, &publish),
      AZ_MQTT_ERROR_PROTOCOL);
  // User Property and Subscription Identifier may repeat.
  uint8_t repeatable[] = { 0x00, 0x01, 't', 0x0E, 0x26, 0x00, 0x01, 'k', 0x00, 0x01, 'v', 0x26,
                           0x00, 0x01, 'k', 0x00, 0x01, 'v' };
  uint8_t two_subscription_ids[] = { 0x00, 0x01, 't', 0x04, 0x0B, 0x01, 0x0B, 0x02 };
  assert_int_equal(
      az_mqtt5_codec_decode_publish(AZ_SPAN_FROM_BUFFER(repeatable), 0x00, &publish), AZ_OK);
  assert_int_equal(
      az_mqtt5_codec_decode_publish(AZ_SPAN_FROM_BUFFER(two_subscription_ids), 0x00, &publish),
      AZ_OK);

  az_mqtt5_ack_data ack;
  memset(&ack, 0, sizeof(ack));
  uint8_t two_reason_strings[]
      = { 0x00, 0x01, 0x00, 0x08, 0x1F, 0x00, 0x01, 'a', 0x1F, 0x00, 0x01, 'b' };
  assert_int_equal(
      az_mqtt5_codec_decode_ack(AZ_SPAN_FROM_BUFFER(two_reason_strings), &ack),
      AZ_MQTT_ERROR_PROTOCOL);
}
#endif

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_decoded_ack_has_status_ok),
    cmocka_unit_test(a_publish_with_qos_3_is_malformed),
    cmocka_unit_test(a_qos1_publish_with_packet_id_0_is_malformed),
    cmocka_unit_test(connack_flags_and_codes_are_checked),
    cmocka_unit_test(acknowledgements_with_packet_id_0_are_malformed),
#if AZ_MQTT_TEST_VERSION == 5
    cmocka_unit_test(zero_valued_properties_are_protocol_errors),
    cmocka_unit_test(one_byte_flag_properties_must_be_0_or_1),
    cmocka_unit_test(single_use_properties_must_not_repeat),
#endif
    cmocka_unit_test(a_suback_without_codes_is_malformed),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
