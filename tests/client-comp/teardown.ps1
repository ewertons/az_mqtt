# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
<# teardown.ps1 – Stop and remove all perf-test Docker containers. #>
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
& docker compose -f (Join-Path $ScriptDir "docker-compose.yml") down -v
Write-Host "Infrastructure removed.  Result files in results\ are preserved."
