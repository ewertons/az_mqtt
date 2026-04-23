<# run_az_mqtt5_mbedtls.ps1 – Run the az_mqtt5 (C) perf test built with the
   mbedTLS TLS backend, via Docker. #>

$ErrorActionPreference = "Stop"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ResultsDir = Join-Path $ScriptDir "results"
$ImageName  = "perf-az-mqtt5-mbedtls:latest"
$ClientTag  = "az_mqtt5_mbedtls"

$Host_      = if ($env:PERF_HOST)      { $env:PERF_HOST }      else { "emqx" }
$Port       = if ($env:PERF_PORT)      { $env:PERF_PORT }      else { "1883" }
$MsgCount   = if ($env:PERF_MSG_COUNT) { $env:PERF_MSG_COUNT } else { "10000" }
$Payload    = if ($env:PERF_PAYLOAD)   { $env:PERF_PAYLOAD }   else { "128" }
$Duration   = if ($env:PERF_DURATION)  { $env:PERF_DURATION }  else { "30" }

$Timestamp  = Get-Date -Format "yyyyMMdd_HHmmss"
$OutFile    = Join-Path $ResultsDir "${ClientTag}_${Timestamp}.json"

New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null

$img = docker images -q $ImageName 2>$null
if (-not $img) {
    Write-Error "Docker image '$ImageName' not found – run setup.ps1 first."
    exit 1
}

Write-Host "=== Running $ClientTag (C / mbedTLS) perf test (Docker) ==="
Write-Host "  host=$Host_ port=$Port msgs=$MsgCount payload=$Payload duration=${Duration}s"

# Run and capture stdout (JSON). Rewrite the embedded "client" label so the
# downstream comparison can distinguish the two az_mqtt5 variants.
$json = docker run --rm --network client-comp_default `
    $ImageName $Host_ $Port $MsgCount $Payload $Duration
$json = $json -replace '"client":\s*"az_mqtt5"', "`"client`": `"$ClientTag`""

$json | Set-Content $OutFile

Write-Host "=== $ClientTag results ==="
Get-Content $OutFile
Write-Host ""
Write-Host "Saved to: $OutFile"
