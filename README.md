# Azure MQTT C Clients

This repository contains two zero-allocation MQTT client libraries in C, sharing a similar architecture and API style.

## Client options

- [az_mqtt5](az_mqtt5/README.md)
  - MQTT 5.0 client
  - Includes MQTT 5 packet/property support and compliance notes
  - Sample: [az_mqtt5/samples/az_mqtt5_sample_connect.c](az_mqtt5/samples/az_mqtt5_sample_connect.c)

- [az_mqtt3](az_mqtt3/README.md)
  - MQTT 3.1.1-only client variant
  - Focused on MQTT 3.1.1 wire format and behavior
  - Sample: [az_mqtt3/samples/az_mqtt3_sample_connect.c](az_mqtt3/samples/az_mqtt3_sample_connect.c)

## Repository layout

- [az_mqtt5](az_mqtt5)
- [az_mqtt3](az_mqtt3)
- [LICENSE](LICENSE)
