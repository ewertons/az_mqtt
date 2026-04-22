<# run.ps1 – One-shot script: setup, run all three clients sequentially, compare, teardown.
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
Write-Host ">>> Step 1/6: Setup (build Docker images + start broker)"
& "$ScriptDir\setup.ps1"
Write-Host ""

# -- 2. Run az_mqtt5 (C) --
Write-Host ">>> Step 2/6: Run az_mqtt5 (C)"
& "$ScriptDir\run_az_mqtt5.ps1"
Write-Host ""

# -- 3. Run Paho MQTT C --
Write-Host ">>> Step 3/6: Run Paho MQTT C"
& "$ScriptDir\run_paho.ps1"
Write-Host ""

# -- 4. Run azure_mqtt (Rust) --
Write-Host ">>> Step 4/6: Run azure_mqtt (Rust)"
& "$ScriptDir\run_azure_mqtt.ps1"
Write-Host ""

# -- 5. Compare --
Write-Host ">>> Step 5/6: Compare results"
& "$ScriptDir\compare.ps1"
Write-Host ""

# -- 6. Teardown --
if ($Keep) {
    Write-Host ">>> Step 6/6: Teardown SKIPPED (-Keep flag)."
    Write-Host "  Run  .\teardown.ps1  when done."
} else {
    Write-Host ">>> Step 6/6: Teardown"
    & "$ScriptDir\teardown.ps1"
}

Write-Host ""
Write-Host "Done. Results are in: $ScriptDir\results\"
