# Performance Tests – az_mqtt5 vs Paho MQTT C

Comparative performance benchmarks measuring throughput (msg/s), CPU usage,
and peak memory for the `az_mqtt5_client` library and Eclipse Paho MQTT C,
running against the same EMQX broker.

## Prerequisites

| Tool | Notes |
|------|-------|
| Docker + Docker Compose | Runs EMQX, Prometheus, Grafana, cAdvisor |
| CMake ≥ 3.10 | Builds perf binaries |
| C compiler (gcc/clang/MSVC) | C99 support |
| OpenSSL dev headers | Required by az_mqtt5 transport layer |
| Paho MQTT C (`libpaho-mqtt-dev` or vcpkg `paho-mqtt`) | For the comparison binary |
| jq (Linux) | Used by comparison script |

## Quick Start

### Linux / macOS

```bash
cd test/perf-conf

# 1. Install deps, build binaries, start Docker infrastructure
./setup.sh

# 2. Run each test (results written to results/)
./run_az_mqtt5.sh
./run_paho.sh

# 3. Compare
./compare.sh

# 4. Tear down containers
./teardown.sh
```

### Windows (PowerShell)

```powershell
cd test\perf-conf

.\setup.ps1
.\run_az_mqtt5.ps1
.\run_paho.ps1
.\compare.ps1
.\teardown.ps1
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
PERF_MSG_COUNT=100000 PERF_PAYLOAD=1024 PERF_DURATION=60 ./run_az_mqtt5.sh
```

## Output Format

Each run produces a JSON file in `results/`:

```json
{
  "client": "az_mqtt5",
  "messages_sent": 10000,
  "messages_received": 9950,
  "elapsed_sec": 2.340,
  "send_rate_msg_sec": 4273.5,
  "recv_rate_msg_sec": 4252.1,
  "user_cpu_sec": 0.180,
  "sys_cpu_sec": 0.090,
  "total_cpu_sec": 0.270,
  "peak_rss_bytes": 3145728
}
```

Broker-side Prometheus snapshots are saved alongside each run for parity
checking between rounds.

## Monitoring Dashboards

While tests run, open:

- **EMQX Dashboard**: http://localhost:18083 (default admin/public)
- **Prometheus**: http://localhost:9090
- **Grafana**: http://localhost:3000 (admin/admin)
- **cAdvisor**: http://localhost:8080

## Adapting for Rust

The same infrastructure works for a Rust MQTT client —
just compile your Rust binary and invoke it the same way:

```bash
# Build your Rust binary
cargo build --release -p my_mqtt_perf

# Run it against the same broker (emit the same JSON schema to stdout)
./target/release/my_mqtt_perf localhost 1883 10000 128 30 > results/rust_$(date +%s).json
```

Then extend `compare.sh` to include the Rust result file, or simply
inspect the JSON side-by-side.

## File Overview

```
test/perf-conf/
├── CMakeLists.txt          # Builds both perf binaries
├── docker-compose.yml      # EMQX + Prometheus + Grafana + cAdvisor
├── prometheus.yml          # Scrape config for Prometheus
├── perf_az_mqtt5.c         # az_mqtt5_client perf test program
├── perf_paho.c             # Paho MQTT C perf test program
├── setup.sh / setup.ps1    # Install deps, build, start Docker
├── run_az_mqtt5.sh / .ps1  # Run az_mqtt5 test, capture results
├── run_paho.sh / .ps1      # Run Paho test, capture results
├── compare.sh / .ps1       # Consolidate & compare results
├── teardown.sh / .ps1      # Stop Docker containers
└── results/                # Output directory (gitignored)
```
