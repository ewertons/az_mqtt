// Copyright (c) az_mqtt5_client contributors. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file perf_az_mqtt5.c
 * @brief Performance test for az_mqtt5_client.
 *
 * Connects to a broker, subscribes to its own topic, publishes N messages at
 * QoS 0 and QoS 1, counts how many it sends/receives, then prints a JSON
 * report with throughput and resource-usage stats.
 *
 * Usage:
 *   perf_az_mqtt5 [host] [port] [msg_count] [payload_bytes] [duration_sec]
 *
 * Defaults: localhost 1883 10000 128 30
 */

#include <az_mqtt5/az_mqtt5_client.h>
#include <azure/core/az_span.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#endif

// ─────────────── tunables ────────────────────────────────────

#define SEND_BUFFER_SIZE (64 * 1024)
#define RECV_BUFFER_SIZE (64 * 1024)
#define MAX_USER_PROPERTIES 4
#define MAX_SUBACK_REASON_CODES 4
#define MAX_PAYLOAD_SIZE 8192

// ─────────────── static buffers (zero-allocation) ────────────

static uint8_t s_send_buf[SEND_BUFFER_SIZE];
static uint8_t s_recv_buf[RECV_BUFFER_SIZE];
static uint8_t s_transport_buf[256];

static az_mqtt5_user_property s_connack_up[MAX_USER_PROPERTIES];
static az_mqtt5_user_property s_pub_up[MAX_USER_PROPERTIES];
static az_mqtt5_user_property s_sub_up[MAX_USER_PROPERTIES];
static az_mqtt5_user_property s_ack_up[MAX_USER_PROPERTIES];
static az_mqtt5_user_property s_disc_up[MAX_USER_PROPERTIES];
static az_mqtt5_reason_code s_sub_rc[MAX_SUBACK_REASON_CODES];
static int32_t s_pub_sub_ids[MAX_USER_PROPERTIES];

static uint8_t s_payload[MAX_PAYLOAD_SIZE];

// ─────────────── counters ────────────────────────────────────

static int64_t g_pub_sent = 0;
static int64_t g_pub_received = 0;
static int64_t g_puback_received = 0;

// ─────────────── resource helpers ────────────────────────────

typedef struct
{
  double wall_sec;
  double user_cpu_sec;
  double sys_cpu_sec;
  int64_t peak_rss_bytes;
} resource_snapshot;

