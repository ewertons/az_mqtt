// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_server.c
 * @brief In-process TCP/TLS peer for transport tests. See test_server.h.
 */

#include "test_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
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
  volatile int stop;
  volatile int accepted;
  volatile int handshakes;
  volatile bool saw_client_cert;
  int silent_fds[MAX_SILENT];
  int silent_count;
  SSL_CTX* ctx;
  char ca_path[64];
  char client_cert_path[64];
  char client_key_path[64];
};

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
} conn;

static int _read(conn* c, uint8_t* buf, int len)
{
  return c->ssl != NULL ? SSL_read(c->ssl, buf, len) : (int)recv(c->fd, buf, (size_t)len, 0);
}

static bool _write(conn* c, uint8_t const* buf, int len)
{
  int n = c->ssl != NULL ? SSL_write(c->ssl, buf, len)
                         : (int)send(c->fd, buf, (size_t)len, MSG_NOSIGNAL);
  return n == len;
}

static bool _wait_readable(test_server* s, conn* c)
{
  while (!s->stop)
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
  static const uint8_t connack_v5[] = { 0x20, 0x03, 0x00, 0x00, 0x00 };
  static const uint8_t connack_v3[] = { 0x20, 0x02, 0x00, 0x00 };
  bool ok = body[6] == 5 ? _write(c, connack_v5, (int)sizeof(connack_v5))
                         : _write(c, connack_v3, (int)sizeof(connack_v3));
  if (!ok)
  {
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
      return;
    }
    if (type == 12)
    {
      static const uint8_t pingresp[] = { 0xD0, 0x00 };
      (void)_write(c, pingresp, (int)sizeof(pingresp));
    }
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
  while (!s->stop)
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
    s->accepted++;
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
    conn c = { fd, NULL };
    if (s->options.tls)
    {
      struct timeval tv = { 5, 0 };
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      c.ssl = SSL_new(s->ctx);
      if (c.ssl != NULL && SSL_set_fd(c.ssl, fd) == 1 && SSL_accept(c.ssl) == 1)
      {
        X509* peer = SSL_get1_peer_certificate(c.ssl);
        s->saw_client_cert = peer != NULL;
        X509_free(peer);
        s->handshakes++;
        _serve(s, &c);
      }
      SSL_free(c.ssl);
    }
    else
    {
      _serve(s, &c);
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
  s->stop = 1;
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
int test_server_accepted(test_server const* s) { return s->accepted; }
int test_server_handshakes(test_server const* s) { return s->handshakes; }
bool test_server_saw_client_cert(test_server const* s) { return s->saw_client_cert; }
