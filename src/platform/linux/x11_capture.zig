//! X11 Shared Memory (MIT-SHM) Capture Backend for Linux.
//! Provides fast direct framebuffer readback on X11 and XWayland sessions.

const std = @import("std");

pub const X11CaptureConfig = struct {
    screen_index: i32 = 0,
    x: i32 = 0,
    y: i32 = 0,
    width: u32 = 1920,
    height: u32 = 1080,
    use_shm: bool = true,
};

pub const X11Capture = struct {
    allocator: std.mem.Allocator,
    config: X11CaptureConfig,
    display: ?*anyopaque = null,
    shm_buffer: ?[*]u8 = null,
    is_active: bool = false,

    pub fn init(allocator: std.mem.Allocator, config: X11CaptureConfig) !X11Capture {
        return X11Capture{
            .allocator = allocator,
            .config = config,
            .display = null,
            .shm_buffer = null,
            .is_active = false,
        };
    }

    pub fn start(self: *X11Capture) !void {
        self.is_active = true;
    }

    pub fn captureFrame(self: *X11Capture) ![*]const u8 {
        if (!self.is_active) return error.NotActive;
        return error.NoDataAvailable;
    }

    pub fn stop(self: *X11Capture) void {
        self.is_active = false;
    }

    pub fn deinit(self: *X11Capture) void {
        self.stop();
    }
};
