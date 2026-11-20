# OBS AirPlay Screen Cast

Cast your **iPhone or iPad screen & audio** directly into **OBS Studio** over Wi-Fi — no iOS app or cable required.

Works on **Windows 10/11 (64-bit)** with **OBS Studio 30, 31, 32, and 33+**.

## Download

Get the latest installer from the **[Releases page](https://github.com/shwnmcd/obs-airplay/releases/latest)**:

- **`obs-airplay-1.2.1-windows-x64-Setup.exe`** — One-click installer (recommended)
- **`obs-airplay-1.2.1-windows-x64-portable.zip`** — Portable archive

## Features

- **Zero-App iOS Casting** — Mirrors directly from iPhone/iPad Control Center with H.264/HEVC video and synced audio.
- **Rounded Corners & Back Shadow** — Built-in GPU shader for smooth rounded corners, optional border, and drop shadow right in Source Properties.
- **Stream-Safe 2FA Security** — 4-digit PIN code + Allow/Deny popup inside OBS, automatically hidden from OBS Display/Window Capture.
- **Smart Wi-Fi Detection** — Automatically skips VPN and virtual adapters.

## Quick Start

1. Run **`obs-airplay-1.2.1-windows-x64-Setup.exe`** and open **OBS Studio**.
2. Under **Sources**, click **+** → **iPhone / iPad Screen Cast (AirPlay)**.
3. On your iPhone/iPad, open **Control Center → Screen Mirroring** and select **OBS iPhone Cast**.
4. Enter the 4-digit code shown inside OBS and click **Allow & Trust**.

<details>
<summary>Building from source (Windows / MSYS2 UCRT64)</summary>

```powershell
# 1. Fetch UxPlay, libobs, and FFmpeg 8.0 sources
powershell -ExecutionPolicy Bypass -File .\scripts\fetch-deps.ps1

# 2. Build minimal static FFmpeg decoders (run in MSYS2 UCRT64 bash)
C:\msys64\usr\bin\bash.exe -l ./scripts/build-static-ffmpeg.sh

# 3. Build and package release artifacts
powershell -ExecutionPolicy Bypass -File .\scripts\package-release.ps1
```

</details>

## License

[GPL-3.0](LICENSE)
