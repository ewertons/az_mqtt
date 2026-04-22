# MQTT Client Comparison Report

_Generated: 2026-04-22 16:25:03 -07:00_

This report compares three MQTT 5 client implementations running an identical
publish/subscribe workload against the same broker, inside equivalent Linux
containers. Each client is built with production / size-optimized release flags.

## Clients Under Test

| Client | Language | Version | Notes |
|---|---|---|---|
| `az_mqtt5` | C99 | 1.6.0-beta.1 (azure-sdk-for-c submodule) | Zero dynamic allocation; static buffers supplied by caller. |
| `paho_mqtt_c` | C | v1.3.14 (Eclipse Paho MQTT C, fetched via CMake FetchContent) | Async API (MQTTAsync), internal threads, heap-allocated internals. |
| `azure_mqtt` | Rust | azure_mqtt v0.1.0 (local path dep in perf_azure_mqtt/azure_mqtt_src/) | Tokio async runtime, OpenSSL (vendored). |

## Test Environment

| | |
|---|---|
| Host OS | Microsoft Windows NT 10.0.26200.0 / PowerShell 7.6.0 |
| Docker | Docker version 29.4.0, build 9d7ad9f |
| Container base | Ubuntu 24.04.4 LTS |
| C compiler | gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 |
| Broker | EMQX (`docker-compose.yml` service `emqx`), TCP 1883, no TLS, no auth |
| Network | Docker user-defined bridge (`client-comp_default`) |

## Workload

Each client, in its own container:

1. Connects to the broker (TCP, MQTT 5).
2. Subscribes to its own topic filter (`perf/<client>/#`) at QoS 1.
3. Publishes the target number of messages to its own topic at QoS 1 (so each
   message loops back and is also counted as *received*). Publishes are issued
   as fast as the client accepts them; no artificial rate-limiting.
4. Drains: waits for remaining inbound messages + PUBACKs using a no-progress
   watchdog (3 s idle, 120 s hard cap).
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

All binaries are built in **Release** mode with settings aimed at minimum
on-disk size and minimum resident code pages.

### C clients (`az_mqtt5` and `paho_mqtt_c`)

Applied to the `perf_az_mqtt5` / `perf_paho` CMake targets in
`tests/client-comp/CMakeLists.txt` when `CMAKE_BUILD_TYPE=Release` and the
compiler is GCC or Clang:

| Flag | Purpose |
|---|---|
| `-Os` | Optimize for size instead of speed. |
| `-ffunction-sections` | Put every function in its own ELF section. |
| `-fdata-sections` | Put every data object in its own ELF section. |
| `-fno-unwind-tables` | Omit `.eh_frame` (no C++ EH / no stack unwinders). |
| `-fno-asynchronous-unwind-tables` | Omit `.eh_frame_hdr` as well. |
| `-Wl,--gc-sections` | Drop unreferenced sections at link time (dead-code elimination). |
| `-Wl,-s` | Strip ELF symbol/relocation tables from the final binary. |

### Rust client (`azure_mqtt`)

Applied via `[profile.release]` in `perf_azure_mqtt/Cargo.toml`:

| Setting | Purpose |
|---|---|
| `opt-level = "z"` | Aggressive size optimization. |
| `lto = true` | Fat link-time optimization across all crates. |
| `codegen-units = 1` | Single codegen unit: maximum inlining/DCE. |
| `strip = "symbols"` | Strip symbol tables from the final ELF. |
| `panic = "abort"` | Drop the unwinding runtime; smaller binary. |
| `debug = false` | No debuginfo. |
| `incremental = false` | Disable incremental compilation in release. |

## Results

### Throughput & CPU

| Metric | az_mqtt5 (C) | paho_mqtt (C) | azure_mqtt (Rust) |
|---|---:|---:|---:|
| Messages sent | 10000 | 10000 | 10000 |
| Messages received | 1245 | 1128 | 1128 |
| PUBACKs received | 10000 | 10000 | 10000 |
| Elapsed (s) | 5.056 | 4.816 | 4.538 |
| Send rate (msg/s) | 1978 | 2076.4 | 2203.7 |
| Recv rate (msg/s) | 246.3 | 234.2 | 248.6 |
| User CPU (s) | 0.035 | 0.916 | 1.09 |
| System CPU (s) | 0.44 | 0.885 | 0.9 |
| Total CPU (s) | 0.476 | 1.802 | 1.99 |

