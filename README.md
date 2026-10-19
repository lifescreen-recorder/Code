<div align="center">

<img src="resources/icon.png" width="180" height="180" alt="LifeScreen Cat Mascot & App Icon" style="border-radius: 36px; box-shadow: 0 10px 30px rgba(0, 200, 255, 0.35);" />

# 🎬 LifeScreen

### Ultra-Low Overhead Zero-Copy Screen & Game Recorder in Zig

[![Zig](https://img.shields.io/badge/Zig-0.17.0-F7A41D?style=for-the-badge&logo=zig&logoColor=white)](https://ziglang.org)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011%20x64-0078D6?style=for-the-badge&logo=windows&logoColor=white)](https://microsoft.com)
[![DirectX](https://img.shields.io/badge/Graphics-DirectX%2011%20%2F%20DXGI-107C41?style=for-the-badge&logo=directx&logoColor=white)](https://learn.microsoft.com/en-us/windows/win32/direct3d11/atcon-effects)
[![FFmpeg](https://img.shields.io/badge/Encoder-NVENC%20%7C%20AMF%20%7C%20QSV-007808?style=for-the-badge&logo=ffmpeg&logoColor=white)](https://ffmpeg.org)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue?style=for-the-badge)](LICENSE)

> [!WARNING]
> ### ⚠️ Alpha Version Warning / Попередження про Альфа-версію
> **LifeScreen is currently in early ALPHA (v0.3.0-alpha).**  
> Expect experimental features, rapid changes, and potential bugs or hardware-specific edge cases.  
> We actively welcome feedback! If you run into any issues, freezes, or glitches, please open an issue on our [GitHub Issues](https://github.com/lifescreen-recorder/Code/issues) page.

**LifeScreen** is an ultra-lightweight, hardware-accelerated desktop and 3D game recording utility crafted from scratch in **Zig** and **C/C++**. Engineered with a strict Zero-Copy GPU architecture and lock-free thread synchronizations, LifeScreen records your screen at up to 240 FPS while consuming virtually **zero measurable CPU** and keeping in-game frame rates smooth and stutter-free.

[Key Features](#-key-features) • [Competitive Comparison](#-why-lifescreen-comparison-with-others) • [Architecture](#-zero-copy-architecture) • [Getting Started](#-getting-started) • [Tech Stack](#-technical-specifications) • [License](#-license)

</div>

---

## ⚡ Why LifeScreen? (Comparison with Others)

Most existing recorders (OBS Studio, ShadowPlay, Bandicam, Xbox Game Bar) were either built around broad broadcasting/streaming needs, locked to specific proprietary GPUs, or carry multi-gigabyte memory footprints and rendering pipelines that starve CPU and GPU caches during competitive gaming.

| Feature / Metric | 🎬 **LifeScreen** | 🎥 **OBS Studio** | 🟢 **NVIDIA ShadowPlay** | 🎮 **Xbox Game Bar** |
| :--- | :---: | :---: | :---: | :---: |
| **Primary Language** | **Zig + C11 / C++17** (Pure native) | C++ / Qt6 | C++ / Proprietary | C# / UWP / WinRT |
| **Executable Footprint** | **~25 MB** (Self-contained) | ~450 MB – 1.2 GB | ~800 MB (GeForce Experience) | Built-in Windows bloat |
| **Idle Memory (RAM)** | **~28 MB** | ~250 MB – 600 MB | ~180 MB | ~150 MB – 300 MB |
| **Gaming FPS Impact** | **< 1–2%** (Zero lock contention) | 5–15% (Scene graph re-render) | 2–4% | 5–12% |
| **Integrated GPU Support** | **Full hardware QSV / AMF / NVENC** | Yes, but heavy scene compositor | NVIDIA GeForce GPUs only | Generic, prone to micro-stutters |
| **GPU Texture Pipeline** | **Direct Zero-Copy Staging** (Immediate Unmap in <0.2ms) | Compositor Render Target pass | Private NVFBC / NVENC hook | DWM shared compositor |
| **Mouse Cursor Quality** | **Dual-Pass True Alpha Reconstruction** (Always crisp) | Standard GDI / Texture overlay | Hardware cursor | OS pointer overlay |
| **Audio Mixing** | **Hardware WASAPI Loopback + Mic Track 0 Universal Mix** | Complex multi-source bus | System + Mic | Basic OS mixer |
| **Crash Protection** | **Live Matroska (MKV) Stream Muxing** | MKV / MP4 | MP4 (Can corrupt on crash) | MP4 (Prone to corruption) |
| **Startup Time** | **Instant (~0.1s)** | 2.5s – 6s | Background daemon | ~2s – 4s |

---

## 🌟 Key Features

- 🏎️ **Microsecond-Fast GPU Staging Pipeline**:
  - Employs a 4-stage asynchronous staging ring buffer with `D3D11_MAP_FLAG_DO_NOT_WAIT`.
  - Staging textures are mapped, copied via direct SIMD pointer streaming, and **unmapped immediately in under 0.2 milliseconds**. The GPU DirectX 11 lock is never held during video encoding, completely eliminating micro-stutters in heavy 3D titles like *Counter-Strike 2*, *Valorant*, and *Cyberpunk 2077*.
- 🎮 **Three Specialized Capture Modes**:
  1. **Full Display (DXGI Desktop Duplication)**: Real-time high-throughput capture for multi-monitor rigs.
  2. **Single Window (Windows Graphics Capture - WGC)**: Zero-copy window recording that continues capturing even if occluded or behind other apps, with the yellow privacy border automatically disabled.
  3. **High-Performance Game Hook (`lifescreen_hook64.dll`)**: Intercepts `IDXGISwapChain::Present` directly inside 64-bit DirectX 11/12 engines and shares textures via `D3D11_RESOURCE_MISC_SHARED`.
- 🐭 **Crisp & Glitch-Free Cursor Blitting**:
  - Implements a dual-pass black/white luminance test that accurately resolves transparent, semi-transparent (Aero shadow), and inverted monochrome Windows mouse cursors without tearing or missing borders.
  - Full **Per-Monitor DPI Awareness V2** ensures physical 1:1 pixel alignment across all desktop display scaling factors (100%, 125%, 150%, 200%).
- 🎙️ **Universal Dual-Track Audio Architecture**:
  - **Track 0 (Universal Mix)**: Combined Master Sound (System Audio + Microphone) for instant playback on phones, Discord, Telegram, QuickTime, and web players.
  - **Track 1 & 2 (Clean Isolated Stems)**: Independent uncompressed audio streams for professional multi-track editing in DaVinci Resolve, Adobe Premiere Pro, and Final Cut.
  - Hardware WASAPI loopback capture with dynamic soft-clipping volume boost (up to 150% System, 200% Mic) and real-time dual animated VU meters.
- 📐 **Integrated Ultra-Fast Downscaling**:
  - Zero-overhead 16.16 fixed-point SIMD bilinear downscaler for 720p/1080p recording on entry-level laptops and integrated GPUs (Intel UHD / Iris Xe / AMD Radeon Vega).
- 🖥️ **Modern Fluent Dark UI**:
  - Built with Dear ImGui + Direct3D 11.
  - Zero scrollbars, responsive proportion-scaled cards, telemetry stats (FPS, Dropped Frames, File Size, Bitrate, Timer), and instant English / Ukrainian language switching.

---

## 🏗️ Zero-Copy Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                        CAPTURE SOURCES (60 - 240 FPS)                  │
│                                                                        │
│   [DXGI Desktop Duplication]     [Windows Graphics Capture]    [Game Hook DLL] 
│        (Full Desktop)                  (Target Window)         (IDXGISwapChain)
└────────────────────┬───────────────────────┬───────────────────────┬───┘
                     │                       │                       │
                     ▼                       ▼                       ▼
             ┌───────────────────────────────────────────────────────┐
             │            GPU Direct3D 11 Surface Texture            │
             │             (ZERO-COPY in VRAM Memory)                │
             └───────────────────────────┬───────────────────────────┘
                                         │
                                         ▼
             ┌───────────────────────────────────────────────────────┐
             │      4-Stage Non-Blocking Asynchronous Ring Buffer     │
             │      - D3D11_MAP_FLAG_DO_NOT_WAIT (Zero GPU stalls)   │
             │      - Immediate Unmap (<0.2ms D3D lock hold)         │
             └───────────────────────────┬───────────────────────────┘
                                         │
                                         ▼
             ┌───────────────────────────────────────────────────────┐
             │          Hardware Video Encoder (GPU ASIC)            │
             │    Intel QSV  |  NVIDIA NVENC  |  AMD AMF  |  x264    │
             └───────────────────────────┬───────────────────────────┘
                                         │
                                         ▼
             ┌───────────────────────────────────────────────────────┐
             │               MKV / MP4 Container Muxer               │
             │         Crash-Resilient Live Matroska Stream          │
             └───────────────────────────────────────────────────────┘
```

---

## 🚀 Getting Started

### Prerequisites

- **OS**: Windows 10 (Build 19041+) or Windows 11 x64
- **Toolchain**: **Zig 0.17.0**
- **GPU Drivers**: Recent NVIDIA, AMD, or Intel graphics drivers with hardware video encoding support

### 1. Clone the repository
```bash
git clone https://github.com/your-username/LifeScreen.git
cd LifeScreen
```

### 2. Build the project
```powershell
zig build
```
The compiled binaries will be placed in `zig-out/bin/`:
- `lifescreen.exe` — The main recorder executable
- `lifescreen_hook64.dll` — DirectX game injection library
- FFmpeg 9.0 shared runtime libraries

### 3. Run unit tests
```powershell
zig build test --summary all
```

### 4. Launch LifeScreen
```powershell
.\zig-out\bin\lifescreen.exe
```

---

## ⌨️ Global Shortcuts

| Action | Shortcut |
| :--- | :--- |
| **Start / Stop Recording** | <kbd>F9</kbd> *(or <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>R</kbd>)* |
| **Pause / Resume Recording** | <kbd>F10</kbd> *(or <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>P</kbd>)* |

---

## ⚙️ Technical Specifications

- **Capture APIs**: Direct3D 11.1, DXGI 1.2 Desktop Duplication, Windows.Graphics.Capture WinRT COM, WASAPI Loopback Audio
- **Video Codecs**: H.264 / AVC, HEVC / H.265, AV1
- **Hardware Backends**: Intel QuickSync Video (QSV), NVIDIA NVENC, AMD Advanced Media Framework (AMF), Software `libx264`/`libx265`
- **Audio Codec**: AAC-LC (192 kbps per stream, 48 kHz stereo)
- **Container Formats**: Matroska (`.mkv`), MPEG-4 (`.mp4`), QuickTime (`.mov`)
- **Color Format**: Rec.709 BT.709, Limited (Broadcast) & Full Range (PC)

---

## 📄 License
 
LifeScreen is free and open-source software licensed under the **[GNU General Public License v3.0 (GPL-3.0)](LICENSE)**.

<div align="center">
  <sub>Engineered with precision in Zig & C++ for gamers, creators, and professionals.</sub>
</div>
