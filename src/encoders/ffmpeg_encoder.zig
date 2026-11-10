//! Zig wrapper around FFmpeg video encoding pipeline (NVENC / AMF / QSV / x264).
const std = @import("std");
const types = @import("../core/types.zig");
const config = @import("../config/config.zig");

pub const EncodedPacket = extern struct {
    data: [*]const u8,
    size: usize,
    pts: i64,
    dts: i64,
    is_keyframe: c_int,
};

pub const PacketCallback = ?*const fn (user_data: ?*anyopaque, packet: *const EncodedPacket) callconv(.c) void;

pub const VideoEncoderConfig = extern struct {
    codec: c_int,
    backend: c_int,
    rate_control: c_int,
    width: u32,
    height: u32,
    fps: u32,
    quality_cqp: u32,
    bitrate_kbps: u32,
    keyframe_interval_secs: u32,
    preset: ?[*:0]const u8,
    d3d11_device: ?*anyopaque,
    d3d11_context: ?*anyopaque = null,
    src_width: u32 = 0,
    src_height: u32 = 0,
    color_range: u32 = 0,
    capture_cursor: bool = true,
    qsv_low_power: u32 = 1,
    qsv_async_depth: u32 = 1,
    wmf_scenario: u32 = 1,
    wmf_rate_control: u32 = 2,
    wmf_quality: u32 = 70,
    wmf_hw_encoding: u32 = 1,
    x264_tune: u32 = 0,
    x264_profile: u32 = 0,
    x264_threads: u32 = 0,
    openh264_slices: u32 = 1,
    mjpeg_qscale: u32 = 3,
    mjpeg_huffman: u32 = 1,
    low_end_mode: u32 = 0,
};

extern "c" fn ffmpeg_bridge_probe_encoders() callconv(.c) u32;
extern "c" fn ffmpeg_encoder_create(cfg: *const VideoEncoderConfig, callback: PacketCallback, user_data: ?*anyopaque) callconv(.c) ?*anyopaque;
extern "c" fn ffmpeg_encoder_send_d3d11_texture(ctx: *anyopaque, d3d11_texture: *anyopaque, pts_nanos: i64) callconv(.c) c_int;
extern "c" fn ffmpeg_encoder_repeat_last_frame(ctx: *anyopaque, pts_nanos: i64) callconv(.c) c_int;
extern "c" fn ffmpeg_encoder_send_cpu_frame(ctx: *anyopaque, bgra_data: [*]const u8, linesize: u32, pts_nanos: i64) callconv(.c) c_int;
extern "c" fn ffmpeg_encoder_flush(ctx: *anyopaque) callconv(.c) c_int;
extern "c" fn ffmpeg_encoder_destroy(ctx: *anyopaque) callconv(.c) void;

pub const EncoderCapabilities = struct {
    nvenc_h264: bool,
    nvenc_hevc: bool,
    nvenc_av1: bool,
    amf_h264: bool,
    amf_hevc: bool,
    amf_av1: bool,
    qsv_h264: bool,
    qsv_hevc: bool,
    qsv_av1: bool,
    sw_x264: bool,
    sw_x265: bool,
    wmf_h264: bool,
    wmf_hevc: bool,
    mjpeg: bool,
    openh264: bool,

    pub fn probe() EncoderCapabilities {
        const caps = ffmpeg_bridge_probe_encoders();
        return .{
            .nvenc_h264 = (caps & (1 << 0)) != 0,
            .nvenc_hevc = (caps & (1 << 1)) != 0,
            .nvenc_av1 = (caps & (1 << 2)) != 0,
            .amf_h264 = (caps & (1 << 3)) != 0,
            .amf_hevc = (caps & (1 << 4)) != 0,
            .amf_av1 = (caps & (1 << 5)) != 0,
            .qsv_h264 = (caps & (1 << 6)) != 0,
            .qsv_hevc = (caps & (1 << 7)) != 0,
            .qsv_av1 = (caps & (1 << 8)) != 0,
            .sw_x264 = (caps & (1 << 9)) != 0,
            .sw_x265 = (caps & (1 << 10)) != 0,
            .wmf_h264 = (caps & (1 << 11)) != 0,
            .wmf_hevc = (caps & (1 << 12)) != 0,
            .mjpeg = (caps & (1 << 13)) != 0,
            .openh264 = (caps & (1 << 14)) != 0,
        };
    }
};

