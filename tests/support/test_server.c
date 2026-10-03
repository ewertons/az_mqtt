// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_server.c
 * @brief In-process TCP/TLS peer for transport tests. See test_server.h.
 */

#include "test_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#define MAX_SILENT 8

struct test_server
{
  test_server_options options;
  int listen_fd;
  uint16_t port;
  pthread_t thread;
  // Shared with the server thread: guarded by lock.
  pthread_mutex_t lock;
  int stop;
  int accepted;
  int handshakes;
  bool saw_client_cert;
  int pingreqs;
  int publishes;
  int pubrels;
  int last_pubrel_reason;
  int pubcomps;
  int last_pubcomp_reason;
  bool client_closed;
  int close_notifies;
  int ws_pongs;
  int ws_unmasked;
  int ws_client_close_code;
  char ws_request[2048];
  int silent_fds[MAX_SILENT];
  int silent_count;
  SSL_CTX* ctx;
  char ca_path[64];
  char client_cert_path[64];
  char client_key_path[64];
};

/** @brief Read a shared int under the lock. */
static int _get(test_server* s, int const* field)
{
  pthread_mutex_lock(&s->lock);
  int v = *field;
  pthread_mutex_unlock(&s->lock);
  return v;
}

/** @brief Add to a shared int under the lock. */
static void _add(test_server* s, int* field, int delta)
{
  pthread_mutex_lock(&s->lock);
  *field += delta;
  pthread_mutex_unlock(&s->lock);
}

static bool _stopping(test_server* s) { return _get(s, &s->stop) != 0; }

test_server_options test_server_options_default(void)
{
  test_server_options o;
  memset(&o, 0, sizeof(o));
  o.tls = true;
  o.san = "DNS:localhost,IP:127.0.0.1";
  o.behavior = TEST_SERVER_MQTT;
  return o;
}

// ──────────────────────── Certificates ───────────────────────

static bool _add_ext(X509* cert, X509V3_CTX* ctx, int nid, char const* value)
{
  X509_EXTENSION* ext = X509V3_EXT_conf_nid(NULL, ctx, nid, value);
  if (ext == NULL)
  {
    return false;
  }
  bool ok = X509_add_ext(cert, ext, -1) == 1;
  X509_EXTENSION_free(ext);
  return ok;
}

static X509* _make_cert(
    EVP_PKEY* key,
    char const* cn,
    X509* issuer,
    EVP_PKEY* issuer_key,
    bool is_ca,
    char const* san,
    long not_before_s,
    long not_after_s)
{
  static long serial = 1;
  X509* cert = X509_new();
  if (cert == NULL)
  {
    return NULL;
  }
  X509_set_version(cert, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(cert), serial++);
  X509_gmtime_adj(X509_getm_notBefore(cert), not_before_s);
  X509_gmtime_adj(X509_getm_notAfter(cert), not_after_s);
  X509_set_pubkey(cert, key);
  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char const*)cn, -1, -1, 0);
  X509_set_issuer_name(cert, issuer != NULL ? X509_get_subject_name(issuer) : name);

  X509V3_CTX v3;
  X509V3_set_ctx_nodb(&v3);
  X509V3_set_ctx(&v3, issuer != NULL ? issuer : cert, cert, NULL, NULL, 0);
  bool ok = _add_ext(cert, &v3, NID_basic_constraints, is_ca ? "critical,CA:TRUE" : "CA:FALSE")
      && _add_ext(
             cert,
             &v3,
             NID_key_usage,
             is_ca ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature")
      && _add_ext(cert, &v3, NID_subject_key_identifier, "hash")
      && (is_ca || _add_ext(cert, &v3, NID_ext_key_usage, "serverAuth,clientAuth"))
      && (san == NULL || _add_ext(cert, &v3, NID_subject_alt_name, san))
      && X509_sign(cert, issuer_key != NULL ? issuer_key : key, EVP_sha256()) > 0;
  if (!ok)
  {
    X509_free(cert);
    return NULL;
  }
  return cert;
}

static bool _write_pem(char* path, size_t cap, X509* cert, EVP_PKEY* key)
{
  snprintf(path, cap, "/tmp/azmqtt_test_XXXXXX");
  int fd = mkstemp(path);
  if (fd < 0)
  {
    return false;
  }
  FILE* f = fdopen(fd, "w");
  if (f == NULL)
  {
    close(fd);
    return false;
  }
  bool ok = cert != NULL ? PEM_write_X509(f, cert) == 1
                         : PEM_write_PrivateKey(f, key, NULL, NULL, 0, NULL, NULL) == 1;
  fclose(f);
  return ok;
}

