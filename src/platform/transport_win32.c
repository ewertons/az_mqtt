// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "az_mqtt_http_connect.h"

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/az_span.h>

#include <stdio.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <schannel.h>
#include <security.h>
#include <wincrypt.h>

#pragma comment(lib, "ws2_32.lib")

#define AZ_MQTT_SCHANNEL_IO_BUFFER_SIZE 65536

/** @brief Connect progress (connect_start / connect_poll). */
typedef enum
{
  _WIN_IDLE = 0,
  _WIN_TCP,
  _WIN_PROXY,
  _WIN_TLS,
  _WIN_CONNECTED,
} _win_state;

struct az_mqtt_transport
{
  SOCKET socket_fd;
  bool connected;
  _win_state state;
  /** @brief Native error callback; connect_attempt counts az_mqtt_transport_connect_start(). */
  az_mqtt_transport_error_fn error_callback;
  void* error_context;
  uint32_t connect_attempt;
  /** @brief WSA error of the last address that failed (each is reported as it fails). */
  int last_socket_error;
  /** @brief The handshake is running: its native errors belong to AZ_MQTT_ERROR_TLS_HANDSHAKE. */
  bool in_handshake;

  /** @brief Proxy to connect through (az_mqtt_transport_set_proxy()); NULL: none. */
  az_mqtt_proxy_options const* proxy;
  /**
   * @brief The current connect's proxy (set_proxy() affects only the next connect), the server
   * to reach through it, request bytes sent, and the reply so far.
   */
  az_mqtt_proxy_options const* attempt_proxy;
  az_span proxy_target_host;
  uint16_t proxy_target_port;
  int32_t proxy_request_sent;
  bool proxy_request_done;
  _az_mqtt_http_connect_reply proxy_reply;

  /** @brief _WIN_TCP: resolved addresses, the next one to try, and when the current one started. */
  struct addrinfo* addresses;
  struct addrinfo* next_address;
  int64_t attempt_start_ms;

#ifdef AZ_MQTT_TLS_SCHANNEL
  bool tls_active;
  bool has_cred;
  bool has_context;

  CredHandle cred_handle;
  CtxtHandle context_handle;
  SecPkgContext_StreamSizes stream_sizes;
  PCCERT_CONTEXT remote_server_cert;

  uint8_t encrypted_in[AZ_MQTT_SCHANNEL_IO_BUFFER_SIZE];
  int32_t encrypted_in_len;

  uint8_t decrypted_pending[AZ_MQTT_SCHANNEL_IO_BUFFER_SIZE];
  int32_t decrypted_pending_len;
  int32_t decrypted_pending_pos;

  uint8_t tls_send_buf[AZ_MQTT_SCHANNEL_IO_BUFFER_SIZE];

  /** @brief Handshake: TLS requested, server name, trust anchor (NULL: system store). */
  bool tls_requested;
  bool handshake_needs_read;
  char host[256];
  HCERTSTORE custom_root;
#endif
};

AZ_NODISCARD int32_t az_mqtt_transport_sizeof(void) { return (int32_t)sizeof(az_mqtt_transport); }

static bool _wsa_inited = false;

static az_result _ensure_wsa(void)
{
  if (!_wsa_inited)
  {
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
      return AZ_MQTT_ERROR_TRANSPORT;
    }
    _wsa_inited = true;
  }
  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt_transport_init(az_mqtt_transport* transport)
{
  _az_PRECONDITION_NOT_NULL(transport);

  az_result rc = _ensure_wsa();
  if (az_result_failed(rc))
  {
    return rc;
  }

  memset(transport, 0, sizeof(*transport));
  transport->socket_fd = INVALID_SOCKET;
  return AZ_OK;
}

static az_result _span_to_cstr(az_span src, char* buf, int32_t buf_size)
{
  int32_t len = az_span_size(src);
  if (len >= buf_size)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  memcpy(buf, az_span_ptr(src), (size_t)len);
  buf[len] = '\0';
  return AZ_OK;
}

static int64_t _now_ms(void) { return (int64_t)GetTickCount64(); }

/** @brief Deadline @p timeout_ms from now; -1 (none) for a negative timeout. */
static int64_t _deadline(int32_t timeout_ms) { return timeout_ms < 0 ? -1 : _now_ms() + timeout_ms; }

/** @brief Milliseconds left until @p deadline_ms, 0 if passed, -1 if none. */
static int32_t _remaining(int64_t deadline_ms)
{
  if (deadline_ms < 0)
  {
    return -1;
  }
  int64_t const left = deadline_ms - _now_ms();
  return left <= 0 ? 0 : (left > INT32_MAX ? INT32_MAX : (int32_t)left);
}

static void _release_addresses(az_mqtt_transport* transport)
{
  if (transport->addresses != NULL)
  {
    freeaddrinfo(transport->addresses);
  }
  transport->addresses = NULL;
  transport->next_address = NULL;
}

/** @brief Report one native error to the callback, if any. */
static void _report_error(
    az_mqtt_transport* transport,
    az_mqtt_native_error_source source,
    int32_t code,
    az_result result)
{
  if (transport->error_callback != NULL)
  {
    az_mqtt_native_error const error = { source, code, result, transport->connect_attempt };
    transport->error_callback(&error, transport->error_context);
  }
}

/**
 * @brief The result for WSA error @p err (0: orderly close): AZ_MQTT_ERROR_CONNECTION_CLOSED,
 * AZ_MQTT_ERROR_CONNECTION_REFUSED or AZ_MQTT_ERROR_TRANSPORT.
 */
static az_result _wsa_result(int err)
{
  switch (err)
  {
    case 0:
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAENOTCONN:
    case WSAESHUTDOWN:
      return AZ_MQTT_ERROR_CONNECTION_CLOSED;
    case WSAECONNREFUSED:
      return AZ_MQTT_ERROR_CONNECTION_REFUSED;
    default:
      return AZ_MQTT_ERROR_TRANSPORT;
  }
}

/**
 * @brief _wsa_result() of @p err, reported unless @p err is 0 (as AZ_MQTT_ERROR_TLS_HANDSHAKE
 * during the handshake).
 */
static az_result _socket_error(az_mqtt_transport* transport, int err)
{
  az_result const rc = transport->in_handshake ? AZ_MQTT_ERROR_TLS_HANDSHAKE : _wsa_result(err);
  transport->last_socket_error = err;
  if (err != 0)
  {
    _report_error(transport, AZ_MQTT_NATIVE_ERROR_SOCKET, err, rc);
  }
  return rc;
}

