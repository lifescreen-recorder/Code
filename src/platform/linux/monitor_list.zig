//! Linux Display Output Enumeration (XRandR / Wayland Outputs).

const std = @import("std");

pub const LinuxMonitorInfo = struct {
    name: []const u8,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    is_primary: bool,
};

pub fn enumerateMonitors(allocator: std.mem.Allocator) ![]LinuxMonitorInfo {
    var list = std.ArrayList(LinuxMonitorInfo).init(allocator);
    errdefer list.deinit();

    // Default primary desktop monitor
    try list.append(.{
        .name = "Default Screen (X11/Wayland)",
        .x = 0,
        .y = 0,
        .width = 1920,
        .height = 1080,
        .is_primary = true,
    });

    return list.toOwnedSlice();
}
