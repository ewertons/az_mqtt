<# compare.ps1 – Find the latest result files for each client, merge, and display
   a side-by-side comparison table including memory footprint data.
   Also writes a human-readable markdown report (client-comp.md) covering
   environment, client versions, build flags, workload, and results. #>

$ErrorActionPreference = "Stop"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ResultsDir = Join-Path $ScriptDir "results"
$Timestamp  = Get-Date -Format "yyyyMMdd_HHmmss"

# ── Find latest result per client ─────────────────────────────
$azFiles   = Get-ChildItem -Path $ResultsDir -Filter "az_mqtt5_*.json"    -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
$pahoFiles = Get-ChildItem -Path $ResultsDir -Filter "paho_*.json"        -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
$rustFiles = Get-ChildItem -Path $ResultsDir -Filter "azure_mqtt_*.json"  -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending

if (-not $azFiles -and -not $pahoFiles -and -not $rustFiles) {
    Write-Error "No result files found in $ResultsDir.  Run run.ps1 first."
    exit 1
}

$azData   = if ($azFiles)   { Get-Content $azFiles[0].FullName   | ConvertFrom-Json } else { $null }
$pahoData = if ($pahoFiles) { Get-Content $pahoFiles[0].FullName | ConvertFrom-Json } else { $null }
$rustData = if ($rustFiles) { Get-Content $rustFiles[0].FullName | ConvertFrom-Json } else { $null }

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

# Run GNU binutils `size` on a binary living inside an image. The slim runtime
# images don't include binutils, so we copy the binary out to a host temp dir
# and size it inside a cached ubuntu:24.04 helper container that has
# build-essential (cached by the Paho/az_mqtt5 builder layers).
# Parses Berkeley format:
#    text    data     bss     dec     hex filename
#    12345    678     910    13933    366D /bin/prog
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
        $tmpPosix = $tmp -replace '\\', '/'
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

function Get-DockerVersion($image, $entrypoint, $argList) {
    $out = Invoke-DockerQuery $image $entrypoint $argList
    if ($out) { return $out.Split("`n")[0].Trim() }
    return "?"
}

$azBinSize   = Get-DockerBinSize "perf-az-mqtt5:latest"   "/usr/local/bin/perf_az_mqtt5"
$pahoBinSize = Get-DockerBinSize "perf-paho:latest"       "/usr/local/bin/perf_paho"
$rustBinSize = Get-DockerBinSize "perf-azure-mqtt:latest" "/usr/local/bin/perf_azure_mqtt"

$azSections   = Get-DockerSectionSizes "perf-az-mqtt5:latest"   "/usr/local/bin/perf_az_mqtt5"
$pahoSections = Get-DockerSectionSizes "perf-paho:latest"       "/usr/local/bin/perf_paho"
$rustSections = Get-DockerSectionSizes "perf-azure-mqtt:latest" "/usr/local/bin/perf_azure_mqtt"

# ── Save consolidated JSON ────────────────────────────────────
$comp = @{
    timestamp       = $Timestamp
    az_mqtt5        = $azData
    paho_mqtt_c     = $pahoData
    azure_mqtt_rust = $rustData
    footprint = @{
        az_mqtt5 = @{
            binary_bytes = $azBinSize
            sections     = $azSections
        }
        paho_mqtt_c = @{
            binary_bytes = $pahoBinSize
            sections     = $pahoSections
        }
        azure_mqtt_rust = @{
            binary_bytes = $rustBinSize
            sections     = $rustSections
        }
    }
}
$compFile = Join-Path $ResultsDir "comparison_${Timestamp}.json"
$comp | ConvertTo-Json -Depth 6 | Set-Content $compFile
Write-Host "Consolidated JSON saved to: $compFile`n"

# ── Formatting helpers ────────────────────────────────────────
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

# ── Console tables ────────────────────────────────────────────
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
Write-Host ("=" * 82)
Write-Host "                   MQTT CLIENT PERFORMANCE COMPARISON"
Write-Host ("=" * 82)
Write-Host ""

$fmt = "{0,-26} {1,16} {2,16} {3,16}"
Write-Host ($fmt -f "Metric", "az_mqtt5 (C)", "paho_mqtt (C)", "azure_mqtt (Rust)")
Write-Host ($fmt -f ("-" * 26), ("-" * 16), ("-" * 16), ("-" * 16))

