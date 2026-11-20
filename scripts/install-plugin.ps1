param(
    [string]$BuildDir = "$PSScriptRoot\..\build",
    [string]$InstallRoot = "C:\ProgramData\obs-studio\plugins\obs-airplay",
    [switch]$AddFirewallRules,
    [switch]$FirewallOnly
)

$ErrorActionPreference = "Stop"

if (-not $FirewallOnly) {
    $dllPath = Join-Path $BuildDir "obs-airplay.dll"
    $dataDir = Join-Path $PSScriptRoot "..\data"

    if (-not (Test-Path $dllPath)) {
        throw "Built plugin not found at $dllPath. Run scripts\build-windows.ps1 first."
    }

    $binTarget = Join-Path $InstallRoot "bin\64bit"
    $dataTarget = Join-Path $InstallRoot "data"
    $destDll = Join-Path $binTarget "obs-airplay.dll"

    New-Item -ItemType Directory -Force -Path $binTarget | Out-Null
    New-Item -ItemType Directory -Force -Path $dataTarget | Out-Null

    $needCopy = $true
    if (Test-Path $destDll) {
        $srcHash = (Get-FileHash $dllPath -Algorithm SHA256).Hash
        $dstHash = (Get-FileHash $destDll -Algorithm SHA256).Hash
        if ($srcHash -eq $dstHash) {
            $needCopy = $false
            Write-Host "obs-airplay.dll is already up-to-date in $binTarget (skipping copy)." -ForegroundColor Cyan
        }
    }

    if ($needCopy) {
        try {
            Copy-Item -Force $dllPath $destDll
        } catch {
            Write-Host "Could not overwrite obs-airplay.dll because OBS Studio is currently using it. Close OBS Studio and re-run to update the DLL." -ForegroundColor Yellow
            if (-not $AddFirewallRules) { throw }
        }
    }

    Copy-Item -Recurse -Force (Join-Path $dataDir "*") $dataTarget

    Write-Host "Installed obs-airplay plugin to: $InstallRoot" -ForegroundColor Green
    Write-Host "  DLL:    $destDll"
    Write-Host "  Locale: $(Join-Path $dataTarget 'locale\en-US.ini')"
}

if ($AddFirewallRules -or $FirewallOnly) {
    Write-Host "Configuring Windows Firewall rules for OBS AirPlay..." -ForegroundColor Cyan
    $obsExe = "C:\Program Files\obs-studio\bin\64bit\obs64.exe"
    $ruleNames = @(
        "OBS AirPlay Mirroring (TCP)",
        "OBS AirPlay Mirroring (UDP)"
    )
    foreach ($r in $ruleNames) {
        Remove-NetFirewallRule -DisplayName $r -ErrorAction SilentlyContinue | Out-Null
    }
    New-NetFirewallRule -DisplayName "OBS AirPlay Mirroring (TCP)" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 7000,7001,7100 -Program $obsExe -Profile Any -RemoteAddress LocalSubnet | Out-Null
    New-NetFirewallRule -DisplayName "OBS AirPlay Mirroring (UDP)" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 5353,6000,6001,7011 -Program $obsExe -Profile Any -RemoteAddress LocalSubnet | Out-Null
    Write-Host "Firewall rules added (LocalSubnet allowed on Private, Domain, and Public profiles)." -ForegroundColor Green
} else {
    Write-Host ""
    Write-Host "TIP: If your iPhone does not see the receiver, ensure OBS Studio is allowed through Windows Firewall on Private networks (UDP 5353, 6000-6001, 7011; TCP 7000-7001, 7100), or re-run this script as Administrator with -AddFirewallRules." -ForegroundColor Yellow
}