/** @brief Start a non-blocking connect to the next untried address. */
static az_result _tcp_connect_next(az_mqtt_transport* transport)
{
  while (transport->next_address != NULL)
  {
    struct addrinfo* const a = transport->next_address;
    transport->next_address = a->ai_next;
    SOCKET fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd == INVALID_SOCKET)
    {
      _socket_error(transport, WSAGetLastError());
      continue;
    }
    u_long non_blocking = 1;
    if (ioctlsocket(fd, FIONBIO, &non_blocking) == 0
        && (connect(fd, a->ai_addr, (int)a->ai_addrlen) == 0
            || WSAGetLastError() == WSAEWOULDBLOCK))
    {
      transport->socket_fd = fd;
      transport->attempt_start_ms = _now_ms();
      return AZ_OK;
    }
    _socket_error(transport, WSAGetLastError());
    closesocket(fd);
  }
  return AZ_MQTT_ERROR_TRANSPORT;
}

/** @brief Resolve @p host (the one blocking step) and start connecting to the first address. */
static az_result _tcp_connect_start(az_mqtt_transport* transport, az_span host, uint16_t port)
{
  char host_str[256];
  az_result rc = _span_to_cstr(host, host_str, (int32_t)sizeof(host_str));
  if (az_result_failed(rc))
  {
    return rc;
  }

  char port_str[6];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* res = NULL;
  int const resolved = getaddrinfo(host_str, port_str, &hints, &res);
  if (resolved != 0 || res == NULL)
  {
    _report_error(
        transport, AZ_MQTT_NATIVE_ERROR_NAME_RESOLUTION, resolved, AZ_MQTT_ERROR_NAME_RESOLUTION);
    return AZ_MQTT_ERROR_NAME_RESOLUTION;
  }
  transport->addresses = res;
  transport->next_address = res;
  rc = _tcp_connect_next(transport);
  if (az_result_failed(rc))
  {
    rc = _wsa_result(transport->last_socket_error);
    _release_addresses(transport);
  }
  return rc;
}

/**
 * @brief Wait up to @p deadline_ms for the TCP connect; on success the socket is
 * made blocking again, with AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS on sends.
 *
 * An address that neither connects nor fails within
 * AZ_MQTT_TRANSPORT_ADDRESS_ATTEMPT_MS is abandoned while others remain.
 *
 * @retval AZ_MQTT_ERROR_TIMEOUT Still connecting.
 */
