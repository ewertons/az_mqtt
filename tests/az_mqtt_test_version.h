// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_test_version.h
 * @brief Selects the codec a version-dependent test runs with.
 *
 * Such tests are built once per enabled protocol version with
 * AZ_MQTT_TEST_VERSION=3 or 5, and link only that codec.
 */
#ifndef AZ_MQTT_TEST_VERSION_H
#define AZ_MQTT_TEST_VERSION_H

#if AZ_MQTT_TEST_VERSION == 5
#include <az_mqtt/az_mqtt5.h>
/** @brief Codec under test. */
#define AZ_MQTT_TEST_CODEC (&az_mqtt5_codec)
#elif AZ_MQTT_TEST_VERSION == 3
#include <az_mqtt/az_mqtt3.h>
/** @brief Codec under test. */
#define AZ_MQTT_TEST_CODEC (&az_mqtt3_codec)
#else
#error "Define AZ_MQTT_TEST_VERSION as 3 or 5."
#endif

#endif // AZ_MQTT_TEST_VERSION_H
