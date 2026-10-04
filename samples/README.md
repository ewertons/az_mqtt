# Samples

Each sample is one short C file using the public API directly. All memory is caller storage: no
allocation. `az_mqtt_sample_common.h/.c` only reads the settings from environment variables.

| Sample | mqttv3 | mqttv5 | Shows |
|---|---|---|---|
| connect | [az_mqtt3_sample_connect.c](az_mqtt3_sample_connect.c) | [az_mqtt5_sample_connect.c](az_mqtt5_sample_connect.c) | TCP; subscribe, publish, receive the message back, disconnect |
| tls | [az_mqtt3_sample_tls.c](az_mqtt3_sample_tls.c) | [az_mqtt5_sample_tls.c](az_mqtt5_sample_tls.c) | TLS; optional client certificate (mutual TLS) and HTTP CONNECT proxy; native errors |
| websocket | [az_mqtt3_sample_websocket.c](az_mqtt3_sample_websocket.c) | [az_mqtt5_sample_websocket.c](az_mqtt5_sample_websocket.c) | MQTT over WebSockets (ws, or wss); optional proxy |
| nonblocking | [az_mqtt3_sample_nonblocking.c](az_mqtt3_sample_nonblocking.c) | [az_mqtt5_sample_nonblocking.c](az_mqtt5_sample_nonblocking.c) | `connect_start` and `process_loop` in an application loop; QoS 1 acknowledgements |
| request_response | | [az_mqtt5_sample_request_response.c](az_mqtt5_sample_request_response.c) | MQTT 5.0 Response Topic, Correlation Data, User Properties, Content Type |

The websocket samples are built only with `AZ_MQTT_ENABLE_WEBSOCKETS=ON` (the default).

## Settings

| Variable | Meaning | Default |
|---|---|---|
| `AZ_MQTT_SAMPLE_HOST` | Broker host name or IP address | `localhost` |
| `AZ_MQTT_SAMPLE_PORT` | Broker port | connect, nonblocking, request_response: 1883; tls: 8883; websocket: 80 |
| `AZ_MQTT_SAMPLE_CLIENT_ID` | MQTT client identifier | per sample |
| `AZ_MQTT_SAMPLE_USERNAME`, `AZ_MQTT_SAMPLE_PASSWORD` | MQTT credentials | none |
| `AZ_MQTT_SAMPLE_TLS` | `1`: TLS for the websocket samples (wss) | `0` |
| `AZ_MQTT_SAMPLE_CA_CERT` | CA certificate file (PEM) to trust | system store (OpenSSL, Schannel); mbedTLS has none: required |
| `AZ_MQTT_SAMPLE_CLIENT_CERT`, `AZ_MQTT_SAMPLE_CLIENT_KEY` | Client certificate and key files (PEM): mutual TLS | none (not supported by Schannel) |
| `AZ_MQTT_SAMPLE_WEBSOCKET_PATH` | WebSocket request path | `/mqtt` |
| `AZ_MQTT_SAMPLE_PROXY_HOST`, `AZ_MQTT_SAMPLE_PROXY_PORT` | HTTP CONNECT proxy | none (direct); port 3128 |
| `AZ_MQTT_SAMPLE_PROXY_USERNAME`, `AZ_MQTT_SAMPLE_PROXY_PASSWORD` | Proxy credentials (HTTP Basic) | none |

## Run against a local broker

1. Start Mosquitto with plain (1883), TLS (8883), ws (8080) and wss (8081) listeners. Both scripts
   write the CA certificate to `tests/broker/certs/ca.crt`.

   ```bash
   tests/start_broker.sh            # Docker (Windows: tests\start_broker.ps1); "stop" to stop
   eng/ci/start-broker.sh           # or a local mosquitto, mosquitto_pub and openssl
   ```

2. Build:

   ```bash
   cmake -S . -B build -DAZ_MQTT_BUILD_SAMPLES=ON
   cmake --build build
   ```

3. Run (Linux/macOS shown; executables are in `build/samples`, or `build/samples/Debug` with
   Visual Studio):

   ```bash
   build/samples/az_mqtt5_sample_connect
   build/samples/az_mqtt5_sample_nonblocking
   build/samples/az_mqtt5_sample_request_response
   AZ_MQTT_SAMPLE_CA_CERT=tests/broker/certs/ca.crt build/samples/az_mqtt5_sample_tls
   AZ_MQTT_SAMPLE_PORT=8080 build/samples/az_mqtt5_sample_websocket
   AZ_MQTT_SAMPLE_TLS=1 AZ_MQTT_SAMPLE_PORT=8081 AZ_MQTT_SAMPLE_CA_CERT=tests/broker/certs/ca.crt \
     build/samples/az_mqtt5_sample_websocket
   ```

   The mqttv3 samples take the same settings. Each prints what it sends and receives, and exits
   with 0 on success.

Expected output of `az_mqtt5_sample_connect` (packet identifiers and reason codes depend on the
broker):

```text
Connecting to localhost:1883
[CONNACK] reason=0x00 session_present=0
Subscribe sent (packet_id=1): 0x00010000
Publish sent (packet_id=2): 0x00010000
[SUBACK] packet_id=1 reason_codes=[0x01]
[PUBLISH received] topic="az-mqtt-sample/mqtt5/hello" qos=1 payload="Hello from az_mqtt5_client"
[PUBACK] packet_id=2 reason=0x00
Disconnected: 0x00010000
```

`0x00010000` is `AZ_OK`. Failures print an `az_result` such as `0x80060018`
(`AZ_MQTT_ERROR_CONNECTION_REFUSED`; see `inc/az_mqtt/az_mqtt_types.h`). The tls and websocket
samples also print each native error behind a failure (`[transport] source=... code=...`). One for
`localhost` resolving first to `::1`, refused, before `127.0.0.1` connects is expected with the local
broker.

## Other brokers

Set the host, port and credentials, e.g. a TLS broker with a public certificate:

```bash
AZ_MQTT_SAMPLE_HOST=broker.example.com AZ_MQTT_SAMPLE_USERNAME=user \
  AZ_MQTT_SAMPLE_PASSWORD=secret build/samples/az_mqtt5_sample_tls
```

Through a proxy, add `AZ_MQTT_SAMPLE_PROXY_HOST` (and its port and credentials): TLS runs inside the
tunnel, end to end with the broker.
