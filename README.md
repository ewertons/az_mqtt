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

## CI and local validation

[.github/workflows/ci.yml](.github/workflows/ci.yml) runs on every pull request and on `main`.
Warnings in our code are errors in every job.

| Job | What it covers |
|-----|----------------|
| Linux | {az_mqtt5, az_mqtt3} × {OpenSSL, mbedTLS, no TLS} × {gcc, clang}: build and all tests, including e2e against a local Mosquitto (plain and TLS) |
| Sanitizers | ASan + UBSan (+ leak check) over all tests |
| Hardened | Release build with `_FORTIFY_SOURCE=3`, stack protector, CET, full RELRO and PIE, verified on every executable, then all tests |
| Windows | MSVC `/W4 /WX`, Schannel, all tests |

The Linux steps are scripts, so the same run works locally:

```bash
eng/ci/start-broker.sh                                   # Mosquitto on 1883 (plain) and 8883 (TLS)
eng/ci/install-mbedtls.sh 3.6.7 "$PWD/build/mbedtls"     # only for the mbedTLS backend
MBEDTLS_PREFIX="$PWD/build/mbedtls" eng/ci/build-and-test.sh az_mqtt5 mbedtls   # [debug|asan|hardened]
```