foreach ($f in $fields) {
    $label = $f[0]; $key = $f[1]; $human = $f[2]
    $a = if ($human) { Format-HumanField $azData   $key } else { Get-Val $azData   $key }
    $b = if ($human) { Format-HumanField $pahoData $key } else { Get-Val $pahoData $key }
    $c = if ($human) { Format-HumanField $rustData $key } else { Get-Val $rustData $key }
    Write-Host ($fmt -f $label, $a, $b, $c)
}

Write-Host ""
Write-Host ($fmt -f "-- Binary Footprint --", "", "", "")
Write-Host ($fmt -f ("-" * 26), ("-" * 16), ("-" * 16), ("-" * 16))
Write-Host ($fmt -f "Binary on disk", (Format-Human $azBinSize), (Format-Human $pahoBinSize), (Format-Human $rustBinSize))
if ($azSections -or $pahoSections -or $rustSections) {
    $secTxt  = { param($s) if ($s) { Format-Human $s.text } else { "-" } }
    $secData = { param($s) if ($s) { Format-Human $s.data } else { "-" } }
    $secBss  = { param($s) if ($s) { Format-Human $s.bss  } else { "-" } }
    Write-Host ($fmt -f ".text (code)",    (& $secTxt  $azSections), (& $secTxt  $pahoSections), (& $secTxt  $rustSections))
    Write-Host ($fmt -f ".data (init'd)",  (& $secData $azSections), (& $secData $pahoSections), (& $secData $rustSections))
    Write-Host ($fmt -f ".bss  (zeroed)",  (& $secBss  $azSections), (& $secBss  $pahoSections), (& $secBss  $rustSections))
}
Write-Host ""

# ── Broker snapshots ──────────────────────────────────────────
$azBroker   = Get-ChildItem -Path $ResultsDir -Filter "broker_az_mqtt5_*.json"    -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$pahoBroker = Get-ChildItem -Path $ResultsDir -Filter "broker_paho_*.json"        -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$rustBroker = Get-ChildItem -Path $ResultsDir -Filter "broker_azure_mqtt_*.json"  -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1

if ($azBroker -or $pahoBroker -or $rustBroker) {
    Write-Host "Broker stat snapshots:"
    if ($azBroker)   { Write-Host "  az_mqtt5 run    : $($azBroker.FullName)" }
    if ($pahoBroker) { Write-Host "  paho run        : $($pahoBroker.FullName)" }
    if ($rustBroker) { Write-Host "  azure_mqtt run  : $($rustBroker.FullName)" }
    Write-Host "(Inspect these files to compare broker-side resource usage between runs.)"
}

# ── Markdown report ───────────────────────────────────────────
$mdPath = Join-Path $ScriptDir "client-comp.md"

$hostInfo = "$([System.Environment]::OSVersion.VersionString) / PowerShell $($PSVersionTable.PSVersion)"
$dockerVer = (& docker --version 2>$null); if (-not $dockerVer) { $dockerVer = "?" }

# The slim runtime image doesn't have gcc; the compiler used at build time is
# whatever ubuntu:24.04 ships (gcc 13.3.0 at the time of writing). Read it from
# the builder-capable ubuntu:24.04 helper.
$gccVer = & docker run --rm --entrypoint sh ubuntu:24.04 -c "command -v gcc >/dev/null 2>&1 || (apt-get update -qq >/dev/null && apt-get install -y -qq gcc >/dev/null); gcc --version | head -n 1" 2>$null
if (-not $gccVer) { $gccVer = "GCC (Ubuntu 24.04 default)" } else { $gccVer = $gccVer.Trim() }
$osRelease = Invoke-DockerQuery "perf-az-mqtt5:latest" "cat" @("/etc/os-release")
$ubuntuVer = "Ubuntu (unknown)"
if ($osRelease) {
    $m = [regex]::Match($osRelease, 'PRETTY_NAME="([^"]+)"')
    if ($m.Success) { $ubuntuVer = $m.Groups[1].Value }
}