pub const FfmpegEncoder = struct {
    handle: *anyopaque,

    pub fn init(
        cfg: config.Config,
        width: u32,
        height: u32,
        src_width: u32,
        src_height: u32,
        d3d11_device: ?*anyopaque,
        d3d11_context: ?*anyopaque,
        callback: PacketCallback,
        user_data: ?*anyopaque,
    ) !FfmpegEncoder {
        const codec_int: c_int = switch (cfg.encoder.codec) {
            .h264 => 0,
            .hevc => 1,
            .av1 => 2,
            .mjpeg => 3,
        };

        const backend_int: c_int = switch (cfg.encoder.preferred_backend) {
            .auto => 0,
            .nvenc => 1,
            .amf => 2,
            .qsv => 3,
            .software => 4,
            .wmf => 5,
            .openh264 => 6,
            .intel_msdk => 7,
        };

        const rc_int: c_int = switch (cfg.encoder.rate_control) {
            .cqp => 0,
            .crf => 1,
            .vbr => 2,
            .cbr => 3,
        };

        const preset_str: [*:0]const u8 = switch (cfg.encoder.preset) {
            .ultra_fast => "ultrafast",
            .performance => "superfast",
            .balanced => "medium",
            .quality => "slow",
            .p1 => "p1",
            .p2 => "p2",
            .p3 => "p3",
            .p4 => "p4",
            .p5 => "p5",
            .p6 => "p6",
            .p7 => "p7",
        };

        const color_range_int: u32 = switch (cfg.video.color_range) {
            .limited => 0,
            .full => 1,
        };

        const c_cfg = VideoEncoderConfig{
            .codec = codec_int,
            .backend = backend_int,
            .rate_control = rc_int,
            .width = width,
            .height = height,
            .fps = cfg.video.fps,
            .quality_cqp = cfg.encoder.quality_cqp,
            .bitrate_kbps = cfg.encoder.bitrate_kbps,
            .keyframe_interval_secs = cfg.encoder.keyframe_interval_secs,
            .preset = preset_str,
            .d3d11_device = d3d11_device,
            .d3d11_context = d3d11_context,
            .src_width = src_width,
            .src_height = src_height,
            .color_range = color_range_int,
            .capture_cursor = cfg.video.capture_cursor,
            .qsv_low_power = if (cfg.encoder.qsv_low_power) 1 else 0,
            .qsv_async_depth = cfg.encoder.qsv_async_depth,
            .wmf_scenario = cfg.encoder.wmf_scenario,
            .wmf_rate_control = cfg.encoder.wmf_rate_control,
            .wmf_quality = cfg.encoder.wmf_quality,
            .wmf_hw_encoding = if (cfg.encoder.wmf_hw_encoding) 1 else 0,
            .x264_tune = cfg.encoder.x264_tune,
            .x264_profile = cfg.encoder.x264_profile,
            .x264_threads = cfg.encoder.x264_threads,
            .openh264_slices = cfg.encoder.openh264_slices,
            .mjpeg_qscale = cfg.encoder.mjpeg_qscale,
            .mjpeg_huffman = cfg.encoder.mjpeg_huffman,
            .low_end_mode = if (cfg.video.low_end_mode) 1 else 0,
        };

        const handle = ffmpeg_encoder_create(&c_cfg, callback, user_data);
        if (handle == null) return error.EncoderInitializationFailed;

        return .{ .handle = handle.? };
    }

    pub fn sendD3D11Texture(self: *FfmpegEncoder, texture: *anyopaque, pts_nanos: u64) !void {
        const ret = ffmpeg_encoder_send_d3d11_texture(self.handle, texture, @intCast(pts_nanos));
        if (ret < 0) return error.EncodeFrameFailed;
    }

    pub fn repeatLastFrame(self: *FfmpegEncoder, pts_nanos: u64) !void {
        const ret = ffmpeg_encoder_repeat_last_frame(self.handle, @intCast(pts_nanos));
        if (ret < 0) return error.EncodeFrameFailed;
    }

    pub fn sendCpuFrame(self: *FfmpegEncoder, bgra_data: []const u8, linesize: u32, pts_nanos: u64) !void {
        const ret = ffmpeg_encoder_send_cpu_frame(self.handle, bgra_data.ptr, linesize, @intCast(pts_nanos));
        if (ret < 0) return error.EncodeFrameFailed;
    }

    pub fn flush(self: *FfmpegEncoder) void {
        _ = ffmpeg_encoder_flush(self.handle);
    }

    pub fn deinit(self: *FfmpegEncoder) void {
        ffmpeg_encoder_destroy(self.handle);
    }
};
