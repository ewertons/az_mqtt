/**
 * @file perf_paho.c
 * @brief Performance test using Eclipse Paho MQTT C (MQTTClient synchronous API).
 *
 * Same workload as perf_az_mqtt5.c: connect, subscribe to own topic, publish
 * N messages at QoS 1, count sends/receives, report JSON.
 *
 * Build (Linux):
 *   gcc -O2 -o perf_paho perf_paho.c -lpaho-mqtt3c -lrt
 *
 * Build (Windows / vcpkg):
 *   cl /O2 perf_paho.c paho-mqtt3c.lib ws2_32.lib
 *
 * Usage:
 *   perf_paho [host] [port] [msg_count] [payload_bytes] [duration_sec]
 */

#include <MQTTClient.h>

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

#define MAX_PAYLOAD_SIZE 8192

static char s_payload[MAX_PAYLOAD_SIZE];
static volatile int64_t g_pub_received = 0;

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
    s.peak_rss_bytes = (int64_t)pmc.PeakWorkingSetSize;
  else
    s.peak_rss_bytes = 0;
#else
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  s.user_cpu_sec = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6;
  s.sys_cpu_sec = (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
  s.peak_rss_bytes = (int64_t)ru.ru_maxrss * 1024;
#endif
  return s;
}

// ─────────────── Paho callback ───────────────────────────────

static int on_message(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
  (void)context;
  (void)topicLen;
  g_pub_received++;
  MQTTClient_freeMessage(&message);
  MQTTClient_free(topicName);
  return 1; // tell Paho we consumed it
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

  // Build URI
  char uri[512];
  snprintf(uri, sizeof(uri), "tcp://%s:%d", host, port);

  MQTTClient client;
  int rc = MQTTClient_create(&client, uri, "perf-paho-c",
      MQTTCLIENT_PERSISTENCE_NONE, NULL);
  if (rc != MQTTCLIENT_SUCCESS)
  {
    fprintf(stderr, "MQTTClient_create failed: %d\n", rc);
    return 1;
  }

  MQTTClient_setCallbacks(client, NULL, NULL, on_message, NULL);

  MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
  conn_opts.keepAliveInterval = 60;
  conn_opts.cleansession = 1;
  conn_opts.MQTTVersion = MQTTVERSION_5;

  rc = MQTTClient_connect(client, &conn_opts);
  if (rc != MQTTCLIENT_SUCCESS)
  {
    fprintf(stderr, "connect failed: %d\n", rc);
    MQTTClient_destroy(&client);
    return 1;
  }
  fprintf(stderr, "[paho] connected\n");

  // Subscribe
  rc = MQTTClient_subscribe(client, "perf/paho/#", 1);
  if (rc != MQTTCLIENT_SUCCESS)
  {
    fprintf(stderr, "subscribe failed: %d\n", rc);
    MQTTClient_disconnect(client, 1000);
    MQTTClient_destroy(&client);
    return 1;
  }

  // Publish loop
  resource_snapshot snap_start = _snap();
  double deadline = _now_sec() + (double)duration_sec;

  int64_t pub_sent = 0;
  MQTTClient_deliveryToken token;

  while (pub_sent < msg_count && _now_sec() < deadline)
  {
    rc = MQTTClient_publish(client, "perf/paho/data",
        payload_bytes, s_payload, 1 /*qos*/, 0 /*retain*/, &token);
    if (rc == MQTTCLIENT_SUCCESS)
    {
      pub_sent++;
    }
    else
    {
      fprintf(stderr, "[paho] publish err: %d at msg %lld\n", rc, (long long)pub_sent);
    }

    // Yield periodically so receive callback can fire
    if (pub_sent % 100 == 0)
    {
      MQTTClient_yield();
    }
  }

  // Drain remaining
  fprintf(stderr, "[paho] draining remaining messages...\n");
  double drain_deadline = _now_sec() + 5.0;
  while (_now_sec() < drain_deadline)
  {
    MQTTClient_yield();
  }

  resource_snapshot snap_end = _snap();

  MQTTClient_disconnect(client, 1000);
  MQTTClient_destroy(&client);

  // JSON report
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
  printf("  \"pubacks_received\": 0,\n");
  printf("  \"elapsed_sec\": %.3f,\n", elapsed);
  printf("  \"send_rate_msg_sec\": %.1f,\n", elapsed > 0 ? (double)pub_sent / elapsed : 0);
  printf("  \"recv_rate_msg_sec\": %.1f,\n", elapsed > 0 ? (double)g_pub_received / elapsed : 0);
  printf("  \"user_cpu_sec\": %.3f,\n", user_cpu);
  printf("  \"sys_cpu_sec\": %.3f,\n", sys_cpu);
  printf("  \"total_cpu_sec\": %.3f,\n", user_cpu + sys_cpu);
  printf("  \"peak_rss_bytes\": %lld\n", (long long)snap_end.peak_rss_bytes);
  printf("}\n");

  return 0;
}
