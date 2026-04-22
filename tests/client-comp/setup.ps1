<# setup.ps1 – Install dependencies, build perf binaries, and start infrastructure.
   Run from the tests\perf-comp\ directory. #>

$ErrorActionPreference = "Stop"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ResultsDir = Join-Path $ScriptDir "results"
$BuildDir   = Join-Path $ScriptDir "build"

Write-Host "=== [1/5] Checking prerequisites ==="
foreach ($cmd in @("cmake", "docker")) {
    if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) {
        Write-Error "$cmd is required but not found in PATH."
        exit 1
    }
}
Write-Host "  cmake and docker found."

$HasCargo = [bool](Get-Command "cargo" -ErrorAction SilentlyContinue)
if ($HasCargo) {
    Write-Host "  cargo found."
} else {
    Write-Host "  WARNING: cargo (Rust) not found – azure_mqtt perf test will be skipped."
}

# vcpkg-based Paho install hint
if (-not $env:VCPKG_ROOT) {
    Write-Host "  TIP: set VCPKG_ROOT and run:  vcpkg install paho-mqtt:x64-windows"
}

Write-Host "=== [2/5] Building C perf binaries ==="
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$cmakeArgs = @("-S", $ScriptDir, "-B", $BuildDir, "-DCMAKE_BUILD_TYPE=Release")
if ($env:VCPKG_ROOT) {
    $toolchain = Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
}
& cmake @cmakeArgs
& cmake --build $BuildDir --config Release

Write-Host "=== [3/5] Cloning and building azure_mqtt (Rust) perf binary ==="
$RustPerfDir   = Join-Path $ScriptDir "perf_azure_mqtt"
$AzureMqttSrc  = Join-Path $RustPerfDir "azure_mqtt_src"

if ($HasCargo) {
    if (-not (Test-Path (Join-Path $AzureMqttSrc ".git"))) {
        Write-Host "Cloning https://github.com/Azure/mqtt-client..."
        & git clone --depth 1 https://github.com/Azure/mqtt-client $AzureMqttSrc
    } else {
        Write-Host "azure_mqtt source already present, pulling latest..."
        & git -C $AzureMqttSrc pull --ff-only 2>$null
    }
    & cargo build --release --manifest-path (Join-Path $RustPerfDir "Cargo.toml")
} else {
    Write-Host "  SKIP: cargo not available."
}

Write-Host "=== [4/5] Starting infrastructure (EMQX + Prometheus + Grafana + cAdvisor) ==="
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

Write-Host "=== [5/5] Setup complete ==="
Write-Host "  C Binaries  : $BuildDir\Release\perf_az_mqtt5.exe   $BuildDir\Release\perf_paho.exe"
Write-Host "  Rust Binary : $RustPerfDir\target\release\perf_azure_mqtt.exe"
Write-Host "  Results     : $ResultsDir\"
Write-Host "  EMQX        : mqtt://localhost:1883  dashboard http://localhost:18083"
Write-Host "  Prometheus  : http://localhost:9090"
Write-Host "  Grafana     : http://localhost:3000  (admin/admin)"
Write-Host ""
Write-Host "Next: run  .\run_az_mqtt5.ps1  .\run_paho.ps1  .\run_azure_mqtt.ps1"
Write-Host "Next: run  .\run_az_mqtt5.ps1  and  .\run_paho.ps1"
