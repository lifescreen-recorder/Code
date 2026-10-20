//! Unified Linux Video Capture Backend Factory (Auto-switching between PipeWire and X11).

const std = @import("std");
const pipewire = @import("pipewire_capture.zig");
const x11 = @import("x11_capture.zig");

pub const LinuxSessionType = enum {
    wayland,
    x11,
    unknown,
};

pub fn detectSessionType() LinuxSessionType {
    if (std.posix.getenv("WAYLAND_DISPLAY") != null) {
        return .wayland;
    }
    if (std.posix.getenv("XDG_SESSION_TYPE")) |session_type| {
        if (std.mem.eql(u8, session_type, "wayland")) return .wayland;
        if (std.mem.eql(u8, session_type, "x11")) return .x11;
    }
    if (std.posix.getenv("DISPLAY") != null) {
        return .x11;
    }
    return .unknown;
}
