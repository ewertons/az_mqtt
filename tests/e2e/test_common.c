// Copyright (c) az_mqtt5_client contributors. All rights reserved.
// SPDX-License-Identifier: MIT

#include "test_common.h"

#include <string.h>

void az_mqtt5_e2e_fixture_reset(az_mqtt5_e2e_fixture* fixture)
{
  memset(fixture, 0, sizeof(*fixture));
}

az_result az_mqtt5_e2e_init_client(
    az_mqtt5_e2e_fixture* fixture,
    az_mqtt5_client* client,
    az_mqtt5_e2e_client_params const* params)
{
  if (az_mqtt5_transport_sizeof() > E2E_TRANSPORT_BUF_SIZE)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }

  az_mqtt5_transport* transport = (az_mqtt5_transport*)fixture->transport_buf;
  az_result rc = az_mqtt5_transport_init(transport);
  if (az_result_failed(rc))
  {
    return rc;
  }

  az_mqtt5_connect_options connect_opts = az_mqtt5_connect_options_default();
  connect_opts.client_id = params->client_id;
  connect_opts.keep_alive_seconds = params->keep_alive_seconds;
  connect_opts.clean_start = params->clean_start;

  az_mqtt5_client_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.transport = transport;
  opts.send_buffer = AZ_SPAN_FROM_BUFFER(fixture->send_buf);
  opts.receive_buffer = AZ_SPAN_FROM_BUFFER(fixture->recv_buf);
  opts.connect_options = connect_opts;
  opts.hostname = params->hostname;
  opts.port = params->port;
  opts.tls_options = params->tls_options;

  opts.on_connack = params->on_connack;
  opts.on_publish = params->on_publish;
  opts.on_suback = params->on_suback;
  opts.on_unsuback = params->on_unsuback;
  opts.on_puback = params->on_puback;
  opts.on_pubcomp = params->on_pubcomp;
  opts.on_disconnect = params->on_disconnect;

  opts.connack_user_properties = fixture->connack_props;
  opts.connack_user_property_capacity = E2E_MAX_USER_PROPS;
  opts.publish_user_properties = fixture->publish_props;
  opts.publish_user_property_capacity = E2E_MAX_USER_PROPS;
  opts.publish_subscription_identifiers = fixture->publish_subscription_ids;
  opts.publish_subscription_identifier_capacity = E2E_MAX_USER_PROPS;
  opts.suback_reason_codes = fixture->suback_reasons;
  opts.suback_reason_code_capacity = E2E_MAX_REASON_CODES;
  opts.suback_user_properties = fixture->suback_props;
  opts.suback_user_property_capacity = E2E_MAX_USER_PROPS;
  opts.ack_user_properties = fixture->ack_props;
  opts.ack_user_property_capacity = E2E_MAX_USER_PROPS;
  opts.disconnect_user_properties = fixture->disconnect_props;
  opts.disconnect_user_property_capacity = E2E_MAX_USER_PROPS;

  return az_mqtt5_client_init(client, &opts);
}

az_result az_mqtt5_e2e_wait_until(
    az_mqtt5_client* client,
    int32_t max_iterations,
    int32_t process_loop_timeout_ms,
    bool (*condition)(void))
{
  for (int32_t i = 0; i < max_iterations; ++i)
  {
    if (condition())
    {
      return AZ_OK;
    }

    az_result rc = az_mqtt5_client_process_loop(client, process_loop_timeout_ms);
    if (az_result_failed(rc))
    {
      return rc;
    }
  }

  return condition() ? AZ_OK : AZ_MQTT5_ERROR_TIMEOUT;
}

void az_mqtt5_e2e_copy_span_to_cstr(az_span src, char* dest, size_t capacity)
{
  int32_t src_len = az_span_size(src);
  size_t to_copy = (src_len < (int32_t)(capacity - 1)) ? (size_t)src_len : (capacity - 1);
  if (to_copy > 0)
  {
    memcpy(dest, az_span_ptr(src), to_copy);
  }
  dest[to_copy] = '\0';
}
