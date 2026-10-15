# Downloads Mosquitto, Leshan and a Java runtime into servers/.local/ -
# portable, no admin rights, nothing installed system-wide.
#
#   powershell -ExecutionPolicy Bypass -File scripts\get-servers.ps1
#
# Mosquitto for Windows only ships as an installer; it is unpacked with 7-Zip
# if 7-Zip is installed, otherwise run:  winget install EclipseFoundation.Mosquitto

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "servers\.local"
New-Item -ItemType Directory -Force $dest | Out-Null

$leshanVersion = "2.0.0-M18"
$mosqVersion = "2.1.2"

# ---- Leshan demo server jar (Maven Central) ----
$jar = Join-Path $dest "leshan-demo-server.jar"
if (-not (Test-Path $jar)) {
    Write-Host "Downloading Leshan $leshanVersion ..."
    Invoke-WebRequest "https://repo1.maven.org/maven2/org/eclipse/leshan/leshan-demo-server/$leshanVersion/leshan-demo-server-$leshanVersion-jar-with-dependencies.jar" -OutFile $jar
}

# ---- Java 21 runtime (Eclipse Temurin), unless java 17+ is already on PATH ----
$haveJava = $false
try { $v = (& java -version 2>&1 | Select-Object -First 1); if ($v -match '"(\d+)') { $haveJava = [int]$Matches[1] -ge 17 } } catch {}
if (-not $haveJava -and -not (Get-ChildItem $dest -Directory -Filter "jdk-*" -ErrorAction SilentlyContinue)) {
    Write-Host "Downloading Java 21 runtime (Temurin) ..."
    $zip = Join-Path $dest "jre.zip"
    Invoke-WebRequest "https://api.adoptium.net/v3/binary/latest/21/ga/windows/x64/jre/hotspot/normal/eclipse" -OutFile $zip
    Expand-Archive $zip -DestinationPath $dest -Force
    Remove-Item $zip
}

# ---- Mosquitto ----
$mosqExe = Join-Path $dest "mosquitto\mosquitto.exe"
if (-not (Test-Path $mosqExe) -and -not (Get-Command mosquitto -ErrorAction SilentlyContinue) -and
    -not (Test-Path "C:\Program Files\mosquitto\mosquitto.exe")) {
    $installer = Join-Path $dest "mosquitto-install.exe"
    Write-Host "Downloading Mosquitto $mosqVersion ..."
    Invoke-WebRequest "https://mosquitto.org/files/binary/win64/mosquitto-$mosqVersion-install-windows-x64.exe" -OutFile $installer
    $sevenZip = @("C:\Program Files\7-Zip\7z.exe", "C:\Program Files (x86)\7-Zip\7z.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($sevenZip) {
        & $sevenZip x -y "-o$(Join-Path $dest 'mosquitto')" $installer | Out-Null
        Remove-Item $installer
    } else {
        Write-Host "7-Zip not found: run the installer yourself ($installer)"
        Write-Host "or:  winget install EclipseFoundation.Mosquitto"
    }
}
Write-Host "Done. Start the servers with scripts\start-mosquitto.ps1 and scripts\start-leshan.ps1"
