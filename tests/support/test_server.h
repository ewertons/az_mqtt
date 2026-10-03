// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_server.h
 * @brief In-process TCP/TLS peer for transport tests (POSIX + OpenSSL, test-only).
 *
 * Mints its own CA and server certificate per instance, so certificate
 * validation can be exercised without a broker. Answers an MQTT CONNECT with a
 * success CONNACK of the protocol level the client asked for.
 */
#ifndef AZ_MQTT_TEST_SERVER_H
#define AZ_MQTT_TEST_SERVER_H

#include <stdbool.h>
#include <stdint.h>

/** @brief What the server does once a client connects. */
typedef enum
{
  /** @brief Complete TLS (if enabled), answer CONNECT with CONNACK, then idle. */
  TEST_SERVER_MQTT = 0,
  /** @brief Accept TCP and never read or write (a peer that stops answering). */
  TEST_SERVER_SILENT,
  /** @brief Answer CONNECT, then close the connection. */
  TEST_SERVER_CLOSE_AFTER_CONNACK,
  /** @brief Answer CONNECT, then write only the start of a TLS record (TLS only). */
  TEST_SERVER_PARTIAL_TLS_RECORD,
  /** @brief Answer CONNECT, then never read again (the client's sends back up). */
  TEST_SERVER_STOP_READING,
  /** @brief Answer CONNECT, then send DISCONNECT (0x8B) and close (MQTT 5; 3.1.1 just closes). */
  TEST_SERVER_DISCONNECT_AFTER_CONNACK,
} test_server_behavior;

/** @brief How a WebSocket server answers the upgrade request. */
typedef enum
{
  /** @brief 101 with every header right. */
  TEST_SERVER_WS_ACCEPT = 0,
  /** @brief 403, then close. */
  TEST_SERVER_WS_REFUSE,
  /** @brief 101 with a wrong Sec-WebSocket-Accept. */
  TEST_SERVER_WS_BAD_ACCEPT,
  /** @brief 101 with 2 KB of extra headers, written a few bytes at a time, then a ping. */
  TEST_SERVER_WS_SLOW_LONG_REPLY,
} test_server_ws_reply;

typedef struct
{
  bool tls;
  /** @brief MQTT over WebSockets: an upgrade first, then every packet in binary frames. */
  bool websocket;
  test_server_ws_reply ws_reply;
  /** @brief Send each packet one byte per frame (fragmented), each frame after a ping "hi". */
  bool ws_fragment;
  /** @brief After CONNACK: send a close frame with this code (0: none) and await the echo. */
  uint16_t ws_close_code;
  /** @brief After CONNACK: send a masked frame (servers must not). */
  bool ws_masked_frame;
  /** @brief subjectAltName of the server certificate, e.g. "DNS:localhost,IP:127.0.0.1". */
  char const* san;
  /** @brief Sign the server certificate with a CA the client is not given. */
  bool untrusted_ca;
  /** @brief Server certificate validity ended before the test ran. */
  bool expired;
  /** @brief Require and verify a client certificate issued by the test CA. */
  bool require_client_cert;
  test_server_behavior behavior;
  /** @brief Never answer PINGREQ. */
  bool no_pingresp;
  /** @brief Server Keep Alive to put in an MQTT 5 CONNACK (see server_keep_alive_present). */
  uint16_t server_keep_alive;
  /** @brief Send server_keep_alive even when 0; otherwise 0 means "no property". */
  bool server_keep_alive_present;
  /** @brief QoS 0 PUBLISHes written in one burst right after the CONNACK. */
  int burst_publishes;
  /** @brief CONNACK reason (MQTT 5) or return code (MQTT 3.1.1); 0 accepts. */
  uint8_t connack_code;
  /** @brief Reason codes per SUBACK; 0 answers SUBSCRIBE with one granted QoS 0. */
  int suback_codes;
  /** @brief MQTT 5: after CONNACK, send one PUBLISH with this many user properties and subscription identifiers (max 12). */
  int publish_properties;
  /** @brief After CONNACK, send an empty AUTH packet (valid in MQTT 5, reserved type in 3.1.1). */
  bool send_auth;
  /** @brief After CONNACK, send an empty DISCONNECT (valid in MQTT 5, client-only in 3.1.1). */
  bool send_empty_disconnect;
  /** @brief TLS: wait this long after accepting before answering the ClientHello. */
  int handshake_delay_ms;
  /** @brief MQTT 5 CONNACK Receive Maximum; 0: absent. */
  uint16_t receive_maximum;
  /** @brief MQTT 5 CONNACK Maximum QoS (0 or 1), sent when maximum_qos_present. */
  uint8_t maximum_qos;
  bool maximum_qos_present;
  /** @brief MQTT 5 CONNACK Retain Available = 0. */
  bool retain_unavailable;
  /** @brief MQTT 5 CONNACK Maximum Packet Size; 0: absent. */
  uint32_t maximum_packet_size;
  /** @brief Answer PUBLISH QoS 1 with PUBACK, QoS 2 with PUBREC, and PUBREL with PUBCOMP. */
  bool ack_publishes;
  /** @brief With ack_publishes: never acknowledge the first PUBLISH. */
  bool hold_first_publish;
  /** @brief MQTT 5: reason code in the PUBRECs sent (0: success). */
  uint8_t pubrec_reason;
  /**
   * @brief After CONNACK: QoS 2 PUBLISH with packet id 7, its DUP resend, PUBREL 7, a new
   * QoS 2 PUBLISH with packet id 7, PUBREL 7.
   */
  bool send_qos2_sequence;
  /** @brief After CONNACK: PUBACK, PUBCOMP, PUBREC, PUBREL, SUBACK, UNSUBACK for unused packet ids. */
  bool send_unknown_acks;
  /** @brief Close the connection right after CONNACK (TLS: without close_notify). */
  bool close_after_connack;
} test_server_options;

