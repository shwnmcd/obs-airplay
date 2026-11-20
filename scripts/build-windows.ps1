param(
    [string]$BuildDir = "$PSScriptRoot\..\build",
    [string]$ObsDir = "C:\Program Files\obs-studio",
    [string]$MsysUcrtBin = "C:\msys64\ucrt64\bin"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path "$MsysUcrtBin\cmake.exe")) {
    throw "MSYS2 UCRT64 toolchain not found at $MsysUcrtBin. Install MSYS2 and run pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-openssl mingw-w64-ucrt-x86_64-libplist mingw-w64-ucrt-x86_64-tools-git"
}

$env:PATH = "$MsysUcrtBin;$env:PATH"
$rootDir = Resolve-Path "$PSScriptRoot\.."

Write-Host "Configuring obs-airplay..." -ForegroundColor Cyan
cmake -G Ninja -S $rootDir -B $BuildDir `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=gcc `
    -DCMAKE_CXX_COMPILER=g++ `
    -DOBS_STUDIO_DIR="$ObsDir"

Write-Host "Building obs-airplay.dll..." -ForegroundColor Cyan
cmake --build $BuildDir --parallel

Write-Host "Build succeeded: $BuildDir\obs-airplay.dll" -ForegroundColor Green