static bool _setup_tls(test_server* s)
{
  const long day = 24L * 3600L;
  EVP_PKEY* ca_key = EVP_EC_gen("P-256");
  EVP_PKEY* rogue_key = EVP_EC_gen("P-256");
  EVP_PKEY* srv_key = EVP_EC_gen("P-256");
  EVP_PKEY* cli_key = EVP_EC_gen("P-256");
  X509* ca = _make_cert(ca_key, "az-mqtt-test-ca", NULL, NULL, true, NULL, -day, 30 * day);
  X509* rogue = _make_cert(rogue_key, "az-mqtt-rogue-ca", NULL, NULL, true, NULL, -day, 30 * day);
  X509* signer = s->options.untrusted_ca ? rogue : ca;
  EVP_PKEY* signer_key = s->options.untrusted_ca ? rogue_key : ca_key;
  X509* srv = _make_cert(
      srv_key,
      "az-mqtt-test-server",
      signer,
      signer_key,
      false,
      s->options.san,
      s->options.expired ? -2 * day : -day,
      s->options.expired ? -day : 30 * day);
  X509* cli = _make_cert(cli_key, "az-mqtt-test-client", ca, ca_key, false, NULL, -day, 30 * day);

  bool ok = ca_key && rogue_key && srv_key && cli_key && ca && rogue && srv && cli
      && _write_pem(s->ca_path, sizeof(s->ca_path), ca, NULL)
      && _write_pem(s->client_cert_path, sizeof(s->client_cert_path), cli, NULL)
      && _write_pem(s->client_key_path, sizeof(s->client_key_path), NULL, cli_key);

  if (ok)
  {
    s->ctx = SSL_CTX_new(TLS_server_method());
    ok = s->ctx != NULL && SSL_CTX_use_certificate(s->ctx, srv) == 1
        && SSL_CTX_add1_chain_cert(s->ctx, signer) == 1
        && SSL_CTX_use_PrivateKey(s->ctx, srv_key) == 1;
  }
  if (ok && s->options.require_client_cert)
  {
    X509_STORE* store = SSL_CTX_get_cert_store(s->ctx);
    ok = X509_STORE_add_cert(store, ca) == 1;
    SSL_CTX_set_verify(s->ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
  }

  X509_free(ca);
  X509_free(rogue);
  X509_free(srv);
  X509_free(cli);
  EVP_PKEY_free(ca_key);
  EVP_PKEY_free(rogue_key);
  EVP_PKEY_free(srv_key);
  EVP_PKEY_free(cli_key);
  return ok;
}

// ──────────────────────── Connection handling ────────────────

typedef struct
{
  int fd;
  SSL* ssl;
  /** @brief Fixed header flags of the last packet read. */
  uint8_t flags;
  test_server* s;
  /** @brief Upgraded: packets travel in WebSocket frames. */
  bool ws;
  /** @brief Payload bytes left in the client's current data frame, its mask, and the offset. */
  uint64_t ws_left;
  uint8_t ws_mask[4];
  uint64_t ws_offset;
} conn;

static int _raw_read(conn* c, uint8_t* buf, int len)
{
  return c->ssl != NULL ? SSL_read(c->ssl, buf, len) : (int)recv(c->fd, buf, (size_t)len, 0);
}

static bool _raw_write(conn* c, uint8_t const* buf, int len)
{
  int n = c->ssl != NULL ? SSL_write(c->ssl, buf, len)
                         : (int)send(c->fd, buf, (size_t)len, MSG_NOSIGNAL);
  return n == len;
}

static bool _wait_readable(test_server* s, conn* c);

static bool _raw_read_exact(conn* c, uint8_t* buf, int len)
{
  for (int got = 0; got < len;)
  {
    if (!_wait_readable(c->s, c))
    {
      return false;
    }
    int n = _raw_read(c, buf + got, len - got);
    if (n <= 0)
    {
      return false;
    }
    got += n;
  }
  return true;
}

/** @brief Read a client frame header (and a control frame whole); false on a close or error. */
static bool _ws_next_data_frame(conn* c)
{
  test_server* s = c->s;
  while (c->ws_left == 0)
  {
    uint8_t h[2];
    if (!_raw_read_exact(c, h, 2))
    {
      return false;
    }
    uint64_t n = h[1] & 0x7F;
    int const extra = n == 127 ? 8 : (n == 126 ? 2 : 0);
    uint8_t e[8];
    if (!_raw_read_exact(c, e, extra))
    {
      return false;
    }
    if (extra > 0)
    {
      n = 0;
      for (int i = 0; i < extra; i++)
      {
        n = n << 8 | e[i];
      }
    }
    if ((h[1] & 0x80) == 0)
    {
      _add(s, &s->ws_unmasked, 1);
      return false;
    }
    if (!_raw_read_exact(c, c->ws_mask, 4))
    {
      return false;
    }
    uint8_t const opcode = h[0] & 0x0F;
    if ((opcode & 0x08) == 0)
    {
      c->ws_left = n;
      c->ws_offset = 0;
      continue; // Empty data frames are skipped.
    }
    uint8_t p[125];
    if (n > sizeof(p) || !_raw_read_exact(c, p, (int)n))
    {
      return false;
    }
    for (uint64_t i = 0; i < n; i++)
    {
      p[i] ^= c->ws_mask[i & 3];
    }
    if (opcode == 0x8)
    {
      pthread_mutex_lock(&s->lock);
      s->ws_client_close_code = n >= 2 ? (p[0] << 8 | p[1]) : 1005;
      pthread_mutex_unlock(&s->lock);
      return false;
    }
    if (opcode == 0xA && n == 2 && p[0] == 'h' && p[1] == 'i')
    {
      _add(s, &s->ws_pongs, 1);
    }
  }
  return true;
}

static int _read(conn* c, uint8_t* buf, int len)
{
  if (!c->ws)
  {
    return _raw_read(c, buf, len);
  }
  if (!_ws_next_data_frame(c))
  {
    return 0;
  }
  int const take = (uint64_t)len < c->ws_left ? len : (int)c->ws_left;
  int const got = _raw_read(c, buf, take);
  for (int i = 0; i < got; i++)
  {
    buf[i] ^= c->ws_mask[(c->ws_offset + (uint64_t)i) & 3];
  }
  if (got > 0)
  {
    c->ws_left -= (uint64_t)got;
    c->ws_offset += (uint64_t)got;
  }
  return got;
}

/** @brief A server frame (unmasked) with @p b0 and @p len payload bytes. */
static bool _ws_write_frame(conn* c, uint8_t b0, uint8_t const* buf, int len)
{
  uint8_t frame[4096 + 4];
  int n = 0;
  frame[n++] = b0;
  if (len < 126)
  {
    frame[n++] = (uint8_t)len;
  }
  else
  {
    frame[n++] = 126;
    frame[n++] = (uint8_t)(len >> 8);
    frame[n++] = (uint8_t)len;
  }
  if (len > 4096)
  {
    return false;
  }
  memcpy(frame + n, buf, (size_t)len);
  return _raw_write(c, frame, n + len);
}

static bool _write(conn* c, uint8_t const* buf, int len)
{
  if (!c->ws)
  {
    return _raw_write(c, buf, len);
  }
  if (!c->s->options.ws_fragment)
  {
    return _ws_write_frame(c, 0x82, buf, len);
  }
  for (int i = 0; i < len; i++)
  {
    static uint8_t const ping[] = { 0x89, 0x02, 'h', 'i' };
    uint8_t const b0 = (uint8_t)((i == 0 ? 0x02 : 0x00) | (i == len - 1 ? 0x80 : 0x00));
    if (!_raw_write(c, ping, (int)sizeof(ping)) || !_ws_write_frame(c, b0, buf + i, 1))
    {
      return false;
    }
  }
  return true;
}

/** @brief Read the upgrade request and answer it (see test_server_ws_reply). */
static bool _ws_upgrade(test_server* s, conn* c)
{
  char request[sizeof(s->ws_request)];
  int n = 0;
  while (n < (int)sizeof(request) - 1
         && !(n >= 4 && memcmp(request + n - 4, "\r\n\r\n", 4) == 0))
  {
    if (!_raw_read_exact(c, (uint8_t*)request + n, 1))
    {
      return false;
    }
    n++;
  }
  request[n] = '\0';
  pthread_mutex_lock(&s->lock);
  memcpy(s->ws_request, request, (size_t)n + 1);
  pthread_mutex_unlock(&s->lock);

  if (s->options.ws_reply == TEST_SERVER_WS_REFUSE)
  {
    static char const refused[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
    (void)_raw_write(c, (uint8_t const*)refused, (int)sizeof(refused) - 1);
    return false;
  }
  char const* key = strstr(request, "Sec-WebSocket-Key: ");
  char const* key_end = key != NULL ? strstr(key, "\r\n") : NULL;
  if (key_end == NULL || key_end - key - 19 != 24)
  {
    return false;
  }
  char joined[24 + 36 + 1];
  memcpy(joined, key + 19, 24);
  memcpy(joined + 24, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
  uint8_t digest[EVP_MAX_MD_SIZE];
  unsigned int digest_size = 0;
  char accept[64];
  if (EVP_Digest(joined, 60, digest, &digest_size, EVP_sha1(), NULL) != 1)
  {
    return false;
  }
  EVP_EncodeBlock((unsigned char*)accept, digest, (int)digest_size);
  if (s->options.ws_reply == TEST_SERVER_WS_BAD_ACCEPT)
  {
    accept[0] = accept[0] == 'A' ? 'B' : 'A';
  }
  static char reply[4096];
  int len = snprintf(
      reply,
      sizeof(reply),
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: %s\r\nSec-WebSocket-Protocol: mqtt\r\n",
      accept);
  bool const slow = s->options.ws_reply == TEST_SERVER_WS_SLOW_LONG_REPLY;
  for (int i = 0; slow && i < 32; i++)
  {
    len += snprintf(reply + len, sizeof(reply) - (size_t)len, "X-Padding-%02d: %060d\r\n", i, i);
  }
  len += snprintf(reply + len, sizeof(reply) - (size_t)len, "\r\n%s", slow ? "\x89\x02hi" : "");
  for (int sent = 0; sent < len;)
  {
    int const piece = slow ? (len - sent < 7 ? len - sent : 7) : len;
    if (!_raw_write(c, (uint8_t const*)reply + sent, piece))
    {
      return false;
    }
    sent += piece;
    if (slow)
    {
      usleep(500);
    }
  }
  c->ws = s->options.ws_reply != TEST_SERVER_WS_BAD_ACCEPT;
  return c->ws;
}

static bool _wait_readable(test_server* s, conn* c)
{
  while (!_stopping(s))
  {
    if (c->ssl != NULL && SSL_pending(c->ssl) > 0)
    {
      return true;
    }
    struct pollfd p = { c->fd, POLLIN, 0 };
    int r = poll(&p, 1, 50);
    if (r > 0)
    {
      return true;
    }
    if (r < 0)
    {
      return false;
    }
  }
  return false;
}

static bool _read_exact(test_server* s, conn* c, uint8_t* buf, int len)
{
  int got = 0;
  while (got < len)
  {
    if (!_wait_readable(s, c))
    {
      return false;
    }
    int n = _read(c, buf + got, len - got);
    if (n <= 0)
    {
      return false;
    }
    got += n;
  }
  return true;
}

/** @brief Read one MQTT packet; returns its type nibble or -1. Body truncated to cap. */
static int _read_packet(test_server* s, conn* c, uint8_t* body, int cap, int* out_len)
{
  uint8_t b;
  if (!_read_exact(s, c, &b, 1))
  {
    return -1;
  }
  int type = b >> 4;
  c->flags = b & 0x0F;
  int len = 0;
  int shift = 0;
  do
  {
    if (!_read_exact(s, c, &b, 1) || shift > 21)
    {
      return -1;
    }
    len |= (b & 0x7F) << shift;
    shift += 7;
  } while (b & 0x80);
  int keep = len < cap ? len : cap;
  if (!_read_exact(s, c, body, keep))
  {
    return -1;
  }
  for (int i = keep; i < len; i++)
  {
    if (!_read_exact(s, c, &b, 1))
    {
      return -1;
    }
  }
  *out_len = keep;
  return type;
}

static void _serve(test_server* s, conn* c)
{
  uint8_t body[512];
  int len = 0;
  if (_read_packet(s, c, body, (int)sizeof(body), &len) != 1 || len < 7)
  {
    return;
  }
  // CONNECT variable header: 00 04 'M' 'Q' 'T' 'T' <level>
  bool const v5 = body[6] == 5;
  uint8_t const connack_v3[] = { 0x20, 0x02, 0x00, s->options.connack_code };
  uint8_t props[24];
  int p = 0;
  if (s->options.server_keep_alive != 0 || s->options.server_keep_alive_present)
  {
    props[p++] = 0x13; // Server Keep Alive
    props[p++] = (uint8_t)(s->options.server_keep_alive >> 8);
    props[p++] = (uint8_t)(s->options.server_keep_alive & 0xFF);
  }
  if (s->options.receive_maximum != 0)
  {
    props[p++] = 0x21;
    props[p++] = (uint8_t)(s->options.receive_maximum >> 8);
    props[p++] = (uint8_t)(s->options.receive_maximum & 0xFF);
  }
  if (s->options.maximum_qos_present)
  {
    props[p++] = 0x24;
    props[p++] = s->options.maximum_qos;
  }
  if (s->options.retain_unavailable)
  {
    props[p++] = 0x25;
    props[p++] = 0x00;
  }
  if (s->options.maximum_packet_size != 0)
  {
    props[p++] = 0x27;
    for (int shift = 24; shift >= 0; shift -= 8)
    {
      props[p++] = (uint8_t)(s->options.maximum_packet_size >> shift);
    }
  }
  uint8_t connack_v5[5 + sizeof(props)] = { 0x20, (uint8_t)(3 + p), 0x00, s->options.connack_code, (uint8_t)p };
  memcpy(&connack_v5[5], props, (size_t)p);
  bool ok = v5 ? _write(c, connack_v5, 5 + p) : _write(c, connack_v3, (int)sizeof(connack_v3));
  if (!ok || s->options.connack_code != 0 || s->options.close_after_connack)
  {
    return;
  }
  if (c->ws && s->options.ws_close_code != 0)
  {
    uint8_t const close_frame[] = { 0x88, 0x02, (uint8_t)(s->options.ws_close_code >> 8),
                                    (uint8_t)s->options.ws_close_code };
    uint8_t b;
    if (_raw_write(c, close_frame, (int)sizeof(close_frame)))
    {
      (void)_read_exact(s, c, &b, 1); // Ends at the client's close frame.
    }
    return;
  }
  if (c->ws && s->options.ws_masked_frame)
  {
    // A masked binary frame. Read as unmasked, the same bytes are a valid stream (a PINGRESP,
    // an empty binary frame, a pong): only the mask bit tells them apart.
    static uint8_t const masked[] = { 0x82, 0x82, 0xD0, 0x00, 0x82, 0x00, 0x8A, 0x00 };
    (void)_raw_write(c, masked, (int)sizeof(masked));
    usleep(200 * 1000);
    return;
  }
  if (s->options.burst_publishes > 0)
  {
    // QoS 0 PUBLISH "t" / "p": 30 len 00 01 't' [00 props] 'p'
    uint8_t burst[64 * 8];
    int n = 0;
    for (int i = 0; i < s->options.burst_publishes && n + 8 <= (int)sizeof(burst); i++)
    {
      burst[n++] = 0x30;
      burst[n++] = v5 ? 0x05 : 0x04;
      burst[n++] = 0x00;
      burst[n++] = 0x01;
      burst[n++] = 't';
      if (v5)
      {
        burst[n++] = 0x00;
      }
      burst[n++] = 'p';
    }
    if (s->options.ticket_before_burst && c->ssl != NULL)
    {
      int const on = 1; // Without Nagle the flight leaves now, not at the client's delayed ACK.
      (void)setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
      // Hold the ticket and the burst in one buffered BIO so they leave in one flight.
      BIO* const net = SSL_get_wbio(c->ssl);
      BIO* const buffered = BIO_new(BIO_f_buffer());
      if (buffered == NULL || !BIO_up_ref(net))
      {
        BIO_free(buffered);
        return;
      }
      BIO_push(buffered, net);
      SSL_set0_wbio(c->ssl, buffered);
      bool const flushed = SSL_new_session_ticket(c->ssl) == 1 && SSL_do_handshake(c->ssl) == 1
          && _write(c, burst, n) && BIO_flush(buffered) == 1;
      if (!flushed)
      {
        return;
      }
    }
    else if (!_write(c, burst, n))
    {
      return;
    }
  }
  if (v5 && s->options.publish_properties > 0 && s->options.publish_properties <= 12)
  {
    // QoS 0 PUBLISH "t" / "p" with N x (user property "k"="v", subscription identifier i+1).
    int const count = s->options.publish_properties;
    uint8_t pub[128];
    int n = 0;
    pub[n++] = 0x30;
    pub[n++] = (uint8_t)(3 + 1 + count * 9 + 1);
    pub[n++] = 0x00;
    pub[n++] = 0x01;
    pub[n++] = 't';
    pub[n++] = (uint8_t)(count * 9);
    for (int i = 0; i < count; i++)
    {
      static const uint8_t user_property[] = { 0x26, 0x00, 0x01, 'k', 0x00, 0x01, 'v' };
      memcpy(&pub[n], user_property, sizeof(user_property));
      n += (int)sizeof(user_property);
      pub[n++] = 0x0B;
      pub[n++] = (uint8_t)(i + 1);
    }
    pub[n++] = 'p';
    if (!_write(c, pub, n))
    {
      return;
    }
  }
  if (s->options.send_empty_disconnect)
  {
    static const uint8_t disconnect[] = { 0xE0, 0x00 };
    if (!_write(c, disconnect, (int)sizeof(disconnect)))
    {
      return;
    }
  }
  if (s->options.send_auth)
  {
    static const uint8_t auth[] = { 0xF0, 0x00 };
    if (!_write(c, auth, (int)sizeof(auth)))
    {
      return;
    }
  }
  if (s->options.send_qos2_sequence)
  {
    // PUBLISH QoS 2 "t"/"p" id 7: 0x34 len 00 01 't' 00 07 [00 props] 'p'; DUP sets 0x08.
    uint8_t seq[64];
    int n = 0;
    uint8_t const kinds[] = { 0x34, 0x3C, 0x62, 0x34, 0x62 };
    for (size_t i = 0; i < sizeof(kinds); i++)
    {
      seq[n++] = kinds[i];
      if (kinds[i] == 0x62)
      {
        seq[n++] = 0x02; // PUBREL 7
        seq[n++] = 0x00;
        seq[n++] = 0x07;
        continue;
      }
      seq[n++] = v5 ? 0x07 : 0x06;
      seq[n++] = 0x00;
      seq[n++] = 0x01;
      seq[n++] = 't';
      seq[n++] = 0x00;
      seq[n++] = 0x07;
      if (v5)
      {
        seq[n++] = 0x00;
      }
      seq[n++] = 'p';
    }
    if (!_write(c, seq, n))
    {
      return;
    }
  }
  if (s->options.send_unknown_acks)
  {
    static const uint8_t acks_v3[] = { 0x40, 0x02, 0x41, 0x41, 0x70, 0x02, 0x42, 0x42, 0x50, 0x02,
                                       0x43, 0x43, 0x62, 0x02, 0x44, 0x44, 0x90, 0x03, 0x45, 0x45,
                                       0x00, 0xB0, 0x02, 0x46, 0x46 };
    static const uint8_t acks_v5[] = { 0x40, 0x02, 0x41, 0x41, 0x70, 0x02, 0x42, 0x42, 0x50, 0x02,
                                       0x43, 0x43, 0x62, 0x02, 0x44, 0x44, 0x90, 0x04, 0x45, 0x45,
                                       0x00, 0x00, 0xB0, 0x04, 0x46, 0x46, 0x00, 0x00 };
    bool const sent = v5 ? _write(c, acks_v5, (int)sizeof(acks_v5))
                         : _write(c, acks_v3, (int)sizeof(acks_v3));
    if (!sent)
    {
      return;
    }
  }
  if (s->options.behavior == TEST_SERVER_DISCONNECT_AFTER_CONNACK)
  {
    static const uint8_t disconnect_v5[] = { 0xE0, 0x01, 0x8B };
    if (v5)
    {
      (void)_write(c, disconnect_v5, (int)sizeof(disconnect_v5));
    }
    usleep(100 * 1000);
    return;
  }
  if (s->options.behavior == TEST_SERVER_PARTIAL_TLS_RECORD)
  {
    // Application-data record header announcing 64 bytes, then only 5 of them,
    // written under OpenSSL so the client sees a record that never completes.
    static const uint8_t partial[] = { 0x17, 0x03, 0x03, 0x00, 0x40, 1, 2, 3, 4, 5 };
    (void)send(c->fd, partial, sizeof(partial), MSG_NOSIGNAL);
    while (!_stopping(s))
    {
      usleep(20 * 1000);
    }
    return;
  }
  if (s->options.behavior == TEST_SERVER_STOP_READING)
  {
    while (!_stopping(s))
    {
      usleep(20 * 1000);
    }
    return;
  }
  if (s->options.behavior == TEST_SERVER_CLOSE_AFTER_CONNACK)
  {
    // Plain close (FIN). The client's next write draws an RST and the one after
    // that fails with EPIPE, which is what raises SIGPIPE.
    usleep(200 * 1000);
    return;
  }
  for (;;)
  {
    int type = _read_packet(s, c, body, (int)sizeof(body), &len);
    if (type < 0 || type == 14)
    {
      pthread_mutex_lock(&s->lock);
      s->client_closed = !s->stop;
      pthread_mutex_unlock(&s->lock);
      return;
    }
    if (type == 8 && len >= 2)
    {
      // SUBACK: same packet id, then reason codes (MQTT 5 adds an empty property length).
      int codes = s->options.suback_codes > 0 ? s->options.suback_codes : 1;
      if (codes > 64)
      {
        codes = 64; // Bounded before it sizes both the length byte and the payload.
      }
      uint8_t suback[4 + 1 + 64];
      int n = 0;
      suback[n++] = 0x90;
      suback[n++] = (uint8_t)(2 + (v5 ? 1 : 0) + codes);
      suback[n++] = body[0];
      suback[n++] = body[1];
      if (v5)
      {
        suback[n++] = 0x00;
      }
      for (int i = 0; i < codes; i++)
      {
        suback[n++] = 0x00;
      }
      (void)_write(c, suback, n);
    }
    if (type == 3)
    {
      _add(s, &s->publishes, 1);
      int const qos = (c->flags >> 1) & 0x03;
      int const id_at = len >= 2 ? 2 + ((body[0] << 8) | body[1]) : len;
      bool const held = s->options.hold_first_publish && test_server_publishes(s) == 1;
      if (s->options.ack_publishes && !held && qos > 0 && id_at + 2 <= len)
      {
        bool const reason = v5 && qos == 2 && s->options.pubrec_reason != 0;
        uint8_t const ack[] = { (uint8_t)(qos == 1 ? 0x40 : 0x50), (uint8_t)(reason ? 3 : 2),
                                body[id_at], body[id_at + 1], s->options.pubrec_reason };
        (void)_write(c, ack, reason ? 5 : 4);
      }
    }
    if (type == 6 && len >= 2)
    {
      pthread_mutex_lock(&s->lock);
      s->pubrels++;
      s->last_pubrel_reason = len >= 3 ? body[2] : 0;
      pthread_mutex_unlock(&s->lock);
      if (s->options.ack_publishes)
      {
        uint8_t const pubcomp[] = { 0x70, 0x02, body[0], body[1] };
        (void)_write(c, pubcomp, (int)sizeof(pubcomp));
      }
    }
    if (type == 7)
    {
      pthread_mutex_lock(&s->lock);
      s->pubcomps++;
      s->last_pubcomp_reason = len >= 3 ? body[2] : 0;
      pthread_mutex_unlock(&s->lock);
    }
    if (type == 10 && len >= 2)
    {
      // UNSUBACK for one topic filter (MQTT 5: empty properties, reason 0).
      uint8_t const unsuback[] = { 0xB0, (uint8_t)(v5 ? 4 : 2), body[0], body[1], 0x00, 0x00 };
      (void)_write(c, unsuback, v5 ? 6 : 4);
    }
    if (type == 12)
    {
      _add(s, &s->pingreqs, 1);
      static const uint8_t pingresp[] = { 0xD0, 0x00 };
      if (!s->options.no_pingresp)
      {
        (void)_write(c, pingresp, (int)sizeof(pingresp));
      }
    }
  }
}

/** @brief Upgrade first if asked; then serve, and catch the close frame that may follow. */
static void _serve_maybe_ws(test_server* s, conn* c)
{
  if (s->options.websocket && !_ws_upgrade(s, c))
  {
    usleep(100 * 1000); // Let the client read the reply before the close.
    return;
  }
  _serve(s, c);
  struct pollfd p = { c->fd, POLLIN, 0 };
  if (c->ws && ((c->ssl != NULL && SSL_pending(c->ssl) > 0) || poll(&p, 1, 1000) > 0))
  {
    (void)_ws_next_data_frame(c);
  }
}

static void* _run(void* arg)
{
  test_server* s = (test_server*)arg;
  // SIGPIPE from write() goes to the writing thread: blocking it here keeps the
  // server's own writes from masking or causing one in the client under test.
  sigset_t pipe_set;
  sigemptyset(&pipe_set);
  sigaddset(&pipe_set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &pipe_set, NULL);
  while (!_stopping(s))
  {
    struct pollfd p = { s->listen_fd, POLLIN, 0 };
    if (poll(&p, 1, 50) <= 0)
    {
      continue;
    }
    int fd = accept(s->listen_fd, NULL, NULL);
    if (fd < 0)
    {
      continue;
    }
    _add(s, &s->accepted, 1);
    if (s->options.behavior == TEST_SERVER_SILENT)
    {
      if (s->silent_count < MAX_SILENT)
      {
        s->silent_fds[s->silent_count++] = fd;
      }
      else
      {
        close(fd);
      }
      continue;
    }
    conn c;
    memset(&c, 0, sizeof(c));
    c.fd = fd;
    c.s = s;
    if (s->options.tls)
    {
      struct timeval tv = { 5, 0 };
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      if (s->options.handshake_delay_ms > 0)
      {
        usleep((useconds_t)s->options.handshake_delay_ms * 1000);
      }
      c.ssl = SSL_new(s->ctx);
      if (c.ssl != NULL && SSL_set_fd(c.ssl, fd) == 1 && SSL_accept(c.ssl) == 1)
      {
        X509* peer = SSL_get1_peer_certificate(c.ssl);
        pthread_mutex_lock(&s->lock);
        s->saw_client_cert = peer != NULL;
        s->handshakes++;
        pthread_mutex_unlock(&s->lock);
        X509_free(peer);
        _serve_maybe_ws(s, &c);
        // Read what the client sends after its last packet (e.g. DISCONNECT): its close_notify.
        uint8_t rest[64];
        struct pollfd readable = { fd, POLLIN, 0 };
        for (int i = 0; i < 4 && (SSL_get_shutdown(c.ssl) & SSL_RECEIVED_SHUTDOWN) == 0
             && (SSL_pending(c.ssl) > 0 || poll(&readable, 1, 250) > 0);
             i++)
        {
          if (SSL_read(c.ssl, rest, (int)sizeof(rest)) <= 0)
          {
            break;
          }
        }
        if ((SSL_get_shutdown(c.ssl) & SSL_RECEIVED_SHUTDOWN) != 0)
        {
          _add(s, &s->close_notifies, 1);
        }
      }
      SSL_free(c.ssl);
    }
    else
    {
      _serve_maybe_ws(s, &c);
    }
    close(fd);
  }
  return NULL;
}

// ──────────────────────── Public API ─────────────────────────

test_server* test_server_start(test_server_options const* options)
{
  test_server* s = (test_server*)calloc(1, sizeof(*s));
  if (s == NULL)
  {
    return NULL;
  }
  s->options = *options;
  s->listen_fd = -1;
  s->ws_client_close_code = -1;
  pthread_mutex_init(&s->lock, NULL);
  if (s->options.tls && !_setup_tls(s))
  {
    test_server_stop(s);
    return NULL;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t alen = sizeof(addr);
  s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (s->listen_fd >= 0 && s->options.behavior == TEST_SERVER_STOP_READING)
  {
    int small = 4096; // Inherited by accepted sockets: backs the client up quickly.
    setsockopt(s->listen_fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
  }
  if (s->listen_fd < 0 || bind(s->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0
      || listen(s->listen_fd, 8) != 0
      || getsockname(s->listen_fd, (struct sockaddr*)&addr, &alen) != 0)
  {
    test_server_stop(s);
    return NULL;
  }
  s->port = ntohs(addr.sin_port);
  if (pthread_create(&s->thread, NULL, _run, s) != 0)
  {
    s->thread = 0;
    test_server_stop(s);
    return NULL;
  }
  return s;
}

void test_server_stop(test_server* s)
{
  if (s == NULL)
  {
    return;
  }
  pthread_mutex_lock(&s->lock);
  s->stop = 1;
  pthread_mutex_unlock(&s->lock);
  if (s->thread != 0)
  {
    pthread_join(s->thread, NULL);
  }
  for (int i = 0; i < s->silent_count; i++)
  {
    close(s->silent_fds[i]);
  }
  if (s->listen_fd >= 0)
  {
    close(s->listen_fd);
  }
  SSL_CTX_free(s->ctx);
  pthread_mutex_destroy(&s->lock);
  if (s->ca_path[0] != '\0')
  {
    remove(s->ca_path);
    remove(s->client_cert_path);
    remove(s->client_key_path);
  }
  free(s);
}

uint16_t test_server_port(test_server const* s) { return s->port; }
char const* test_server_ca_path(test_server const* s) { return s->ca_path; }
char const* test_server_client_cert_path(test_server const* s) { return s->client_cert_path; }
char const* test_server_client_key_path(test_server const* s) { return s->client_key_path; }
int test_server_accepted(test_server* s) { return _get(s, &s->accepted); }
int test_server_handshakes(test_server* s) { return _get(s, &s->handshakes); }
bool test_server_saw_client_cert(test_server* s)
{
  pthread_mutex_lock(&s->lock);
  bool v = s->saw_client_cert;
  pthread_mutex_unlock(&s->lock);
  return v;
}
int test_server_pingreqs(test_server* s) { return _get(s, &s->pingreqs); }
int test_server_publishes(test_server* s) { return _get(s, &s->publishes); }
int test_server_pubrels(test_server* s) { return _get(s, &s->pubrels); }
int test_server_last_pubrel_reason(test_server* s) { return _get(s, &s->last_pubrel_reason); }
int test_server_pubcomps(test_server* s) { return _get(s, &s->pubcomps); }
int test_server_last_pubcomp_reason(test_server* s) { return _get(s, &s->last_pubcomp_reason); }
int test_server_ws_pongs(test_server* s) { return _get(s, &s->ws_pongs); }
int test_server_ws_unmasked(test_server* s) { return _get(s, &s->ws_unmasked); }
int test_server_ws_client_close_code(test_server* s) { return _get(s, &s->ws_client_close_code); }
bool test_server_ws_request_has(test_server* s, char const* text)
{
  pthread_mutex_lock(&s->lock);
  bool v = strstr(s->ws_request, text) != NULL;
  pthread_mutex_unlock(&s->lock);
  return v;
}
int test_server_close_notifies(test_server* s) { return _get(s, &s->close_notifies); }
bool test_server_client_closed(test_server* s)
{
  pthread_mutex_lock(&s->lock);
  bool v = s->client_closed;
  pthread_mutex_unlock(&s->lock);
  return v;
}
