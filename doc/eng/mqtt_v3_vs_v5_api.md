# MQTT 3.1.1 and 5.0: one client, per-version codecs

## Decision

One client source and one API (`az_mqtt_client_*`), one codec per protocol version. The client
is built per library with its codec bound at build time:

- `az_mqtt::mqtt3` / `az_mqtt::mqtt5`: calls go straight to that codec; the other version is
  not compiled in.
- `az_mqtt::multi`: both codecs; each client picks one from `options.protocol_version`.

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
  az_mqtt_client.h      // client API
  az_mqtt_types.h       // options and callback data (mqttv5 fields ignored with mqttv3)
  az_mqtt_transport.h   // TCP/TLS
  az_mqtt3.h            // az_mqtt3_codec_*, AZ_MQTT3_PROTOCOL_VERSION, AZ_MQTT3_CONNACK_*
  az_mqtt5.h            // az_mqtt5_codec_* (incl. AUTH), AZ_MQTT5_PROTOCOL_VERSION
src/
  az_mqtt_client.c               // codec calls via _AZ_MQTT_CODEC(client, fn)
  codec/az_mqtt_codec_internal.h // wire primitives, static: each codec compiles its own copy
  codec/az_mqtt3_codec.c
  codec/az_mqtt5_codec.c
  platform/                      // transport backends
```

| CMake target | Contents |
|---|---|
| `az_mqtt::base` | transport |
| `az_mqtt::mqtt3` | client + mqttv3 codec |
| `az_mqtt::mqtt5` | client + mqttv5 codec |
| `az_mqtt::multi` | client + both codecs |

An application links exactly one of `mqtt3`, `mqtt5`, `multi` (they define the same client
symbols).

---

### Why build-time binding

A first version selected the codec at run time through a function table. It kept the other
version out of the binary, but cost code: calls through the table are not inlined, and the table
keeps every codec function alive, used or not. With build-time binding a single-version client
compiles to the same code as the previous per-version libraries.

Measured on the connect samples (x86-64, GCC 12, `-Os`, no TLS; previous `az_mqtt3` /
`az_mqtt5` = 100 %):

| | mqttv3 `.text` | mqttv5 `.text` |
|---|---|---|
| Previous libraries | 25,215 B | 29,573 B |
| This layout | 23,920 B | 29,573 B |
| This layout, LTO (previous: 13,443 / 18,138 B) | 13,020 B | 18,138 B |

`az_mqtt_client` / `az_mqtt_client_options` stay 432 / 384 B: `protocol_version` fills
existing padding. Figures exclude the PUBLISH property-bounds fix, which adds 67 B (80 B with
LTO) to mqttv5 and nothing to mqttv3.

`az_mqtt::multi` compiles both codecs and branches per call; base + multi is 20,953 B of
`.text`, against 29,068 B for the previous two libraries linked together.
