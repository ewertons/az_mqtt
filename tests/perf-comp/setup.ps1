<# setup.ps1 – Install dependencies, build perf binaries, and start infrastructure.
   Run from the test\perf-conf\ directory. #>

$ErrorActionPreference = "Stop"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ResultsDir = Join-Path $ScriptDir "results"
$BuildDir   = Join-Path $ScriptDir "build"

Write-Host "=== [1/4] Checking prerequisites ==="
foreach ($cmd in @("cmake", "docker")) {
    if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) {
        Write-Error "$cmd is required but not found in PATH."
        exit 1
    }
}
Write-Host "  cmake and docker found."

# vcpkg-based Paho install hint
if (-not $env:VCPKG_ROOT) {
    Write-Host "  TIP: set VCPKG_ROOT and run:  vcpkg install paho-mqtt:x64-windows"
}

Write-Host "=== [2/4] Building perf binaries ==="
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$cmakeArgs = @("-S", $ScriptDir, "-B", $BuildDir, "-DCMAKE_BUILD_TYPE=Release")
if ($env:VCPKG_ROOT) {
    $toolchain = Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
}
& cmake @cmakeArgs
& cmake --build $BuildDir --config Release

Write-Host "=== [3/4] Starting infrastructure (EMQX + Prometheus + Grafana + cAdvisor) ==="
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

Write-Host "=== [4/4] Setup complete ==="
Write-Host "  Binaries : $BuildDir\Release\perf_az_mqtt5.exe   $BuildDir\Release\perf_paho.exe"
Write-Host "  Results  : $ResultsDir\"
Write-Host "  EMQX     : mqtt://localhost:1883  dashboard http://localhost:18083"
Write-Host "  Prometheus : http://localhost:9090"
Write-Host "  Grafana  : http://localhost:3000  (admin/admin)"
Write-Host ""
Write-Host "Next: run  .\run_az_mqtt5.ps1  and  .\run_paho.ps1"
