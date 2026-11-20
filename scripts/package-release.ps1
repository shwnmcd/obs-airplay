param(
    [string]$Version = "1.2.0",
    [string]$MsysUcrtBin = "C:\msys64\ucrt64\bin",
    [string]$ObsDir = "C:\Program Files\obs-studio"
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$root = (Resolve-Path "$PSScriptRoot\..").Path
$buildDir = Join-Path $root "build"
$distDir = Join-Path $root "dist"
$stageDir = Join-Path $distDir "portable-stage"

$env:PATH = "$MsysUcrtBin;$env:PATH"

Write-Host "=== 1. Configuring & building obs-airplay v$Version ===" -ForegroundColor Cyan
cmake -G Ninja -S $root -B $buildDir `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=gcc `
    -DCMAKE_CXX_COMPILER=g++ `
    -DOBS_STUDIO_DIR="$ObsDir"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

$dllPath = Join-Path $buildDir "obs-airplay.dll"
$effectPath = Join-Path $root "data\airplay_frame.effect"
$localePath = Join-Path $root "data\locale\en-US.ini"
$licensePath = Join-Path $root "LICENSE"

if (-not (Test-Path $dllPath)) { throw "Missing built DLL: $dllPath" }

# Strip debug symbols from release copy to keep installer & zip lean
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
$releaseDll = Join-Path $buildDir "obs-airplay-stripped.dll"
Copy-Item -Force $dllPath $releaseDll
& "$MsysUcrtBin\strip.exe" --strip-unneeded $releaseDll
if ($LASTEXITCODE -ne 0) { throw "strip failed" }

Write-Host "=== 2. Building standalone Windows Setup GUI Installer ===" -ForegroundColor Cyan
$rcTemplate = Get-Content (Join-Path $root "installer\setup.rc.in") -Raw
$rcRendered = $rcTemplate `
    -replace "@PLUGIN_DLL_PATH@", ($releaseDll -replace "\\", "/") `
    -replace "@FRAME_EFFECT_PATH@", ($effectPath -replace "\\", "/") `
    -replace "@LOCALE_EN_US_PATH@", ($localePath -replace "\\", "/") `
    -replace "@LICENSE_PATH@", ($licensePath -replace "\\", "/")

$rcOut = Join-Path $buildDir "setup.rc"
$resObj = Join-Path $buildDir "setup.res.o"
Set-Content -Path $rcOut -Value $rcRendered -Encoding ASCII

& "$MsysUcrtBin\windres.exe" -I (Join-Path $root "installer") -O coff -i $rcOut -o $resObj
if ($LASTEXITCODE -ne 0) { throw "windres failed" }

$setupExe = Join-Path $distDir "obs-airplay-$Version-windows-x64-Setup.exe"
& "$MsysUcrtBin\g++.exe" -O2 -DNDEBUG -std=gnu++17 -mwindows -municode `
    -I (Join-Path $root "installer") `
    (Join-Path $root "installer\setup.cpp") $resObj `
    -o $setupExe `
    -static -static-libgcc -static-libstdc++ `
    -lgdi32 -lgdiplus -ldwmapi -ladvapi32 -lshell32 -lole32
if ($LASTEXITCODE -ne 0) { throw "Setup installer link failed" }

Write-Host "Built installer: $setupExe" -ForegroundColor Green

Write-Host "=== 3. Creating Portable OBS Studio Plugin Archive (.zip) ===" -ForegroundColor Cyan
if (Test-Path $stageDir) { Remove-Item -Recurse -Force $stageDir }
$pluginStage = Join-Path $stageDir "obs-airplay"
New-Item -ItemType Directory -Force -Path (Join-Path $pluginStage "bin\64bit") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $pluginStage "data\locale") | Out-Null

Copy-Item -Force $releaseDll (Join-Path $pluginStage "bin\64bit\obs-airplay.dll")
Copy-Item -Force $effectPath (Join-Path $pluginStage "data\airplay_frame.effect")
Copy-Item -Force $localePath (Join-Path $pluginStage "data\locale\en-US.ini")
Copy-Item -Force $licensePath (Join-Path $stageDir "LICENSE.txt")
Copy-Item -Force (Join-Path $root "README.md") (Join-Path $stageDir "README.md")

