// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_codec_fixed_header.c
 * @brief _az_mqtt_decode_fixed_header(): reserved flags per packet type.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "az_mqtt_codec_internal.h"

/** @brief Decode a fixed header with first byte @p first_byte and Remaining Length 0. */
static az_result _decode(uint8_t first_byte)
{
  uint8_t bytes[] = { first_byte, 0x00 };
  az_span src = AZ_SPAN_FROM_BUFFER(bytes);
  az_mqtt_packet_type type;
  uint8_t flags;
  int32_t remaining;
  return _az_mqtt_decode_fixed_header(&src, &type, &flags, &remaining);
}

static void only_the_reserved_flags_are_accepted(void** state)
{
  (void)state;
  for (unsigned type = 1; type <= 15; type++)
  {
    for (unsigned flags = 0; flags <= 15; flags++)
    {
      bool const requires_0010 = type == AZ_MQTT_PACKET_TYPE_PUBREL
          || type == AZ_MQTT_PACKET_TYPE_SUBSCRIBE || type == AZ_MQTT_PACKET_TYPE_UNSUBSCRIBE;
      bool const valid = type == AZ_MQTT_PACKET_TYPE_PUBLISH || flags == (requires_0010 ? 2u : 0u);
      az_result const rc = _decode((uint8_t)((type << 4) | flags));
      if (valid)
      {
        assert_int_equal(rc, AZ_OK);
      }
      else
      {
        assert_int_equal(rc, AZ_MQTT_ERROR_MALFORMED_PACKET);
      }
    }
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(only_the_reserved_flags_are_accepted),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