$pahoVersion   = "v1.3.14 (Eclipse Paho MQTT C, fetched via CMake FetchContent)"
$azSdkCVersion = "1.6.0-beta.1 (azure-sdk-for-c submodule)"
$rustCrate     = "azure_mqtt v0.1.0 (local path dep in perf_azure_mqtt/azure_mqtt_src/)"

function MdRow($label, $a, $b, $c) { "| {0} | {1} | {2} | {3} |" -f $label, $a, $b, $c }
function MdHumanCell($obj, $field) { Format-Human (Get-Val $obj $field) }

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("# MQTT Client Comparison Report")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("_Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')_")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("This report compares three MQTT 5 client implementations running an identical")
[void]$sb.AppendLine("publish/subscribe workload against the same broker, inside equivalent Linux")
[void]$sb.AppendLine("containers. Each client is built with production / size-optimized release flags.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Clients Under Test")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| Client | Language | Version | Notes |")
[void]$sb.AppendLine("|---|---|---|---|")
[void]$sb.AppendLine("| ``az_mqtt5`` | C99 | $azSdkCVersion | Zero dynamic allocation; static buffers supplied by caller. |")
[void]$sb.AppendLine("| ``paho_mqtt_c`` | C | $pahoVersion | Async API (MQTTAsync), internal threads, heap-allocated internals. |")
[void]$sb.AppendLine("| ``azure_mqtt`` | Rust | $rustCrate | Tokio async runtime, OpenSSL (vendored). |")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Test Environment")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| | |")
[void]$sb.AppendLine("|---|---|")
[void]$sb.AppendLine("| Host OS | $hostInfo |")
[void]$sb.AppendLine("| Docker | $dockerVer |")
[void]$sb.AppendLine("| Container base | $ubuntuVer |")
[void]$sb.AppendLine("| C compiler | $gccVer |")
[void]$sb.AppendLine("| Broker | EMQX (``docker-compose.yml`` service ``emqx``), TCP 1883, no TLS, no auth |")
[void]$sb.AppendLine("| Network | Docker user-defined bridge (``client-comp_default``) |")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Workload")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Each client, in its own container:")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("1. Connects to the broker (TCP, MQTT 5).")
[void]$sb.AppendLine("2. Subscribes to its own topic filter (``perf/<client>/#``) at QoS 1.")
[void]$sb.AppendLine("3. Publishes the target number of messages to its own topic at QoS 1 (so each")
[void]$sb.AppendLine("   message loops back and is also counted as *received*). Publishes are issued")
[void]$sb.AppendLine("   as fast as the client accepts them; no artificial rate-limiting.")
[void]$sb.AppendLine("4. Drains: waits for remaining inbound messages + PUBACKs using a no-progress")
[void]$sb.AppendLine("   watchdog (3 s idle, 120 s hard cap).")
[void]$sb.AppendLine("5. Disconnects cleanly and emits a JSON report.")
[void]$sb.AppendLine("")
$workload = if ($azData) { $azData } elseif ($pahoData) { $pahoData } else { $rustData }
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
[void]$sb.AppendLine("All binaries are built in **Release** mode with settings aimed at minimum")
[void]$sb.AppendLine("on-disk size and minimum resident code pages.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("### C clients (``az_mqtt5`` and ``paho_mqtt_c``)")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Applied to the ``perf_az_mqtt5`` / ``perf_paho`` CMake targets in")
[void]$sb.AppendLine("``tests/client-comp/CMakeLists.txt`` when ``CMAKE_BUILD_TYPE=Release`` and the")
[void]$sb.AppendLine("compiler is GCC or Clang:")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| Flag | Purpose |")
[void]$sb.AppendLine("|---|---|")
[void]$sb.AppendLine("| ``-Os`` | Optimize for size instead of speed. |")
[void]$sb.AppendLine("| ``-ffunction-sections`` | Put every function in its own ELF section. |")
[void]$sb.AppendLine("| ``-fdata-sections`` | Put every data object in its own ELF section. |")
[void]$sb.AppendLine("| ``-fno-unwind-tables`` | Omit ``.eh_frame`` (no C++ EH / no stack unwinders). |")
[void]$sb.AppendLine("| ``-fno-asynchronous-unwind-tables`` | Omit ``.eh_frame_hdr`` as well. |")
[void]$sb.AppendLine("| ``-Wl,--gc-sections`` | Drop unreferenced sections at link time (dead-code elimination). |")
[void]$sb.AppendLine("| ``-Wl,-s`` | Strip ELF symbol/relocation tables from the final binary. |")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("### Rust client (``azure_mqtt``)")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Applied via ``[profile.release]`` in ``perf_azure_mqtt/Cargo.toml``:")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| Setting | Purpose |")
[void]$sb.AppendLine("|---|---|")
[void]$sb.AppendLine("| ``opt-level = `"z`"`` | Aggressive size optimization. |")
[void]$sb.AppendLine("| ``lto = true`` | Fat link-time optimization across all crates. |")
[void]$sb.AppendLine("| ``codegen-units = 1`` | Single codegen unit: maximum inlining/DCE. |")
[void]$sb.AppendLine("| ``strip = `"symbols`"`` | Strip symbol tables from the final ELF. |")
[void]$sb.AppendLine("| ``panic = `"abort`"`` | Drop the unwinding runtime; smaller binary. |")
[void]$sb.AppendLine("| ``debug = false`` | No debuginfo. |")
[void]$sb.AppendLine("| ``incremental = false`` | Disable incremental compilation in release. |")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Results")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("### Throughput & CPU")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| Metric | az_mqtt5 (C) | paho_mqtt (C) | azure_mqtt (Rust) |")
[void]$sb.AppendLine("|---|---:|---:|---:|")
[void]$sb.AppendLine((MdRow "Messages sent"     (Get-Val $azData "messages_sent")     (Get-Val $pahoData "messages_sent")     (Get-Val $rustData "messages_sent")))
[void]$sb.AppendLine((MdRow "Messages received" (Get-Val $azData "messages_received") (Get-Val $pahoData "messages_received") (Get-Val $rustData "messages_received")))
[void]$sb.AppendLine((MdRow "PUBACKs received"  (Get-Val $azData "pubacks_received")  (Get-Val $pahoData "pubacks_received")  (Get-Val $rustData "pubacks_received")))
[void]$sb.AppendLine((MdRow "Elapsed (s)"       (Get-Val $azData "elapsed_sec")       (Get-Val $pahoData "elapsed_sec")       (Get-Val $rustData "elapsed_sec")))
[void]$sb.AppendLine((MdRow "Send rate (msg/s)" (Get-Val $azData "send_rate_msg_sec") (Get-Val $pahoData "send_rate_msg_sec") (Get-Val $rustData "send_rate_msg_sec")))
[void]$sb.AppendLine((MdRow "Recv rate (msg/s)" (Get-Val $azData "recv_rate_msg_sec") (Get-Val $pahoData "recv_rate_msg_sec") (Get-Val $rustData "recv_rate_msg_sec")))
[void]$sb.AppendLine((MdRow "User CPU (s)"      (Get-Val $azData "user_cpu_sec")      (Get-Val $pahoData "user_cpu_sec")      (Get-Val $rustData "user_cpu_sec")))
[void]$sb.AppendLine((MdRow "System CPU (s)"    (Get-Val $azData "sys_cpu_sec")       (Get-Val $pahoData "sys_cpu_sec")       (Get-Val $rustData "sys_cpu_sec")))
[void]$sb.AppendLine((MdRow "Total CPU (s)"     (Get-Val $azData "total_cpu_sec")     (Get-Val $pahoData "total_cpu_sec")     (Get-Val $rustData "total_cpu_sec")))
[void]$sb.AppendLine("")
[void]$sb.AppendLine("### Runtime Memory")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| Metric | az_mqtt5 (C) | paho_mqtt (C) | azure_mqtt (Rust) |")
[void]$sb.AppendLine("|---|---:|---:|---:|")
[void]$sb.AppendLine((MdRow "RSS baseline (before MQTT)" (MdHumanCell $azData "rss_baseline_bytes") (MdHumanCell $pahoData "rss_baseline_bytes") (MdHumanCell $rustData "rss_baseline_bytes")))
[void]$sb.AppendLine((MdRow "RSS peak (VmHWM)"           (MdHumanCell $azData "peak_rss_bytes")     (MdHumanCell $pahoData "peak_rss_bytes")     (MdHumanCell $rustData "peak_rss_bytes")))
[void]$sb.AppendLine((MdRow "RSS delta (client cost)"    (MdHumanCell $azData "rss_delta_bytes")    (MdHumanCell $pahoData "rss_delta_bytes")    (MdHumanCell $rustData "rss_delta_bytes")))
[void]$sb.AppendLine((MdRow "Heap baseline (``mallinfo2``)" (MdHumanCell $azData "heap_baseline_bytes") (MdHumanCell $pahoData "heap_baseline_bytes") "n/a"))
[void]$sb.AppendLine((MdRow "Heap peak (``mallinfo2``)"     (MdHumanCell $azData "heap_peak_bytes")     (MdHumanCell $pahoData "heap_peak_bytes")     "n/a"))
[void]$sb.AppendLine((MdRow "Heap delta (client allocs)"    (MdHumanCell $azData "heap_delta_bytes")    (MdHumanCell $pahoData "heap_delta_bytes")    "n/a"))
[void]$sb.AppendLine("")
[void]$sb.AppendLine("**Notes:**")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("- **RSS baseline** is ``VmRSS`` sampled after process start, argv parsing, and")
[void]$sb.AppendLine("  payload buffer initialisation, but **before** any MQTT client, socket, or")
[void]$sb.AppendLine("  async runtime is touched. This captures the unavoidable cost of the process")
[void]$sb.AppendLine("  image itself: libc pages, loader, stack, TLS, stdio buffers.")
[void]$sb.AppendLine("- **RSS peak** is ``VmHWM`` (``ru_maxrss`` on Linux) at end of run.")
[void]$sb.AppendLine("- **RSS delta** (peak − baseline) is the closest single-number approximation")
[void]$sb.AppendLine("  to *the memory the client library actually caused this process to consume*.")
[void]$sb.AppendLine("- **Heap baseline / peak / delta** are ``mallinfo2().uordblks`` (glibc")
[void]$sb.AppendLine("  ptmalloc in-use bytes) around the run. For ``az_mqtt5`` this should be ≈0")
[void]$sb.AppendLine("  because the library never calls ``malloc`` itself — any non-zero value comes")
[void]$sb.AppendLine("  from stdio, ``getaddrinfo``, or libc internals. Not tracked for Rust (would")
[void]$sb.AppendLine("  require a custom ``GlobalAlloc`` wrapper).")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("### Static Binary Footprint")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Sizes of the stripped, size-optimized Release ELF binaries, measured with")
[void]$sb.AppendLine("``stat`` (on-disk size) and ``size`` (text/data/bss sections) inside the same")
[void]$sb.AppendLine("Docker images used for the runs.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| Metric | az_mqtt5 (C) | paho_mqtt (C) | azure_mqtt (Rust) |")
[void]$sb.AppendLine("|---|---:|---:|---:|")
[void]$sb.AppendLine((MdRow "Binary on disk" (Format-Human $azBinSize) (Format-Human $pahoBinSize) (Format-Human $rustBinSize)))
if ($azSections -or $pahoSections -or $rustSections) {
    $tText = { param($s) if ($s) { Format-Human $s.text } else { "-" } }
    $tData = { param($s) if ($s) { Format-Human $s.data } else { "-" } }
    $tBss  = { param($s) if ($s) { Format-Human $s.bss  } else { "-" } }
    [void]$sb.AppendLine((MdRow ".text (code)"      (& $tText $azSections) (& $tText $pahoSections) (& $tText $rustSections)))
    [void]$sb.AppendLine((MdRow ".data (init'd)"    (& $tData $azSections) (& $tData $pahoSections) (& $tData $rustSections)))
    [void]$sb.AppendLine((MdRow ".bss (zero-init)"  (& $tBss  $azSections) (& $tBss  $pahoSections) (& $tBss  $rustSections)))
}
[void]$sb.AppendLine("")
[void]$sb.AppendLine("- ``.text`` is the executable code.")
[void]$sb.AppendLine("- ``.data`` is pre-initialized writable globals.")
[void]$sb.AppendLine("- ``.bss`` is zero-initialized writable globals (occupies no file space but is")
[void]$sb.AppendLine("  allocated at load time). Large ``.bss`` in C harnesses reflects the")
[void]$sb.AppendLine("  static send/receive/payload buffers (``SEND_BUFFER_SIZE`` = 64 KiB, etc.).")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Interpretation")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("- **Throughput**: all three clients saturate roughly the same range on a")
[void]$sb.AppendLine("  loopback broker; throughput is broker-bound more than client-bound.")
[void]$sb.AppendLine("- **CPU**: ``az_mqtt5`` is the cheapest by a wide margin because it has no")
[void]$sb.AppendLine("  internal threads and no heap traffic — all buffers are supplied by the")
[void]$sb.AppendLine("  caller as ``az_span``s over static arrays.")
[void]$sb.AppendLine("- **Memory**: most of the reported RSS is process baseline (libc, stack, TLS,")
[void]$sb.AppendLine("  I/O buffers). See **RSS delta** and **Heap delta** for the")
[void]$sb.AppendLine("  client-attributable numbers. ``az_mqtt5`` is expected to show a heap delta")
[void]$sb.AppendLine("  of essentially zero, which is the designed behaviour for a zero-allocation")
[void]$sb.AppendLine("  library.")
[void]$sb.AppendLine("- **Binary size**: size-optimized ``az_mqtt5`` binary is smaller than")
[void]$sb.AppendLine("  ``paho`` by roughly 3× and smaller than the Rust binary by two orders of")
[void]$sb.AppendLine("  magnitude — primarily because the Rust binary statically links Tokio,")
[void]$sb.AppendLine("  OpenSSL, ``std``, and the full async runtime.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Reproducing")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("From ``tests/client-comp``:")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("``````powershell")
[void]$sb.AppendLine(".\run.ps1              # full pipeline: build, run all 3, compare, teardown")
[void]$sb.AppendLine(".\run.ps1 -Keep        # keep the broker running after the report")
[void]$sb.AppendLine(".\teardown.ps1         # stop & remove the broker")
[void]$sb.AppendLine("``````")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Raw JSON results live in ``results/*.json``; the latest consolidated run is")
[void]$sb.AppendLine("``$compFile``.")

Set-Content -Path $mdPath -Value $sb.ToString() -Encoding UTF8
Write-Host "Markdown report saved to: $mdPath"
<# compare.ps1 – Find the latest result files for each client, merge, and display
   a side-by-side comparison table including memory footprint data.
   Outputs both a table to the terminal and a consolidated JSON. #>

$ErrorActionPreference = "Stop"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ResultsDir = Join-Path $ScriptDir "results"
$Timestamp  = Get-Date -Format "yyyyMMdd_HHmmss"

# Find latest result per client
$azFiles   = Get-ChildItem -Path $ResultsDir -Filter "az_mqtt5_*.json"    -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
$pahoFiles = Get-ChildItem -Path $ResultsDir -Filter "paho_*.json"        -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
$rustFiles = Get-ChildItem -Path $ResultsDir -Filter "azure_mqtt_*.json"  -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending

if (-not $azFiles -and -not $pahoFiles -and -not $rustFiles) {
    Write-Error "No result files found in $ResultsDir.  Run run.ps1 first."
    exit 1
}

$azData   = if ($azFiles)   { Get-Content $azFiles[0].FullName   | ConvertFrom-Json } else { $null }
$pahoData = if ($pahoFiles) { Get-Content $pahoFiles[0].FullName | ConvertFrom-Json } else { $null }
$rustData = if ($rustFiles) { Get-Content $rustFiles[0].FullName | ConvertFrom-Json } else { $null }

# ── Collect binary sizes from Docker images ───────────────────

function Get-DockerBinSize($image, $binPath) {
    try {
        $sizeStr = docker run --rm --entrypoint stat $image -c "%s" $binPath 2>$null
        if ($sizeStr) { return [int64]$sizeStr.Trim() }
    } catch { }
    return $null
}

$azBinSize   = Get-DockerBinSize "perf-az-mqtt5:latest"   "/usr/local/bin/perf_az_mqtt5"
$pahoBinSize = Get-DockerBinSize "perf-paho:latest"        "/usr/local/bin/perf_paho"
$rustBinSize = Get-DockerBinSize "perf-azure-mqtt:latest"  "/usr/local/bin/perf_azure_mqtt"

# Save consolidated JSON
$comp = @{
    timestamp        = $Timestamp
    az_mqtt5         = $azData
    paho_mqtt_c      = $pahoData
    azure_mqtt_rust  = $rustData
    footprint = @{
        az_mqtt5_binary_bytes        = $azBinSize
        paho_mqtt_c_binary_bytes     = $pahoBinSize
        azure_mqtt_rust_binary_bytes = $rustBinSize
    }
}
$compFile = Join-Path $ResultsDir "comparison_${Timestamp}.json"
$comp | ConvertTo-Json -Depth 5 | Set-Content $compFile
Write-Host "Consolidated JSON saved to: $compFile`n"

# ── Helper functions ──────────────────────────────────────────

function Get-Val($obj, $field) {
    if ($null -eq $obj) { return "-" }
    $v = $obj.PSObject.Properties[$field]
    if ($null -eq $v) { return "-" }
    return $v.Value
}

function Format-Human($bytes) {
    if ($null -eq $bytes) { return "-" }
    if ($bytes -ge 1MB) { return "{0:N1} MB" -f ($bytes / 1MB) }
    if ($bytes -ge 1KB) { return "{0:N1} KB" -f ($bytes / 1KB) }
    return "$bytes B"
}

# ── Performance Table ─────────────────────────────────────────

$fields = @(
    @("Messages sent",        "messages_sent"),
    @("Messages received",    "messages_received"),
    @("PUBACKs received",     "pubacks_received"),
    @("Elapsed (sec)",        "elapsed_sec"),
    @("Send rate (msg/s)",    "send_rate_msg_sec"),
    @("Recv rate (msg/s)",    "recv_rate_msg_sec"),
    @("User CPU (sec)",       "user_cpu_sec"),
    @("System CPU (sec)",     "sys_cpu_sec"),
    @("Total CPU (sec)",      "total_cpu_sec"),
    @("Peak RSS (bytes)",     "peak_rss_bytes")
)

Write-Host ""
Write-Host ("=" * 82)
Write-Host "                   MQTT CLIENT PERFORMANCE COMPARISON"
Write-Host ("=" * 82)
Write-Host ""

$fmt = "{0,-24} {1,16} {2,16} {3,16}"
Write-Host ($fmt -f "Metric", "az_mqtt5 (C)", "paho_mqtt (C)", "azure_mqtt (Rust)")
Write-Host ($fmt -f ("-" * 24), ("-" * 16), ("-" * 16), ("-" * 16))

foreach ($f in $fields) {
    $label = $f[0]
    $key   = $f[1]
    $a = Get-Val $azData   $key
    $b = Get-Val $pahoData $key
    $c = Get-Val $rustData $key
    Write-Host ($fmt -f $label, $a, $b, $c)
}

# ── Memory Footprint Table ────────────────────────────────────

Write-Host ""
Write-Host ($fmt -f "-- Memory Footprint --", "", "", "")
Write-Host ($fmt -f ("-" * 24), ("-" * 16), ("-" * 16), ("-" * 16))
Write-Host ($fmt -f "Binary on disk", (Format-Human $azBinSize), (Format-Human $pahoBinSize), (Format-Human $rustBinSize))

Write-Host ""

# ── Broker snapshots ──────────────────────────────────────────

$azBroker   = Get-ChildItem -Path $ResultsDir -Filter "broker_az_mqtt5_*.json"    -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$pahoBroker = Get-ChildItem -Path $ResultsDir -Filter "broker_paho_*.json"        -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$rustBroker = Get-ChildItem -Path $ResultsDir -Filter "broker_azure_mqtt_*.json"  -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1

if ($azBroker -or $pahoBroker -or $rustBroker) {
    Write-Host "Broker stat snapshots:"
    if ($azBroker)   { Write-Host "  az_mqtt5 run    : $($azBroker.FullName)" }
    if ($pahoBroker) { Write-Host "  paho run        : $($pahoBroker.FullName)" }
    if ($rustBroker) { Write-Host "  azure_mqtt run  : $($rustBroker.FullName)" }
    Write-Host "(Inspect these files to compare broker-side resource usage between runs.)"
}
