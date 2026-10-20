//! Linux Audio Capture (PulseAudio / PipeWire Pulse Loopback).
//! Records system audio and microphone inputs on Linux desktop.

const std = @import("std");

pub const LinuxAudioSource = enum {
    default_sink_monitor, // System loopback audio
    default_source,       // Microphone
    custom_device,
};

pub const LinuxAudioCapture = struct {
    allocator: std.mem.Allocator,
    source: LinuxAudioSource,
    sample_rate: u32 = 48000,
    channels: u32 = 2,
    is_active: bool = false,

    pub fn initLoopback(allocator: std.mem.Allocator, sample_rate: u32, channels: u32) !LinuxAudioCapture {
        return LinuxAudioCapture{
            .allocator = allocator,
            .source = .default_sink_monitor,
            .sample_rate = sample_rate,
            .channels = channels,
            .is_active = false,
        };
    }

    pub fn start(self: *LinuxAudioCapture) !void {
        self.is_active = true;
    }

    pub fn stop(self: *LinuxAudioCapture) void {
        self.is_active = false;
    }

    pub fn deinit(self: *LinuxAudioCapture) void {
        self.stop();
    }
};
