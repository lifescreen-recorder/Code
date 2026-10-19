//! Matroska (MKV) container muxer writing encoded video packets and audio to disk.
const std = @import("std");
const config = @import("../config/config.zig");
const ffmpeg_encoder = @import("ffmpeg_encoder.zig");

pub const MuxerVideoStreamConfig = extern struct {
    width: u32,
    height: u32,
    fps: u32,
    codec: c_int,
    extradata: ?[*]const u8 = null,
    extradata_size: usize = 0,
};

pub const MuxerAudioStreamConfig = extern struct {
    sample_rate: u32 = 48000,
    channels: u32 = 2,
    bitrate_kbps: u32 = 192,
    enable_multi_track: u32 = 0,
};

extern "c" fn ffmpeg_muxer_create(
    output_file: [*:0]const u8,
    encoder_ctx: ?*anyopaque,
    audio_cfg: ?*const MuxerAudioStreamConfig,
) callconv(.c) ?*anyopaque;

extern "c" fn ffmpeg_muxer_write_video_packet(muxer: *anyopaque, pkt: *const ffmpeg_encoder.EncodedPacket) callconv(.c) c_int;
extern "c" fn ffmpeg_muxer_write_audio_samples(muxer: *anyopaque, pcm_samples: [*]const f32, sample_count: u32, pts_nanos: i64) callconv(.c) c_int;
extern "c" fn ffmpeg_muxer_write_audio_samples_track(muxer: *anyopaque, track_index: c_int, pcm_samples: [*]const f32, sample_count: u32, pts_nanos: i64) callconv(.c) c_int;
extern "c" fn ffmpeg_muxer_close(muxer: *anyopaque) callconv(.c) c_int;

pub const Muxer = struct {
    handle: *anyopaque,

    pub fn init(
        output_file_z: [*:0]const u8,
        encoder_handle: ?*anyopaque,
        has_audio: bool,
        enable_multi_track: bool,
    ) !Muxer {
        const a_cfg = if (has_audio) MuxerAudioStreamConfig{
            .sample_rate = 48000,
            .channels = 2,
            .bitrate_kbps = 192,
            .enable_multi_track = if (enable_multi_track) 1 else 0,
        } else null;

        const handle = ffmpeg_muxer_create(
            output_file_z,
            encoder_handle,
            if (has_audio) &a_cfg.? else null,
        );

        if (handle == null) return error.MuxerCreationFailed;
        return .{ .handle = handle.? };
    }

    pub fn writeVideoPacket(self: *Muxer, pkt: *const ffmpeg_encoder.EncodedPacket) !void {
        const ret = ffmpeg_muxer_write_video_packet(self.handle, pkt);
        if (ret < 0) return error.MuxerWriteVideoFailed;
    }

    pub fn writeAudioSamples(self: *Muxer, samples: []const f32, pts_nanos: u64) !void {
        const sample_count = @as(u32, @intCast(samples.len / 2)); // 2 channels
        const ret = ffmpeg_muxer_write_audio_samples(self.handle, samples.ptr, sample_count, @intCast(pts_nanos));
        if (ret < 0) return error.MuxerWriteAudioFailed;
    }

    pub fn writeAudioSamplesTrack(self: *Muxer, track_index: u32, samples: []const f32, pts_nanos: u64) !void {
        const sample_count = @as(u32, @intCast(samples.len / 2)); // 2 channels
        const ret = ffmpeg_muxer_write_audio_samples_track(self.handle, @intCast(track_index), samples.ptr, sample_count, @intCast(pts_nanos));
        if (ret < 0) return error.MuxerWriteAudioFailed;
    }

    pub fn deinit(self: *Muxer) void {
        _ = ffmpeg_muxer_close(self.handle);
    }
};