static az_result _tcp_connect_poll(az_mqtt_transport* transport, int64_t deadline_ms)
{
  while (transport->socket_fd != INVALID_SOCKET)
  {
    int32_t wait_ms = _remaining(deadline_ms);
    bool const bounded_attempt = transport->next_address != NULL;
    if (bounded_attempt)
    {
      int32_t const attempt_left
          = _remaining(transport->attempt_start_ms + AZ_MQTT_TRANSPORT_ADDRESS_ATTEMPT_MS);
      if (wait_ms < 0 || attempt_left < wait_ms)
      {
        wait_ms = attempt_left;
      }
    }

    fd_set write_fds;
    fd_set error_fds;
    FD_ZERO(&write_fds);
    FD_ZERO(&error_fds);
    FD_SET(transport->socket_fd, &write_fds);
    FD_SET(transport->socket_fd, &error_fds);
    struct timeval tv;
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;
    int const sel = select(0, NULL, &write_fds, &error_fds, wait_ms < 0 ? NULL : &tv);
    if (sel < 0)
    {
      return _socket_error(transport, WSAGetLastError());
    }
    if (sel == 0
        && !(bounded_attempt
             && _now_ms() - transport->attempt_start_ms >= AZ_MQTT_TRANSPORT_ADDRESS_ATTEMPT_MS))
    {
      return AZ_MQTT_ERROR_TIMEOUT;
    }

    int err = 0;
    int len = (int)sizeof(err);
    if (sel > 0 && FD_ISSET(transport->socket_fd, &write_fds)
        && getsockopt(transport->socket_fd, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0)
    {
      u_long blocking = 0;
      DWORD const send_timeout_ms = AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS;
      if (ioctlsocket(transport->socket_fd, FIONBIO, &blocking) != 0
          || setsockopt(
                 transport->socket_fd,
                 SOL_SOCKET,
                 SO_SNDTIMEO,
                 (char const*)&send_timeout_ms,
                 (int)sizeof(send_timeout_ms))
              != 0)
      {
        return _socket_error(transport, WSAGetLastError());
      }
      _release_addresses(transport);
      return AZ_OK;
    }

    // Failed, or its attempt budget ran out: move on to the next address.
    _socket_error(transport, sel == 0 ? WSAETIMEDOUT : (err != 0 ? err : WSAGetLastError()));
    closesocket(transport->socket_fd);
    transport->socket_fd = INVALID_SOCKET;
    if (az_result_failed(_tcp_connect_next(transport)))
    {
      break;
    }
  }
  return _wsa_result(transport->last_socket_error);
}

static az_result _socket_wait_readable(az_mqtt_transport* transport, int32_t timeout_ms, bool* out_ready)
{
  SOCKET const fd = transport->socket_fd;
  fd_set read_fds;
  FD_ZERO(&read_fds);
  FD_SET(fd, &read_fds);

  struct timeval tv;
  struct timeval* tv_ptr = NULL;

  if (timeout_ms >= 0)
  {
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    tv_ptr = &tv;
  }

  int sel = select(0 /* ignored on Windows */, &read_fds, NULL, NULL, tv_ptr);
  if (sel < 0)
  {
    return _socket_error(transport, WSAGetLastError());
  }

  *out_ready = (sel > 0);
  return AZ_OK;
}

/**
 * @brief Receive what is available within @p timeout_ms (*out_n 0: nothing yet).
 * @retval AZ_MQTT_ERROR_CONNECTION_CLOSED The peer closed the connection.
 */
static az_result _socket_recv_timeout(
    az_mqtt_transport* transport,
    uint8_t* buf,
    int32_t capacity,
    int32_t timeout_ms,
    int32_t* out_n)
{
  bool ready = false;
  az_result rc = _socket_wait_readable(transport, timeout_ms, &ready);
  if (az_result_failed(rc))
  {
    return rc;
  }
  if (!ready)
  {
    *out_n = 0;
    return AZ_OK;
  }

  int n = recv(transport->socket_fd, (char*)buf, capacity, 0);
  if (n <= 0)
  {
    transport->connected = false;
    return _socket_error(transport, n == 0 ? 0 : WSAGetLastError());
  }

  *out_n = n;
  return AZ_OK;
}

static az_result _socket_send_all(az_mqtt_transport* transport, uint8_t const* data, int32_t len)
{
  int32_t sent_total = 0;
  while (sent_total < len)
  {
    int n = send(transport->socket_fd, (char const*)data + sent_total, len - sent_total, 0);
    if (n <= 0)
    {
      int const err = WSAGetLastError();
      if (err == WSAETIMEDOUT && !transport->in_handshake) // SO_SNDTIMEO elapsed.
      {
        _report_error(transport, AZ_MQTT_NATIVE_ERROR_SOCKET, err, AZ_MQTT_ERROR_TIMEOUT);
        return AZ_MQTT_ERROR_TIMEOUT;
      }
      return _socket_error(transport, err);
    }
    sent_total += n;
  }
  return AZ_OK;
}

#ifdef AZ_MQTT_TLS_SCHANNEL
static bool _handle_is_valid(SecHandle const* handle)
{
  return (handle->dwLower != 0 || handle->dwUpper != 0);
}

static void _reset_security_handle(SecHandle* handle)
{
  handle->dwLower = 0;
  handle->dwUpper = 0;
}

static az_result _host_to_wstr(az_span host, wchar_t* out_host, int32_t out_host_len)
{
  char host_ascii[256];
  az_result rc = _span_to_cstr(host, host_ascii, (int32_t)sizeof(host_ascii));
  if (az_result_failed(rc))
  {
    return rc;
  }

  int wlen = MultiByteToWideChar(CP_UTF8, 0, host_ascii, -1, out_host, out_host_len);
  if (wlen <= 0)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  return AZ_OK;
}

/** @brief Report @p err (Win32 error, SECURITY_STATUS) as a TLS error; AZ_MQTT_ERROR_TRANSPORT. */
static az_result _tls_setup_failure(az_mqtt_transport* transport, DWORD err)
{
  _report_error(transport, AZ_MQTT_NATIVE_ERROR_TLS, (int32_t)err, AZ_MQTT_ERROR_TRANSPORT);
  return AZ_MQTT_ERROR_TRANSPORT;
}

static az_result _decode_cert_file_to_der(
    az_mqtt_tls_options const* tls_options,
    uint8_t* der_out,
    DWORD der_capacity,
    DWORD* out_der_len,
    az_mqtt_transport* transport)
{
  char cert_path[260];
  az_result rc = _span_to_cstr(tls_options->ca_cert_path, cert_path, (int32_t)sizeof(cert_path));
  if (az_result_failed(rc))
  {
    return rc;
  }

  HANDLE h = CreateFileA(cert_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
  {
    return _tls_setup_failure(transport, GetLastError());
  }

  LARGE_INTEGER file_size;
  BOOL const size_ok = GetFileSizeEx(h, &file_size);
  if (!size_ok || file_size.QuadPart <= 0 || file_size.QuadPart > 64 * 1024)
  {
    DWORD const err = size_ok ? ERROR_INVALID_DATA : GetLastError(); // Empty or over 64 KiB.
    CloseHandle(h);
    return _tls_setup_failure(transport, err);
  }

  uint32_t file_len = (uint32_t)file_size.QuadPart;
  char file_buf[(64 * 1024) + 1];
  DWORD read_bytes = 0;
  BOOL ok = ReadFile(h, file_buf, file_len, &read_bytes, NULL);
  DWORD const read_error = ok ? ERROR_HANDLE_EOF : GetLastError(); // Short read: EOF.
  CloseHandle(h);
  if (!ok || read_bytes != file_len)
  {
    return _tls_setup_failure(transport, read_error);
  }

  file_buf[file_len] = '\0';

  if (strstr(file_buf, "-----BEGIN CERTIFICATE-----") != NULL)
  {
    DWORD decoded_len = 0;
    if (!CryptStringToBinaryA(file_buf, file_len, CRYPT_STRING_BASE64HEADER, NULL, &decoded_len, NULL, NULL))
    {
      return _tls_setup_failure(transport, GetLastError());
    }

    if (decoded_len > der_capacity)
    {
      return AZ_ERROR_NOT_ENOUGH_SPACE;
    }

    if (!CryptStringToBinaryA(
            file_buf,
            file_len,
            CRYPT_STRING_BASE64HEADER,
            der_out,
            &decoded_len,
            NULL,
            NULL))
    {
      return _tls_setup_failure(transport, GetLastError());
    }

    *out_der_len = decoded_len;
    return AZ_OK;
  }

  if (file_len > der_capacity)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }

  memcpy(der_out, file_buf, file_len);
  *out_der_len = file_len;
  return AZ_OK;
}

static az_result _build_custom_root_store(
    az_mqtt_tls_options const* tls_options,
    HCERTSTORE* out_store,
    az_mqtt_transport* transport)
{
  *out_store = NULL;

  if (az_span_size(tls_options->ca_cert_path) == 0)
  {
    return AZ_OK;
  }

  uint8_t der_buf[64 * 1024];
  DWORD der_len = 0;
  az_result rc
      = _decode_cert_file_to_der(tls_options, der_buf, (DWORD)sizeof(der_buf), &der_len, transport);
  if (az_result_failed(rc))
  {
    return rc;
  }

  HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, X509_ASN_ENCODING, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
  if (store == NULL)
  {
    return _tls_setup_failure(transport, GetLastError());
  }

  if (!CertAddEncodedCertificateToStore(
          store,
          X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
          der_buf,
          der_len,
          CERT_STORE_ADD_REPLACE_EXISTING,
          NULL))
  {
    DWORD const err = GetLastError();
    CertCloseStore(store, 0);
    return _tls_setup_failure(transport, err);
  }

  *out_store = store;
  return AZ_OK;
}

/** @brief Check @p server_cert chains to @p custom_root (NULL: system store) and names @p host. */
static az_result _validate_server_certificate(
    char const* host,
    HCERTSTORE custom_root,
    PCCERT_CONTEXT server_cert,
    az_mqtt_transport* transport)
{
  wchar_t host_w[256];
  az_result rc = _host_to_wstr(
      az_span_create_from_str((char*)(uintptr_t)host),
      host_w,
      (int32_t)sizeof(host_w) / (int32_t)sizeof(host_w[0]));
  if (az_result_failed(rc))
  {
    return rc;
  }

  HCERTCHAINENGINE chain_engine = NULL;
  if (custom_root != NULL)
  {
    CERT_CHAIN_ENGINE_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cbSize = sizeof(cfg);
    cfg.hExclusiveRoot = custom_root;

    if (!CertCreateCertificateChainEngine(&cfg, &chain_engine))
    {
      return _tls_setup_failure(transport, GetLastError());
    }
  }

  CERT_CHAIN_PARA chain_para;
  memset(&chain_para, 0, sizeof(chain_para));
  chain_para.cbSize = sizeof(chain_para);

  PCCERT_CHAIN_CONTEXT chain_ctx = NULL;
  BOOL chain_ok = CertGetCertificateChain(
      chain_engine,
      server_cert,
      NULL,
      server_cert->hCertStore,
      &chain_para,
      0,
      NULL,
      &chain_ctx);

  if (!chain_ok || chain_ctx == NULL)
  {
    DWORD const chain_error = GetLastError();
    _report_error(
        transport, AZ_MQTT_NATIVE_ERROR_TLS_VERIFY, (int32_t)chain_error, AZ_MQTT_ERROR_TLS_VERIFY);
    if (chain_engine != NULL)
    {
      CertFreeCertificateChainEngine(chain_engine);
    }
    return AZ_MQTT_ERROR_TLS_VERIFY;
  }

  HTTPSPolicyCallbackData https_policy;
  memset(&https_policy, 0, sizeof(https_policy));
  https_policy.cbStruct = sizeof(https_policy);
  https_policy.dwAuthType = AUTHTYPE_SERVER;
  https_policy.pwszServerName = host_w;

  CERT_CHAIN_POLICY_PARA policy_para;
  memset(&policy_para, 0, sizeof(policy_para));
  policy_para.cbSize = sizeof(policy_para);
  policy_para.pvExtraPolicyPara = &https_policy;

  CERT_CHAIN_POLICY_STATUS policy_status;
  memset(&policy_status, 0, sizeof(policy_status));
  policy_status.cbSize = sizeof(policy_status);

  BOOL policy_ok = CertVerifyCertificateChainPolicy(
      CERT_CHAIN_POLICY_SSL, chain_ctx, &policy_para, &policy_status);
  DWORD const policy_error = policy_ok ? policy_status.dwError : GetLastError(); // Before cleanup.

  CertFreeCertificateChain(chain_ctx);
  if (chain_engine != NULL)
  {
    CertFreeCertificateChainEngine(chain_engine);
  }

  if (!policy_ok || policy_status.dwError != 0)
  {
    _report_error(
        transport,
        AZ_MQTT_NATIVE_ERROR_TLS_VERIFY,
        (int32_t)policy_error,
        AZ_MQTT_ERROR_TLS_VERIFY);
    return AZ_MQTT_ERROR_TLS_VERIFY;
  }

  return AZ_OK;
}

/** @brief Acquire credentials and keep what the handshake needs; reads @p tls_options only here. */
static az_result _schannel_prepare(
    az_mqtt_transport* transport,
    az_span host,
    az_mqtt_tls_options const* tls_options)
{
  _reset_security_handle(&transport->cred_handle);
  _reset_security_handle(&transport->context_handle);
  transport->encrypted_in_len = 0;
  transport->handshake_needs_read = false;

  az_result rc = _span_to_cstr(host, transport->host, (int32_t)sizeof(transport->host));
  if (az_result_failed(rc))
  {
    return rc;
  }
  rc = _build_custom_root_store(tls_options, &transport->custom_root, transport);
  if (az_result_failed(rc))
  {
    return rc;
  }

  SCHANNEL_CRED cred;
  memset(&cred, 0, sizeof(cred));
  cred.dwVersion = SCHANNEL_CRED_VERSION;
  cred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_USE_STRONG_CRYPTO;

  TimeStamp expiry;
  SECURITY_STATUS sec = AcquireCredentialsHandleA(
      NULL,
      UNISP_NAME_A,
      SECPKG_CRED_OUTBOUND,
      NULL,
      &cred,
      NULL,
      NULL,
      &transport->cred_handle,
      &expiry);
  if (sec != SEC_E_OK)
  {
    return _tls_setup_failure(transport, (DWORD)sec);
  }
  transport->has_cred = true;
  transport->tls_requested = true;
  return AZ_OK;
}

static az_result _schannel_handshake_steps(az_mqtt_transport* transport, int64_t deadline_ms);

/**
 * @brief Progress the TLS handshake until done, failed or @p deadline_ms.
 *
 * Resumable: a wait for server data that reaches the deadline returns
 * AZ_MQTT_ERROR_TIMEOUT and the next call continues from there.
 */
static az_result _schannel_handshake_steps(az_mqtt_transport* transport, int64_t deadline_ms)
{
  DWORD const req_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY
      | ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
  DWORD out_flags = 0;
  TimeStamp expiry;
  az_result rc;

  for (;;)
  {
    if (transport->handshake_needs_read)
    {
      if (transport->encrypted_in_len == (int32_t)sizeof(transport->encrypted_in))
      {
        return AZ_MQTT_ERROR_TLS_HANDSHAKE;
      }
      bool ready = false;
      rc = _socket_wait_readable(transport, _remaining(deadline_ms), &ready);
      if (az_result_failed(rc))
      {
        return AZ_MQTT_ERROR_TLS_HANDSHAKE;
      }
      if (!ready)
      {
        return AZ_MQTT_ERROR_TIMEOUT;
      }
      int const n = recv(
          transport->socket_fd,
          (char*)transport->encrypted_in + transport->encrypted_in_len,
          (int)sizeof(transport->encrypted_in) - transport->encrypted_in_len,
          0);
      if (n <= 0)
      {
        _socket_error(transport, n == 0 ? 0 : WSAGetLastError()); // Error or closed mid-handshake.
        return AZ_MQTT_ERROR_TLS_HANDSHAKE;
      }
      transport->encrypted_in_len += n;
      transport->handshake_needs_read = false;
    }

    SecBuffer out_buf;
    SecBufferDesc out_desc;
    memset(&out_buf, 0, sizeof(out_buf));
    memset(&out_desc, 0, sizeof(out_desc));
    out_buf.BufferType = SECBUFFER_TOKEN;
    out_desc.ulVersion = SECBUFFER_VERSION;
    out_desc.cBuffers = 1;
    out_desc.pBuffers = &out_buf;

    SECURITY_STATUS status;
    CtxtHandle new_context;
    _reset_security_handle(&new_context);

    if (!transport->has_context)
    {
      status = InitializeSecurityContextA(
          &transport->cred_handle,
          NULL,
          transport->host,
          req_flags,
          0,
          SECURITY_NATIVE_DREP,
          NULL,
          0,
          &new_context,
          &out_desc,
          &out_flags,
          &expiry);
    }
    else
    {
      SecBuffer in_bufs[2];
      SecBufferDesc in_desc;
      memset(&in_bufs, 0, sizeof(in_bufs));
      memset(&in_desc, 0, sizeof(in_desc));
      in_bufs[0].BufferType = SECBUFFER_TOKEN;
      in_bufs[0].pvBuffer = transport->encrypted_in;
      in_bufs[0].cbBuffer = (unsigned long)transport->encrypted_in_len;
      in_bufs[1].BufferType = SECBUFFER_EMPTY;
      in_desc.ulVersion = SECBUFFER_VERSION;
      in_desc.cBuffers = 2;
      in_desc.pBuffers = in_bufs;

      status = InitializeSecurityContextA(
          &transport->cred_handle,
          &transport->context_handle,
          transport->host,
          req_flags,
          0,
          SECURITY_NATIVE_DREP,
          &in_desc,
          0,
          &new_context,
          &out_desc,
          &out_flags,
          &expiry);

      if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED)
      {
        if (in_bufs[1].BufferType == SECBUFFER_EXTRA)
        {
          memmove(
              transport->encrypted_in,
              transport->encrypted_in + (transport->encrypted_in_len - (int32_t)in_bufs[1].cbBuffer),
              in_bufs[1].cbBuffer);
          transport->encrypted_in_len = (int32_t)in_bufs[1].cbBuffer;
        }
        else
        {
          transport->encrypted_in_len = 0;
        }
      }

      if (status == SEC_E_INCOMPLETE_MESSAGE)
      {
        transport->handshake_needs_read = true;
        continue;
      }
    }

    if ((status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED) && _handle_is_valid(&new_context))
    {
      transport->context_handle = new_context;
      transport->has_context = true;
    }

    if (out_buf.cbBuffer > 0 && out_buf.pvBuffer != NULL)
    {
      rc = _socket_send_all(transport, (uint8_t const*)out_buf.pvBuffer, (int32_t)out_buf.cbBuffer);
      FreeContextBuffer(out_buf.pvBuffer);
      if (az_result_failed(rc))
      {
        return AZ_MQTT_ERROR_TLS_HANDSHAKE;
      }
    }

    if (status == SEC_E_OK)
    {
      break;
    }
    if (status != SEC_I_CONTINUE_NEEDED)
    {
      _report_error(
          transport, AZ_MQTT_NATIVE_ERROR_TLS, (int32_t)status, AZ_MQTT_ERROR_TLS_HANDSHAKE);
      return AZ_MQTT_ERROR_TLS_HANDSHAKE;
    }
    transport->handshake_needs_read = transport->encrypted_in_len == 0;
  }

  SECURITY_STATUS sec = QueryContextAttributesA(
      &transport->context_handle,
      SECPKG_ATTR_STREAM_SIZES,
      &transport->stream_sizes);
  if (sec != SEC_E_OK)
  {
    _report_error(transport, AZ_MQTT_NATIVE_ERROR_TLS, (int32_t)sec, AZ_MQTT_ERROR_TLS_HANDSHAKE);
    return AZ_MQTT_ERROR_TLS_HANDSHAKE;
  }

  // SCH_CRED_MANUAL_CRED_VALIDATION turns Schannel's own check off, so the
  // chain and host name must be validated here before any MQTT byte is sent.
  PCCERT_CONTEXT server_cert = NULL;
  sec = QueryContextAttributesA(
      &transport->context_handle, SECPKG_ATTR_REMOTE_CERT_CONTEXT, (PVOID)&server_cert);
  if (sec != SEC_E_OK || server_cert == NULL)
  {
    _report_error(transport, AZ_MQTT_NATIVE_ERROR_TLS, (int32_t)sec, AZ_MQTT_ERROR_TLS_HANDSHAKE);
    return AZ_MQTT_ERROR_TLS_HANDSHAKE;
  }
  rc = _validate_server_certificate(
      transport->host, transport->custom_root, server_cert, transport);
  CertFreeCertificateContext(server_cert);
  if (az_result_failed(rc))
  {
    return rc;
  }

  transport->tls_active = true;
  transport->decrypted_pending_len = 0;
  transport->decrypted_pending_pos = 0;
  return AZ_OK;
}

