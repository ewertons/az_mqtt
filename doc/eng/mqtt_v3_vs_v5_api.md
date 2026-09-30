# MQTT 3.1.1 and 5.0: one client, per-version codecs

## Decision

One version-neutral client (`az_mqtt_client`) plus one codec library per protocol version
(`az_mqtt::mqtt3`, `az_mqtt::mqtt5`). The application selects the version at run time by passing
`&az_mqtt3_codec` or `&az_mqtt5_codec` in `az_mqtt_client_options.codec`.

This replaces the earlier layout of two separate libraries (`az_mqtt5`, and `az_mqtt3` kept in
sync as a copy), which duplicated the client, transport and types.

### Why MQTT v3.1.1 is not a subset of v5

The two protocols differ in ways that go deeper than a flag at connect time:

| Concern | MQTT v5 | MQTT v3.1.1 |
|---|---|---|
| **Packet format** | Fixed header + properties block (VBI length + key-value pairs) | Fixed header, no properties block at all |
| **CONNACK** | reason code (1 byte) + properties | return code (1 byte), no properties |
| **SUBACK payload** | reason codes (per MQTT5 enum) | return codes (0x00 / 0x01 / 0x02 / 0x80) |
| **PUBACK/PUBREC/PUBREL/PUBCOMP** | reason code + properties | just 2-byte packet ID, nothing else |
| **DISCONNECT** | reason code + properties, client-initiated or broker-initiated | client-only, no payload |
| **AUTH packet** | exists | does not exist |
| **User properties** | everywhere | do not exist |
| **Topic aliases** | exist | do not exist |
| **Session expiry** | numeric, in properties | boolean `clean session` flag |

The per-version differences live entirely in the codecs. The client only sees decoded packets
through the shared types in `az_mqtt_types.h`.

---

### Layout

```
inc/az_mqtt/
  az_mqtt_client.h      // client API (version-neutral)
  az_mqtt_types.h       // options and callback data (mqttv5 fields ignored with mqttv3)
  az_mqtt_transport.h   // TCP/TLS
  az_mqtt_codec.h       // az_mqtt_codec function table + fixed header / PINGREQ
  az_mqtt3.h            // az_mqtt3_codec, AZ_MQTT3_CONNACK_*
  az_mqtt5.h            // az_mqtt5_codec (incl. AUTH)
src/
  az_mqtt_client.c
  codec/az_mqtt_codec_common.c   // wire primitives shared by both codecs
  codec/az_mqtt3_codec.c
  codec/az_mqtt5_codec.c
  platform/                      // transport backends
```

| CMake target | Contents |
|---|---|
| `az_mqtt::core` | client, transport, common codec primitives |
| `az_mqtt::mqtt3` | mqttv3 codec + `az_mqtt::core` |
| `az_mqtt::mqtt5` | mqttv5 codec + `az_mqtt::core` |

---

### Linking only one version

- The client reaches the codec only through `options.codec`, so `az_mqtt::core` references
  no `az_mqtt3_*` / `az_mqtt5_*` symbol.
- Each codec is a separate library; an application links, and carries, only the ones it
  names. CI fails if the core references a codec, or if a single-version sample contains the
  other version's symbols.
- `AZ_MQTT_ENABLE_MQTT3` / `AZ_MQTT_ENABLE_MQTT5` let a build omit a codec entirely.

Remaining shared costs, relative to the previous per-version libraries (x86-64, `-Os`):

| | Cost |
|---|---|
| `az_mqtt_client` | +8 B (`codec` pointer) |
| Code, no LTO | mqttv3 sample −610 B, mqttv5 sample +550 B |
| Code, LTO | mqttv3 sample +1.3 KB, mqttv5 sample +1.6 KB: codec calls are indirect, so they are not inlined into the caller and unused operations (e.g. UNSUBSCRIBE) are kept |
| mqttv5-only option fields (`buffers`, properties) | unchanged: the previous `az_mqtt3` types were already copies of the mqttv5 ones |
