# MQTT v3.1.1 Support: Single Library vs Separate Library

## Recommendation: separate `az_mqtt3`

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

Every decode path in `az_mqtt3_codec.c` has a `_decode_*_props` step that is v5-only. Adding
v3.1.1 inside that codec means either duplicating the path or adding a version branch in every
function, bloating the code and making the zero-allocation guarantees harder to reason about.

The callback data types (`az_mqtt3_connack_data`, `az_mqtt3_ack_data`, `az_mqtt3_suback_data`,
`az_mqtt3_disconnect_data`) all carry v5-only fields (user properties, reason strings,
subscription identifiers). Those cannot be shared cleanly with a v3.1.1 client without either
wasting stack space or forking the structs.

---

### What a separate `az_mqtt3` would look like

The transport layer is **fully reusable as-is** — `az_mqtt3_transport` and its platform backends
are protocol-agnostic TCP/TLS. Zero changes needed there.

The codec and client layers would be small:

```
az_mqtt3/
  inc/az_mqtt3/
    az_mqtt3_client.h    // ~40 lines — simpler options, no buffers.* needed
    az_mqtt3_codec.h
    az_mqtt3_types.h     // ConnAck, Publish, SubAck structs — much smaller
  src/
    az_mqtt3_codec.c     // encode/decode without any properties block
    az_mqtt3_client.c    // same connect/subscribe/publish/process loop pattern
```

The v3.1.1 codec is noticeably simpler (no VBI property sections to parse), so `az_mqtt3_codec.c`
would be roughly half the size of the current `az_mqtt3_codec.c`.

---

### What sharing is practical

| Layer | Share? | How |
|---|---|---|
| Transport (`az_mqtt3_transport.*`) | Yes | Link against same object; expose via `az_mqtt3_transport.h` which is already protocol-agnostic |
| Fixed-header VBI encode/decode helpers | Yes | Move to a shared internal header `az_mqtt_internal.h` (not public API) |
| CMake target | Separate | `az_mqtt3::client` alias, same repo, same `deps/azure-sdk-for-c` |
| Tests/broker scripts | Yes | Same broker already speaks both protocol versions |

---

### Summary

**Create a separate `az_mqtt3`.**
Reuse the transport layer and any byte-level encoding helpers through a private shared header;
keep the codec and client APIs completely separate. This gives callers a smaller binary when they
only need v3.1.1, keeps both codecs readable and independently testable, and avoids growing the
v5 API surface with version-branching complexity.
