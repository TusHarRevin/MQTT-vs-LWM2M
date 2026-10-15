# Starts the Leshan demo server: web UI/REST on 8080, CoAP 5683, CoAP/DTLS 5684.
#
#   powershell -ExecutionPolicy Bypass -File scripts\start-leshan.ps1

param([int]$WebPort = 8080)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$local = Join-Path $root "servers\.local"
$jar = Join-Path $local "leshan-demo-server.jar"
if (-not (Test-Path $jar)) { throw "Leshan jar not found - run scripts\get-servers.ps1 first" }

$java = Get-ChildItem $local -Directory -Filter "jdk-*" -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.FullName "bin\java.exe" } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $java) { $java = "java" }

Write-Host "Leshan: http://127.0.0.1:$WebPort  (Ctrl+C to stop)"
& $java -Xmx256m -jar $jar -wp $WebPort