/** @brief _schannel_handshake_steps(), with its socket errors reported as part of the handshake. */
static az_result _schannel_handshake(az_mqtt_transport* transport, int64_t deadline_ms)
{
  transport->in_handshake = true;
  az_result const rc = _schannel_handshake_steps(transport, deadline_ms);
  transport->in_handshake = false;
  return rc;
}

static az_result _schannel_send(az_mqtt_transport* transport, az_span data)
{
  uint8_t* ptr = az_span_ptr(data);
  int32_t remaining = az_span_size(data);

  while (remaining > 0)
  {
    int32_t max_payload = (int32_t)transport->stream_sizes.cbMaximumMessage;
    int32_t chunk = remaining < max_payload ? remaining : max_payload;

    int32_t header = (int32_t)transport->stream_sizes.cbHeader;
    int32_t trailer = (int32_t)transport->stream_sizes.cbTrailer;
    int32_t total = header + chunk + trailer;

    if (total > (int32_t)sizeof(transport->tls_send_buf))
    {
      return AZ_ERROR_NOT_ENOUGH_SPACE;
    }

    memcpy(transport->tls_send_buf + header, ptr, (size_t)chunk);

    SecBuffer bufs[4];
    SecBufferDesc desc;
    memset(&bufs, 0, sizeof(bufs));
    memset(&desc, 0, sizeof(desc));

    bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
    bufs[0].pvBuffer = transport->tls_send_buf;
    bufs[0].cbBuffer = (unsigned long)header;

    bufs[1].BufferType = SECBUFFER_DATA;
    bufs[1].pvBuffer = transport->tls_send_buf + header;
    bufs[1].cbBuffer = (unsigned long)chunk;

    bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
    bufs[2].pvBuffer = transport->tls_send_buf + header + chunk;
    bufs[2].cbBuffer = (unsigned long)trailer;

    bufs[3].BufferType = SECBUFFER_EMPTY;

    desc.ulVersion = SECBUFFER_VERSION;
    desc.cBuffers = 4;
    desc.pBuffers = bufs;

    SECURITY_STATUS sec = EncryptMessage(&transport->context_handle, 0, &desc, 0);
    if (sec != SEC_E_OK)
    {
      _report_error(transport, AZ_MQTT_NATIVE_ERROR_TLS, (int32_t)sec, AZ_MQTT_ERROR_TRANSPORT);
      return AZ_MQTT_ERROR_TRANSPORT;
    }

    int32_t to_send = (int32_t)(bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer);
    az_result rc = _socket_send_all(transport, transport->tls_send_buf, to_send);
    if (az_result_failed(rc))
    {
      return rc;
    }

    ptr += chunk;
    remaining -= chunk;
  }

  return AZ_OK;
}