# Add simple double-click Install.bat & Uninstall.bat for portable zip users
$installBat = @'
@echo off
setlocal
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting Administrator privileges to install OBS AirPlay Screen Cast...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
echo Installing OBS AirPlay Screen Cast to C:\ProgramData\obs-studio\plugins\obs-airplay ...
set "TARGET=C:\ProgramData\obs-studio\plugins\obs-airplay"
mkdir "%TARGET%\bin\64bit" 2>nul
mkdir "%TARGET%\data\locale" 2>nul
copy /Y "%~dp0obs-airplay\bin\64bit\obs-airplay.dll" "%TARGET%\bin\64bit\obs-airplay.dll"
if %errorlevel% neq 0 (
    echo ERROR: Could not copy obs-airplay.dll. Make sure OBS Studio is closed and try again.
    pause
    exit /b 1
)
copy /Y "%~dp0obs-airplay\data\airplay_frame.effect" "%TARGET%\data\airplay_frame.effect" >nul
copy /Y "%~dp0obs-airplay\data\locale\en-US.ini" "%TARGET%\data\locale\en-US.ini" >nul
netsh advfirewall firewall delete rule name="OBS AirPlay Mirroring (TCP)" >nul 2>&1
netsh advfirewall firewall delete rule name="OBS AirPlay Mirroring (UDP)" >nul 2>&1
netsh advfirewall firewall add rule name="OBS AirPlay Mirroring (TCP)" dir=in action=allow program="C:\Program Files\obs-studio\bin\64bit\obs64.exe" protocol=TCP localport=7000,7001,7100 remoteip=localsubnet profile=any enable=yes >nul
netsh advfirewall firewall add rule name="OBS AirPlay Mirroring (UDP)" dir=in action=allow program="C:\Program Files\obs-studio\bin\64bit\obs64.exe" protocol=UDP localport=5353,6000,6001,7011 remoteip=localsubnet profile=any enable=yes >nul
echo.
echo Installed successfully! Open OBS Studio and add "iPhone / iPad Screen Cast (AirPlay)".
pause
'@
Set-Content -Path (Join-Path $stageDir "Install.bat") -Value $installBat -Encoding ASCII

$uninstallBat = @'
@echo off
setlocal
net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
rmdir /S /Q "C:\ProgramData\obs-studio\plugins\obs-airplay" 2>nul
netsh advfirewall firewall delete rule name="OBS AirPlay Mirroring (TCP)" >nul 2>&1
netsh advfirewall firewall delete rule name="OBS AirPlay Mirroring (UDP)" >nul 2>&1
echo OBS AirPlay Screen Cast has been uninstalled.
pause
'@
Set-Content -Path (Join-Path $stageDir "Uninstall.bat") -Value $uninstallBat -Encoding ASCII

$portableZip = Join-Path $distDir "obs-airplay-$Version-windows-x64-portable.zip"
if (Test-Path $portableZip) { Remove-Item -Force $portableZip }
Compress-Archive -Path (Join-Path $stageDir "*") -DestinationPath $portableZip -CompressionLevel Optimal
Remove-Item -Recurse -Force $stageDir

Write-Host "Built portable zip: $portableZip" -ForegroundColor Green

Write-Host "=== 4. Generating SHA256SUMS.txt & RELEASE_NOTES.md ===" -ForegroundColor Cyan
$hashSetup = (Get-FileHash $setupExe -Algorithm SHA256).Hash.ToLower()
$hashZip = (Get-FileHash $portableZip -Algorithm SHA256).Hash.ToLower()
$setupName = Split-Path $setupExe -Leaf
$zipName = Split-Path $portableZip -Leaf

$sums = "$hashSetup  $setupName`n$hashZip  $zipName`n"
Set-Content -Path (Join-Path $distDir "SHA256SUMS.txt") -Value $sums -Encoding ASCII

$notes = @"
# OBS AirPlay Screen Cast v$Version (Windows x64)

Native zero-app **iPhone & iPad AirPlay Screen Mirroring** plugin for **OBS Studio 33+**.

## Highlights
- **Zero-App iOS Screen & Audio Mirroring**: Swipe down Control Center → Screen Mirroring on your iPhone/iPad and cast straight into OBS with H.264/HEVC (optional D3D11VA hardware decode) and synchronized AAC-ELD/AAC-LC/ALAC audio.
- **Smart Network Interface Selection**: Automatically skips VPN/virtual adapters (Radmin VPN, Tailscale, Hyper-V, WSL) and picks your local Wi-Fi/LAN adapter, with a manual interface picker in Source Properties.
- **GPU Rounded Corners, Border & Back Shadow**: Built-in SDF GPU shader with live-editable corner radius, bezel border stroke, and soft drop shadow with zero clipping on portrait/landscape rotation.
- **Stream-Safe 2FA Security & Trusted Devices**:
  - One-Time 4-Digit 2FA PIN per connection, Ed25519 Pairing PIN + Remember Trusted iPhones, or Static Password
  - In-OBS **Yami Dark**-themed 2FA Code & **Allow & Trust / Allow Once / Deny** approval popups protected by ``WDA_EXCLUDEFROMCAPTURE`` (invisible to OBS Display Capture)
  - 3-strike brute-force rate limiting and Strict Allowlist mode

## Downloads
- **Recommended — One-Click Windows Installer**: ``$setupName``
  - Installs the plugin into ``C:\ProgramData\obs-studio\plugins\obs-airplay``, configures LocalSubnet-only Windows Firewall rules, and registers a clean Windows Installed Apps uninstaller.
- **Portable Archive**: ``$zipName``
  - Includes the standard OBS plugin folder structure plus double-click ``Install.bat`` / ``Uninstall.bat``.

## SHA-256 Checksums
``````text
$hashSetup  $setupName
$hashZip  $zipName
``````
"@
Set-Content -Path (Join-Path $distDir "RELEASE_NOTES.md") -Value $notes -Encoding UTF8

Write-Host ""
Write-Host "Release artifacts ready in: $distDir" -ForegroundColor Green
Get-ChildItem $distDir | Format-Table Name, Length -AutoSize
