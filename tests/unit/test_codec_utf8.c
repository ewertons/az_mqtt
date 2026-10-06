// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full
// license information.

/**
 * @file test_codec_utf8.c
 * @brief _az_mqtt_utf8_valid(), and reading and writing UTF-8 strings and
 * binary data.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "az_mqtt_codec_internal.h"

/** @brief Span over string literal @p lit, without its terminating NUL. */
#define _SPAN(lit)                                                             \
  az_span_create((uint8_t *)(uintptr_t)(lit), (int32_t)sizeof(lit) - 1)

static void well_formed_utf8_is_valid(void **state) {
  (void)state;
  assert_true(_az_mqtt_utf8_valid(AZ_SPAN_EMPTY));
  assert_true(
      _az_mqtt_utf8_valid(AZ_SPAN_FROM_STR("devices/d1/messages/events/")));
  assert_true(_az_mqtt_utf8_valid(_SPAN("\x7F")));
  assert_true(_az_mqtt_utf8_valid(_SPAN("\xC2\x80")));         // U+0080
  assert_true(_az_mqtt_utf8_valid(_SPAN("\xE0\xA0\x80")));     // U+0800
  assert_true(_az_mqtt_utf8_valid(_SPAN("\xED\x9F\xBF")));     // U+D7FF
  assert_true(_az_mqtt_utf8_valid(_SPAN("\xEE\x80\x80")));     // U+E000
  assert_true(_az_mqtt_utf8_valid(_SPAN("\xEF\xBF\xBF")));     // U+FFFF
  assert_true(_az_mqtt_utf8_valid(_SPAN("\xF0\x90\x80\x80"))); // U+10000
  assert_true(_az_mqtt_utf8_valid(_SPAN("\xF4\x8F\xBF\xBF"))); // U+10FFFF
}

static void malformed_utf8_and_nul_are_invalid(void **state) {
  (void)state;
  assert_false(_az_mqtt_utf8_valid(_SPAN("a"
                                         "\x00"
                                         "b")));            // U+0000
  assert_false(_az_mqtt_utf8_valid(_SPAN("\x80")));         // Lone continuation
  assert_false(_az_mqtt_utf8_valid(_SPAN("\xC0\x80")));     // Overlong U+0000
  assert_false(_az_mqtt_utf8_valid(_SPAN("\xC1\xBF")));     // Overlong
  assert_false(_az_mqtt_utf8_valid(_SPAN("\xE0\x9F\xBF"))); // Overlong
  assert_false(_az_mqtt_utf8_valid(_SPAN("\xED\xA0\x80"))); // U+D800
  assert_false(_az_mqtt_utf8_valid(_SPAN("\xED\xBF\xBF"))); // U+DFFF
  assert_false(_az_mqtt_utf8_valid(_SPAN("\xF0\x8F\xBF\xBF"))); // Overlong
  assert_false(
      _az_mqtt_utf8_valid(_SPAN("\xF4\x90\x80\x80"))); // Above U+10FFFF
  assert_false(_az_mqtt_utf8_valid(_SPAN("\xF5\x80\x80\x80")));
  assert_false(_az_mqtt_utf8_valid(_SPAN("\xFF")));
  assert_false(_az_mqtt_utf8_valid(_SPAN("\xE2\x82")));     // Truncated
  assert_false(_az_mqtt_utf8_valid(_SPAN("\xE2\x28\xA1"))); // Bad continuation
  assert_false(_az_mqtt_utf8_valid(_SPAN("\xF0\x90\x80\x28")));
}

static void
reading_a_string_checks_it_and_reading_binary_does_not(void **state) {
  (void)state;
  az_span out;
  az_span src = _SPAN("\x00\x03"
                      "a"
                      "\x00"
                      "b");
  assert_int_equal(_az_mqtt_read_utf8_string(&src, &out),
                   AZ_MQTT_ERROR_MALFORMED_PACKET);
  src = _SPAN("\x00\x03"
              "a"
              "\x00"
              "b");
  assert_int_equal(_az_mqtt_read_binary_data(&src, &out), AZ_OK);
  assert_int_equal(az_span_size(out), 3);
  assert_int_equal(az_span_size(src), 0);
  src = _SPAN("\x00\x02\xC3\xA9"); // "é"
  assert_int_equal(_az_mqtt_read_utf8_string(&src, &out), AZ_OK);
  assert_int_equal(az_span_size(out), 2);
}

static void
writing_a_string_checks_it_and_writing_binary_does_not(void **state) {
  (void)state;
  uint8_t buf[16];
  az_span dest = AZ_SPAN_FROM_BUFFER(buf);
  assert_int_equal(_az_mqtt_write_utf8_string(&dest, _SPAN("a"
                                                           "\x00"
                                                           "b")),
                   AZ_ERROR_ARG);
  assert_int_equal(_az_mqtt_write_utf8_string(&dest, _SPAN("\xC0\x80")),
                   AZ_ERROR_ARG);
  assert_int_equal(az_span_size(dest),
                   (int32_t)sizeof(buf)); // Nothing written.
  assert_int_equal(_az_mqtt_write_binary_data(&dest, _SPAN("a"
                                                           "\x00"
                                                           "b")),
                   AZ_OK);
  assert_int_equal(_az_mqtt_write_utf8_string(&dest, _SPAN("\xC3\xA9")), AZ_OK);
  assert_int_equal(az_span_size(dest), (int32_t)sizeof(buf) - 5 - 4);
}

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(well_formed_utf8_is_valid),
      cmocka_unit_test(malformed_utf8_and_nul_are_invalid),
      cmocka_unit_test(reading_a_string_checks_it_and_reading_binary_does_not),
      cmocka_unit_test(writing_a_string_checks_it_and_writing_binary_does_not),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
