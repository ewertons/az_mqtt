<# compare.ps1 – Find the latest result files for each client, merge, and display
   a side-by-side comparison table.  Outputs both a table to the terminal and a
   consolidated JSON to results\comparison_<ts>.json. #>

$ErrorActionPreference = "Stop"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ResultsDir = Join-Path $ScriptDir "results"
$Timestamp  = Get-Date -Format "yyyyMMdd_HHmmss"

# Find latest result per client
$azFiles   = Get-ChildItem -Path $ResultsDir -Filter "az_mqtt5_*.json" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
$pahoFiles = Get-ChildItem -Path $ResultsDir -Filter "paho_*.json"     -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending

if (-not $azFiles -and -not $pahoFiles) {
    Write-Error "No result files found in $ResultsDir.  Run run_az_mqtt5.ps1 and run_paho.ps1 first."
    exit 1
}

$azData   = if ($azFiles)   { Get-Content $azFiles[0].FullName   | ConvertFrom-Json } else { $null }
$pahoData = if ($pahoFiles) { Get-Content $pahoFiles[0].FullName | ConvertFrom-Json } else { $null }

# Save consolidated JSON
$comp = @{
    timestamp   = $Timestamp
    az_mqtt5    = $azData
    paho_mqtt_c = $pahoData
}
$compFile = Join-Path $ResultsDir "comparison_${Timestamp}.json"
$comp | ConvertTo-Json -Depth 5 | Set-Content $compFile
Write-Host "Consolidated JSON saved to: $compFile`n"

# ---- Table ----

function Get-Val($obj, $field) {
    if ($null -eq $obj) { return "-" }
    $v = $obj.PSObject.Properties[$field]
    if ($null -eq $v) { return "-" }
    return $v.Value
}

function Get-Delta($a, $b) {
    if ($a -eq "-" -or $b -eq "-" -or $b -eq 0) { return "-" }
    $d = (([double]$a - [double]$b) / [double]$b) * 100
    return ("{0:+0.0;-0.0}%" -f $d)
}

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
Write-Host ("=" * 78)
Write-Host "                  MQTT CLIENT PERFORMANCE COMPARISON"
Write-Host ("=" * 78)
Write-Host ""

$fmt = "{0,-28} {1,18} {2,18} {3,12}"
Write-Host ($fmt -f "Metric", "az_mqtt5", "paho_mqtt_c", "Delta")
Write-Host ($fmt -f ("-" * 28), ("-" * 18), ("-" * 18), ("-" * 12))

foreach ($f in $fields) {
    $label = $f[0]
    $key   = $f[1]
    $a = Get-Val $azData   $key
    $b = Get-Val $pahoData $key
    $d = Get-Delta $a $b
    Write-Host ($fmt -f $label, $a, $b, $d)
}

Write-Host ""
Write-Host "(Delta = (az_mqtt5 - paho) / paho * 100.  Negative = az_mqtt5 uses fewer resources.)"
Write-Host ""

# Broker snapshots
$azBroker   = Get-ChildItem -Path $ResultsDir -Filter "broker_az_mqtt5_*.json" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$pahoBroker = Get-ChildItem -Path $ResultsDir -Filter "broker_paho_*.json"     -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1

if ($azBroker -or $pahoBroker) {
    Write-Host "Broker stat snapshots:"
    if ($azBroker)   { Write-Host "  az_mqtt5 run : $($azBroker.FullName)" }
    if ($pahoBroker) { Write-Host "  paho run     : $($pahoBroker.FullName)" }
    Write-Host "(Inspect these files to compare broker-side resource usage between runs.)"
}
