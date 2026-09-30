# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
<# compare.ps1 – Find the latest result files for each client, merge, and display
   a side-by-side comparison table including memory footprint data.
   Also writes a human-readable markdown report (client-comp.md) covering
   environment, client versions, build flags, workload, and results.

   Four clients are compared:
     - az_mqtt5 (C / OpenSSL)
     - az_mqtt5 (C / mbedTLS)
     - paho_mqtt_c
     - azure_mqtt (Rust)
#>

$ErrorActionPreference = "Stop"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ResultsDir = Join-Path $ScriptDir "results"
$Timestamp  = Get-Date -Format "yyyyMMdd_HHmmss"

# ── Find latest result per client ─────────────────────────────
$azOpensslFiles = Get-ChildItem -Path $ResultsDir -Filter "az_mqtt5_openssl_*.json" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
$azMbedtlsFiles = Get-ChildItem -Path $ResultsDir -Filter "az_mqtt5_mbedtls_*.json" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
$pahoFiles      = Get-ChildItem -Path $ResultsDir -Filter "paho_*.json"             -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
$rustFiles      = Get-ChildItem -Path $ResultsDir -Filter "azure_mqtt_*.json"       -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending

if (-not $azOpensslFiles -and -not $azMbedtlsFiles -and -not $pahoFiles -and -not $rustFiles) {
    Write-Error "No result files found in $ResultsDir.  Run run.ps1 first."
    exit 1
}

$azOpensslData = if ($azOpensslFiles) { Get-Content $azOpensslFiles[0].FullName | ConvertFrom-Json } else { $null }
$azMbedtlsData = if ($azMbedtlsFiles) { Get-Content $azMbedtlsFiles[0].FullName | ConvertFrom-Json } else { $null }
$pahoData      = if ($pahoFiles)      { Get-Content $pahoFiles[0].FullName      | ConvertFrom-Json } else { $null }
$rustData      = if ($rustFiles)      { Get-Content $rustFiles[0].FullName      | ConvertFrom-Json } else { $null }

# ── Docker-side introspection helpers ─────────────────────────
function Invoke-DockerQuery($image, $entrypoint, $argList) {
    try {
        $out = & docker run --rm --entrypoint $entrypoint $image @argList 2>$null
        if ($LASTEXITCODE -eq 0) { return ($out -join "`n") }
    } catch { }
    return $null
}

function Get-DockerBinSize($image, $binPath) {
    $s = Invoke-DockerQuery $image "stat" @("-c", "%s", $binPath)
    if ($s) { return [int64]$s.Trim() }
    return $null
}