static az_result _schannel_consume_decrypted(az_mqtt_transport* transport, az_span buffer, az_span* out_received)
{
  int32_t available = transport->decrypted_pending_len - transport->decrypted_pending_pos;
  if (available <= 0)
  {
    *out_received = AZ_SPAN_EMPTY;
    return AZ_OK;
  }

  int32_t to_copy = available;
  if (to_copy > az_span_size(buffer))
  {
    to_copy = az_span_size(buffer);
  }

  memcpy(
      az_span_ptr(buffer),
      transport->decrypted_pending + transport->decrypted_pending_pos,
      (size_t)to_copy);
  transport->decrypted_pending_pos += to_copy;

  if (transport->decrypted_pending_pos >= transport->decrypted_pending_len)
  {
    transport->decrypted_pending_len = 0;
    transport->decrypted_pending_pos = 0;
  }

  *out_received = az_span_slice(buffer, 0, to_copy);
  return AZ_OK;
}

static az_result _schannel_receive(
    az_mqtt_transport* transport,
    az_span buffer,
    int32_t timeout_ms,
    az_span* out_received)
{
  az_result rc = _schannel_consume_decrypted(transport, buffer, out_received);
  if (az_result_failed(rc) || az_span_size(*out_received) > 0)
  {
    return rc;
  }

  for (;;)
  {
    if (transport->encrypted_in_len > 0)
    {
      SecBuffer bufs[4];
      SecBufferDesc desc;
      memset(&bufs, 0, sizeof(bufs));
      memset(&desc, 0, sizeof(desc));

      bufs[0].BufferType = SECBUFFER_DATA;
      bufs[0].pvBuffer = transport->encrypted_in;
      bufs[0].cbBuffer = (unsigned long)transport->encrypted_in_len;
      bufs[1].BufferType = SECBUFFER_EMPTY;
      bufs[2].BufferType = SECBUFFER_EMPTY;
      bufs[3].BufferType = SECBUFFER_EMPTY;

      desc.ulVersion = SECBUFFER_VERSION;
      desc.cBuffers = 4;
      desc.pBuffers = bufs;

      SECURITY_STATUS sec = DecryptMessage(&transport->context_handle, &desc, 0, NULL);
      if (sec == SEC_E_OK)
      {
        SecBuffer* data_buf = NULL;
        SecBuffer* extra_buf = NULL;

        for (int i = 0; i < 4; ++i)
        {
          if (bufs[i].BufferType == SECBUFFER_DATA)
          {
            data_buf = &bufs[i];
          }
          else if (bufs[i].BufferType == SECBUFFER_EXTRA)
          {
            extra_buf = &bufs[i];
          }
        }

        if (data_buf != NULL && data_buf->cbBuffer > 0)
        {
          if ((int32_t)data_buf->cbBuffer > (int32_t)sizeof(transport->decrypted_pending))
          {
            return AZ_ERROR_NOT_ENOUGH_SPACE;
          }

          memcpy(transport->decrypted_pending, data_buf->pvBuffer, data_buf->cbBuffer);
          transport->decrypted_pending_len = (int32_t)data_buf->cbBuffer;
          transport->decrypted_pending_pos = 0;
        }

        if (extra_buf != NULL && extra_buf->cbBuffer > 0)
        {
          memmove(
              transport->encrypted_in,
              transport->encrypted_in + (transport->encrypted_in_len - (int32_t)extra_buf->cbBuffer),
              extra_buf->cbBuffer);
          transport->encrypted_in_len = (int32_t)extra_buf->cbBuffer;
        }
        else
        {
          transport->encrypted_in_len = 0;
        }

        if (transport->decrypted_pending_len > 0)
        {
          return _schannel_consume_decrypted(transport, buffer, out_received);
        }

        continue;
      }
      else if (sec == SEC_E_INCOMPLETE_MESSAGE)
      {
        // Need more encrypted bytes from socket.
      }
      else if (sec == SEC_I_CONTEXT_EXPIRED)
      {
        transport->connected = false; // close_notify
        return _socket_error(transport, 0);
      }
      else
      {
        _report_error(transport, AZ_MQTT_NATIVE_ERROR_TLS, (int32_t)sec, AZ_MQTT_ERROR_TRANSPORT);
        return AZ_MQTT_ERROR_TRANSPORT;
      }
    }

    if (transport->encrypted_in_len >= (int32_t)sizeof(transport->encrypted_in))
    {
      return AZ_ERROR_NOT_ENOUGH_SPACE;
    }

    int32_t n = 0;
    rc = _socket_recv_timeout(
        transport,
        transport->encrypted_in + transport->encrypted_in_len,
        (int32_t)sizeof(transport->encrypted_in) - transport->encrypted_in_len,
        timeout_ms,
        &n);
    if (az_result_failed(rc))
    {
      return rc;
    }

    if (n == 0)
    {
      *out_received = AZ_SPAN_EMPTY;
      return AZ_OK;
    }

    transport->encrypted_in_len += n;
  }
}

