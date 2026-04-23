// Copyright (c) az_mqtt5_client contributors. All rights reserved.
// SPDX-License-Identifier: MIT

#ifndef AZ_MQTT5_E2E_TEST_COMMON_H
#define AZ_MQTT5_E2E_TEST_COMMON_H

#include <az_mqtt5/az_mqtt5_client.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define E2E_SEND_BUF_SIZE 2048
#define E2E_RECV_BUF_SIZE 2048
#define E2E_MAX_USER_PROPS 4
#define E2E_MAX_REASON_CODES 4
#define E2E_TRANSPORT_BUF_SIZE (256 * 1024)

typedef struct
{
  uint8_t send_buf[E2E_SEND_BUF_SIZE];
  uint8_t recv_buf[E2E_RECV_BUF_SIZE];
  uint8_t transport_buf[E2E_TRANSPORT_BUF_SIZE];

  az_mqtt5_user_property connack_props[E2E_MAX_USER_PROPS];
  az_mqtt5_user_property publish_props[E2E_MAX_USER_PROPS];
  az_mqtt5_user_property suback_props[E2E_MAX_USER_PROPS];
  az_mqtt5_user_property ack_props[E2E_MAX_USER_PROPS];
  az_mqtt5_user_property disconnect_props[E2E_MAX_USER_PROPS];
  az_mqtt5_reason_code suback_reasons[E2E_MAX_REASON_CODES];
  int32_t publish_subscription_ids[E2E_MAX_USER_PROPS];
} az_mqtt5_e2e_fixture;

typedef struct
{
  az_span client_id;
  az_span hostname;
  uint16_t port;
  uint16_t keep_alive_seconds;
  bool clean_start;
  az_mqtt5_tls_options const* tls_options;

  az_mqtt5_on_connack_fn on_connack;
  az_mqtt5_on_publish_received_fn on_publish;
  az_mqtt5_on_suback_fn on_suback;
  az_mqtt5_on_unsuback_fn on_unsuback;
  az_mqtt5_on_puback_fn on_puback;
  az_mqtt5_on_pubcomp_fn on_pubcomp;
  az_mqtt5_on_disconnect_fn on_disconnect;
} az_mqtt5_e2e_client_params;

void az_mqtt5_e2e_fixture_reset(az_mqtt5_e2e_fixture* fixture);

az_result az_mqtt5_e2e_init_client(
    az_mqtt5_e2e_fixture* fixture,
    az_mqtt5_client* client,
    az_mqtt5_e2e_client_params const* params);

az_result az_mqtt5_e2e_wait_until(
    az_mqtt5_client* client,
    int32_t max_iterations,
    int32_t process_loop_timeout_ms,
    bool (*condition)(void));

void az_mqtt5_e2e_copy_span_to_cstr(az_span src, char* dest, size_t capacity);

#endif // AZ_MQTT5_E2E_TEST_COMMON_H