function Get-DockerSectionSizes($image, $binPath) {
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("size_" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    try {
        $cid = (& docker create --entrypoint "/bin/true" $image 2>$null).Trim()
        if (-not $cid) { return $null }
        try {
            & docker cp ("{0}:{1}" -f $cid, $binPath) (Join-Path $tmp "bin") 2>$null | Out-Null
        } finally {
            & docker rm $cid 2>$null | Out-Null
        }
        $out = & docker run --rm -v "${tmp}:/work" --entrypoint sh ubuntu:24.04 -c "command -v size >/dev/null 2>&1 || (apt-get update -qq >/dev/null && apt-get install -y -qq binutils >/dev/null); size /work/bin" 2>$null
        if (-not $out) { return $null }
        $lines = ($out -join "`n") -split "`n" | Where-Object { $_.Trim() -ne "" }
        if ($lines.Count -lt 2) { return $null }
        $cols = ($lines[-1].Trim() -split '\s+')
        if ($cols.Count -lt 3) { return $null }
        return [pscustomobject]@{
            text = [int64]$cols[0]
            data = [int64]$cols[1]
            bss  = [int64]$cols[2]
        }
    } finally {
        Remove-Item -Recurse -Force -Path $tmp -ErrorAction SilentlyContinue
    }
}

$azOpensslBinSize = Get-DockerBinSize "perf-az-mqtt5-openssl:latest" "/usr/local/bin/perf_az_mqtt5"
$azMbedtlsBinSize = Get-DockerBinSize "perf-az-mqtt5-mbedtls:latest" "/usr/local/bin/perf_az_mqtt5"
$pahoBinSize      = Get-DockerBinSize "perf-paho:latest"             "/usr/local/bin/perf_paho"
$rustBinSize      = Get-DockerBinSize "perf-azure-mqtt:latest"       "/usr/local/bin/perf_azure_mqtt"

$azOpensslSections = Get-DockerSectionSizes "perf-az-mqtt5-openssl:latest" "/usr/local/bin/perf_az_mqtt5"
$azMbedtlsSections = Get-DockerSectionSizes "perf-az-mqtt5-mbedtls:latest" "/usr/local/bin/perf_az_mqtt5"
$pahoSections      = Get-DockerSectionSizes "perf-paho:latest"             "/usr/local/bin/perf_paho"
$rustSections      = Get-DockerSectionSizes "perf-azure-mqtt:latest"       "/usr/local/bin/perf_azure_mqtt"

$comp = @{
    timestamp           = $Timestamp
    az_mqtt5_openssl    = $azOpensslData
    az_mqtt5_mbedtls    = $azMbedtlsData
    paho_mqtt_c         = $pahoData
    azure_mqtt_rust     = $rustData
    footprint = @{
        az_mqtt5_openssl = @{ binary_bytes = $azOpensslBinSize; sections = $azOpensslSections }
        az_mqtt5_mbedtls = @{ binary_bytes = $azMbedtlsBinSize; sections = $azMbedtlsSections }
        paho_mqtt_c      = @{ binary_bytes = $pahoBinSize;      sections = $pahoSections      }
        azure_mqtt_rust  = @{ binary_bytes = $rustBinSize;      sections = $rustSections      }
    }
}
$compFile = Join-Path $ResultsDir "comparison_${Timestamp}.json"
$comp | ConvertTo-Json -Depth 6 | Set-Content $compFile
Write-Host "Consolidated JSON saved to: $compFile`n"

function Get-Val($obj, $field) {
    if ($null -eq $obj) { return "-" }
    $v = $obj.PSObject.Properties[$field]
    if ($null -eq $v) { return "-" }
    return $v.Value
}

function Format-Human($bytes) {
    if ($null -eq $bytes -or $bytes -eq "" -or $bytes -eq "-") { return "-" }
    try { $b = [double]$bytes } catch { return "$bytes" }
    if ([math]::Abs($b) -ge 1MB) { return "{0:N1} MB" -f ($b / 1MB) }
    if ([math]::Abs($b) -ge 1KB) { return "{0:N1} KB" -f ($b / 1KB) }
    return ("{0} B" -f [int64]$b)
}

function Format-HumanField($obj, $field) { Format-Human (Get-Val $obj $field) }

function SecField($s, $name) {
    if ($null -eq $s) { return "-" }
    return Format-Human $s.$name
}

$fields = @(
    @("Messages sent",         "messages_sent",      $false),
    @("Messages received",     "messages_received",  $false),
    @("PUBACKs received",      "pubacks_received",   $false),
    @("Elapsed (sec)",         "elapsed_sec",        $false),
    @("Send rate (msg/s)",     "send_rate_msg_sec",  $false),
    @("Recv rate (msg/s)",     "recv_rate_msg_sec",  $false),
    @("User CPU (sec)",        "user_cpu_sec",       $false),
    @("System CPU (sec)",      "sys_cpu_sec",        $false),
    @("Total CPU (sec)",       "total_cpu_sec",      $false),
    @("Peak RSS",              "peak_rss_bytes",     $true),
    @("RSS baseline",          "rss_baseline_bytes", $true),
    @("RSS delta (peak-base)", "rss_delta_bytes",    $true),
    @("Heap baseline",         "heap_baseline_bytes",$true),
    @("Heap peak",             "heap_peak_bytes",    $true),
    @("Heap delta",            "heap_delta_bytes",   $true)
)

Write-Host ""
Write-Host ("=" * 110)
Write-Host "                        MQTT CLIENT PERFORMANCE COMPARISON"
Write-Host ("=" * 110)
Write-Host ""

$fmt = "{0,-26} {1,16} {2,16} {3,16} {4,16}"
Write-Host ($fmt -f "Metric", "az_mqtt5 (ossl)", "az_mqtt5 (mbed)", "paho_mqtt (C)", "azure_mqtt (Rust)")
Write-Host ($fmt -f ("-" * 26), ("-" * 16), ("-" * 16), ("-" * 16), ("-" * 16))

foreach ($f in $fields) {
    $label = $f[0]; $key = $f[1]; $human = $f[2]
    $a = if ($human) { Format-HumanField $azOpensslData $key } else { Get-Val $azOpensslData $key }
    $b = if ($human) { Format-HumanField $azMbedtlsData $key } else { Get-Val $azMbedtlsData $key }
    $c = if ($human) { Format-HumanField $pahoData      $key } else { Get-Val $pahoData      $key }
    $d = if ($human) { Format-HumanField $rustData      $key } else { Get-Val $rustData      $key }
    Write-Host ($fmt -f $label, $a, $b, $c, $d)
}

Write-Host ""
Write-Host ($fmt -f "-- Binary Footprint --", "", "", "", "")
Write-Host ($fmt -f ("-" * 26), ("-" * 16), ("-" * 16), ("-" * 16), ("-" * 16))
Write-Host ($fmt -f "Binary on disk",   (Format-Human $azOpensslBinSize), (Format-Human $azMbedtlsBinSize), (Format-Human $pahoBinSize), (Format-Human $rustBinSize))
Write-Host ($fmt -f ".text (code)",     (SecField $azOpensslSections text), (SecField $azMbedtlsSections text), (SecField $pahoSections text), (SecField $rustSections text))
Write-Host ($fmt -f ".data (init'd)",   (SecField $azOpensslSections data), (SecField $azMbedtlsSections data), (SecField $pahoSections data), (SecField $rustSections data))
Write-Host ($fmt -f ".bss (zeroed)",    (SecField $azOpensslSections bss),  (SecField $azMbedtlsSections bss),  (SecField $pahoSections bss),  (SecField $rustSections bss))
Write-Host ""

$mdPath = Join-Path $ScriptDir "client-comp.md"

$hostInfo  = "$([System.Environment]::OSVersion.VersionString) / PowerShell $($PSVersionTable.PSVersion)"
$dockerVer = (& docker --version 2>$null); if (-not $dockerVer) { $dockerVer = "?" }

$gccVer = & docker run --rm --entrypoint sh ubuntu:24.04 -c "command -v gcc >/dev/null 2>&1 || (apt-get update -qq >/dev/null && apt-get install -y -qq gcc >/dev/null); gcc --version | head -n 1" 2>$null
if (-not $gccVer) { $gccVer = "GCC (Ubuntu 24.04 default)" } else { $gccVer = $gccVer.Trim() }

$osRelease = Invoke-DockerQuery "perf-az-mqtt5-openssl:latest" "cat" @("/etc/os-release")
$ubuntuVer = "Ubuntu (unknown)"
if ($osRelease) {
    $m = [regex]::Match($osRelease, 'PRETTY_NAME="([^"]+)"')
    if ($m.Success) { $ubuntuVer = $m.Groups[1].Value }
}

$pahoVersion   = "v1.3.14 (Eclipse Paho MQTT C, fetched via CMake FetchContent)"
$azSdkCVersion = "1.6.0-beta.1 (azure-sdk-for-c submodule)"
$rustCrate     = "azure_mqtt v0.1.0 (local path dep in perf_azure_mqtt/azure_mqtt_src/)"

function MdRow4($label, $a, $b, $c, $d) { "| {0} | {1} | {2} | {3} | {4} |" -f $label, $a, $b, $c, $d }
function MdCell($obj, $field) { Get-Val $obj $field }
function MdHumanCell($obj, $field) { Format-Human (Get-Val $obj $field) }

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("# MQTT Client Comparison Report")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("_Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')_")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("This report compares **four** MQTT 5 client configurations running an identical")
[void]$sb.AppendLine("publish/subscribe workload against the same broker, inside equivalent Linux")
[void]$sb.AppendLine("containers. Each binary is built with production / size-optimized release flags.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Two of the four are the **same C source** (``az_mqtt5``) but linked against")
[void]$sb.AppendLine("different TLS backends — OpenSSL vs mbedTLS — so the footprint delta between")
[void]$sb.AppendLine("them isolates the TLS wrapper cost.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Clients Under Test")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| Client | Language | Version | TLS backend | Notes |")
[void]$sb.AppendLine("|---|---|---|---|---|")
[void]$sb.AppendLine("| ``az_mqtt5`` (openssl) | C99 | $azSdkCVersion | OpenSSL (``libssl3``, dynamic) | ``src/platform/transport_posix.c``. |")
[void]$sb.AppendLine("| ``az_mqtt5`` (mbedtls) | C99 | $azSdkCVersion | mbedTLS (dynamic) | ``src/platform/transport_mbedtls.c``. Same MQTT core. |")
[void]$sb.AppendLine("| ``paho_mqtt_c`` | C | $pahoVersion | OpenSSL | Async API (MQTTAsync), internal threads, heap-allocated internals. |")
[void]$sb.AppendLine("| ``azure_mqtt`` | Rust | $rustCrate | OpenSSL (vendored, static) | Tokio async runtime. |")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Test Environment")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| | |")
[void]$sb.AppendLine("|---|---|")
[void]$sb.AppendLine("| Host OS | $hostInfo |")
[void]$sb.AppendLine("| Docker | $dockerVer |")
[void]$sb.AppendLine("| Container base | $ubuntuVer |")
[void]$sb.AppendLine("| C compiler | $gccVer |")
[void]$sb.AppendLine("| Broker | EMQX, TCP 1883, no TLS, no auth |")
[void]$sb.AppendLine("| Network | Docker user-defined bridge (``client-comp_default``) |")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("> **Note on runtime TLS.** All runs connect to the broker over plain TCP (port")
[void]$sb.AppendLine("> 1883). TLS code is **linked in but not exercised** at runtime. Both C")
[void]$sb.AppendLine("> ``az_mqtt5`` variants **dynamically** link their TLS library (``libssl3`` /")
[void]$sb.AppendLine("> ``libmbedtls`` come from the runtime container), so the static binary size")
[void]$sb.AppendLine("> reflects only the wrapper transport code — not the TLS library proper.")
[void]$sb.AppendLine("> CPU / RSS / heap numbers are pure TCP.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Workload")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Each client, in its own container:")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("1. Connects to the broker (TCP, MQTT 5).")
[void]$sb.AppendLine("2. Subscribes to ``perf/<client>/#`` at QoS 1.")
[void]$sb.AppendLine("3. Publishes the target number of messages at QoS 1 (so each message loops back).")
[void]$sb.AppendLine("4. Drains remaining inbound + PUBACKs (3 s idle / 120 s hard cap).")
[void]$sb.AppendLine("5. Disconnects cleanly and emits a JSON report.")
[void]$sb.AppendLine("")
$workload = if ($azOpensslData) { $azOpensslData } elseif ($azMbedtlsData) { $azMbedtlsData } elseif ($pahoData) { $pahoData } else { $rustData }
$msgCount = Get-Val $workload "messages_sent"
$payload  = Get-Val $workload "payload_bytes"
[void]$sb.AppendLine("| Parameter | Value |")
[void]$sb.AppendLine("|---|---|")
[void]$sb.AppendLine("| Messages per run | $msgCount |")
[void]$sb.AppendLine("| Payload size | $payload bytes |")
[void]$sb.AppendLine("| QoS | 1 |")
[void]$sb.AppendLine("| Duration cap | 30 s publish window + up to 120 s drain |")
[void]$sb.AppendLine("| Clean session | yes |")
[void]$sb.AppendLine("| Keep-alive | 60 s |")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Production Build Flags")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("### C clients")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| Flag | Purpose |")
[void]$sb.AppendLine("|---|---|")
[void]$sb.AppendLine("| ``-Os`` | Optimize for size. |")
[void]$sb.AppendLine("| ``-ffunction-sections`` / ``-fdata-sections`` | One ELF section per symbol. |")
[void]$sb.AppendLine("| ``-fno-unwind-tables`` / ``-fno-asynchronous-unwind-tables`` | Omit ``.eh_frame[_hdr]``. |")
[void]$sb.AppendLine("| ``-Wl,--gc-sections`` | Drop unreferenced sections at link time. |")
[void]$sb.AppendLine("| ``-Wl,-s`` | Strip symbol/relocation tables. |")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("The two ``az_mqtt5`` variants differ only by ``-DAZ_MQTT_TLS_BACKEND=openssl`` vs")
[void]$sb.AppendLine("``-DAZ_MQTT_TLS_BACKEND=mbedtls`` passed at CMake configure time, which selects")
[void]$sb.AppendLine("``src/platform/transport_posix.c`` vs ``src/platform/transport_mbedtls.c`` and")
[void]$sb.AppendLine("links the matching TLS libraries.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("### Rust client")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| Setting | Purpose |")
[void]$sb.AppendLine("|---|---|")
[void]$sb.AppendLine("| ``opt-level = `"z`"`` | Aggressive size optimization. |")
[void]$sb.AppendLine("| ``lto = true`` | Fat LTO across all crates. |")
[void]$sb.AppendLine("| ``codegen-units = 1`` | Max inlining / DCE. |")
[void]$sb.AppendLine("| ``strip = `"symbols`"`` | Strip symbols. |")
[void]$sb.AppendLine("| ``panic = `"abort`"`` | Drop the unwinding runtime. |")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Results")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("### Throughput & CPU")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| Metric | az_mqtt5 (openssl) | az_mqtt5 (mbedtls) | paho_mqtt (C) | azure_mqtt (Rust) |")
[void]$sb.AppendLine("|---|---:|---:|---:|---:|")
[void]$sb.AppendLine((MdRow4 "Messages sent"     (MdCell $azOpensslData "messages_sent")     (MdCell $azMbedtlsData "messages_sent")     (MdCell $pahoData "messages_sent")     (MdCell $rustData "messages_sent")))
[void]$sb.AppendLine((MdRow4 "Messages received" (MdCell $azOpensslData "messages_received") (MdCell $azMbedtlsData "messages_received") (MdCell $pahoData "messages_received") (MdCell $rustData "messages_received")))
[void]$sb.AppendLine((MdRow4 "PUBACKs received"  (MdCell $azOpensslData "pubacks_received")  (MdCell $azMbedtlsData "pubacks_received")  (MdCell $pahoData "pubacks_received")  (MdCell $rustData "pubacks_received")))
[void]$sb.AppendLine((MdRow4 "Elapsed (s)"       (MdCell $azOpensslData "elapsed_sec")       (MdCell $azMbedtlsData "elapsed_sec")       (MdCell $pahoData "elapsed_sec")       (MdCell $rustData "elapsed_sec")))
[void]$sb.AppendLine((MdRow4 "Send rate (msg/s)" (MdCell $azOpensslData "send_rate_msg_sec") (MdCell $azMbedtlsData "send_rate_msg_sec") (MdCell $pahoData "send_rate_msg_sec") (MdCell $rustData "send_rate_msg_sec")))
[void]$sb.AppendLine((MdRow4 "Recv rate (msg/s)" (MdCell $azOpensslData "recv_rate_msg_sec") (MdCell $azMbedtlsData "recv_rate_msg_sec") (MdCell $pahoData "recv_rate_msg_sec") (MdCell $rustData "recv_rate_msg_sec")))
[void]$sb.AppendLine((MdRow4 "User CPU (s)"      (MdCell $azOpensslData "user_cpu_sec")      (MdCell $azMbedtlsData "user_cpu_sec")      (MdCell $pahoData "user_cpu_sec")      (MdCell $rustData "user_cpu_sec")))
[void]$sb.AppendLine((MdRow4 "System CPU (s)"    (MdCell $azOpensslData "sys_cpu_sec")       (MdCell $azMbedtlsData "sys_cpu_sec")       (MdCell $pahoData "sys_cpu_sec")       (MdCell $rustData "sys_cpu_sec")))
[void]$sb.AppendLine((MdRow4 "Total CPU (s)"     (MdCell $azOpensslData "total_cpu_sec")     (MdCell $azMbedtlsData "total_cpu_sec")     (MdCell $pahoData "total_cpu_sec")     (MdCell $rustData "total_cpu_sec")))
[void]$sb.AppendLine("")
[void]$sb.AppendLine("### Runtime Memory")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| Metric | az_mqtt5 (openssl) | az_mqtt5 (mbedtls) | paho_mqtt (C) | azure_mqtt (Rust) |")
[void]$sb.AppendLine("|---|---:|---:|---:|---:|")
[void]$sb.AppendLine((MdRow4 "RSS baseline (before MQTT)" (MdHumanCell $azOpensslData "rss_baseline_bytes") (MdHumanCell $azMbedtlsData "rss_baseline_bytes") (MdHumanCell $pahoData "rss_baseline_bytes") (MdHumanCell $rustData "rss_baseline_bytes")))
[void]$sb.AppendLine((MdRow4 "RSS peak (VmHWM)"           (MdHumanCell $azOpensslData "peak_rss_bytes")     (MdHumanCell $azMbedtlsData "peak_rss_bytes")     (MdHumanCell $pahoData "peak_rss_bytes")     (MdHumanCell $rustData "peak_rss_bytes")))
[void]$sb.AppendLine((MdRow4 "RSS delta (client cost)"    (MdHumanCell $azOpensslData "rss_delta_bytes")    (MdHumanCell $azMbedtlsData "rss_delta_bytes")    (MdHumanCell $pahoData "rss_delta_bytes")    (MdHumanCell $rustData "rss_delta_bytes")))
[void]$sb.AppendLine((MdRow4 "Heap baseline (``mallinfo2``)" (MdHumanCell $azOpensslData "heap_baseline_bytes") (MdHumanCell $azMbedtlsData "heap_baseline_bytes") (MdHumanCell $pahoData "heap_baseline_bytes") "n/a"))
[void]$sb.AppendLine((MdRow4 "Heap peak (``mallinfo2``)"     (MdHumanCell $azOpensslData "heap_peak_bytes")     (MdHumanCell $azMbedtlsData "heap_peak_bytes")     (MdHumanCell $pahoData "heap_peak_bytes")     "n/a"))
[void]$sb.AppendLine((MdRow4 "Heap delta (client allocs)"    (MdHumanCell $azOpensslData "heap_delta_bytes")    (MdHumanCell $azMbedtlsData "heap_delta_bytes")    (MdHumanCell $pahoData "heap_delta_bytes")    "n/a"))
[void]$sb.AppendLine("")
[void]$sb.AppendLine("**Notes:**")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("- **RSS baseline** is sampled after process start, argv parsing, and payload")
[void]$sb.AppendLine("  buffer init — but **before** any MQTT client, socket, or async runtime is")
[void]$sb.AppendLine("  touched.")
[void]$sb.AppendLine("- **RSS peak** is ``VmHWM`` (``ru_maxrss``) at end of run.")
[void]$sb.AppendLine("- **RSS delta** (peak − baseline) is the closest single-number approximation")
[void]$sb.AppendLine("  to *the memory the client library actually caused this process to consume*.")
[void]$sb.AppendLine("- **Heap** numbers use ``mallinfo2().uordblks`` (glibc ptmalloc in-use bytes).")
[void]$sb.AppendLine("  ``az_mqtt5`` never calls ``malloc`` itself; any non-zero value comes from")
[void]$sb.AppendLine("  libc internals, stdio, or ``getaddrinfo``. Not tracked for Rust.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("### Static Binary Footprint")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Sizes of the stripped, size-optimized Release ELF binaries, measured with")
[void]$sb.AppendLine("``stat`` (on-disk size) and ``size`` (text/data/bss sections) inside each image.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| Metric | az_mqtt5 (openssl) | az_mqtt5 (mbedtls) | paho_mqtt (C) | azure_mqtt (Rust) |")
[void]$sb.AppendLine("|---|---:|---:|---:|---:|")
[void]$sb.AppendLine((MdRow4 "Binary on disk"   (Format-Human $azOpensslBinSize) (Format-Human $azMbedtlsBinSize) (Format-Human $pahoBinSize) (Format-Human $rustBinSize)))
[void]$sb.AppendLine((MdRow4 ".text (code)"     (SecField $azOpensslSections text) (SecField $azMbedtlsSections text) (SecField $pahoSections text) (SecField $rustSections text)))
[void]$sb.AppendLine((MdRow4 ".data (init'd)"   (SecField $azOpensslSections data) (SecField $azMbedtlsSections data) (SecField $pahoSections data) (SecField $rustSections data)))
[void]$sb.AppendLine((MdRow4 ".bss (zero-init)" (SecField $azOpensslSections bss)  (SecField $azMbedtlsSections bss)  (SecField $pahoSections bss)  (SecField $rustSections bss)))
[void]$sb.AppendLine("")
[void]$sb.AppendLine("- ``.text`` is executable code. Both ``az_mqtt5`` variants dynamically link")
[void]$sb.AppendLine("  their TLS library, so the TLS code itself does **not** appear here; the")
[void]$sb.AppendLine("  small delta between them reflects only the wrapper (``transport_posix.c`` vs")
[void]$sb.AppendLine("  ``transport_mbedtls.c``).")
[void]$sb.AppendLine("- ``.data`` is pre-initialized writable globals.")
[void]$sb.AppendLine("- ``.bss`` is zero-initialized writable globals — dominated by the caller's")
[void]$sb.AppendLine("  static send / receive / payload / transport buffers.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Interpretation")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("- **Throughput / CPU**: unchanged by TLS backend at runtime (runtime is plain")
[void]$sb.AppendLine("  TCP). The two ``az_mqtt5`` columns should be statistically identical; any")
[void]$sb.AppendLine("  delta is measurement noise.")
[void]$sb.AppendLine("- **Binary footprint**: with *dynamic* TLS linkage the openssl-vs-mbedtls gap")
[void]$sb.AppendLine("  is small (only the wrapper differs). To surface the real TLS library")
[void]$sb.AppendLine("  footprint, rebuild against static TLS libraries.")
[void]$sb.AppendLine("- **Memory**: most RSS is the process baseline (libc, stack, thread-local")
[void]$sb.AppendLine("  storage, I/O buffers). See **RSS delta** and **Heap delta** for the")
[void]$sb.AppendLine("  client-attributable portion. ``az_mqtt5``'s heap delta is essentially zero")
[void]$sb.AppendLine("  (by design — no internal ``malloc``).")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Reproducing")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("From ``tests/client-comp``:")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("``````powershell")
[void]$sb.AppendLine(".\run.ps1              # full pipeline: build, run all 4, compare, teardown")
[void]$sb.AppendLine(".\run.ps1 -Keep        # keep the broker running after the report")
[void]$sb.AppendLine(".\teardown.ps1         # stop & remove the broker")
[void]$sb.AppendLine("``````")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Raw JSON results live in ``results/*.json``; the latest consolidated run is")
[void]$sb.AppendLine("``$compFile``.")

Set-Content -Path $mdPath -Value $sb.ToString() -Encoding UTF8
Write-Host "Markdown report saved to: $mdPath"
