// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_test_version.h
 * @brief Protocol version a version-dependent test runs with.
 *
 * Such tests are built with AZ_MQTT_TEST_VERSION=3 or 5 and linked to
 * az_mqtt::mqtt3 / az_mqtt::mqtt5, or, with AZ_MQTT_TEST_MULTI, to az_mqtt::multi.
 */
#ifndef AZ_MQTT_TEST_VERSION_H
#define AZ_MQTT_TEST_VERSION_H

#if AZ_MQTT_TEST_VERSION == 5
#include <az_mqtt/az_mqtt5.h>
#define _AZ_MQTT_TEST_PROTOCOL AZ_MQTT5_PROTOCOL_VERSION
#elif AZ_MQTT_TEST_VERSION == 3
#include <az_mqtt/az_mqtt3.h>
#define _AZ_MQTT_TEST_PROTOCOL AZ_MQTT3_PROTOCOL_VERSION
#else
#error "Define AZ_MQTT_TEST_VERSION as 3 or 5."
#endif

/** @brief az_mqtt_client_options.protocol_version: required by az_mqtt::multi, 0 (library default) otherwise. */
#if defined(AZ_MQTT_TEST_MULTI)
#define AZ_MQTT_TEST_PROTOCOL_VERSION _AZ_MQTT_TEST_PROTOCOL
#else
#define AZ_MQTT_TEST_PROTOCOL_VERSION 0
#endif

#endif // AZ_MQTT_TEST_VERSION_H
