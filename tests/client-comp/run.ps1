<# run.ps1 – One-shot script: setup, run all four clients sequentially, compare, teardown.
   All clients run in Ubuntu 24.04 Docker containers for fair comparison.

   Usage:
     .\run.ps1                       # defaults
     $env:PERF_MSG_COUNT=50000; .\run.ps1  # override tunables
     .\run.ps1 -Keep                 # skip teardown (leave Docker running)
#>
param(
    [switch]$Keep
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

Write-Host "================================================================"
Write-Host "          MQTT Client Comparison - Full Pipeline"
Write-Host "          (all clients in Ubuntu 24.04 containers)"
Write-Host "================================================================"
Write-Host ""

# -- 1. Setup --
Write-Host ">>> Step 1/7: Setup (build Docker images + start broker)"
& "$ScriptDir\setup.ps1"
Write-Host ""

# -- 2. Run az_mqtt5 (C / OpenSSL) --
Write-Host ">>> Step 2/7: Run az_mqtt5 (C / OpenSSL)"
& "$ScriptDir\run_az_mqtt5_openssl.ps1"
Write-Host ""

# -- 3. Run az_mqtt5 (C / mbedTLS) --
Write-Host ">>> Step 3/7: Run az_mqtt5 (C / mbedTLS)"
& "$ScriptDir\run_az_mqtt5_mbedtls.ps1"
Write-Host ""

# -- 4. Run Paho MQTT C --
Write-Host ">>> Step 4/7: Run Paho MQTT C"
& "$ScriptDir\run_paho.ps1"
Write-Host ""

# -- 5. Run azure_mqtt (Rust) --
Write-Host ">>> Step 5/7: Run azure_mqtt (Rust)"
& "$ScriptDir\run_azure_mqtt.ps1"
Write-Host ""

# -- 6. Compare --
Write-Host ">>> Step 6/7: Compare results"
& "$ScriptDir\compare.ps1"
Write-Host ""

# -- 7. Teardown --
if ($Keep) {
    Write-Host ">>> Step 7/7: Teardown SKIPPED (-Keep flag)."
    Write-Host "  Run  .\teardown.ps1  when done."
} else {
    Write-Host ">>> Step 7/7: Teardown"
    & "$ScriptDir\teardown.ps1"
}

Write-Host ""
Write-Host "Done. Results are in: $ScriptDir\results\"
