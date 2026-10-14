// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_codec_user_properties.c
 * @brief az_mqttv5 User Properties without an array: az_mqtt5_codec_next_user_property() over
 * az_mqtt5_publish_data.properties, and encoding from az_mqtt5_publish_options.user_property_next.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include <az_mqtt5/az_mqtt5_codec.h>

/* Properties block: Content Type "a", User Property k0=v0, Correlation Data
 * {1}, Subscription Identifier 5, User Property ""="" , Message Expiry 7. */
static const uint8_t k_props_block[] = {
  0x03, 0x00, 0x01, 'a', // Content Type
  0x26, 0x00, 0x02, 'k',  '0',  0x00, 0x02, 'v', '0', // User Property
  0x09, 0x00, 0x01, 0x01, // Correlation Data
  0x0B, 0x05, // Subscription Identifier
  0x26, 0x00, 0x00, 0x00, 0x00, // User Property, both empty
  0x02, 0x00, 0x00, 0x00, 0x07, // Message Expiry Interval
};

static void codec_next_user_property_skips_other_properties(void** state)
{
  (void)state;
  az_span props = az_span_create((uint8_t*)(uintptr_t)k_props_block, sizeof(k_props_block));
  int32_t cursor = 0;
  az_mqtt5_user_property p;

  assert_int_equal(az_mqtt5_codec_next_user_property(props, &cursor, &p), AZ_OK);
  assert_true(az_span_is_content_equal(p.key, AZ_SPAN_FROM_STR("k0")));
  assert_true(az_span_is_content_equal(p.value, AZ_SPAN_FROM_STR("v0")));
  assert_int_equal(az_mqtt5_codec_next_user_property(props, &cursor, &p), AZ_OK);
  assert_int_equal(az_span_size(p.key), 0);
  assert_int_equal(az_span_size(p.value), 0);
  assert_int_equal(az_mqtt5_codec_next_user_property(props, &cursor, &p), AZ_ERROR_ITEM_NOT_FOUND);
  assert_int_equal(cursor, (int32_t)sizeof(k_props_block));
  assert_int_equal(az_mqtt5_codec_next_user_property(props, &cursor, &p), AZ_ERROR_ITEM_NOT_FOUND);
  assert_int_equal(
      az_mqtt5_codec_next_user_property(AZ_SPAN_EMPTY, &(int32_t){ 0 }, &p),
      AZ_ERROR_ITEM_NOT_FOUND);
}

static void codec_next_user_property_reports_malformed_blocks(void** state)
{
  (void)state;
  static const uint8_t truncated[] = { 0x26, 0x00, 0x02, 'k' };
  static const uint8_t unknown_id[] = { 0x7F, 0x00 };
  static const uint8_t no_value[] = { 0x26, 0x00, 0x01, 'k' };
  static const uint8_t bad_utf8[] = { 0x26, 0x00, 0x01, 0xFF, 0x00, 0x00 };
  static const uint8_t truncated_skip[] = { 0x02, 0x00, 0x00 };
  struct
  {
    const uint8_t* bytes;
    int32_t len;
  } const cases[] = {
    { truncated, sizeof(truncated) },
    { unknown_id, sizeof(unknown_id) },
    { no_value, sizeof(no_value) },
    { bad_utf8, sizeof(bad_utf8) },
    { truncated_skip, sizeof(truncated_skip) },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    int32_t cursor = 0;
    az_mqtt5_user_property p;
    assert_int_equal(
        az_mqtt5_codec_next_user_property(
            az_span_create((uint8_t*)(uintptr_t)cases[i].bytes, cases[i].len), &cursor, &p),
        AZ_MQTT_ERROR_MALFORMED_PACKET);
  }
  int32_t past = 5;
  az_mqtt5_user_property p;
  assert_int_equal(
      az_mqtt5_codec_next_user_property(
          az_span_create((uint8_t*)(uintptr_t)truncated, sizeof(truncated)), &past, &p),
      AZ_ERROR_ARG);
}

