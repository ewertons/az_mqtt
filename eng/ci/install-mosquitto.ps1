# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Build a pinned Mosquitto broker and clients with MSVC (x64): built-in WebSockets, TLS from
# the OpenSSL in "$env:ProgramFiles\OpenSSL" (as on GitHub's Windows runners), and cJSON.
# Every .exe and .dll they need is copied flat into <prefix>; put <prefix> on PATH.
#
#   eng/ci/install-mosquitto.ps1 <prefix>
#
# Sources come from GitHub at the tags below and must match the pinned commits.
param([Parameter(Mandatory)][string]$Prefix)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

$mosquitto = @{ Repo = 'eclipse-mosquitto/mosquitto'; Tag = 'v2.1.2'; Commit = '99fa50f30e325609394c324c8ff71cfbbe95d8ab' }
$cjson = @{ Repo = 'DaveGamble/cJSON'; Tag = 'v1.7.19'; Commit = 'c859b25da02955fef659d658b8f324b5cde87be3' }
$openssl = Join-Path $env:ProgramFiles 'OpenSSL'

function Get-Source([hashtable]$src, [string]$dir) {
  git -c advice.detachedHead=false clone --quiet --depth 1 --branch $src.Tag "https://github.com/$($src.Repo).git" $dir
  $head = git -C $dir rev-parse HEAD
  if ($head -ne $src.Commit) { throw "$($src.Repo) $($src.Tag) is $head, expected $($src.Commit)" }
}

$work = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
$deps = Join-Path $work 'deps'
$stage = Join-Path $work 'stage'
try {
  Get-Source $cjson "$work/cjson"
  cmake -S "$work/cjson" -B "$work/cjson-build" -A x64 -DCMAKE_INSTALL_PREFIX="$deps" `
    -DBUILD_SHARED_LIBS=ON -DENABLE_CJSON_TEST=OFF -DENABLE_CJSON_UTILS=OFF
  cmake --build "$work/cjson-build" --config Release
  cmake --install "$work/cjson-build" --config Release

  Get-Source $mosquitto "$work/mosquitto"
  # Broker and clients only: no threading (pthreads), plugins, apps, HTTP API, docs or tests.
  cmake -S "$work/mosquitto" -B "$work/mosquitto-build" -A x64 -DCMAKE_INSTALL_PREFIX="$stage" `
    -DCMAKE_PREFIX_PATH="$deps" -DOPENSSL_ROOT_DIR="$openssl" `
    -DWITH_WEBSOCKETS=ON -DWITH_WEBSOCKETS_BUILTIN=ON -DWITH_THREADING=OFF -DWITH_LIB_CPP=OFF `
    -DWITH_PLUGINS=OFF -DWITH_APPS=OFF -DWITH_CTRL_SHELL=OFF -DWITH_HTTP_API=OFF `
    -DWITH_DOCS=OFF -DWITH_TESTS=OFF -DWITH_LTO=OFF
  cmake --build "$work/mosquitto-build" --config Release
  cmake --install "$work/mosquitto-build" --config Release

  New-Item -ItemType Directory -Force $Prefix | Out-Null
  $files = @(Get-ChildItem $stage, $deps -Recurse -Include *.exe, *.dll) +
    @(Get-ChildItem "$openssl/bin/*" -Include libssl-*.dll, libcrypto-*.dll)
  $files | Copy-Item -Destination $Prefix -Force
  foreach ($exe in 'mosquitto.exe', 'mosquitto_pub.exe') {
    if (-not (Test-Path (Join-Path $Prefix $exe))) { throw "$exe was not built" }
  }
  Get-ChildItem $Prefix | ForEach-Object { $_.Name }
} finally {
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
