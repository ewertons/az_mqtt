// Copyright (c) az_mqtt5_client contributors. All rights reserved.
// SPDX-License-Identifier: MIT

#ifndef AZ_MQTT5_SAMPLE_CONFIG_H
#define AZ_MQTT5_SAMPLE_CONFIG_H

#include <az_mqtt5/az_mqtt5_client.h>

// Buffer sizes used by the sample.
#define SEND_BUFFER_SIZE 4096
#define RECV_BUFFER_SIZE 4096
#define MAX_USER_PROPERTIES 8
#define MAX_SUBACK_REASON_CODES 8

// Reinterpret any fixed-size array as a byte array to use AZ_SPAN_FROM_BUFFER.
#define AZ_MQTT5_SPAN_FROM_ARRAY(ARRAY) \
  AZ_SPAN_FROM_BUFFER(*(uint8_t(*)[sizeof(ARRAY)])(ARRAY))

// Static buffers (zero allocation).
static uint8_t s_send_buffer[SEND_BUFFER_SIZE];
static uint8_t s_recv_buffer[RECV_BUFFER_SIZE];
static uint8_t s_transport_buffer[256]; // Sized >= az_mqtt5_transport_sizeof()

static az_mqtt5_user_property s_connack_user_props[MAX_USER_PROPERTIES];
static az_mqtt5_user_property s_publish_user_props[MAX_USER_PROPERTIES];
static az_mqtt5_user_property s_suback_user_props[MAX_USER_PROPERTIES];
static az_mqtt5_user_property s_ack_user_props[MAX_USER_PROPERTIES];
static az_mqtt5_user_property s_disconnect_user_props[MAX_USER_PROPERTIES];
static az_mqtt5_reason_code s_suback_reason_codes[MAX_SUBACK_REASON_CODES];
static int32_t s_publish_sub_ids[MAX_USER_PROPERTIES];

#endif // AZ_MQTT5_SAMPLE_CONFIG_H
