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

    // Build payload
    let payload: Bytes = (0..payload_bytes)
        .map(|i| b'A' + (i % 26) as u8)
        .collect::<Vec<u8>>()
        .into();

    let received = Arc::new(AtomicU64::new(0));

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

    let ConnectResult::Success(connection, _connack, _disconnect_handle) = connect_result else {
        eprintln!("[azure_mqtt] connect failed");
        std::process::exit(1);
    };
    eprintln!("[azure_mqtt] connected");

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

    // Spawn connection runner
    let conn_handle = tokio::spawn(async move {
        let _ = connection.run_until_disconnect().await;
    });

    // Spawn receiver
    let recv_count = received.clone();
    let recv_handle = tokio::spawn(async move {
        receive_loop(receiver, recv_count).await;
    });

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
            Ok(_ct) => {
                pub_sent += 1;
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

    // Drain
    eprintln!("[azure_mqtt] draining remaining messages...");
    tokio::time::sleep(Duration::from_secs(5)).await;

    let elapsed = start.elapsed().as_secs_f64();
    let (user_cpu_after, sys_cpu_after, peak_rss) = get_resource_usage();
    let pub_received = received.load(Ordering::Relaxed);

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
        pubacks_received: 0,
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
