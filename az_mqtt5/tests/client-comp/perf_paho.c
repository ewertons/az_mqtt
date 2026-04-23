// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
/**
 * @file perf_paho.c
 * @brief Performance test using Eclipse Paho MQTT C (MQTTAsync non-blocking API).
 *
 * Same workload as perf_az_mqtt5.c: connect, subscribe to own topic, publish
 * N messages at QoS 1, count sends/receives, report JSON.
 *
 * Build (Linux):
 *   gcc -O2 -o perf_paho perf_paho.c -lpaho-mqtt3a -lrt
 *
 * Usage:
 *   perf_paho [host] [port] [msg_count] [payload_bytes] [duration_sec]
 */

#include <MQTTAsync.h>

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
#ifdef __GLIBC__
#include <malloc.h>
#endif
#endif

#define MAX_PAYLOAD_SIZE 8192

static char s_payload[MAX_PAYLOAD_SIZE];
static volatile int64_t g_pub_received = 0;
static volatile int64_t g_pubacks_received = 0;
static volatile int g_connected = 0;
static volatile int g_subscribed = 0;
static volatile int g_connect_failed = 0;

// ─────────────── resource helpers ────────────────────────────

typedef struct
{
  double wall_sec;
  double user_cpu_sec;
  double sys_cpu_sec;
  int64_t peak_rss_bytes;
  int64_t current_rss_bytes;
  int64_t heap_bytes;
} resource_snapshot;

#ifndef _WIN32
static int64_t _read_vmrss_bytes(void)
{
  FILE* f = fopen("/proc/self/status", "r");
  if (!f) return 0;
  char line[256];
  int64_t kb = 0;
  while (fgets(line, sizeof(line), f))
  {
    if (strncmp(line, "VmRSS:", 6) == 0)
    {
      (void)sscanf(line + 6, "%lld", (long long*)&kb);
      break;
    }
  }
  fclose(f);
  return kb * 1024;
}

static int64_t _read_heap_bytes(void)
{
#ifdef __GLIBC__
  struct mallinfo2 mi = mallinfo2();
  return (int64_t)mi.uordblks;
#else
  return 0;
#endif
}
#endif

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
    s.peak_rss_bytes = (int64_t)pmc.PeakWorkingSetSize;
  else
    s.peak_rss_bytes = 0;
#else
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  s.user_cpu_sec = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6;
  s.sys_cpu_sec = (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
  s.peak_rss_bytes = (int64_t)ru.ru_maxrss * 1024;
  s.current_rss_bytes = _read_vmrss_bytes();
  s.heap_bytes = _read_heap_bytes();
#endif
#ifdef _WIN32
  s.current_rss_bytes = 0;
  s.heap_bytes = 0;
#endif
  return s;
}

// ─────────────── Paho async callbacks ────────────────────────

static int on_message(void* context, char* topicName, int topicLen, MQTTAsync_message* message)
{
  (void)context;
  (void)topicLen;
  g_pub_received++;
  MQTTAsync_freeMessage(&message);
  MQTTAsync_free(topicName);
  return 1;
}

static void on_connect(void* context, MQTTAsync_successData* response)
{
  (void)context;
  (void)response;
  g_connected = 1;
}

static void on_connect_failure(void* context, MQTTAsync_failureData* response)
{
  (void)context;
  fprintf(stderr, "connect failed: %d\n", response ? response->code : -1);
  g_connect_failed = 1;
}

static void on_subscribe(void* context, MQTTAsync_successData* response)
{
  (void)context;
  (void)response;
  g_subscribed = 1;
}

static void on_send(void* context, MQTTAsync_successData* response)
{
  (void)context;
  (void)response;
  g_pubacks_received++;
}

static void _sleep_ms(int ms)
{
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  usleep(ms * 1000);
#endif
}

// ─────────────── main ────────────────────────────────────────

