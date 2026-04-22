<# run_paho.ps1 – Run the Paho MQTT C performance test and capture resource stats. #>

$ErrorActionPreference = "Stop"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$BuildDir   = Join-Path $ScriptDir "build"
$ResultsDir = Join-Path $ScriptDir "results"
$Binary     = Join-Path $BuildDir "Release\perf_paho.exe"

$Host_      = if ($env:PERF_HOST)      { $env:PERF_HOST }      else { "localhost" }
$Port       = if ($env:PERF_PORT)      { $env:PERF_PORT }      else { "1883" }
$MsgCount   = if ($env:PERF_MSG_COUNT) { $env:PERF_MSG_COUNT } else { "10000" }
$Payload    = if ($env:PERF_PAYLOAD)   { $env:PERF_PAYLOAD }   else { "128" }
$Duration   = if ($env:PERF_DURATION)  { $env:PERF_DURATION }  else { "30" }

$Timestamp  = Get-Date -Format "yyyyMMdd_HHmmss"
$OutFile    = Join-Path $ResultsDir "paho_${Timestamp}.json"
$BrokerFile = Join-Path $ResultsDir "broker_paho_${Timestamp}.json"

New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null

if (-not (Test-Path $Binary)) {
    Write-Error "Binary not found: $Binary – run setup.ps1 first (and ensure paho-mqtt is installed via vcpkg)."
    exit 1
}

Write-Host "=== Running Paho MQTT C perf test ==="
Write-Host "  host=$Host_ port=$Port msgs=$MsgCount payload=$Payload duration=${Duration}s"

try { $BrokerBefore = Invoke-RestMethod -Uri "http://localhost:18083/api/v5/prometheus/stats" -ErrorAction SilentlyContinue } catch { $BrokerBefore = "" }

& $Binary $Host_ $Port $MsgCount $Payload $Duration > $OutFile

try { $BrokerAfter = Invoke-RestMethod -Uri "http://localhost:18083/api/v5/prometheus/stats" -ErrorAction SilentlyContinue } catch { $BrokerAfter = "" }

@{ broker_stats_before = "$BrokerBefore"; broker_stats_after = "$BrokerAfter" } | ConvertTo-Json | Set-Content $BrokerFile

Write-Host "=== Paho results ==="
Get-Content $OutFile
Write-Host ""
Write-Host "Saved to: $OutFile"
Write-Host "Broker snapshot: $BrokerFile"