/* Reader over N generated properties; `extra` makes the second pass differ. */
typedef struct
{
  int32_t count;
  int32_t passes;
  int32_t extra;
  az_result fail_with;
  char keys[40][4];
} reader_ctx;

static az_result read_props(void* context, int32_t* cursor, az_mqtt5_user_property* out)
{
  reader_ctx* r = (reader_ctx*)context;
  if (*cursor == 0)
  {
    r->passes++;
  }
  if (r->fail_with != AZ_OK && *cursor == 3)
  {
    return r->fail_with;
  }
  int32_t const n = r->count + (r->passes > 1 ? r->extra : 0);
  if (*cursor >= n)
  {
    return AZ_ERROR_ITEM_NOT_FOUND;
  }
  out->key = az_span_create_from_str(r->keys[*cursor]);
  out->value = AZ_SPAN_FROM_STR("v");
  (*cursor)++;
  return AZ_OK;
}

static void codec_encodes_user_properties_from_a_reader(void** state)
{
  (void)state;
  reader_ctx r = { .count = 30, .fail_with = AZ_OK };
  az_mqtt5_user_property array[30];
  for (int32_t i = 0; i < 30; ++i)
  {
    snprintf(r.keys[i], sizeof(r.keys[i]), "k%d", (int)i);
    array[i].key = az_span_create_from_str(r.keys[i]);
    array[i].value = AZ_SPAN_FROM_STR("v");
  }
  az_mqtt5_publish_options from_array = az_mqtt5_publish_options_default();
  from_array.topic = AZ_SPAN_FROM_STR("t");
  from_array.payload = AZ_SPAN_FROM_STR("p");
  from_array.user_properties = array;
  from_array.user_property_count = 30;
  az_mqtt5_publish_options from_reader = from_array;
  from_reader.user_properties = NULL;
  from_reader.user_property_count = 0;
  from_reader.user_property_next = read_props;
  from_reader.user_property_context = &r;

  uint8_t a[512];
  uint8_t b[512];
  az_span sa = AZ_SPAN_FROM_BUFFER(a);
  az_span sb = AZ_SPAN_FROM_BUFFER(b);
  assert_int_equal(az_mqtt5_codec_encode_publish(&sa, &from_array, 0), AZ_OK);
  assert_int_equal(az_mqtt5_codec_encode_publish(&sb, &from_reader, 0), AZ_OK);
  int32_t const len = (int32_t)sizeof(a) - az_span_size(sa);
  assert_int_equal(len, (int32_t)sizeof(b) - az_span_size(sb));
  assert_memory_equal(a, b, (size_t)len);

  // Decoding gives every property back through the raw view, none dropped.
  az_mqtt5_publish_data data = { 0 };
  uint8_t const* body = a + 3; // Fixed header byte and a 2-byte Remaining Length.
  assert_int_equal(
      az_mqtt5_codec_decode_publish(
          az_span_create((uint8_t*)(uintptr_t)body, len - 3), (uint8_t)(a[0] & 0x0F), &data),
      AZ_OK);
  assert_int_equal(data.user_property_count, 0); // No array given: the old path keeps none.
  int32_t cursor = 0;
  int32_t seen = 0;
  az_mqtt5_user_property p;
  while (az_mqtt5_codec_next_user_property(data.properties, &cursor, &p) == AZ_OK)
  {
    assert_true(az_span_is_content_equal(p.key, array[seen].key));
    seen++;
  }
  assert_int_equal(seen, 30);
}

