# Fuzz targets

libFuzzer targets (`LLVMFuzzerTestOneInput`) for the code that parses what the network sends, with
seed corpora. A fuzzing service (OneFuzz, `libfuzzer` jobs) runs them on a schedule, on Linux and
Windows; this repository runs no fuzzing campaigns. Pull requests replay the seeds and the
regression inputs as tests.

| Target | Seeds | Input | Covers |
|---|---|---|---|
| `az_mqtt3_fuzz_client`, `az_mqtt5_fuzz_client` | `client3`, `client5` | options byte, then the broker's bytes | framing, every packet decoder, in-flight state, resend on resume, callbacks; optionally over WebSockets |
| `az_mqtt_fuzz_transport` | `transport` | options byte, then the bytes received (optionally after a valid WebSocket upgrade reply) | HTTP CONNECT proxy reply, WebSocket upgrade reply and frames |

The options bits are documented at the top of each source file.

## Fuzzing build and drop folder

`AZ_MQTT_BUILD_FUZZERS=ON` builds the targets with libFuzzer: clang with ASan and UBSan, or MSVC
with ASan. It also builds the target `az_mqtt_fuzz_drop`, which fills `<build>/fuzz-drop` with what
the service is given:

- the target executables (Windows: also their PDBs and the ASan runtime DLL);
- `corpus/<seeds>/`: the seed corpora.

The service's job configuration is added to that folder by the pipeline that submits it. It is
not kept in this repository.

Linux:

```bash
cmake -S . -B build/fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DAZ_MQTT_BUILD_FUZZERS=ON \
  -DAZ_MQTT_BUILD_TESTS=OFF -DAZ_MQTT_BUILD_SAMPLES=OFF -DAZ_MQTT_TLS_BACKEND=none
cmake --build build/fuzz
build/fuzz/fuzz-drop/az_mqtt5_fuzz_client -max_total_time=60 -max_len=4096 \
  new-inputs/ build/fuzz/fuzz-drop/corpus/client5
```

Windows (Visual Studio 2022):

```powershell
cmake -S . -B build/fuzz -A x64 -DAZ_MQTT_BUILD_FUZZERS=ON -DAZ_MQTT_BUILD_TESTS=OFF -DAZ_MQTT_BUILD_SAMPLES=OFF
cmake --build build/fuzz --config RelWithDebInfo
build/fuzz/fuzz-drop/az_mqtt5_fuzz_client.exe -max_total_time=60 build/fuzz/fuzz-drop/corpus/client5
```

## Replay tests

With tests on (and fuzzers off), each target is also built as `<target>_replay`. That is the same
code with a plain `main()` (`replay_main.c`), which runs every file in `corpus/<seeds>/` and
`regressions/<seeds>/` once. ctest runs these with every compiler and in the sanitizer jobs.
A build without WebSockets or the proxy skips the inputs for that layer; with neither, it has no
`az_mqtt_fuzz_transport_replay`.

## Fixing an input the service reports

1. Reproduce it: `<target> <input>` (fuzzing build), or `<target>_replay <input>`.
2. Fix the code.
3. Add the input as `regressions/<seeds>/<short-description>` so that every build replays it.
