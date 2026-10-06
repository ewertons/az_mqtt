// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file fuzz_websocket.h
 * @brief Fuzz targets: a valid WebSocket upgrade reply, so that frames can be reached (the key in
 * the request is random).
 */

#ifndef FUZZ_WEBSOCKET_H
#define FUZZ_WEBSOCKET_H

#include "az_mqtt_websocket_internal.h"
#include "test_fake_transport.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/**
 * @brief Once the upgrade request is in @p sent (NUL-terminated), feed a valid reply to it: the
 * Sec-WebSocket-Accept of its key. Whether it was fed.
 */
static bool fuzz_feed_upgrade_reply(test_fake_transport* fake, uint8_t const* sent)
{
  char const* key = strstr((char const*)sent, "\r\nSec-WebSocket-Key: ");
  if (key == NULL || strstr((char const*)sent, "\r\n\r\n") == NULL)
  {
    return false;
  }
  key += 21;
  static char const guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t joined[24 + sizeof(guid) - 1];
  memcpy(joined, key, 24);
  memcpy(joined + 24, guid, sizeof(guid) - 1);
  uint8_t digest[21] = { 0 };
  _az_mqtt_sha1(joined, sizeof(joined), digest);
  static char const b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char accept[28];
  for (int i = 0; i < 7; i++)
  {
    uint32_t const v = (uint32_t)digest[3 * i] << 16 | (uint32_t)digest[3 * i + 1] << 8
        | (uint32_t)digest[3 * i + 2];
    accept[4 * i] = b64[v >> 18 & 63];
    accept[4 * i + 1] = b64[v >> 12 & 63];
    accept[4 * i + 2] = b64[v >> 6 & 63];
    accept[4 * i + 3] = b64[v & 63];
  }
  accept[27] = '='; // 20 bytes: the last group holds 2.
  static char const head[]
      = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
  test_fake_transport_feed(fake, head, (int32_t)sizeof(head) - 1);
  test_fake_transport_feed(fake, accept, 28);
  test_fake_transport_feed(fake, "\r\n\r\n", 4);
  return true;
}

#endif // FUZZ_WEBSOCKET_H