static void codec_refuses_a_reader_that_changes_or_fails(void** state)
{
  (void)state;
  reader_ctx r = { .count = 4, .extra = 1, .fail_with = AZ_OK };
  for (int32_t i = 0; i < 40; ++i)
  {
    snprintf(r.keys[i], sizeof(r.keys[i]), "k%d", (int)i);
  }
  az_mqtt5_publish_options o = az_mqtt5_publish_options_default();
  o.topic = AZ_SPAN_FROM_STR("t");
  o.user_property_next = read_props;
  o.user_property_context = &r;
  uint8_t buf[256];
  az_span s = AZ_SPAN_FROM_BUFFER(buf);
  // More on the write pass than when sized.
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_ARG);

  // Fewer.
  r.passes = 0;
  r.count = 5;
  r.extra = -1;
  s = AZ_SPAN_FROM_BUFFER(buf);
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_ARG);

  // A reader error is the encode's result.
  r.passes = 0;
  r.extra = 0;
  r.fail_with = AZ_ERROR_NOT_SUPPORTED;
  s = AZ_SPAN_FROM_BUFFER(buf);
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_NOT_SUPPORTED);

  // A result that is neither AZ_OK, the end, nor an error is refused, not taken as the end.
  r.passes = 0;
  r.fail_with = (az_result)0;
  s = AZ_SPAN_FROM_BUFFER(buf);
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_ARG);

  // Too many for the buffer: not enough space, not a cut.
  r.fail_with = AZ_OK;
  r.passes = 0;
  r.count = 40;
  uint8_t small[64];
  s = AZ_SPAN_FROM_BUFFER(small);
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_NOT_ENOUGH_SPACE);
}

/* Reader of 2047 properties with 65,535-byte keys and values: 268,310,525 bytes, under the
 * Variable Byte Integer maximum (268,435,455) on its own. */
static char s_big[65535];

static az_result read_big(void* context, int32_t* cursor, az_mqtt5_user_property* out)
{
  (void)context;
  if (*cursor >= 2047)
  {
    return AZ_ERROR_ITEM_NOT_FOUND;
  }
  out->key = az_span_create((uint8_t*)s_big, (int32_t)sizeof(s_big));
  out->value = out->key;
  (*cursor)++;
  return AZ_OK;
}

static void codec_refuses_a_remaining_length_past_the_vbi_maximum(void** state)
{
  (void)state;
  az_mqtt5_publish_options o = az_mqtt5_publish_options_default();
  o.topic = AZ_SPAN_FROM_STR("t");
  o.user_property_next = read_big;
  // Never read: the encode fails on the size alone.
  o.payload = az_span_create((uint8_t*)s_big, 200000);
  uint8_t buf[64];
  az_span s = AZ_SPAN_FROM_BUFFER(buf);
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_NOT_ENOUGH_SPACE);
}

/* Span sizes whose sum overflows int32_t: refused, not wrapped. Never read. */
static az_result read_huge(void* context, int32_t* cursor, az_mqtt5_user_property* out)
{
  (void)context;
  if (*cursor >= 1)
  {
    return AZ_ERROR_ITEM_NOT_FOUND;
  }
  out->key = az_span_create((uint8_t*)s_big, INT32_MAX);
  out->value = out->key;
  (*cursor)++;
  return AZ_OK;
}

static void codec_refuses_property_sizes_that_overflow(void** state)
{
  (void)state;
  az_mqtt5_publish_options o = az_mqtt5_publish_options_default();
  o.topic = AZ_SPAN_FROM_STR("t");
  o.user_property_next = read_huge;
  uint8_t buf[64];
  az_span s = AZ_SPAN_FROM_BUFFER(buf);
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_NOT_ENOUGH_SPACE);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(codec_next_user_property_skips_other_properties),
    cmocka_unit_test(codec_next_user_property_reports_malformed_blocks),
    cmocka_unit_test(codec_encodes_user_properties_from_a_reader),
    cmocka_unit_test(codec_refuses_a_reader_that_changes_or_fails),
    cmocka_unit_test(codec_refuses_a_remaining_length_past_the_vbi_maximum),
    cmocka_unit_test(codec_refuses_property_sizes_that_overflow),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
