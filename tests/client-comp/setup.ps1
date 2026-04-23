# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
<# setup.ps1 – Build Docker images for all perf binaries and start infrastructure.
   Run from the tests\client-comp\ directory. #>

$ErrorActionPreference = "Stop"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ResultsDir = Join-Path $ScriptDir "results"

Write-Host "=== [1/4] Checking prerequisites ==="
foreach ($cmd in @("docker")) {
    if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) {
        Write-Error "$cmd is required but not found in PATH."
        exit 1
    }
}
Write-Host "  docker found."

Write-Host "=== [2/4] Building C perf Docker images ==="
& docker compose -f (Join-Path $ScriptDir "docker-compose.yml") build perf-az-mqtt5-openssl
Write-Host "  Docker image perf-az-mqtt5-openssl:latest built."
& docker compose -f (Join-Path $ScriptDir "docker-compose.yml") build perf-az-mqtt5-mbedtls
Write-Host "  Docker image perf-az-mqtt5-mbedtls:latest built."
& docker compose -f (Join-Path $ScriptDir "docker-compose.yml") build perf-paho
Write-Host "  Docker image perf-paho:latest built."

Write-Host "=== [3/4] Building azure_mqtt (Rust) Docker image ==="
$RustPerfDir   = Join-Path $ScriptDir "perf_azure_mqtt"
$AzureMqttSrc  = Join-Path $RustPerfDir "azure_mqtt_src"

# Ensure azure_mqtt source is cloned (needed as Docker build context)
if (-not (Test-Path (Join-Path $AzureMqttSrc ".git"))) {
    Write-Host "Cloning https://github.com/Azure/mqtt-client..."
    & git clone --depth 1 https://github.com/Azure/mqtt-client $AzureMqttSrc
} else {
    Write-Host "azure_mqtt source already present."
}

& docker compose -f (Join-Path $ScriptDir "docker-compose.yml") build perf-rust
Write-Host "  Docker image perf-azure-mqtt:latest built."

Write-Host "=== [4/4] Starting infrastructure (EMQX broker) ==="
& docker compose -f (Join-Path $ScriptDir "docker-compose.yml") up -d

Write-Host "Waiting for EMQX to become healthy..."
for ($i = 0; $i -lt 30; $i++) {
    $ping = docker exec perf-emqx emqx ping 2>$null
    if ($ping -match "pong") {
        Write-Host "EMQX is ready."
        break
    }
    Start-Sleep -Seconds 2
}

New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null

Write-Host "=== Setup complete ==="
Write-Host "  Images      : perf-az-mqtt5-openssl:latest  perf-az-mqtt5-mbedtls:latest  perf-paho:latest  perf-azure-mqtt:latest"
Write-Host "  Results     : $ResultsDir"
Write-Host "  EMQX        : mqtt://localhost:1883  dashboard http://localhost:18083"
Write-Host ""
Write-Host "Next: run  .\run.ps1"
