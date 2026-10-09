# az_mqtt on ESP-IDF (ESP32)

Connects to a broker over TLS (or TCP), publishes one QoS 1 message, waits for its PUBACK and
disconnects. MQTT 3.1.1 or 5.0. One task owns the client and drives it with `process_loop`.

## Using az_mqtt in an ESP-IDF project

The repository root is an ESP-IDF component (`CMakeLists.txt` → [port/esp-idf](../../port/esp-idf/component.cmake),
[Kconfig](../../Kconfig), [idf_component.yml](../../idf_component.yml)). Clone it with
`--recurse-submodules` into a directory named `az_mqtt` (ESP-IDF names a component after its
directory), add it to `EXTRA_COMPONENT_DIRS`, and `REQUIRES az_mqtt`.

Options (`idf.py menuconfig` → *az_mqtt*):

| Kconfig | Default | |
|---|---|---|
| `AZ_MQTT_ENABLE_MQTTV3` / `AZ_MQTT_ENABLE_MQTTV5` | y / y | client libraries |
| `AZ_MQTT_ENABLE_PROXY` / `AZ_MQTT_ENABLE_WEBSOCKETS` | y / y | as the CMake options |
| `AZ_MQTT_TLS_BACKEND` | mbedTLS | mbedTLS (ESP-IDF's) or none |
| `AZ_MQTT_LOGGING` | y | n: `AZ_NO_LOGGING` |
| `AZ_MQTT_PRECONDITIONS` | y | n: `AZ_NO_PRECONDITION_CHECKING` |
| `AZ_MQTT_BUILD_AZ_CORE` | y | n when another component already compiles azure-sdk-for-c `az_span.c`, `az_log.c`, `az_precondition.c`, `az_base64.c` |

The platform layer is the POSIX one, over lwIP sockets; time is `clock_gettime(CLOCK_MONOTONIC)`
and randomness `getrandom()` (`esp_fill_random()`). TLS trust comes from `ca_cert_pem`, or from
the `configure` hook, e.g. `esp_crt_bundle_attach()` (what this sample does with no CA file).
The transport is caller storage of `az_mqtt_transport_sizeof()` bytes; keep it static, not on the
task stack.

## Build and run

ESP-IDF v6.0 or later (mbedTLS 4); v5.1+ (mbedTLS 3) should work but is not tested in CI.

```bash
idf.py set-target esp32
idf.py menuconfig    # az_mqtt sample: broker, port, TLS, CA file, MQTT version; Example Connection Configuration: Wi-Fi
idf.py build flash monitor
```

`AZ_MQTT_SAMPLE_CA_PEM_FILE` is a PEM file relative to this directory (git-ignored). Empty: the
ESP-IDF certificate bundle.

## CI and QEMU

[eng/ci/esp-idf.sh](../../eng/ci/esp-idf.sh) `build` builds each [ci/sdkconfig.ci.*](ci) variant;
`qemu` runs the sample in QEMU (OpenCores Ethernet, [ci/sdkconfig.qemu](ci/sdkconfig.qemu)) against
a local Mosquitto: mqttv3 and mqttv5 over TLS, mqttv3 over TCP, and a server whose CA is not
trusted (must fail with `AZ_MQTT_ERROR_TLS_VERIFY`).
