# Fuzzing

libFuzzer targets for the parsers of what the network sends. Each runs with ASan and UBSan.

| Target | Input | Covers |
|---|---|---|
| `az_mqtt3_fuzz_client`, `az_mqtt5_fuzz_client` | options byte, then the broker's bytes | framing, every packet decoder, in-flight state, resend on resume, callbacks; optionally over WebSockets |
| `az_mqtt_fuzz_transport` | options byte, then the bytes received | HTTP CONNECT proxy reply, WebSocket upgrade reply and frames |

The options bits are documented at the top of each source file. `corpus/` holds the seed inputs.

```bash
CC=clang FUZZ_SECONDS=600 eng/ci/fuzz.sh
```

This needs clang with libFuzzer (on Debian/Ubuntu: `clang` and `libclang-rt-<version>-dev`). A
crash leaves its input in `build/fuzz/artifacts/`. To reproduce it, run the target on that file:
`build/fuzz/tests/fuzz/<target> <file>`.