typedef struct test_server test_server;

/** @brief Defaults: TLS on, SAN "DNS:localhost,IP:127.0.0.1", MQTT behavior. */
test_server_options test_server_options_default(void);

/**
 * @brief Start a server on 127.0.0.1 with an ephemeral port.
 * @return NULL on failure.
 */
test_server* test_server_start(test_server_options const* options);

void test_server_stop(test_server* server);

uint16_t test_server_port(test_server const* server);

/** @brief PEM file with the CA the client should trust (always the good CA). */
char const* test_server_ca_path(test_server const* server);

/** @brief PEM files of a client identity issued by the trusted CA. */
char const* test_server_client_cert_path(test_server const* server);
char const* test_server_client_key_path(test_server const* server);

/** @brief TCP connections accepted so far. */
int test_server_accepted(test_server* server);

/** @brief TLS handshakes completed so far (server side). */
int test_server_handshakes(test_server* server);

/** @brief Whether the last completed handshake carried a client certificate. */
bool test_server_saw_client_cert(test_server* server);

/** @brief PINGREQs received so far. */
int test_server_pingreqs(test_server* server);

/** @brief PUBLISH packets received. */
int test_server_publishes(test_server* server);

/** @brief PUBREL packets received, and the reason code of the last (0 if absent). */
int test_server_pubrels(test_server* server);
int test_server_last_pubrel_reason(test_server* server);

/** @brief PUBCOMP packets received, and the reason code of the last (0 if absent). */
int test_server_pubcomps(test_server* server);
int test_server_last_pubcomp_reason(test_server* server);

/** @brief Pongs "hi" received (answers to ws_fragment pings). */
int test_server_ws_pongs(test_server* server);
/** @brief Client frames without a mask (clients must mask every frame). */
int test_server_ws_unmasked(test_server* server);
/** @brief Status code of the last close frame from the client: 1005 if none in it, -1 if none. */
int test_server_ws_client_close_code(test_server* server);
/** @brief Whether the last upgrade request contains @p text. */
bool test_server_ws_request_has(test_server* server, char const* text);
/** @brief TLS connections the client ended with close_notify. */
int test_server_close_notifies(test_server* server);
/** @brief Whether the client closed the last connection (orderly or not). */
bool test_server_client_closed(test_server* server);

#endif // AZ_MQTT_TEST_SERVER_H
