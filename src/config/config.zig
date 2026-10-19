// Ось тут вашу ШІ ЇБ"В В РОТИК
const std = @import("std");

pub const CURRENT_CONFIG_VERSION: u32 = 1;

pub const VideoCodec = enum {
    h264,
    hevc,
    av1,
};

pub const EncoderBackendType = enum {
    auto,
    nvenc,
    amf,
    qsv,
    software,
};

pub const RateControlMode = enum {
    cqp,
    crf,
    vbr,
    cbr,
};

pub const PresetQuality = enum {
    ultra_fast,
    performance,
    balanced,
    quality,
    p1,
    p2,
    p3,
    p4,
    p5,
    p6,
    p7,
};

pub const ColorRange = enum {
    limited,
    full,
};

pub const ContainerFormat = enum {
    mkv,
    mp4,
    mov,
};

pub const VideoConfig = struct {
    fps: u32 = 60,
    resolution_width: u32 = 0, // 0 = match display/window
    resolution_height: u32 = 0,
    capture_cursor: bool = true,
    color_range: ColorRange = .limited,
    hdr_10bit: bool = false,
};

pub const EncoderConfig = struct {
    codec: VideoCodec = .h264,
    preferred_backend: EncoderBackendType = .auto,
    rate_control: RateControlMode = .vbr,
    quality_cqp: u32 = 20, // 0-51
    bitrate_kbps: u32 = 15000,
    preset: PresetQuality = .ultra_fast,
    keyframe_interval_secs: u32 = 2,
    container: ContainerFormat = .mkv,
};

pub const AudioConfig = struct {
    system_audio_enabled: bool = true,
    system_audio_volume: f32 = 1.0, // 0.0 - 1.0
    mic_enabled: bool = true,
    mic_volume: f32 = 1.0,
    audio_bitrate_kbps: u32 = 192,
    separate_tracks: bool = false,
    save_separate_audio_file: bool = false,
    selected_audio_output: u32 = 0, // 0 = default, 1..N = device index
    selected_audio_input: u32 = 0, // 0 = default, 1..N = device index
};

pub const OutputConfig = struct {
    output_dir: []const u8 = "",
    filename_pattern: []const u8 = "{date}_{time}_{app}",
    remux_mkv_to_mp4: bool = false,
};

pub const HotkeysConfig = struct {
    record_toggle: []const u8 = "F9",
    pause_toggle: []const u8 = "F10",
};

pub const UIConfig = struct {
    theme: []const u8 = "dark",
    start_minimized: bool = false,
    minimize_to_tray: bool = true,
};

pub const Config = struct {
    version: u32 = CURRENT_CONFIG_VERSION,
    language: []const u8 = "en",
    video: VideoConfig = .{},
    encoder: EncoderConfig = .{},
    audio: AudioConfig = .{},
    output: OutputConfig = .{},
    hotkeys: HotkeysConfig = .{},
    ui: UIConfig = .{},

    pub fn default() Config {
        return .{};
    }
};
