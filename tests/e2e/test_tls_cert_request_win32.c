// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_tls_cert_request_win32.c
 * @brief Windows platform transport: servers that request, or require, a client certificate.
 *
 * eng/ci/start-broker.sh runs `openssl s_server` on 8885 (requests a certificate) and 8886
 * (requires one the CA issued), eng/ci/reneg_server.py on 8887 (requests one by renegotiating),
 * and mints the client certificates in tests/broker/certs.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define CERT(name) E2E_CERTS_DIR "/" name

static union
{
  uint8_t bytes[256 * 1024];
  void* align_pointer;
  int64_t align_int64;
  double align_double;
} s_storage;

static az_mqtt_transport* const s_transport = (az_mqtt_transport*)&s_storage;

static az_mqtt_tls_options tls_default(void)
{
  az_mqtt_tls_options tls = az_mqtt_tls_options_default();
  tls.ca_cert_path = AZ_SPAN_FROM_STR(E2E_CA_CERT_PATH);
  return tls;
}

static az_result connect_tls(uint16_t port, az_mqtt_tls_options const* tls)
{
  az_result const rc
      = az_mqtt_transport_connect(s_transport, AZ_SPAN_FROM_STR("localhost"), port, tls);
  az_mqtt_transport_close(s_transport);
  return rc;
}

/** @brief Connect to the server requiring a certificate, with @p cert and @p key (files). */
static az_result connect_with(char const* cert, char const* key)
{
  az_mqtt_tls_options tls = tls_default();
  tls.client_cert_path = az_span_create_from_str((char*)(uintptr_t)cert);
  tls.client_key_path = az_span_create_from_str((char*)(uintptr_t)key);
  return connect_tls(8886, &tls);
}

/** @brief The content of file @p path, in @p buf. */
static az_span read_file(char const* path, char* buf, DWORD size)
{
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  assert_true(h != INVALID_HANDLE_VALUE);
  DWORD len = 0;
  assert_true(ReadFile(h, buf, size, &len, NULL));
  CloseHandle(h);
  assert_true(len < size);
  return az_span_create((uint8_t*)buf, (int32_t)len);
}

