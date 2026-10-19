//! Thread-safe command queue between UI and Pipeline
const std = @import("std");
const types = @import("types.zig");

pub const CommandType = enum {
    start,
    stop,
    pause,
    @"resume",
    cancel,
};

pub const StartParams = struct {
    target: types.CaptureTarget,
    output_path: []const u8,
};

pub const Command = union(CommandType) {
    start: StartParams,
    stop: void,
    pause: void,
    @"resume": void,
    cancel: void,
};

const ring_buffer = @import("ring_buffer.zig");

/// Lock-free Single-Producer Single-Consumer command queue for UI to Core control
pub const CommandQueue = struct {
    ring: ring_buffer.SpscRingBuffer(Command, 16) = .init(),

    pub fn init() CommandQueue {
        return .{};
    }

    pub fn push(self: *CommandQueue, cmd: Command) bool {
        return self.ring.tryPush(cmd);
    }

    pub fn tryPop(self: *CommandQueue) ?Command {
        return self.ring.tryPop();
    }
};
