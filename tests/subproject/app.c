// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <az_mqtt3/az_mqtt3_client.h>
#include <az_mqtt5/az_mqtt5_client.h>

int main(void) { return az_mqtt_transport_sizeof() > 0 ? 0 : 1; }
