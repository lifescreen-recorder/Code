## [v0.1.1] - 2026-10-22

### Changes:
* feat(installer): implement custom dark theme, modern Segoe UI, DWM dark titlebar, and export developer certificate ([00698ad](https://github.com/lifescreen-recorder/Code/commit/00698ad95fc97c491a0d5e98315a835362ce1f32)) by @lifescreen-recorder
* feat(ci): switch release-bot PR to modern English design, remove instructions, and refine commit diffing ([90b11e7](https://github.com/lifescreen-recorder/Code/commit/90b11e7f14542aec3749c67f99cd2ff4cebaf08d)) by @lifescreen-recorder
* chore(ci): use /tmp for release-bot temporary files and add workflow permission hint ([1941e81](https://github.com/lifescreen-recorder/Code/commit/1941e81eee776ba42142e0e7ae344efc08ab6c95)) by @lifescreen-recorder
* fix(ci): fix yaml syntax indentation in release-bot workflow and use body-path ([99457b7](https://github.com/lifescreen-recorder/Code/commit/99457b7f02f846aebd7fbca07548adf6e1ef67c0)) by @lifescreen-recorder
* feat(installer): stylish branded Inno Setup with cat logo, publisher Baneronetwo (BANSAFAn), and Authenticode signing ([aaa8cfc](https://github.com/lifescreen-recorder/Code/commit/aaa8cfc56eaffc574470446c223250841b6cfccb)) by @lifescreen-recorder
* feat(ci): introduce automated Release Bot with PR changelog and merge-to-release workflow ([815b703](https://github.com/lifescreen-recorder/Code/commit/815b703f1ce8f6f1534291f92f8c7cd29c4e45e5)) by @lifescreen-recorder

# Changelog

All notable changes to **LifeScreen** will be documented in this file.
This project adheres to [Semantic Versioning](https://semver.org/).

## [v0.1.0] - 2026-10-20

### 🚀 Highlights & Features
- Initial public alpha preview release.
- High-performance Zero-Copy GPU Desktop Duplication on Windows (DirectX 11 & DirectX 12).
- Hardware AV1 Next-Gen video encoder auto-prioritization (NVIDIA NVENC, Intel QSV, AMD AMF).
- Windows 11 (24H2) HDR and Wide Color Gamut (WCG) capture support via DXGI 1.5.
- Direct I/O high-throughput NVMe disk writing buffer (256 KB aligned).
- WASAPI Loopback system audio capture with low-latency ring buffer.
- Pure GUI desktop window without background console window.
- Modern Windows Inno Setup installer and portable zip archive.
- Linux and macOS cross-platform capture engine foundation (PipeWire, X11, PulseAudio, ScreenCaptureKit).