static void _schannel_shutdown(az_mqtt_transport* transport)
{
  if (transport->has_context && _handle_is_valid(&transport->context_handle))
  {
    DeleteSecurityContext(&transport->context_handle);
    _reset_security_handle(&transport->context_handle);
  }

  if (transport->has_cred && _handle_is_valid(&transport->cred_handle))
  {
    FreeCredentialsHandle(&transport->cred_handle);
    _reset_security_handle(&transport->cred_handle);
  }

  if (transport->remote_server_cert != NULL)
  {
    CertFreeCertificateContext(transport->remote_server_cert);
    transport->remote_server_cert = NULL;
  }

  if (transport->custom_root != NULL)
  {
    CertCloseStore(transport->custom_root, 0);
    transport->custom_root = NULL;
  }

  transport->tls_active = false;
  transport->tls_requested = false;
  transport->handshake_needs_read = false;
  transport->has_context = false;
  transport->has_cred = false;
  transport->encrypted_in_len = 0;
  transport->decrypted_pending_len = 0;
  transport->decrypted_pending_pos = 0;
}
#endif // AZ_MQTT_TLS_SCHANNEL

AZ_NODISCARD int64_t az_mqtt_transport_clock_ms(void) { return _now_ms(); }

AZ_NODISCARD az_result az_mqtt_transport_connect_start(
    az_mqtt_transport* transport,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options)
{
  _az_PRECONDITION_NOT_NULL(transport);

  az_mqtt_transport_close(transport);
  transport->connect_attempt++;

#ifdef AZ_MQTT_TLS_SCHANNEL
  if (tls_options != NULL)
  {
    az_result rc = az_mqtt_tls_options_check(tls_options);
    if (az_result_failed(rc))
    {
      return rc; // Same contract as the other backends.
    }
    // Not implemented for Schannel yet: refuse rather than connect without
    // the identity, trust or hook the caller asked for.
    if (az_span_size(tls_options->client_cert_path) > 0
        || az_span_size(tls_options->client_cert_pem) > 0
        || az_span_size(tls_options->ca_cert_pem) > 0 || tls_options->configure != NULL)
    {
      return AZ_MQTT_ERROR_NOT_SUPPORTED;
    }
    rc = _schannel_prepare(transport, host, tls_options);
    if (az_result_failed(rc))
    {
      az_mqtt_transport_close(transport);
      return rc;
    }
  }
#else
  if (tls_options != NULL)
  {
    // Built without TLS: never fall back to plaintext.
    return AZ_MQTT_ERROR_NOT_SUPPORTED;
  }
#endif

  az_result rc = AZ_OK;
  transport->attempt_proxy = transport->proxy;
#ifndef AZ_MQTT_NO_PROXY
  if (transport->attempt_proxy != NULL)
  {
    uint8_t request[_AZ_MQTT_HTTP_CONNECT_REQUEST_MAX];
    int32_t size = 0;
    rc = _az_mqtt_http_connect_request( // Only to validate host now.
        transport->attempt_proxy, host, port, AZ_SPAN_FROM_BUFFER(request), &size);
    az_span_fill(AZ_SPAN_FROM_BUFFER(request), 0);
    transport->proxy_target_host = host;
    transport->proxy_target_port = port;
    transport->proxy_request_sent = 0;
    transport->proxy_request_done = false;
    _az_mqtt_http_connect_reply_init(&transport->proxy_reply);
    host = transport->attempt_proxy->host;
    port = transport->attempt_proxy->port;
  }
#endif
  if (az_result_succeeded(rc))
  {
    rc = _tcp_connect_start(transport, host, port);
  }
  if (az_result_failed(rc))
  {
    az_mqtt_transport_close(transport);
    return rc;
  }
  transport->state = _WIN_TCP;
  return AZ_OK;
}

