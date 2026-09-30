# MQTT Client Comparison Report

_Generated: 2026-04-23 00:07:05 -07:00_

This report compares **four** MQTT 5 client configurations running an identical
publish/subscribe workload against the same broker, inside equivalent Linux
containers. Each binary is built with production / size-optimized release flags.

Two of the four are the **same C source** (`az_mqtt5`) but linked against
different TLS backends — OpenSSL vs mbedTLS — so the footprint delta between
them isolates the TLS wrapper cost.

## Clients Under Test

| Client | Language | Version | TLS backend | Notes |
|---|---|---|---|---|
| `az_mqtt5` (openssl) | C99 | 1.6.0-beta.1 (azure-sdk-for-c submodule) | OpenSSL (`libssl3`, dynamic) | `src/platform/transport_posix.c`. |
| `az_mqtt5` (mbedtls) | C99 | 1.6.0-beta.1 (azure-sdk-for-c submodule) | mbedTLS (dynamic) | `src/platform/transport_mbedtls.c`. Same MQTT core. |
| `paho_mqtt_c` | C | v1.3.14 (Eclipse Paho MQTT C, fetched via CMake FetchContent) | OpenSSL | Async API (MQTTAsync), internal threads, heap-allocated internals. |
| `azure_mqtt` | Rust | azure_mqtt v0.1.0 (local path dep in perf_azure_mqtt/azure_mqtt_src/) | OpenSSL (vendored, static) | Tokio async runtime. |

## Test Environment

| | |
|---|---|
| Host OS | Microsoft Windows NT 10.0.26200.0 / PowerShell 7.6.0 |
| Docker | Docker version 29.4.0, build 9d7ad9f |
| Container base | Ubuntu 24.04.4 LTS |
| C compiler | gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 |
| Broker | EMQX, TCP 1883, no TLS, no auth |
| Network | Docker user-defined bridge (`client-comp_default`) |

> **Note on runtime TLS.** All runs connect to the broker over plain TCP (port
> 1883). TLS code is **linked in but not exercised** at runtime. Both C
> `az_mqtt5` variants **dynamically** link their TLS library (`libssl3` /
> `libmbedtls` come from the runtime container), so the static binary size
> reflects only the wrapper transport code — not the TLS library proper.
> CPU / RSS / heap numbers are pure TCP.

## Workload

Each client, in its own container:

1. Connects to the broker (TCP, MQTT 5).
2. Subscribes to `perf/<client>/#` at QoS 1.
3. Publishes the target number of messages at QoS 1 (so each message loops back).
4. Drains remaining inbound + PUBACKs (3 s idle / 120 s hard cap).
5. Disconnects cleanly and emits a JSON report.

| Parameter | Value |
|---|---|
| Messages per run | 10000 |
| Payload size | 128 bytes |
| QoS | 1 |
| Duration cap | 30 s publish window + up to 120 s drain |
| Clean session | yes |
| Keep-alive | 60 s |

## Production Build Flags

### C clients

| Flag | Purpose |
|---|---|
| `-Os` | Optimize for size. |
| `-ffunction-sections` / `-fdata-sections` | One ELF section per symbol. |
| `-fno-unwind-tables` / `-fno-asynchronous-unwind-tables` | Omit `.eh_frame[_hdr]`. |
| `-Wl,--gc-sections` | Drop unreferenced sections at link time. |
| `-Wl,-s` | Strip symbol/relocation tables. |

The two `az_mqtt5` variants differ only by `-DAZ_MQTT_TLS_BACKEND=openssl` vs
`-DAZ_MQTT_TLS_BACKEND=mbedtls` passed at CMake configure time, which selects
`src/platform/transport_posix.c` vs `src/platform/transport_mbedtls.c` and
links the matching TLS libraries.

### Rust client

| Setting | Purpose |
|---|---|
| `opt-level = "z"` | Aggressive size optimization. |
| `lto = true` | Fat LTO across all crates. |
| `codegen-units = 1` | Max inlining / DCE. |
| `strip = "symbols"` | Strip symbols. |
| `panic = "abort"` | Drop the unwinding runtime. |

## Results

### Throughput & CPU