static double _now_sec(void)
{
#ifdef _WIN32
  LARGE_INTEGER freq, cnt;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&cnt);
  return (double)cnt.QuadPart / (double)freq.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static resource_snapshot _snap(void)
{
  resource_snapshot s;
  s.wall_sec = _now_sec();
#ifdef _WIN32
  FILETIME c, e, k, u;
  GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
  s.user_cpu_sec
      = ((double)((int64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 1e7;
  s.sys_cpu_sec
      = ((double)((int64_t)k.dwHighDateTime << 32 | k.dwLowDateTime)) / 1e7;
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
  {
    s.peak_rss_bytes = (int64_t)pmc.PeakWorkingSetSize;
  }
  else
  {
    s.peak_rss_bytes = 0;
  }
#else
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  s.user_cpu_sec = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6;
  s.sys_cpu_sec = (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
  s.peak_rss_bytes = (int64_t)ru.ru_maxrss * 1024; // Linux reports kB
#endif
  return s;
}

// ─────────────── callbacks ───────────────────────────────────

static void on_publish(az_mqtt5_client* client, az_mqtt5_publish_data const* pub)
{
  (void)client;
  (void)pub;
  g_pub_received++;
}

static void on_puback(az_mqtt5_client* client, az_mqtt5_ack_data const* ack)
{
  (void)client;
  (void)ack;
  g_puback_received++;
}

// ─────────────── main ────────────────────────────────────────

int main(int argc, char* argv[])
{
  const char* host = "localhost";
  uint16_t port = 1883;
  int64_t msg_count = 10000;
  int32_t payload_bytes = 128;
  int32_t duration_sec = 30;

  if (argc >= 2) host = argv[1];
  if (argc >= 3) port = (uint16_t)atoi(argv[2]);
  if (argc >= 4) msg_count = atoll(argv[3]);
  if (argc >= 5) payload_bytes = atoi(argv[4]);
  if (argc >= 6) duration_sec = atoi(argv[5]);

  if (payload_bytes > MAX_PAYLOAD_SIZE) payload_bytes = MAX_PAYLOAD_SIZE;

  fprintf(stderr, "[az_mqtt5] host=%s port=%d msgs=%lld payload=%d duration=%ds\n",
      host, port, (long long)msg_count, payload_bytes, duration_sec);

  // Fill payload with a repeating pattern
  for (int32_t i = 0; i < payload_bytes; i++)
  {
    s_payload[i] = (uint8_t)('A' + (i % 26));
  }

  // ── init transport ──
  az_mqtt5_transport* transport = (az_mqtt5_transport*)s_transport_buf;
  az_result rc = az_mqtt5_transport_init(transport);
  if (az_result_failed(rc))
  {
    fprintf(stderr, "transport init failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }

  // ── configure client ──
  az_mqtt5_connect_options conn_opts = az_mqtt5_connect_options_default();
  conn_opts.client_id = AZ_SPAN_FROM_STR("perf-az-mqtt5");
  conn_opts.keep_alive_seconds = 60;
  conn_opts.clean_start = true;

  az_mqtt5_client_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.transport = transport;
  opts.send_buffer = AZ_SPAN_FROM_BUFFER(s_send_buf);
  opts.receive_buffer = AZ_SPAN_FROM_BUFFER(s_recv_buf);
  opts.connect_options = conn_opts;
  opts.hostname = az_span_create((uint8_t*)host, (int32_t)strlen(host));
  opts.port = port;
  opts.on_publish = on_publish;
  opts.on_puback = on_puback;

  opts.connack_user_properties = s_connack_up;
  opts.connack_user_property_capacity = MAX_USER_PROPERTIES;
  opts.publish_user_properties = s_pub_up;
  opts.publish_subscription_identifiers = s_pub_sub_ids;
  opts.suback_reason_codes = s_sub_rc;
  opts.suback_reason_code_capacity = MAX_SUBACK_REASON_CODES;
  opts.suback_user_properties = s_sub_up;
  opts.suback_user_property_capacity = MAX_USER_PROPERTIES;
  opts.ack_user_properties = s_ack_up;
  opts.ack_user_property_capacity = MAX_USER_PROPERTIES;
  opts.disconnect_user_properties = s_disc_up;
  opts.disconnect_user_property_capacity = MAX_USER_PROPERTIES;

  az_mqtt5_client client;
  rc = az_mqtt5_client_init(&client, &opts);
  if (az_result_failed(rc))
  {
    fprintf(stderr, "client init failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }

  // ── connect ──
  rc = az_mqtt5_client_connect(&client, 10000);
  if (az_result_failed(rc))
  {
    fprintf(stderr, "connect failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }
  fprintf(stderr, "[az_mqtt5] connected\n");

  // ── subscribe ──
  az_mqtt5_subscription sub;
  memset(&sub, 0, sizeof(sub));
  sub.topic_filter = AZ_SPAN_FROM_STR("perf/az_mqtt5/#");
  sub.qos = AZ_MQTT5_QOS_AT_LEAST_ONCE;

  uint16_t sub_pid;
  rc = az_mqtt5_client_subscribe(&client, &sub, 1, &sub_pid);
  if (az_result_failed(rc))
  {
    fprintf(stderr, "subscribe failed: 0x%08X\n", (unsigned)rc);
    az_mqtt5_client_disconnect(&client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION);
    return 1;
  }

  // Drain SUBACK
  for (int i = 0; i < 10; i++)
  {
    az_mqtt5_client_process_loop(&client, 500);
  }

  // ── publish loop ──
  resource_snapshot snap_start = _snap();
  double deadline = _now_sec() + (double)duration_sec;

  az_mqtt5_publish_options pub_opts = az_mqtt5_publish_options_default();
  pub_opts.topic = AZ_SPAN_FROM_STR("perf/az_mqtt5/data");
  pub_opts.payload = az_span_create(s_payload, payload_bytes);
  pub_opts.qos = AZ_MQTT5_QOS_AT_LEAST_ONCE;

  while (g_pub_sent < msg_count && _now_sec() < deadline)
  {
    uint16_t pid;
    rc = az_mqtt5_client_publish(&client, &pub_opts, &pid);
    if (az_result_succeeded(rc))
    {
      g_pub_sent++;
    }
    else
    {
      fprintf(stderr, "[az_mqtt5] publish err: 0x%08X at msg %lld\n",
          (unsigned)rc, (long long)g_pub_sent);
    }

    // Process incoming (receive our own messages + pubacks)
    // Drain a few iterations to keep buffers flowing
    if (g_pub_sent % 100 == 0)
    {
      for (int i = 0; i < 5; i++)
      {
        az_mqtt5_client_process_loop(&client, 1);
      }
    }
  }

  // Drain remaining receives
  fprintf(stderr, "[az_mqtt5] draining remaining messages...\n");
  double drain_deadline = _now_sec() + 5.0;
  while (_now_sec() < drain_deadline)
  {
    rc = az_mqtt5_client_process_loop(&client, 100);
    if (az_result_failed(rc))
      break;
  }

  resource_snapshot snap_end = _snap();

  // ── disconnect ──
  az_mqtt5_client_disconnect(&client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION);

  // ── report (JSON to stdout) ──
  double elapsed = snap_end.wall_sec - snap_start.wall_sec;
  double user_cpu = snap_end.user_cpu_sec - snap_start.user_cpu_sec;
  double sys_cpu = snap_end.sys_cpu_sec - snap_start.sys_cpu_sec;

  printf("{\n");
  printf("  \"client\": \"az_mqtt5\",\n");
  printf("  \"host\": \"%s\",\n", host);
  printf("  \"port\": %d,\n", port);
  printf("  \"payload_bytes\": %d,\n", payload_bytes);
  printf("  \"qos\": 1,\n");
  printf("  \"messages_sent\": %lld,\n", (long long)g_pub_sent);
  printf("  \"messages_received\": %lld,\n", (long long)g_pub_received);
  printf("  \"pubacks_received\": %lld,\n", (long long)g_puback_received);
  printf("  \"elapsed_sec\": %.3f,\n", elapsed);
  printf("  \"send_rate_msg_sec\": %.1f,\n", elapsed > 0 ? (double)g_pub_sent / elapsed : 0);
  printf("  \"recv_rate_msg_sec\": %.1f,\n", elapsed > 0 ? (double)g_pub_received / elapsed : 0);
  printf("  \"user_cpu_sec\": %.3f,\n", user_cpu);
  printf("  \"sys_cpu_sec\": %.3f,\n", sys_cpu);
  printf("  \"total_cpu_sec\": %.3f,\n", user_cpu + sys_cpu);
  printf("  \"peak_rss_bytes\": %lld\n", (long long)snap_end.peak_rss_bytes);
  printf("}\n");

  return 0;
}