### Runtime Memory

| Metric | az_mqtt5 (C) | paho_mqtt (C) | azure_mqtt (Rust) |
|---|---:|---:|---:|
| RSS baseline (before MQTT) | 1.5 MB | 1.5 MB | 4.1 MB |
| RSS peak (VmHWM) | 12.3 MB | 12.4 MB | 10.6 MB |
| RSS delta (client cost) | 10.8 MB | 10.9 MB | 6.5 MB |
| Heap baseline (`mallinfo2`) | 2.1 KB | 2.1 KB | n/a |
| Heap peak (`mallinfo2`) | 6.0 KB | 141.2 KB | n/a |
| Heap delta (client allocs) | 3.9 KB | 139.1 KB | n/a |

**Notes:**

- **RSS baseline** is `VmRSS` sampled after process start, argv parsing, and
  payload buffer initialisation, but **before** any MQTT client, socket, or
  async runtime is touched. This captures the unavoidable cost of the process
  image itself: libc pages, loader, stack, TLS, stdio buffers.
- **RSS peak** is `VmHWM` (`ru_maxrss` on Linux) at end of run.
- **RSS delta** (peak − baseline) is the closest single-number approximation
  to *the memory the client library actually caused this process to consume*.
- **Heap baseline / peak / delta** are `mallinfo2().uordblks` (glibc
  ptmalloc in-use bytes) around the run. For `az_mqtt5` this should be ≈0
  because the library never calls `malloc` itself — any non-zero value comes
  from stdio, `getaddrinfo`, or libc internals. Not tracked for Rust (would
  require a custom `GlobalAlloc` wrapper).

### Static Binary Footprint

Sizes of the stripped, size-optimized Release ELF binaries, measured with
`stat` (on-disk size) and `size` (text/data/bss sections) inside the same
Docker images used for the runs.

| Metric | az_mqtt5 (C) | paho_mqtt (C) | azure_mqtt (Rust) |
|---|---:|---:|---:|
| Binary on disk | 54.1 KB | 163.3 KB | 5.6 MB |
| .text (code) | 45.3 KB | 151.0 KB | 5.1 MB |
| .data (init'd) | 848 B | 2.9 KB | 506.6 KB |
| .bss (zero-init) | 137.0 KB | 9.7 KB | 14.9 KB |

- `.text` is the executable code.
- `.data` is pre-initialized writable globals.
- `.bss` is zero-initialized writable globals (occupies no file space but is
  allocated at load time). Large `.bss` in C harnesses reflects the
  static send/receive/payload buffers (`SEND_BUFFER_SIZE` = 64 KiB, etc.).

## Interpretation

- **Throughput**: all three clients saturate roughly the same range on a
  loopback broker; throughput is broker-bound more than client-bound.
- **CPU**: `az_mqtt5` is the cheapest by a wide margin because it has no
  internal threads and no heap traffic — all buffers are supplied by the
  caller as `az_span`s over static arrays.
- **Memory**: most of the reported RSS is process baseline (libc, stack, TLS,
  I/O buffers). See **RSS delta** and **Heap delta** for the
  client-attributable numbers. `az_mqtt5` is expected to show a heap delta
  of essentially zero, which is the designed behaviour for a zero-allocation
  library.
- **Binary size**: size-optimized `az_mqtt5` binary is smaller than
  `paho` by roughly 3× and smaller than the Rust binary by two orders of
  magnitude — primarily because the Rust binary statically links Tokio,
  OpenSSL, `std`, and the full async runtime.

## Reproducing

From `tests/client-comp`:

```powershell
.\run.ps1              # full pipeline: build, run all 3, compare, teardown
.\run.ps1 -Keep        # keep the broker running after the report
.\teardown.ps1         # stop & remove the broker
```

Raw JSON results live in `results/*.json`; the latest consolidated run is
`C:\code\s3\az_mqtt5\tests\client-comp\results\comparison_20260422_162348.json`.