| Metric | az_mqtt5 (openssl) | az_mqtt5 (mbedtls) | paho_mqtt (C) | azure_mqtt (Rust) |
|---|---:|---:|---:|---:|
| Messages sent | 10000 | 10000 | 10000 | 10000 |
| Messages received | 1241 | 1244 | 1170 | 1139 |
| PUBACKs received | 10000 | 10000 | 10000 | 10000 |
| Elapsed (s) | 4.691 | 4.708 | 4.275 | 4.304 |
| Send rate (msg/s) | 2131.7 | 2124.3 | 2339.3 | 2323.3 |
| Recv rate (msg/s) | 264.5 | 264.3 | 273.7 | 264.6 |
| User CPU (s) | 0.031 | 0.081 | 0.809 | 0.96 |
| System CPU (s) | 0.4 | 0.337 | 0.669 | 0.81 |
| Total CPU (s) | 0.431 | 0.418 | 1.478 | 1.77 |

### Runtime Memory

| Metric | az_mqtt5 (openssl) | az_mqtt5 (mbedtls) | paho_mqtt (C) | azure_mqtt (Rust) |
|---|---:|---:|---:|---:|
| RSS baseline (before MQTT) | 3.2 MB | 2.0 MB | 1.5 MB | 4.1 MB |
| RSS peak (VmHWM) | 12.4 MB | 12.3 MB | 12.4 MB | 10.6 MB |
| RSS delta (client cost) | 9.1 MB | 10.3 MB | 10.9 MB | 6.5 MB |
| Heap baseline (`mallinfo2`) | 2.1 KB | 2.1 KB | 2.1 KB | n/a |
| Heap peak (`mallinfo2`) | 6.0 KB | 6.0 KB | 141.2 KB | n/a |
| Heap delta (client allocs) | 3.9 KB | 3.9 KB | 139.1 KB | n/a |

**Notes:**

- **RSS baseline** is sampled after process start, argv parsing, and payload
  buffer init — but **before** any MQTT client, socket, or async runtime is
  touched.
- **RSS peak** is `VmHWM` (`ru_maxrss`) at end of run.
- **RSS delta** (peak − baseline) is the closest single-number approximation
  to *the memory the client library actually caused this process to consume*.
- **Heap** numbers use `mallinfo2().uordblks` (glibc ptmalloc in-use bytes).
  `az_mqtt5` never calls `malloc` itself; any non-zero value comes from
  libc internals, stdio, or `getaddrinfo`. Not tracked for Rust.

### Static Binary Footprint

Sizes of the stripped, size-optimized Release ELF binaries, measured with
`stat` (on-disk size) and `size` (text/data/bss sections) inside each image.

| Metric | az_mqtt5 (openssl) | az_mqtt5 (mbedtls) | paho_mqtt (C) | azure_mqtt (Rust) |
|---|---:|---:|---:|---:|
| Binary on disk | 58.1 KB | 58.1 KB | 163.3 KB | 5.6 MB |
| .text (code) | 48.1 KB | 50.1 KB | 151.0 KB | 5.1 MB |
| .data (init'd) | 1008 B | 1.1 KB | 2.9 KB | 506.6 KB |
| .bss (zero-init) | 152.7 KB | 152.7 KB | 9.7 KB | 14.9 KB |

- `.text` is executable code. Both `az_mqtt5` variants dynamically link
  their TLS library, so the TLS code itself does **not** appear here; the
  small delta between them reflects only the wrapper (`transport_posix.c` vs
  `transport_mbedtls.c`).
- `.data` is pre-initialized writable globals.
- `.bss` is zero-initialized writable globals — dominated by the caller's
  static send / receive / payload / transport buffers.

## Interpretation

- **Throughput / CPU**: unchanged by TLS backend at runtime (runtime is plain
  TCP). The two `az_mqtt5` columns should be statistically identical; any
  delta is measurement noise.
- **Binary footprint**: with *dynamic* TLS linkage the openssl-vs-mbedtls gap
  is small (only the wrapper differs). To surface the real TLS library
  footprint, rebuild against static TLS libraries.
- **Memory**: most RSS is the process baseline (libc, stack, thread-local
  storage, I/O buffers). See **RSS delta** and **Heap delta** for the
  client-attributable portion. `az_mqtt5`'s heap delta is essentially zero
  (by design — no internal `malloc`).

## Reproducing

From `tests/client-comp`:

```powershell
.\run.ps1              # full pipeline: build, run all 4, compare, teardown
.\run.ps1 -Keep        # keep the broker running after the report
.\teardown.ps1         # stop & remove the broker
```

Raw JSON results live in `results/*.json`; the latest consolidated run is
`C:\code\s3\az_mqtt5\tests\client-comp\results\comparison_20260423_000503.json`.

