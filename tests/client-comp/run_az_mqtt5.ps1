<# run_az_mqtt5.ps1 – Run the az_mqtt5 (C) performance test via Docker. #>

$ErrorActionPreference = "Stop"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ResultsDir = Join-Path $ScriptDir "results"
$ImageName  = "perf-az-mqtt5:latest"

$Host_      = if ($env:PERF_HOST)      { $env:PERF_HOST }      else { "emqx" }
$Port       = if ($env:PERF_PORT)      { $env:PERF_PORT }      else { "1883" }
$MsgCount   = if ($env:PERF_MSG_COUNT) { $env:PERF_MSG_COUNT } else { "10000" }
$Payload    = if ($env:PERF_PAYLOAD)   { $env:PERF_PAYLOAD }   else { "128" }
$Duration   = if ($env:PERF_DURATION)  { $env:PERF_DURATION }  else { "30" }

$Timestamp  = Get-Date -Format "yyyyMMdd_HHmmss"
$OutFile    = Join-Path $ResultsDir "az_mqtt5_${Timestamp}.json"

New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null

$img = docker images -q $ImageName 2>$null
if (-not $img) {
    Write-Error "Docker image '$ImageName' not found – run setup.ps1 first."
    exit 1
}

Write-Host "=== Running az_mqtt5 (C) perf test (Docker) ==="
Write-Host "  host=$Host_ port=$Port msgs=$MsgCount payload=$Payload duration=${Duration}s"

$json = docker run --rm --network client-comp_default `
    $ImageName $Host_ $Port $MsgCount $Payload $Duration

$json | Set-Content $OutFile

Write-Host "=== az_mqtt5 results ==="
Get-Content $OutFile
Write-Host ""
Write-Host "Saved to: $OutFile"
