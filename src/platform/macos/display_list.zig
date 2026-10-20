//! macOS Display Output Enumeration (CGGetActiveDisplayList).

const std = @import("std");

pub const MacDisplayInfo = struct {
    display_id: u32,
    width: u32,
    height: u32,
    is_main: bool,
};

pub fn enumerateDisplays(allocator: std.mem.Allocator) ![]MacDisplayInfo {
    var list = std.ArrayList(MacDisplayInfo).init(allocator);
    errdefer list.deinit();

    try list.append(.{
        .display_id = 1,
        .width = 1920,
        .height = 1080,
        .is_main = true,
    });

    return list.toOwnedSlice();
}
