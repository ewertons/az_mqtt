// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

//! Performance test for the azure_mqtt (Rust) client.
//!
//! Same workload as the C perf tests: connect, subscribe to own topic,
//! publish N messages at QoS 1, count sends/receives, report JSON.
//!
//! Usage:
//!   perf_azure_mqtt [host] [port] [msg_count] [payload_bytes] [duration_sec]
//!
//! Defaults: localhost 1883 10000 128 30

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use azure_mqtt::client::{
    Client, ClientOptions, ConnectResult, Connection, ConnectionTransportConfig,
    ConnectionTransportType, KeepAliveConfig, ManualAcknowledgement, Receiver, new_client,
};
use azure_mqtt::packet::{ConnectProperties, PubAckProperties, PublishProperties, QoS, RetainOptions, SubscribeProperties};
use azure_mqtt::topic::{TopicFilter, TopicName};
use bytes::Bytes;
use serde::Serialize;

const MAX_PAYLOAD: usize = 8192;

#[derive(Serialize)]
struct PerfReport {
    client: String,
    host: String,
    port: u16,
    payload_bytes: usize,
    qos: u8,
    messages_sent: u64,
    messages_received: u64,
    pubacks_received: u64,
    elapsed_sec: f64,
    send_rate_msg_sec: f64,
    recv_rate_msg_sec: f64,
    user_cpu_sec: f64,
    sys_cpu_sec: f64,
    total_cpu_sec: f64,
    peak_rss_bytes: u64,
    rss_baseline_bytes: u64,
    rss_delta_bytes: i64,
    // Heap delta is not tracked for Rust (would require a custom GlobalAlloc).
    heap_baseline_bytes: i64,
    heap_peak_bytes: i64,
    heap_delta_bytes: i64,
}

fn get_resource_usage() -> (f64, f64, u64) {
    #[cfg(target_os = "linux")]
    {
        use std::io::Read;
        // user_cpu, sys_cpu from /proc/self/stat
        let mut stat = String::new();
        if let Ok(mut f) = std::fs::File::open("/proc/self/stat") {
            let _ = f.read_to_string(&mut stat);
        }
        let parts: Vec<&str> = stat.split_whitespace().collect();
        let ticks_per_sec = unsafe { libc::sysconf(libc::_SC_CLK_TCK) } as f64;
        let utime = parts.get(13).and_then(|s| s.parse::<u64>().ok()).unwrap_or(0) as f64 / ticks_per_sec;
        let stime = parts.get(14).and_then(|s| s.parse::<u64>().ok()).unwrap_or(0) as f64 / ticks_per_sec;

        // peak RSS from /proc/self/status VmHWM
        let mut status = String::new();
        if let Ok(mut f) = std::fs::File::open("/proc/self/status") {
            let _ = f.read_to_string(&mut status);
        }
        let peak_rss = status
            .lines()
            .find(|l| l.starts_with("VmHWM:"))
            .and_then(|l| l.split_whitespace().nth(1))
            .and_then(|s| s.parse::<u64>().ok())
            .unwrap_or(0)
            * 1024; // kB -> bytes

        (utime, stime, peak_rss)
    }
    #[cfg(not(target_os = "linux"))]
    {
        (0.0, 0.0, 0)
    }
}

fn read_vmrss_bytes() -> u64 {
    #[cfg(target_os = "linux")]
    {
        use std::io::Read;
        let mut status = String::new();
        if let Ok(mut f) = std::fs::File::open("/proc/self/status") {
            let _ = f.read_to_string(&mut status);
        }
        status
            .lines()
            .find(|l| l.starts_with("VmRSS:"))
            .and_then(|l| l.split_whitespace().nth(1))
            .and_then(|s| s.parse::<u64>().ok())
            .unwrap_or(0)
            * 1024
    }
    #[cfg(not(target_os = "linux"))]
    {
        0
    }
}