int main(int argc, char* argv[])
{
  const char* host = "localhost";
  int port = 1883;
  int64_t msg_count = 10000;
  int payload_bytes = 128;
  int duration_sec = 30;

  if (argc >= 2) host = argv[1];
  if (argc >= 3) port = atoi(argv[2]);
  if (argc >= 4) msg_count = atoll(argv[3]);
  if (argc >= 5) payload_bytes = atoi(argv[4]);
  if (argc >= 6) duration_sec = atoi(argv[5]);

  if (payload_bytes > MAX_PAYLOAD_SIZE) payload_bytes = MAX_PAYLOAD_SIZE;

  fprintf(stderr, "[paho] host=%s port=%d msgs=%lld payload=%d duration=%ds\n",
      host, port, (long long)msg_count, payload_bytes, duration_sec);

  for (int i = 0; i < payload_bytes; i++)
  {
    s_payload[i] = (char)('A' + (i % 26));
  }

  // Baseline resource snapshot BEFORE any MQTT/network work.
  resource_snapshot snap_baseline = _snap();

  char uri[512];
  snprintf(uri, sizeof(uri), "tcp://%s:%d", host, port);

  MQTTAsync client;
  int rc = MQTTAsync_create(&client, uri, "perf-paho-c",
      MQTTCLIENT_PERSISTENCE_NONE, NULL);
  if (rc != MQTTASYNC_SUCCESS)
  {
    fprintf(stderr, "MQTTAsync_create failed: %d\n", rc);
    return 1;
  }

  MQTTAsync_setCallbacks(client, NULL, NULL, on_message, NULL);

  // Connect
  MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
  conn_opts.keepAliveInterval = 60;
  conn_opts.cleansession = 1;
  conn_opts.onSuccess = on_connect;
  conn_opts.onFailure = on_connect_failure;

  rc = MQTTAsync_connect(client, &conn_opts);
  if (rc != MQTTASYNC_SUCCESS)
  {
    fprintf(stderr, "MQTTAsync_connect call failed: %d\n", rc);
    MQTTAsync_destroy(&client);
    return 1;
  }

  // Wait for connect callback
  double wait_end = _now_sec() + 10.0;
  while (!g_connected && !g_connect_failed && _now_sec() < wait_end)
  {
    _sleep_ms(10);
  }
  if (!g_connected)
  {
    fprintf(stderr, "connect timed out or failed\n");
    MQTTAsync_destroy(&client);
    return 1;
  }
  fprintf(stderr, "[paho] connected\n");

  // Subscribe
  MQTTAsync_responseOptions sub_opts = MQTTAsync_responseOptions_initializer;
  sub_opts.onSuccess = on_subscribe;
  rc = MQTTAsync_subscribe(client, "perf/paho/#", 1, &sub_opts);
  if (rc != MQTTASYNC_SUCCESS)
  {
    fprintf(stderr, "subscribe call failed: %d\n", rc);
    MQTTAsync_disconnect(client, NULL);
    MQTTAsync_destroy(&client);
    return 1;
  }

  wait_end = _now_sec() + 10.0;
  while (!g_subscribed && _now_sec() < wait_end)
  {
    _sleep_ms(10);
  }

  // Publish loop (non-blocking)
  resource_snapshot snap_start = _snap();
  double deadline = _now_sec() + (double)duration_sec;

  int64_t pub_sent = 0;
  MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
  pubmsg.payload = s_payload;
  pubmsg.payloadlen = payload_bytes;
  pubmsg.qos = 1;
  pubmsg.retained = 0;

  while (pub_sent < msg_count && _now_sec() < deadline)
  {
    MQTTAsync_responseOptions send_opts = MQTTAsync_responseOptions_initializer;
    send_opts.onSuccess = on_send;

    rc = MQTTAsync_sendMessage(client, "perf/paho/data", &pubmsg, &send_opts);
    if (rc == MQTTASYNC_SUCCESS)
    {
      pub_sent++;
    }
    else if (rc == MQTTASYNC_MAX_BUFFERED_MESSAGES)
    {
      // Back-pressure: wait briefly and retry
      _sleep_ms(1);
    }
    else
    {
      fprintf(stderr, "[paho] send err: %d at msg %lld\n", rc, (long long)pub_sent);
    }
  }

  // Drain: wait for PUBACKs and incoming messages.
  // No-progress watchdog: continue as long as either pubacks or inbound
  // messages are still arriving. Stop if neither counter advances for
  // `idle_budget` seconds, or after a hard `max_drain` cap.
  fprintf(stderr, "[paho] draining remaining messages...\n");
  const double idle_budget = 3.0;
  const double max_drain = 120.0;
  double drain_start = _now_sec();
  double last_progress = drain_start;
  int64_t last_received = g_pub_received;
  int64_t last_pubacks = g_pubacks_received;
  while (g_pub_received < pub_sent || g_pubacks_received < pub_sent)
  {
    double now = _now_sec();
    if (now - drain_start > max_drain)
    {
      fprintf(stderr, "[paho] drain hit max %.1fs cap\n", max_drain);
      break;
    }
    if (now - last_progress > idle_budget)
    {
      fprintf(stderr, "[paho] drain idle %.1fs, stopping\n", idle_budget);
      break;
    }
    _sleep_ms(10);
    if (g_pub_received > last_received || g_pubacks_received > last_pubacks)
    {
      last_received = g_pub_received;
      last_pubacks = g_pubacks_received;
      last_progress = _now_sec();
    }
  }

  resource_snapshot snap_end = _snap();

  MQTTAsync_disconnect(client, NULL);
  MQTTAsync_destroy(&client);

  double elapsed = snap_end.wall_sec - snap_start.wall_sec;
  double user_cpu = snap_end.user_cpu_sec - snap_start.user_cpu_sec;
  double sys_cpu = snap_end.sys_cpu_sec - snap_start.sys_cpu_sec;

  printf("{\n");
  printf("  \"client\": \"paho_mqtt_c\",\n");
  printf("  \"host\": \"%s\",\n", host);
  printf("  \"port\": %d,\n", port);
  printf("  \"payload_bytes\": %d,\n", payload_bytes);
  printf("  \"qos\": 1,\n");
  printf("  \"messages_sent\": %lld,\n", (long long)pub_sent);
  printf("  \"messages_received\": %lld,\n", (long long)g_pub_received);
  printf("  \"pubacks_received\": %lld,\n", (long long)g_pubacks_received);
  printf("  \"elapsed_sec\": %.3f,\n", elapsed);
  printf("  \"send_rate_msg_sec\": %.1f,\n", elapsed > 0 ? (double)pub_sent / elapsed : 0);
  printf("  \"recv_rate_msg_sec\": %.1f,\n", elapsed > 0 ? (double)g_pub_received / elapsed : 0);
  printf("  \"user_cpu_sec\": %.3f,\n", user_cpu);
  printf("  \"sys_cpu_sec\": %.3f,\n", sys_cpu);
  printf("  \"total_cpu_sec\": %.3f,\n", user_cpu + sys_cpu);
  printf("  \"peak_rss_bytes\": %lld,\n", (long long)snap_end.peak_rss_bytes);
  printf("  \"rss_baseline_bytes\": %lld,\n", (long long)snap_baseline.current_rss_bytes);
  printf("  \"rss_delta_bytes\": %lld,\n",
      (long long)(snap_end.peak_rss_bytes - snap_baseline.current_rss_bytes));
  printf("  \"heap_baseline_bytes\": %lld,\n", (long long)snap_baseline.heap_bytes);
  printf("  \"heap_peak_bytes\": %lld,\n", (long long)snap_end.heap_bytes);
  printf("  \"heap_delta_bytes\": %lld\n",
      (long long)(snap_end.heap_bytes - snap_baseline.heap_bytes));
  printf("}\n");

  return 0;
}
