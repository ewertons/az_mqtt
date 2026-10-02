// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_native_errors.h
 * @brief Records native transport errors (az_mqtt_transport_error_fn) for tests.
 */
#ifndef TEST_NATIVE_ERRORS_H
#define TEST_NATIVE_ERRORS_H

#include <az_mqtt/az_mqtt_transport.h>

#include <string.h>

/** @brief The errors reported, in order; count may exceed the 16 kept. */
typedef struct
{
  az_mqtt_native_error errors[16];
  int count;
} test_native_errors;

static inline void test_native_errors_clear(test_native_errors* n) { memset(n, 0, sizeof(*n)); }

/** @brief An az_mqtt_transport_error_fn; @p context is a test_native_errors. */
static inline void test_native_errors_record(az_mqtt_native_error const* error, void* context)
{
  test_native_errors* const n = (test_native_errors*)context;
  if (n->count < (int)(sizeof(n->errors) / sizeof(n->errors[0])))
  {
    n->errors[n->count] = *error;
  }
  n->count++;
}

/** @brief How many of the kept errors have @p source. */
static inline int test_native_errors_with_source(
    test_native_errors const* n,
    az_mqtt_native_error_source source)
{
  int found = 0;
  for (int i = 0; i < n->count && i < 16; i++)
  {
    found += n->errors[i].source == source;
  }
  return found;
}

/** @brief The first kept error with @p result, or NULL. */
static inline az_mqtt_native_error const* test_native_errors_first_of(
    test_native_errors const* n,
    az_result result)
{
  for (int i = 0; i < n->count && i < 16; i++)
  {
    if (n->errors[i].result == result)
    {
      return &n->errors[i];
    }
  }
  return NULL;
}

/**
 * @brief Whether every kept error has @p connect_attempt and @p result, except refused addresses
 * (a host name may resolve to an address nothing listens on, e.g. ::1, before the one that works).
 */
static inline bool test_native_errors_belong_to(
    test_native_errors const* n,
    az_result result,
    uint32_t connect_attempt)
{
  for (int i = 0; i < n->count && i < 16; i++)
  {
    az_mqtt_native_error const* e = &n->errors[i];
    bool const refused_address = e->source == AZ_MQTT_NATIVE_ERROR_SOCKET
        && e->result == AZ_MQTT_ERROR_CONNECTION_REFUSED;
    if (e->connect_attempt != connect_attempt || (e->result != result && !refused_address))
    {
      return false;
    }
  }
  return true;
}

/** @brief Whether every kept error has @p result and @p connect_attempt. */
static inline bool test_native_errors_all_belong_to(
    test_native_errors const* n,
    az_result result,
    uint32_t connect_attempt)
{
  for (int i = 0; i < n->count && i < 16; i++)
  {
    if (n->errors[i].result != result || n->errors[i].connect_attempt != connect_attempt)
    {
      return false;
    }
  }
  return true;
}

#endif // TEST_NATIVE_ERRORS_H