#[tokio::main]
async fn main() {
    let args: Vec<String> = std::env::args().collect();
    let host = args.get(1).map(|s| s.as_str()).unwrap_or("localhost").to_string();
    let port: u16 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(1883);
    let msg_count: u64 = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(10000);
    let payload_bytes: usize = args
        .get(4)
        .and_then(|s| s.parse().ok())
        .unwrap_or(128)
        .min(MAX_PAYLOAD);
    let duration_sec: u64 = args.get(5).and_then(|s| s.parse().ok()).unwrap_or(30);

    eprintln!(
        "[azure_mqtt] host={host} port={port} msgs={msg_count} payload={payload_bytes} duration={duration_sec}s"
    );

    // Baseline VmRSS BEFORE any client / tokio runtime work beyond this point.
    let rss_baseline = read_vmrss_bytes();

    // Build payload
    let payload: Bytes = (0..payload_bytes)
        .map(|i| b'A' + (i % 26) as u8)
        .collect::<Vec<u8>>()
        .into();

    let received = Arc::new(AtomicU64::new(0));
    let pubacks = Arc::new(AtomicU64::new(0));

    let options = ClientOptions {
        client_id: Some("perf-azure-mqtt".to_string()),
        ..Default::default()
    };
    let (client, connect_handle, receiver) = new_client(options);

    let connect_result = tokio::task::spawn(connect_handle.connect(
        ConnectionTransportConfig {
            transport_type: ConnectionTransportType::Tcp {
                hostname: host.clone(),
                port,
            },
            timeout: Some(Duration::from_secs(10)),
        },
        true,
        KeepAliveConfig::Infinite,
        None,
        None,
        None,
        ConnectProperties::default(),
        None,
    ))
    .await
    .expect("connect task panicked");

    let connect_result = match connect_result {
        ConnectResult::Success(connection, connack, disconnect_handle) => {
            (connection, connack, disconnect_handle)
        }
        ConnectResult::Failure(_handle, err) => {
            eprintln!("[azure_mqtt] connect failed: {err:?}");
            std::process::exit(1);
        }
    };
    let (connection, _connack, _disconnect_handle) = connect_result;
    eprintln!("[azure_mqtt] connected");

    // Spawn connection runner (must be running before subscribe/publish)
    let conn_handle = tokio::spawn(async move {
        let _ = connection.run_until_disconnect().await;
    });

    // Spawn receiver
    let recv_count = received.clone();
    let recv_handle = tokio::spawn(async move {
        receive_loop(receiver, recv_count).await;
    });

    // Subscribe
    let sub_ct = client
        .subscribe(
            TopicFilter::new("perf/azure_mqtt/#").unwrap(),
            QoS::AtLeastOnce,
            false,
            RetainOptions::default(),
            SubscribeProperties::default(),
        )
        .await
        .expect("subscribe send failed");
    let _ = sub_ct.await;
    eprintln!("[azure_mqtt] subscribed");

    // Publish loop
    let (user_cpu_before, sys_cpu_before, _) = get_resource_usage();
    let start = Instant::now();
    let deadline = start + Duration::from_secs(duration_sec);
    let mut pub_sent: u64 = 0;

    let topic = TopicName::new("perf/azure_mqtt/data").unwrap();

    while pub_sent < msg_count && Instant::now() < deadline {
        match client
            .publish_qos1(
                topic.clone(),
                payload.clone(),
                false,
                PublishProperties::default(),
            )
            .await
        {
            Ok(ct) => {
                pub_sent += 1;
                // Spawn a task to await the PUBACK completion token so we
                // don't serialize publishes on broker round-trips.
                let puback_counter = pubacks.clone();
                tokio::spawn(async move {
                    let _ = ct.await;
                    puback_counter.fetch_add(1, Ordering::Relaxed);
                });
            }
            Err(e) => {
                eprintln!("[azure_mqtt] publish err at msg {pub_sent}: {e:?}");
                break;
            }
        }

        // Yield periodically
        if pub_sent % 100 == 0 {
            tokio::task::yield_now().await;
        }
    }

    // Drain: no-progress watchdog on both received and pubacks. Stop when
    // neither counter advances for `idle_budget`, or after `max_drain`.
    eprintln!("[azure_mqtt] draining remaining messages...");
    let idle_budget = Duration::from_secs(3);
    let max_drain = Duration::from_secs(120);
    let drain_start = Instant::now();
    let mut last_progress = drain_start;
    let mut last_received = received.load(Ordering::Relaxed);
    let mut last_pubacks = pubacks.load(Ordering::Relaxed);
    loop {
        let cur_received = received.load(Ordering::Relaxed);
        let cur_pubacks = pubacks.load(Ordering::Relaxed);
        if cur_received >= pub_sent && cur_pubacks >= pub_sent {
            break;
        }
        let now = Instant::now();
        if now.duration_since(drain_start) > max_drain {
            eprintln!("[azure_mqtt] drain hit max {}s cap", max_drain.as_secs());
            break;
        }
        if now.duration_since(last_progress) > idle_budget {
            eprintln!(
                "[azure_mqtt] drain idle {}s, stopping",
                idle_budget.as_secs()
            );
            break;
        }
        if cur_received > last_received || cur_pubacks > last_pubacks {
            last_received = cur_received;
            last_pubacks = cur_pubacks;
            last_progress = now;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }

    let elapsed = start.elapsed().as_secs_f64();
    let (user_cpu_after, sys_cpu_after, peak_rss) = get_resource_usage();
    let pub_received = received.load(Ordering::Relaxed);
    let pub_pubacks = pubacks.load(Ordering::Relaxed);

    // Abort background tasks
    conn_handle.abort();
    recv_handle.abort();

    let report = PerfReport {
        client: "azure_mqtt".to_string(),
        host,
        port,
        payload_bytes,
        qos: 1,
        messages_sent: pub_sent,
        messages_received: pub_received,
        pubacks_received: pub_pubacks,
        elapsed_sec: (elapsed * 1000.0).round() / 1000.0,
        send_rate_msg_sec: if elapsed > 0.0 {
            (pub_sent as f64 / elapsed * 10.0).round() / 10.0
        } else {
            0.0
        },
        recv_rate_msg_sec: if elapsed > 0.0 {
            (pub_received as f64 / elapsed * 10.0).round() / 10.0
        } else {
            0.0
        },
        user_cpu_sec: ((user_cpu_after - user_cpu_before) * 1000.0).round() / 1000.0,
        sys_cpu_sec: ((sys_cpu_after - sys_cpu_before) * 1000.0).round() / 1000.0,
        total_cpu_sec: (((user_cpu_after - user_cpu_before) + (sys_cpu_after - sys_cpu_before))
            * 1000.0)
            .round()
            / 1000.0,
        peak_rss_bytes: peak_rss,
        rss_baseline_bytes: rss_baseline,
        rss_delta_bytes: peak_rss as i64 - rss_baseline as i64,
        heap_baseline_bytes: -1,
        heap_peak_bytes: -1,
        heap_delta_bytes: -1,
    };

    println!("{}", serde_json::to_string_pretty(&report).unwrap());
}

async fn receive_loop(mut receiver: Receiver, count: Arc<AtomicU64>) {
    loop {
        match receiver.recv().await {
            Some((_publish, ack)) => {
                count.fetch_add(1, Ordering::Relaxed);
                // Auto-ack by dropping for QoS 0, explicit for QoS 1
                if let ManualAcknowledgement::QoS1(puback_token) = ack {
                    let _ = puback_token.accept(PubAckProperties::default()).await;
                }
            }
            None => break,
        }
    }
}
