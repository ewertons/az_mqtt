# Client Comparison – az_mqtt5 (C) vs Paho MQTT C vs azure_mqtt (Rust)

Comparative benchmarks measuring throughput (msg/s), CPU usage, peak memory,
and binary footprint for three MQTT client libraries running against the same
EMQX broker.

## Prerequisites

| Tool | Notes |
|------|-------|
| Docker + Docker Compose | Runs EMQX, Prometheus, Grafana, cAdvisor |
| CMake ≥ 3.10 | Builds C perf binaries |
| C compiler (gcc/clang/MSVC) | C99 support |
| Rust toolchain (cargo) | Builds azure_mqtt perf binary |
| OpenSSL dev headers | Required by az_mqtt5 and azure_mqtt |
| Paho MQTT C (`libpaho-mqtt-dev` or vcpkg `paho-mqtt`) | For the Paho comparison binary |
| jq (Linux) | Used by comparison script |

## Quick Start

A single script handles the full pipeline — setup, run all clients, compare,
and teardown:

### Linux / macOS

```bash
cd tests/client-comp
./run.sh
```

### Windows (PowerShell)

```powershell
cd tests\client-comp
.\run.ps1
```

To keep the Docker infrastructure running after the test (useful for
inspecting Grafana dashboards):

```bash
./run.sh --keep       # Linux
.\run.ps1 -Keep       # Windows
```

Then tear down manually later:

```bash
./teardown.sh         # Linux
.\teardown.ps1        # Windows
```

### Running Individual Steps

You can also run each phase independently:

```bash
./setup.sh              # Install deps, build, start Docker
./run_az_mqtt5.sh       # Run az_mqtt5 (C) test
./run_paho.sh           # Run Paho MQTT C test
./run_azure_mqtt.sh     # Run azure_mqtt (Rust) test
./compare.sh            # Merge results + print comparison table
./teardown.sh           # Stop Docker containers
```

## Tunables

Set environment variables before running:

| Variable | Default | Description |
|----------|---------|-------------|
| `PERF_HOST` | `localhost` | Broker hostname |
| `PERF_PORT` | `1883` | Broker port |
| `PERF_MSG_COUNT` | `10000` | Total messages to publish |
| `PERF_PAYLOAD` | `128` | Payload size in bytes (max 8192) |
| `PERF_DURATION` | `30` | Max test duration in seconds |

Example:

```bash
PERF_MSG_COUNT=100000 PERF_PAYLOAD=1024 PERF_DURATION=60 ./run.sh
```

## Output

### Performance Table

The compare script prints a three-column table:

```
Metric                     az_mqtt5 (C)    paho_mqtt (C)  azure_mqtt (Rust)
------------------------   ----------------  ----------------  ----------------
Messages sent                       10000             10000             10000
Send rate (msg/s)                  4273.5            3891.2            4120.8
Recv rate (msg/s)                  4252.1            3870.0            4098.3
User CPU (sec)                      0.180             0.320             0.210
Peak RSS (bytes)                  3145728           8912896           5242880
```

### Memory Footprint Table

Binary sizes and `.text` section sizes are measured from the compiled
binaries and appended to the report:

```
-- Memory Footprint --
------------------------   ----------------  ----------------  ----------------
Binary on disk                    47.2 KB           340.0 KB          1.8 MB
.text section                     26.0 KB           149.4 KB            ...
```

### JSON Report

Each run produces a JSON file in `results/`. The compare script merges
them into `results/comparison_<timestamp>.json` with footprint data.

## Monitoring Dashboards

While tests run, open:

- **EMQX Dashboard**: http://localhost:18083 (default admin/public)
- **Prometheus**: http://localhost:9090
- **Grafana**: http://localhost:3000 (admin/admin)
- **cAdvisor**: http://localhost:8080

## How azure_mqtt (Rust) Is Built

The setup script automatically clones
[https://github.com/Azure/mqtt-client](https://github.com/Azure/mqtt-client)
into `perf_azure_mqtt/azure_mqtt_src/` and builds the Rust perf binary
with `cargo build --release`. No manual steps required.

## File Overview

```
tests/client-comp/
├── run.sh / run.ps1            # Single-command full pipeline
├── setup.sh / setup.ps1        # Install deps, build, start Docker
├── run_az_mqtt5.sh / .ps1      # Run az_mqtt5 (C) test
├── run_paho.sh / .ps1          # Run Paho MQTT C test
├── run_azure_mqtt.sh / .ps1    # Run azure_mqtt (Rust) test
├── compare.sh / .ps1           # Merge results + comparison table
├── teardown.sh / .ps1          # Stop Docker containers
├── CMakeLists.txt              # Builds C perf binaries
├── docker-compose.yml          # EMQX + Prometheus + Grafana + cAdvisor
├── prometheus.yml              # Scrape config for Prometheus
├── perf_az_mqtt5.c             # az_mqtt5 perf test program
├── perf_paho.c                 # Paho MQTT C perf test program
├── perf_azure_mqtt/            # Rust perf test crate
│   ├── Cargo.toml
│   ├── src/main.rs
│   └── azure_mqtt_src/         # Auto-cloned (gitignored)
└── results/                    # Output directory (gitignored)
```
