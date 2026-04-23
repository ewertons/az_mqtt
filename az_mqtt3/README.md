# az_mqtt3

A zero-allocation MQTT 3.1.1 client library for embedded and constrained C environments,
built on top of the [Azure SDK for C](https://github.com/Azure/azure-sdk-for-c) span and
platform abstractions.

## What it is

- **Codec layer** — MQTT 3.1.1 wire format encode/decode for CONNECT, CONNACK, PUBLISH, PUBACK/PUBREC/PUBREL/PUBCOMP, SUBSCRIBE/SUBACK, UNSUBSCRIBE/UNSUBACK, PINGREQ/PINGRESP, DISCONNECT
- **Client layer** — connection management, keep-alive, QoS 0/1/2 ACK dispatch, and a simple event loop
- **Transport layer** — pluggable TCP/TLS backend (Schannel on Windows, OpenSSL on Linux/macOS)

## Getting started

See **[samples/README.md](samples/README.md)** for a step-by-step walkthrough:
start a local broker, build, and run the connect sample.

The sample source is in [samples/az_mqtt3_sample_connect.c](samples/az_mqtt3_sample_connect.c).

## Documentation

| Document | Description |
|----------|-------------|
| [doc/eng/mqtt_v3_vs_v5_api.md](doc/eng/mqtt_v3_vs_v5_api.md) | Design decision: MQTT v3.1.1 vs v5 API |
| [doc/api_design_decisions.md](doc/api_design_decisions.md) | Per-call vs per-session buffer design |

## License

MIT — see [LICENSE](LICENSE).
