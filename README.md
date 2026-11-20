# obs-airplay — Native iPhone / iPad AirPlay Screen Cast Plugin for OBS Studio

A native C/C++ **OBS Studio 30 / 31 / 32 / 33+** plugin that turns OBS Studio directly into a zero-app **Apple AirPlay Screen Mirroring** receiver for iPhone and iPad — complete with **GPU rounded corners & back shadow** and **stream-safe 2FA security**.

No iOS app install, no USB cable, no GStreamer runtime, and no external helper processes — just install, add **iPhone / iPad Screen Cast (AirPlay)** in OBS, swipe down Control Center on your iPhone/iPad, tap **Screen Mirroring**, and choose **OBS iPhone Cast**.

## Features

- **Zero-App iOS Casting**: Works natively from iOS / iPadOS Control Center → **Screen Mirroring**.
- **Native OBS Studio Async Source**: Registers directly as an OBS Input Source (`ios_airplay_source`) with both **Video** and **Audio** in the OBS Mixer.
- **H.264 & H.265 (HEVC) + D3D11VA Hardware Decode**: Supports up to 4K / 60–120 FPS mirroring with automatic portrait/landscape rotation handling and GPU decoding (with automatic CPU fallback).
- **Synchronized iOS Audio**: Decodes AirPlay **AAC-ELD**, **AAC-LC**, **ALAC**, and **PCM** via OBS Studio's bundled FFmpeg 8.0 DLLs, with optional iPhone volume-slider synchronization.
- **GPU Rounded Corners, Border & Back Shadow**: Built-in SDF GPU shader (`airplay_frame.effect`) controllable right inside Source Properties (and available as a standalone OBS filter) without disconnecting your phone.
- **Stream-Safe 2FA Security & Anti-Rando Protection**:
  - **One-Time 4-Digit 2FA PIN**, **Ed25519 Pairing PIN + Trusted Devices**, **Static Password**, or **PC Approval Only**.
  - **In-OBS Yami Dark Popups**: Both the 2FA Code card and **Allow & Trust / Allow Once / Deny** dialog anchor inside OBS Studio and use Windows `WDA_EXCLUDEFROMCAPTURE` so viewers never see codes or device IDs even during Display Capture.
  - **Brute-Force Rate Limiter & Strict Allowlist**: Auto-blocks repeated denials for 60s and supports silent unknown-device blocking.
- **Smart Wi-Fi / LAN Interface Detection**: Automatically skips VPN and virtual adapters (Radmin VPN, Tailscale, WireGuard, Hyper-V, WSL) and advertises on your real local Wi-Fi/LAN.
- **Single Self-Contained DLL**: OpenSSL, `libplist`, `winpthread`, `libgcc`, and `libstdc++` are statically linked — zero external runtime DLLs required.

## Easy Installation (Windows x64)

### Option 1: One-Click Installer (Recommended)
Run **`dist\obs-airplay-1.2.1-windows-x64-Setup.exe`**:
- Closes OBS gracefully if running
- Installs to `C:\ProgramData\obs-studio\plugins\obs-airplay` (and Portable OBS if detected)
- Configures LocalSubnet-only Windows Firewall rules for OBS AirPlay
- Registers a clean uninstaller in **Windows Settings → Installed Apps**

### Option 2: Portable ZIP
Extract **`dist\obs-airplay-1.2.1-windows-x64-portable.zip`** and either:
- Double-click **`Install.bat`**, or
- Copy the `obs-airplay` folder into your portable OBS Studio `plugins/` directory.

## How to Use in OBS Studio

1. Ensure your PC and iPhone/iPad are on the **same Wi-Fi network**.
2. Launch **OBS Studio**.
3. In **Sources**, click **+** → **iPhone / iPad Screen Cast (AirPlay)**.
4. On your iPhone/iPad, open **Control Center** → **Screen Mirroring** → select **OBS iPhone Cast**.
5. Enter the 4-digit **2FA Code** shown inside OBS (hidden from stream capture) and click **Allow & Trust** on your PC.

## Building & Packaging from Source (Windows / MSYS2 UCRT64)

```powershell
# Build plugin + verification suite
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1

# Build full release artifacts (Setup.exe, portable.zip, SHA256SUMS.txt, RELEASE_NOTES.md)
powershell -ExecutionPolicy Bypass -File .\scripts\package-release.ps1
```

## License

GNU General Public License v3.0 (GPL-3.0). See [LICENSE](LICENSE).
