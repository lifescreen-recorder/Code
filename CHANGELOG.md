## [v0.1.1] - 2026-10-28

### Changes:
* feat(ui,i18n): display dynamic current version, allow 10..monitor Hz FPS slider, and add ZH/DE/FR/PL translations ([7c5d96f](https://github.com/lifescreen-recorder/Code/commit/7c5d96f3298da34000d1dd71f1ad61cca9de0736)) by @BANSAFAn
* debug(startup): cache encoder probe and write startup.log diagnostics to %LOCALAPPDATA%\LifeScreen ([d4f05b6](https://github.com/lifescreen-recorder/Code/commit/d4f05b695e1d7c2741dc085329a53201da322aae)) by @BANSAFAn
* chore: remove temporary debug logs ([99798c5](https://github.com/lifescreen-recorder/Code/commit/99798c5e55147a3991a54ffb5c2d841cf978e436)) by @BANSAFAn
* fix(build): target baseline x86-64 CPU on Windows to prevent 0xC000001D illegal-instruction crash on startup ([b8750a8](https://github.com/lifescreen-recorder/Code/commit/b8750a8d1e700084fc83d2cd3095cd3cbfba5ae2)) by @BANSAFAn
* perf(eco-30fps): 1GB RAM optimizations, zero-cost CFR repeat pacing, WGC frame drain, and on-demand Live Preview tab ([4882f6b](https://github.com/lifescreen-recorder/Code/commit/4882f6bb6d6ac76c8ef219053c43cc97b9ef796a)) by @BANSAFAn
* fix(capture): release preview DXGI duplication before start and fix WGC GUIDs ([5fcfe12](https://github.com/lifescreen-recorder/Code/commit/5fcfe12ae1b6e1b2d976f32a2b0a2f82461a6042)) by @BANSAFAn
* perf: eliminate background CPU spinning and throttle preview during gameplay ([5160cbe](https://github.com/lifescreen-recorder/Code/commit/5160cbea94805bfb579ce8d887e2e8dcdc526edf)) by @lifescreen-recorder
* perf(encoder): implement zero-copy D3D11 Video Processor for 4K GPU recording on low-end CPUs ([d64afda](https://github.com/lifescreen-recorder/Code/commit/d64afda65c01ac31f36ad83c4087a227c113fdcb)) by @lifescreen-recorder
* feat(preview): add real-time GPU screen preview viewport and optimize screen capture pacing ([979b7f4](https://github.com/lifescreen-recorder/Code/commit/979b7f42b076599d58beeb0b1c73685f80622739)) by @lifescreen-recorder
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
