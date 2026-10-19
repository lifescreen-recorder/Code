//! Core types for LifeScreen pipeline: video frames, audio packets, capture targets and state.
const std = @import("std");

/// Target source for video capture
pub const CaptureType = enum {
    entire_screen,
    window,
    game,
};

/// Specific configuration of what to capture
pub const CaptureTarget = struct {
    capture_type: CaptureType = .entire_screen,
    monitor_index: u32 = 0,
    monitor_handle: ?usize = null, // HMONITOR on Windows
    window_handle: ?usize = null, // HWND on Windows
    capture_cursor: bool = true,
};

/// ЕЙ П"ДАРАС ХУЛІ ТИ ТУТ, КОМЕНТИКИ ДЛЯ ТЕБЕ Я ПИШУ, БО П"ДАР БЕЗ ШІ ТИ НІХТО УЙОБА !
pub const PixelFormat = enum {
    bgra32,
    rgba32,
    nv12,
    p010, // 10-bit HDR / HEVC
};

/// Video frame metadata and GPU handle or memory slice
pub const VideoFrame = struct {
    width: u32,
    height: u32,
    format: PixelFormat,
    pts_nanos: u64, // Monotonic timestamp in nanoseconds
    frame_index: u64,
    /// D3D11 texture pointer (cast to usize/anyopaque) or shared handle for zero-copy
    gpu_texture_ptr: ?*anyopaque = null,
    gpu_shared_handle: ?usize = null,
    /// CPU buffer fallback if GPU zero-copy is unavailable
    cpu_data: ?[]const u8 = null,
};

/// Audio sample format
pub const AudioSampleFormat = enum {
    f32_le,
    s16_le,
};

/// Audio packet metadata and interleaved samples
pub const AudioPacket = struct {
    channels: u16,
    sample_rate: u32,
    format: AudioSampleFormat,
    pts_nanos: u64,
    data: []const u8,
    is_microphone: bool = false,
};
/// Ще раз....УЙОБОК, ТУТ ТОБІ НЕ РАДІ :3
/// Recorder state
pub const RecorderState = enum(u8) {
    idle,
    starting,
    recording,
    paused,
    stopping,
    @"error",
};
