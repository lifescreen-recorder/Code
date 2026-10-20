//! macOS CoreAudio System Audio Tap and Input Capture Backend.

const std = @import("std");

pub const CoreAudioCapture = struct {
    allocator: std.mem.Allocator,
    sample_rate: u32 = 48000,
    channels: u32 = 2,
    is_active: bool = false,

    pub fn initSystemAudio(allocator: std.mem.Allocator, sample_rate: u32, channels: u32) !CoreAudioCapture {
        return CoreAudioCapture{
            .allocator = allocator,
            .sample_rate = sample_rate,
            .channels = channels,
            .is_active = false,
        };
    }

    pub fn start(self: *CoreAudioCapture) !void {
        self.is_active = true;
    }

    pub fn stop(self: *CoreAudioCapture) void {
        self.is_active = false;
    }

    pub fn deinit(self: *CoreAudioCapture) void {
        self.stop();
    }
};
