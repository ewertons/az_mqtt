// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifndef AZ_MQTT3_SAMPLE_CONFIG_H
#define AZ_MQTT3_SAMPLE_CONFIG_H

#include <az_mqtt3/az_mqtt3_client.h>

// Buffer sizes used by the sample.
#define SEND_BUFFER_SIZE 4096
#define RECV_BUFFER_SIZE 4096
#define MAX_USER_PROPERTIES 8
#define MAX_SUBACK_REASON_CODES 8
// Must be >= az_mqtt3_transport_sizeof() at runtime.
// Schannel transport on Windows is large due to TLS I/O buffers.
#define TRANSPORT_BUFFER_SIZE (256 * 1024)

// Reinterpret any fixed-size array as a byte array to use AZ_SPAN_FROM_BUFFER.
#define AZ_MQTT3_SPAN_FROM_ARRAY(ARRAY) \
  AZ_SPAN_FROM_BUFFER(*(uint8_t(*)[sizeof(ARRAY)])(ARRAY))

// Static buffers (zero allocation).
static uint8_t s_send_buffer[SEND_BUFFER_SIZE];
static uint8_t s_recv_buffer[RECV_BUFFER_SIZE];
static uint8_t s_transport_buffer[TRANSPORT_BUFFER_SIZE];

static az_mqtt3_user_property s_connack_user_props[MAX_USER_PROPERTIES];
static az_mqtt3_user_property s_publish_user_props[MAX_USER_PROPERTIES];
static az_mqtt3_user_property s_suback_user_props[MAX_USER_PROPERTIES];
static az_mqtt3_user_property s_ack_user_props[MAX_USER_PROPERTIES];
static az_mqtt3_user_property s_disconnect_user_props[MAX_USER_PROPERTIES];
static az_mqtt3_reason_code s_suback_reason_codes[MAX_SUBACK_REASON_CODES];
static int32_t s_publish_sub_ids[MAX_USER_PROPERTIES];

#endif // AZ_MQTT3_SAMPLE_CONFIG_H