/** @brief Files in the user's CNG key store (Microsoft Software Key Storage Provider). */
static int count_user_keys(void)
{
  static char const suffix[] = "\\Microsoft\\Crypto\\Keys\\*";
  char dir[MAX_PATH];
  DWORD const len = GetEnvironmentVariableA("APPDATA", dir, sizeof(dir));
  assert_true(len > 0 && len + sizeof(suffix) <= sizeof(dir));
  memcpy(dir + len, suffix, sizeof(suffix));
  WIN32_FIND_DATAA data;
  HANDLE h = FindFirstFileA(dir, &data);
  int count = 0;
  if (h != INVALID_HANDLE_VALUE)
  {
    do
    {
      count += (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    } while (FindNextFileA(h, &data));
    FindClose(h);
  }
  return count;
}

/** @brief The handshake completes without a certificate when the server only asks for one. */
static void a_requested_certificate_is_optional(void** state)
{
  (void)state;
  az_mqtt_tls_options const tls = tls_default();
  assert_int_equal(connect_tls(8885, &tls), AZ_OK);
}

/** @brief A server that requires a certificate refuses the handshake without one. */
static void a_required_certificate_fails_the_handshake(void** state)
{
  (void)state;
  az_mqtt_tls_options const tls = tls_default();
  assert_int_equal(connect_tls(8886, &tls), AZ_MQTT_ERROR_TLS_HANDSHAKE);
}

/** @brief EC and RSA keys, as PKCS#8, SEC1 and PKCS#1. */
static void each_key_format_is_accepted(void** state)
{
  (void)state;
  assert_int_equal(connect_with(CERT("client-ec.crt"), CERT("client-ec.key")), AZ_OK);
  assert_int_equal(connect_with(CERT("client-ec.crt"), CERT("client-ec.sec1.key")), AZ_OK);
  assert_int_equal(connect_with(CERT("client-rsa.crt"), CERT("client-rsa.key")), AZ_OK);
  assert_int_equal(connect_with(CERT("client-rsa.crt"), CERT("client-rsa.pkcs1.key")), AZ_OK);
}

/** @brief CA, certificate and key as PEM in memory. */
static void pem_in_memory_is_accepted(void** state)
{
  (void)state;
  static char ca[8192];
  static char cert[8192];
  static char key[8192];
  az_mqtt_tls_options tls = az_mqtt_tls_options_default();
  tls.ca_cert_pem = read_file(E2E_CA_CERT_PATH, ca, sizeof(ca));
  tls.client_cert_pem = read_file(CERT("client-rsa.crt"), cert, sizeof(cert));
  tls.client_key_pem = read_file(CERT("client-rsa.pkcs1.key"), key, sizeof(key));
  assert_int_equal(connect_tls(8886, &tls), AZ_OK);
}

/** @brief The intermediate after the leaf is sent: the server trusts only the root. */
static void the_chain_is_sent(void** state)
{
  (void)state;
  assert_int_equal(connect_with(CERT("client-chain.crt"), CERT("client-leaf.key")), AZ_OK);
}

/** @brief The server refuses a certificate its CA did not issue: the one given is sent. */
static void an_untrusted_certificate_is_refused(void** state)
{
  (void)state;
  assert_int_equal(
      connect_with(CERT("client-untrusted.crt"), CERT("client-untrusted.key")),
      AZ_MQTT_ERROR_TLS_HANDSHAKE);
}

/**
 * @brief A server that fails validation gets no client certificate, though it requests one: the
 * client stops with AZ_MQTT_ERROR_TLS_VERIFY. Had it sent this certificate, which the server
 * refuses, the server's refusal would have ended the handshake (AZ_MQTT_ERROR_TLS_HANDSHAKE).
 */
static void an_untrusted_server_gets_no_certificate(void** state)
{
  (void)state;
  az_mqtt_tls_options tls = az_mqtt_tls_options_default();
  tls.ca_cert_path = AZ_SPAN_FROM_STR(CERT("ca-bundle-without-ca.crt"));
  tls.client_cert_path = AZ_SPAN_FROM_STR(CERT("client-untrusted.crt"));
  tls.client_key_path = AZ_SPAN_FROM_STR(CERT("client-untrusted.key"));
  assert_int_equal(connect_tls(8886, &tls), AZ_MQTT_ERROR_TLS_VERIFY);
}

/**
 * @brief Ask the server on 8887 to renegotiate, requesting a client certificate; its one-line
 * answer (the certificate it received, if any) into @p answer.
 */
static void renegotiate(az_mqtt_tls_options const* tls, char* answer, int32_t size)
{
  assert_int_equal(
      az_mqtt_transport_connect(s_transport, AZ_SPAN_FROM_STR("localhost"), 8887, tls), AZ_OK);
  assert_int_equal(az_mqtt_transport_send(s_transport, AZ_SPAN_FROM_STR("RENEG\n")), AZ_OK);
  int32_t len = 0;
  for (int i = 0; i < 100 && (len == 0 || answer[len - 1] != '\n'); i++)
  {
    az_span got;
    if (az_result_failed(az_mqtt_transport_receive(
            s_transport, az_span_create((uint8_t*)answer + len, size - 1 - len), 100, &got)))
    {
      break;
    }
    len += az_span_size(got);
  }
  answer[len] = '\0';
  az_mqtt_transport_close(s_transport);
}

/** @brief A certificate requested by renegotiation, after the handshake, is presented. */
static void a_certificate_requested_by_renegotiation_is_presented(void** state)
{
  (void)state;
  char answer[128];
  az_mqtt_tls_options tls = tls_default();
  tls.client_cert_path = AZ_SPAN_FROM_STR(CERT("client-ec.crt"));
  tls.client_key_path = AZ_SPAN_FROM_STR(CERT("client-ec.key"));
  renegotiate(&tls, answer, (int32_t)sizeof(answer));
  assert_string_equal(answer, "client certificate: client-ec\n");
}

/** @brief Without a client certificate, a renegotiation requesting one continues without it. */
static void a_renegotiation_without_a_certificate_continues(void** state)
{
  (void)state;
  char answer[128];
  az_mqtt_tls_options const tls = tls_default();
  renegotiate(&tls, answer, (int32_t)sizeof(answer));
  assert_string_equal(answer, "no client certificate\n");
}

/** @brief Refused before connecting: another certificate's key, an encrypted key, no key. */
static void an_unusable_key_is_refused(void** state)
{
  (void)state;
  assert_int_equal(
      connect_with(CERT("client-ec.crt"), CERT("client-rsa.key")), AZ_MQTT_ERROR_TRANSPORT);
  assert_int_equal(
      connect_with(CERT("client-ec.crt"), CERT("client-ec.encrypted.key")),
      AZ_MQTT_ERROR_TRANSPORT);
  assert_int_equal(
      connect_with(CERT("client-ec.crt"), CERT("client-ec.crt")), AZ_MQTT_ERROR_TRANSPORT);
}

/** @brief The key imported for a connection is deleted when it closes, or fails. */
static void the_imported_key_is_deleted(void** state)
{
  (void)state;
  int const before = count_user_keys();
  az_mqtt_tls_options tls = tls_default();
  tls.client_cert_path = AZ_SPAN_FROM_STR(CERT("client-ec.crt"));
  tls.client_key_path = AZ_SPAN_FROM_STR(CERT("client-ec.key"));
  assert_int_equal(
      az_mqtt_transport_connect(s_transport, AZ_SPAN_FROM_STR("localhost"), 8886, &tls), AZ_OK);
  assert_int_equal(count_user_keys(), before + 1);
  az_mqtt_transport_close(s_transport);
  assert_int_equal(count_user_keys(), before);

  // A failed connect deletes it itself (counted before any close).
  static struct
  {
    char const* cert;
    char const* key;
    az_result result;
  } const failures[] = {
    { CERT("client-untrusted.crt"), CERT("client-untrusted.key"), AZ_MQTT_ERROR_TLS_HANDSHAKE },
    { CERT("client-ec.crt"), CERT("client-rsa.key"), AZ_MQTT_ERROR_TRANSPORT },
  };
  for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++)
  {
    tls.client_cert_path = az_span_create_from_str((char*)(uintptr_t)failures[i].cert);
    tls.client_key_path = az_span_create_from_str((char*)(uintptr_t)failures[i].key);
    assert_int_equal(
        az_mqtt_transport_connect(s_transport, AZ_SPAN_FROM_STR("localhost"), 8886, &tls),
        failures[i].result);
    assert_int_equal(count_user_keys(), before);
    az_mqtt_transport_close(s_transport);
  }
}