/** @brief The state once the connection (or tunnel) to the server is up. */
static _win_state _after_tcp_state(az_mqtt_transport const* transport)
{
#ifdef AZ_MQTT_TLS_SCHANNEL
  if (transport->tls_requested)
  {
    return _WIN_TLS;
  }
#else
  (void)transport;
#endif
  return _WIN_CONNECTED;
}

#ifndef AZ_MQTT_NO_PROXY
/** @brief Report a failure of the tunnel's connection (WSA error @p err; 0: closed) as PROXY. */
static az_result _proxy_socket_failure(az_mqtt_transport* transport, int err)
{
  if (err != 0)
  {
    _report_error(transport, AZ_MQTT_NATIVE_ERROR_SOCKET, err, AZ_MQTT_ERROR_PROXY);
  }
  return AZ_MQTT_ERROR_PROXY;
}

/** @brief Wait up to @p timeout_ms for @p fd to accept data: 1 ready, 0 not yet, -1 failed. */
static int _socket_wait_writable(SOCKET fd, int32_t timeout_ms)
{
  fd_set write_fds;
  FD_ZERO(&write_fds);
  FD_SET(fd, &write_fds);
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  int const sel = select(0, NULL, &write_fds, NULL, timeout_ms < 0 ? NULL : &tv);
  return sel < 0 ? -1 : (sel > 0 ? 1 : 0);
}

/**
 * @brief Send the rest of the CONNECT request by @p deadline_ms, without blocking past it.
 * @retval AZ_MQTT_ERROR_TIMEOUT Not all sent yet; proxy_request_sent keeps the progress.
 */
static az_result _proxy_send_request(az_mqtt_transport* transport, int64_t deadline_ms)
{
  if (transport->proxy_request_done)
  {
    return AZ_OK;
  }
  uint8_t request[_AZ_MQTT_HTTP_CONNECT_REQUEST_MAX];
  int32_t size = 0;
  az_result rc = _az_mqtt_http_connect_request(
      transport->attempt_proxy,
      transport->proxy_target_host,
      transport->proxy_target_port,
      AZ_SPAN_FROM_BUFFER(request),
      &size);
  u_long non_blocking = 1;
  if (az_result_succeeded(rc) && ioctlsocket(transport->socket_fd, FIONBIO, &non_blocking) != 0)
  {
    rc = _proxy_socket_failure(transport, WSAGetLastError());
  }
  while (az_result_succeeded(rc) && transport->proxy_request_sent < size)
  {
    int32_t const sent = transport->proxy_request_sent;
    int const n = send(transport->socket_fd, (char const*)request + sent, size - sent, 0);
    if (n > 0)
    {
      transport->proxy_request_sent += n;
      continue;
    }
    int err = WSAGetLastError();
    int const w = err == WSAEWOULDBLOCK
        ? _socket_wait_writable(transport->socket_fd, _remaining(deadline_ms))
        : -1;
    if (w < 0)
    {
      err = err == WSAEWOULDBLOCK ? WSAGetLastError() : err; // From select().
      rc = _proxy_socket_failure(transport, err);
    }
    else if (w == 0)
    {
      rc = AZ_MQTT_ERROR_TIMEOUT;
    }
  }
  az_span_fill(AZ_SPAN_FROM_BUFFER(request), 0); // It holds the credentials.
  u_long blocking = 0; // Sends after the tunnel rely on SO_SNDTIMEO.
  if (ioctlsocket(transport->socket_fd, FIONBIO, &blocking) != 0
      && (az_result_succeeded(rc) || rc == AZ_MQTT_ERROR_TIMEOUT))
  {
    rc = _proxy_socket_failure(transport, WSAGetLastError());
  }
  transport->proxy_request_done = az_result_succeeded(rc);
  return rc;
}

/**
 * @brief Open the HTTP CONNECT tunnel by @p deadline_ms; resumable. Reads nothing past the reply.
 */
