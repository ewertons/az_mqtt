# az_mqtt

A zero-allocation MQTT client library in C for embedded and constrained environments, built on
the [Azure SDK for C](https://github.com/Azure/azure-sdk-for-c) span and platform abstractions.
It speaks MQTT 3.1.1 (mqttv3) and MQTT 5.0 (mqttv5) through one client API.

## Layers

- **Codecs** — pure encode/decode of every packet type on caller-supplied buffers. One per
  protocol version: `az_mqtt3_codec` and `az_mqtt5_codec`.
- **Client** — connection management, keep-alive, QoS 0/1/2 ACK dispatch and an event loop.
  Version-neutral; it drives whichever codec it is given.
- **Transport** — TCP/TLS: OpenSSL or mbedTLS (POSIX), Schannel (Windows), or plain TCP.

## Choosing the MQTT version

Set the codec when initializing the client:

```c
#include <az_mqtt/az_mqtt_client.h>
#include <az_mqtt/az_mqtt5.h>   /* or az_mqtt3.h */

az_mqtt_client_options options = { 0 };
options.transport = transport;
options.codec = &az_mqtt5_codec; /* or &az_mqtt3_codec */
/* ... buffers, callbacks, connect options ... */
az_result rc = az_mqtt_client_init(&client, &options);
```

With mqttv3, mqttv5-only fields (properties, user properties, `buffers`) are ignored and may be
left zeroed.

Link the target for the version(s) used:

| CMake target | Contents |
|--------------|----------|
| `az_mqtt::core` | Client, transport, shared codec primitives. References neither codec. |
| `az_mqtt::mqtt3` | mqttv3 codec (links `az_mqtt::core`) |
| `az_mqtt::mqtt5` | mqttv5 codec (links `az_mqtt::core`) |

An application that links only one version carries no code from the other; CI checks this on
every build. Linking both is supported (one client per connection, each with its own codec).

### CMake options

| Option | Default | |
|--------|---------|-|
| `AZ_MQTT_ENABLE_MQTT3` | `ON` | Build `az_mqtt::mqtt3` |
| `AZ_MQTT_ENABLE_MQTT5` | `ON` | Build `az_mqtt::mqtt5` |
| `AZ_MQTT_TLS_BACKEND` | `auto` | `auto`, `openssl`, `mbedtls` or `none` |
| `AZ_MQTT_BUILD_SAMPLES` | `ON` | |
| `AZ_MQTT_BUILD_TESTS` | `ON` | |
| `AZ_MQTT_WARNINGS_AS_ERRORS` | `OFF` | |

### Migrating from `az_mqtt5` / `az_mqtt3`

- Include `az_mqtt/...` instead of `az_mqtt5/...` / `az_mqtt3/...`.
- Rename `az_mqtt5_*` / `az_mqtt3_*` (and `AZ_MQTT5_*` / `AZ_MQTT3_*`) to `az_mqtt_*` /
  `AZ_MQTT_*`. Codec functions (`az_mqttN_codec_*`), `AZ_MQTTN_PROTOCOL_VERSION` and
  `AZ_MQTT3_CONNACK_*` keep their names.
- Set `az_mqtt_client_options.codec` (required).
- CMake: link `az_mqtt::mqtt5` / `az_mqtt::mqtt3`; options are now `AZ_MQTT_*`.
- mqttv3: `az_mqtt3_codec_decode_ack` rejects bytes after the packet identifier; AUTH
  encode/decode no longer exist (MQTT 5.0 only).

## Getting started

See [samples/README.md](samples/README.md): start a local broker, build, and run the connect
samples ([mqttv5](samples/az_mqtt5_sample_connect.c), [mqttv3](samples/az_mqtt3_sample_connect.c)).

## Documentation

| Document | Description |
|----------|-------------|
| [doc/eng/mqtt_v5_spec_compliance.md](doc/eng/mqtt_v5_spec_compliance.md) | MQTT 5.0 compliance matrix |
| [doc/eng/mqtt_v311_spec_compliance.md](doc/eng/mqtt_v311_spec_compliance.md) | MQTT 3.1.1 compliance matrix |
| [doc/eng/mqtt_v3_vs_v5_api.md](doc/eng/mqtt_v3_vs_v5_api.md) | Design decision: one client, per-version codecs |
| [doc/api_design_decisions.md](doc/api_design_decisions.md) | Per-call vs per-session buffer design |
| [doc/thread_safety.md](doc/thread_safety.md) | Threading model |
| [doc/memory_footprint.md](doc/memory_footprint.md) | Memory footprint |

## CI and local validation

[.github/workflows/ci.yml](.github/workflows/ci.yml) runs on every pull request and on `main`.
Warnings in our code are errors in every job.

| Job | What it covers |
|-----|----------------|
| Linux | {OpenSSL, mbedTLS 3.6.7 / 4.1.1 / 4.2.0, no TLS} × {gcc, clang}: build, link-isolation check, all tests (mqttv3 and mqttv5), including e2e against a local Mosquitto (plain and TLS) |
| Single version | Builds and tests with only mqttv3 or only mqttv5 enabled |
| Sanitizers | ASan + UBSan (+ leak check) over all tests |
| Hardened | Release build with `_FORTIFY_SOURCE=3`, stack protector, CET, full RELRO and PIE, verified on every executable, then all tests |
| Windows | MSVC `/W4 /WX`, Schannel, all tests |

The Linux steps are scripts, so the same run works locally:

```bash
eng/ci/start-broker.sh                                   # Mosquitto on 1883 (plain) and 8883 (TLS)
eng/ci/install-mbedtls.sh 3.6.7 "$PWD/build/mbedtls"     # only for the mbedTLS backend
MBEDTLS_PREFIX="$PWD/build/mbedtls" eng/ci/build-and-test.sh mbedtls   # [debug|asan|hardened]
```

## License

MIT — see [LICENSE](LICENSE).
