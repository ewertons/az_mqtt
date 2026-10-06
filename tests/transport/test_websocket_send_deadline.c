// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full
// license information.

/**
 * @file test_websocket_send_deadline.c
 * @brief A WebSocket frame sent in pieces to a slow peer is cut off once
 * AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS (1000 here) has passed, not after a full
 * timeout per piece.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "az_mqtt_websocket_internal.h"
#include "test_fake_transport.h"

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_websocket.h>

#include <azure/core/az_base64.h>

#include <string.h>

static uint8_t s_input[1024];
static uint8_t s_sent[65536];

/** @brief Upgrade @p ws over @p fake: answers the request with a valid 101. */
static void _connect(test_fake_transport* fake, az_mqtt_transport* t)
{
  assert_int_equal(az_mqtt_transport_connect_start(t, AZ_SPAN_FROM_STR("h"), 80, NULL), AZ_OK);
  az_result rc = AZ_MQTT_ERROR_TIMEOUT;
  for (int i = 0; i < 10 && fake->send_calls == 0; i++)
  {
    rc = az_mqtt_transport_connect_poll(t, 0);
  }
  assert_int_equal(rc, AZ_MQTT_ERROR_TIMEOUT);
  assert_int_equal(fake->send_calls, 1); // The request.
  char const* key = strstr((char const*)s_sent, "\r\nSec-WebSocket-Key: ");
  assert_non_null(key);
  static char const guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t joined[24 + sizeof(guid) - 1];
  memcpy(joined, key + 21, 24);
  memcpy(joined + 24, guid, sizeof(guid) - 1);
  uint8_t digest[20];
  _az_mqtt_sha1(joined, sizeof(joined), digest);
  char accept[29];
  int32_t written = 0;
  assert_int_equal(
      az_base64_encode(az_span_create((uint8_t*)accept, 28), AZ_SPAN_FROM_BUFFER(digest), &written),
      AZ_OK);
  static char const prefix[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                               "Connection: Upgrade\r\nSec-WebSocket-Accept: ";
  static char const suffix[] = "\r\nSec-WebSocket-Protocol: mqtt\r\n\r\n";
  test_fake_transport_feed(fake, prefix, (int32_t)sizeof(prefix) - 1);
  test_fake_transport_feed(fake, accept, 28);
  test_fake_transport_feed(fake, suffix, (int32_t)sizeof(suffix) - 1);
  for (int i = 0; i < 1000 && rc == AZ_MQTT_ERROR_TIMEOUT; i++)
  {
    rc = az_mqtt_transport_connect_poll(t, 0);
  }
  assert_int_equal(rc, AZ_OK);
}

static void a_frame_to_a_slow_peer_is_cut_off_at_the_send_timeout(void** state)
{
  (void)state;
  static uint8_t data[AZ_MQTT_WEBSOCKET_SEND_CHUNK * 60];
  test_fake_transport fake;
  test_fake_transport_init(&fake, AZ_SPAN_FROM_BUFFER(s_input), AZ_SPAN_FROM_BUFFER(s_sent));
  az_mqtt_websocket ws;
  assert_int_equal(az_mqtt_websocket_init(&ws, &fake.layer.base, NULL), AZ_OK);
  az_mqtt_transport* const t = az_mqtt_websocket_get_transport(&ws);
  _connect(&fake, t);

  // 60+ pieces of 50 ms each: about 3 s, against a 1 s send timeout.
  fake.send_delay_ms = 50;
  int const calls_before = fake.send_calls;
  int64_t const start = az_mqtt_transport_clock_ms();
  assert_int_equal(az_mqtt_transport_send(t, AZ_SPAN_FROM_BUFFER(data)), AZ_MQTT_ERROR_TIMEOUT);
  int64_t const elapsed = az_mqtt_transport_clock_ms() - start;
  // At least the timeout; at most twice that, plus slack for oversleeping.
  assert_in_range(
      elapsed, AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS, 3 * AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS);
  assert_true(fake.send_calls - calls_before < 40); // Cut off well before all pieces.
  // The cut-off frame left the stream unusable: nothing more is sent.
  int const calls_after = fake.send_calls;
  assert_int_equal(az_mqtt_transport_send(t, AZ_SPAN_FROM_STR("x")), AZ_MQTT_ERROR_NOT_CONNECTED);
  assert_int_equal(fake.send_calls, calls_after);
  az_mqtt_transport_close(t);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_frame_to_a_slow_peer_is_cut_off_at_the_send_timeout),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