/** @brief Print each native error: the cause behind a failed assertion. */
static void on_error(az_mqtt_native_error const* error, void* context)
{
  (void)context;
  print_message(
      "native error: source %d, code 0x%08lx, result 0x%08lx\n",
      (int)error->source,
      (unsigned long)(uint32_t)error->code,
      (unsigned long)(uint32_t)error->result);
}

static int setup(void** state)
{
  (void)state;
  if (az_mqtt_transport_init(s_transport) != AZ_OK)
  {
    return -1;
  }
  az_mqtt_transport_set_error_callback(s_transport, on_error, NULL);
  return 0;
}

int main(void)
{
  char const* run_tls = getenv("AZ_MQTT_RUN_TLS_E2E");
  if (run_tls == NULL || strcmp(run_tls, "1") != 0)
  {
    return 0; // Same opt-in as test_ssl_win32.
  }
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_requested_certificate_is_optional),
    cmocka_unit_test(a_required_certificate_fails_the_handshake),
    cmocka_unit_test(each_key_format_is_accepted),
    cmocka_unit_test(pem_in_memory_is_accepted),
    cmocka_unit_test(the_chain_is_sent),
    cmocka_unit_test(an_untrusted_certificate_is_refused),
    cmocka_unit_test(an_untrusted_server_gets_no_certificate),
    cmocka_unit_test(a_certificate_requested_by_renegotiation_is_presented),
    cmocka_unit_test(a_renegotiation_without_a_certificate_continues),
    cmocka_unit_test(an_unusable_key_is_refused),
    cmocka_unit_test(the_imported_key_is_deleted),
  };
  return cmocka_run_group_tests(tests, setup, NULL);
}
