//! Atomic statistics readable by UI thread with zero locking.
const std = @import("std");
const types = @import("types.zig");

pub const AtomicStats = struct {
    state: std.atomic.Value(types.RecorderState) = .init(.idle),
    duration_nanos: std.atomic.Value(u64) = .init(0),
    captured_frames: std.atomic.Value(u64) = .init(0),
    encoded_frames: std.atomic.Value(u64) = .init(0),
    dropped_frames: std.atomic.Value(u64) = .init(0),
    file_bytes: std.atomic.Value(u64) = .init(0),
    bitrate_bps: std.atomic.Value(u64) = .init(0),
    cpu_usage_millipercent: std.atomic.Value(u32) = .init(0), // 100_000 = 100.0%
    gpu_usage_millipercent: std.atomic.Value(u32) = .init(0),
    // Audio levels normalized to 0..10_000 (0.00% to 100.00%)
    audio_peak_system: std.atomic.Value(u32) = .init(0),
    audio_peak_mic: std.atomic.Value(u32) = .init(0),

    pub fn reset(self: *AtomicStats) void {
        self.state.store(.idle, .monotonic);
        self.duration_nanos.store(0, .monotonic);
        self.captured_frames.store(0, .monotonic);
        self.encoded_frames.store(0, .monotonic);
        self.dropped_frames.store(0, .monotonic);
        self.file_bytes.store(0, .monotonic);
        self.bitrate_bps.store(0, .monotonic);
        self.cpu_usage_millipercent.store(0, .monotonic);
        self.gpu_usage_millipercent.store(0, .monotonic);
        self.audio_peak_system.store(0, .monotonic);
        self.audio_peak_mic.store(0, .monotonic);
    }

    /// Snapshot for UI consumption in a single read pass
    pub const Snapshot = struct {
        state: types.RecorderState,
        duration_nanos: u64,
        captured_frames: u64,
        encoded_frames: u64,
        dropped_frames: u64,
        file_bytes: u64,
        bitrate_bps: u64,
        cpu_usage_percent: f32,
        gpu_usage_percent: f32,
        audio_peak_system: f32,
        audio_peak_mic: f32,
    };

    pub fn readSnapshot(self: *const AtomicStats) Snapshot {
        return .{
            .state = self.state.load(.monotonic),
            .duration_nanos = self.duration_nanos.load(.monotonic),
            .captured_frames = self.captured_frames.load(.monotonic),
            .encoded_frames = self.encoded_frames.load(.monotonic),
            .dropped_frames = self.dropped_frames.load(.monotonic),
            .file_bytes = self.file_bytes.load(.monotonic),
            .bitrate_bps = self.bitrate_bps.load(.monotonic),
            .cpu_usage_percent = @as(f32, @floatFromInt(self.cpu_usage_millipercent.load(.monotonic))) / 1000.0,
            .gpu_usage_percent = @as(f32, @floatFromInt(self.gpu_usage_millipercent.load(.monotonic))) / 1000.0,
            .audio_peak_system = @as(f32, @floatFromInt(self.audio_peak_system.load(.monotonic))) / 10000.0,
            .audio_peak_mic = @as(f32, @floatFromInt(self.audio_peak_mic.load(.monotonic))) / 10000.0,
        };
    }
};
