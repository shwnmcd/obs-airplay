param(
    [string]$ObsTag = "33.0.1",
    [string]$FfmpegTag = "n8.0"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$tp = Join-Path $root "third_party"
New-Item -ItemType Directory -Force -Path $tp | Out-Null

$uxplayDir = Join-Path $tp "uxplay"
if (-not (Test-Path $uxplayDir)) {
    Write-Host "Cloning UxPlay..." -ForegroundColor Cyan
    git clone --depth 1 https://github.com/FDH2/UxPlay.git $uxplayDir
    $patch = Join-Path $root "patches\uxplay-windows-obs.patch"
    if (Test-Path $patch) {
        Write-Host "Applying obs-airplay Windows LAN & NTP patch..." -ForegroundColor Cyan
        git -C $uxplayDir apply $patch
    }
}

$obsDir = Join-Path $tp "obs-studio"
if (-not (Test-Path $obsDir)) {
    Write-Host "Fetching libobs headers (OBS Studio $ObsTag)..." -ForegroundColor Cyan
    git clone --depth 1 --filter=blob:none --sparse --branch $ObsTag https://github.com/obsproject/obs-studio.git $obsDir
    git -C $obsDir sparse-checkout set libobs
}

$ffDir = Join-Path $tp "ffmpeg-8.0"
if (-not (Test-Path $ffDir)) {
    Write-Host "Fetching FFmpeg $FfmpegTag public headers..." -ForegroundColor Cyan
    git clone --depth 1 --filter=blob:none --sparse --branch $FfmpegTag https://github.com/FFmpeg/FFmpeg.git $ffDir
    git -C $ffDir sparse-checkout set --no-cone "libavcodec/*.h" "libavutil/*.h" "libswresample/*.h" "libswscale/*.h"
}

Write-Host "All third-party dependencies ready in $tp" -ForegroundColor Green
