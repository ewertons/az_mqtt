# Fuzz targets

libFuzzer targets (`LLVMFuzzerTestOneInput`) for the code that parses what the network sends, with
seed corpora. They are plain libFuzzer targets for a fuzzing service, such as a OneFuzz
`libfuzzer` job: the target binary, plus `corpus/<name>` as the inputs. This repository does
not run fuzzing campaigns.

| Target | Seed corpus | Input | Covers |
|---|---|---|---|
| `az_mqtt3_fuzz_client`, `az_mqtt5_fuzz_client` | `client3`, `client5` | options byte, then the broker's bytes | framing, every packet decoder, in-flight state, resend on resume, callbacks; optionally over WebSockets |
| `az_mqtt_fuzz_transport` | `transport` | options byte, then the bytes received | HTTP CONNECT proxy reply, WebSocket upgrade reply and frames |

The options bits are documented at the top of each source file.

## Build and run locally

Requires clang with libFuzzer. Everything is built with ASan and UBSan.

```bash
cmake -S . -B build/fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DAZ_MQTT_BUILD_FUZZERS=ON \
  -DAZ_MQTT_BUILD_TESTS=OFF -DAZ_MQTT_BUILD_SAMPLES=OFF -DAZ_MQTT_TLS_BACKEND=none
cmake --build build/fuzz
build/fuzz/tests/fuzz/az_mqtt5_fuzz_client -max_total_time=60 -max_len=4096 \
  new-inputs/ tests/fuzz/corpus/client5
```
