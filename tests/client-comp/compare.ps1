<# compare.ps1 – Find the latest result files for each client, merge, and display
   a side-by-side comparison table including memory footprint data.
   Outputs both a table to the terminal and a consolidated JSON. #>

$ErrorActionPreference = "Stop"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ResultsDir = Join-Path $ScriptDir "results"
$BuildDir   = Join-Path $ScriptDir "build"
$RustPerfDir = Join-Path $ScriptDir "perf_azure_mqtt"
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

# ── Collect binary sizes ──────────────────────────────────────

function Get-BinSize($path) {
    if (Test-Path $path) { return (Get-Item $path).Length } else { return $null }
}

$azBin   = Join-Path $BuildDir "Release\perf_az_mqtt5.exe"
$pahoBin = Join-Path $BuildDir "Release\perf_paho.exe"
$rustBin = Join-Path $RustPerfDir "target\release\perf_azure_mqtt.exe"

$azBinSize   = Get-BinSize $azBin
$pahoBinSize = Get-BinSize $pahoBin
$rustBinSize = Get-BinSize $rustBin

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
