# Starts Mosquitto with the playground configuration (plain + TLS listener).
#
#   powershell -ExecutionPolicy Bypass -File scripts\start-mosquitto.ps1
#   ... -Port 21883 -TlsPort 28883     if 1883 is blocked (see docs\troubleshooting.md)

param([int]$Port = 1883, [int]$TlsPort = 8883)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$confDir = Join-Path $root "servers\mosquitto"

$exe = @(
    (Join-Path $root "servers\.local\mosquitto\mosquitto.exe"),
    "C:\Program Files\mosquitto\mosquitto.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) {
    $cmd = Get-Command mosquitto -ErrorAction SilentlyContinue
    if ($cmd) { $exe = $cmd.Source }
}
if (-not $exe) { throw "Mosquitto not found - run scripts\get-servers.ps1 or: winget install EclipseFoundation.Mosquitto" }

# Same config as servers/mosquitto/mosquitto.conf, with the chosen ports.
$conf = Join-Path $env:TEMP "playground-mosquitto.conf"
(Get-Content (Join-Path $confDir "mosquitto.conf")) `
    -replace '^listener 1883$', "listener $Port" `
    -replace '^listener 8883$', "listener $TlsPort" | Set-Content $conf
Write-Host "Mosquitto: MQTT on $Port, MQTT/TLS on $TlsPort  (Ctrl+C to stop)"
Push-Location $confDir   # certs/ paths in the config are relative to here
try { & $exe -c $conf -v } finally { Pop-Location }