static az_result _proxy_tunnel_poll(az_mqtt_transport* transport, int64_t deadline_ms)
{
  az_result const sent = _proxy_send_request(transport, deadline_ms);
  if (az_result_failed(sent))
  {
    return sent;
  }
  for (;;)
  {
    bool ready = false;
    az_result rc = _socket_wait_readable(transport, _remaining(deadline_ms), &ready);
    if (az_result_failed(rc))
    {
      return rc;
    }
    if (!ready)
    {
      return AZ_MQTT_ERROR_TIMEOUT;
    }
    // Peek, then take exactly the reply's bytes: what follows is the tunnelled stream's.
    char buffer[512];
    int const peeked = recv(transport->socket_fd, buffer, (int)sizeof(buffer), MSG_PEEK);
    if (peeked <= 0)
    {
      return _proxy_socket_failure(transport, peeked == 0 ? 0 : WSAGetLastError());
    }
    int32_t consumed = 0;
    rc = _az_mqtt_http_connect_reply_parse(
        &transport->proxy_reply, az_span_create((uint8_t*)buffer, peeked), &consumed);
    if (recv(transport->socket_fd, buffer, consumed, 0) != consumed)
    {
      return _proxy_socket_failure(transport, WSAGetLastError());
    }
    if (rc != AZ_MQTT_ERROR_TIMEOUT)
    {
      if (az_result_failed(rc))
      {
        _report_error(transport, AZ_MQTT_NATIVE_ERROR_PROXY, transport->proxy_reply.status, rc);
      }
      return rc;
    }
  }
}

#endif // AZ_MQTT_NO_PROXY

AZ_NODISCARD az_result
az_mqtt_transport_connect_poll(az_mqtt_transport* transport, int32_t timeout_ms)
{
  _az_PRECONDITION_NOT_NULL(transport);
  int64_t const deadline = _deadline(timeout_ms);
  az_result rc = AZ_OK;

  if (transport->state == _WIN_TCP)
  {
    rc = _tcp_connect_poll(transport, deadline);
    if (rc == AZ_MQTT_ERROR_TIMEOUT)
    {
      return rc;
    }
    if (az_result_succeeded(rc))
    {
      transport->state
          = transport->attempt_proxy != NULL ? _WIN_PROXY : _after_tcp_state(transport);
    }
  }

#ifndef AZ_MQTT_NO_PROXY
  if (az_result_succeeded(rc) && transport->state == _WIN_PROXY)
  {
    rc = _proxy_tunnel_poll(transport, deadline);
    if (rc == AZ_MQTT_ERROR_TIMEOUT)
    {
      return rc;
    }
    if (az_result_succeeded(rc))
    {
      transport->state = _after_tcp_state(transport);
    }
  }
#endif

#ifdef AZ_MQTT_TLS_SCHANNEL
  if (az_result_succeeded(rc) && transport->state == _WIN_TLS)
  {
    rc = _schannel_handshake(transport, deadline);
    if (rc == AZ_MQTT_ERROR_TIMEOUT)
    {
      return rc;
    }
    if (az_result_succeeded(rc))
    {
      transport->state = _WIN_CONNECTED;
    }
  }
#endif

  if (az_result_succeeded(rc) && transport->state != _WIN_CONNECTED)
  {
    rc = AZ_MQTT_ERROR_INVALID_STATE;
  }
  if (az_result_failed(rc))
  {
    az_mqtt_transport_close(transport);
    return rc;
  }
  transport->connected = true;
  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt_transport_connect(
    az_mqtt_transport* transport,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options)
{
  az_result rc = az_mqtt_transport_connect_start(transport, host, port, tls_options);
  if (az_result_succeeded(rc))
  {
    rc = az_mqtt_transport_connect_poll(transport, AZ_MQTT_TRANSPORT_CONNECT_TIMEOUT_MS);
    if (rc == AZ_MQTT_ERROR_TIMEOUT)
    {
      az_mqtt_transport_close(transport);
    }
  }
  return rc;
}

AZ_NODISCARD az_result az_mqtt_transport_send(az_mqtt_transport* transport, az_span data)
{
  _az_PRECONDITION_NOT_NULL(transport);

#ifdef AZ_MQTT_TLS_SCHANNEL
  if (transport->tls_active)
  {
    return _schannel_send(transport, data);
  }
#endif

  return _socket_send_all(transport, az_span_ptr(data), az_span_size(data));
}

AZ_NODISCARD az_result az_mqtt_transport_receive(
    az_mqtt_transport* transport,
    az_span buffer,
    int32_t timeout_ms,
    az_span* out_received)
{
  _az_PRECONDITION_NOT_NULL(transport);
  _az_PRECONDITION_NOT_NULL(out_received);

  *out_received = AZ_SPAN_EMPTY;

#ifdef AZ_MQTT_TLS_SCHANNEL
  if (transport->tls_active)
  {
    return _schannel_receive(transport, buffer, timeout_ms, out_received);
  }
#endif

  int32_t n = 0;
  az_result rc = _socket_recv_timeout(
      transport,
      az_span_ptr(buffer),
      az_span_size(buffer),
      timeout_ms,
      &n);
  if (az_result_failed(rc))
  {
    return rc;
  }

  if (n == 0)
  {
    return AZ_OK;
  }

  *out_received = az_span_slice(buffer, 0, n);
  return AZ_OK;
}

AZ_NODISCARD az_result
az_mqtt_transport_set_proxy(az_mqtt_transport* transport, az_mqtt_proxy_options const* proxy)
{
  _az_PRECONDITION_NOT_NULL(transport);
  az_result const rc = _az_mqtt_http_connect_check(proxy);
  if (az_result_succeeded(rc))
  {
    transport->proxy = proxy != NULL && az_span_size(proxy->host) > 0 ? proxy : NULL;
  }
  return rc;
}

void az_mqtt_transport_set_error_callback(
    az_mqtt_transport* transport,
    az_mqtt_transport_error_fn callback,
    void* context)
{
  _az_PRECONDITION_NOT_NULL(transport);
  transport->error_callback = callback;
  transport->error_context = context;
}

void az_mqtt_transport_close(az_mqtt_transport* transport)
{
  if (transport == NULL)
  {
    return;
  }

#ifdef AZ_MQTT_TLS_SCHANNEL
  _schannel_shutdown(transport);
#endif

  if (transport->socket_fd != INVALID_SOCKET)
  {
    closesocket(transport->socket_fd);
    transport->socket_fd = INVALID_SOCKET;
  }
  _release_addresses(transport);

  transport->state = _WIN_IDLE;
  transport->connected = false;
}
