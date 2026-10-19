//! Paths resolution and filename template formatting.
const std = @import("std");

pub const TemplateContext = struct {
    year: u16 = 2026,
    month: u8 = 10,
    day: u8 = 5,
    hour: u8 = 21,
    minute: u8 = 0,
    second: u8 = 0,
    app_name: []const u8 = "Screen",
    fps: u32 = 60,
    width: u32 = 1920,
    height: u32 = 1080,

    pub fn now(app_name: []const u8, fps: u32, width: u32, height: u32) TemplateContext {
        const epoch_secs = std.time.timestamp();
        const epoch_days = @divFloor(epoch_secs, 86400);
        const day_secs = @mod(epoch_secs, 86400);

        const hour: u8 = @intCast(@divFloor(day_secs, 3600));
        const rem = @mod(day_secs, 3600);
        const minute: u8 = @intCast(@divFloor(rem, 60));
        const second: u8 = @intCast(@mod(rem, 60));

        // Simplified epoch to Y-M-D converter (Civil day algorithm)
        const z = epoch_days + 719468;
        const era: i64 = if (z >= 0) @divFloor(z, 146097) else @divFloor(z - 146096, 146097);
        const doe: u32 = @intCast(z - era * 146097);
        const yoe: u32 = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const y: i64 = @as(i64, yoe) + era * 400;
        const doy: u32 = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const mp: u32 = (5 * doy + 2) / 153;
        const d: u8 = @intCast(doy - (153 * mp + 2) / 5 + 1);
        const m: u8 = @intCast(if (mp < 10) mp + 3 else mp - 9);
        const final_y: u16 = @intCast(if (m <= 2) y + 1 else y);

        return .{
            .year = final_y,
            .month = m,
            .day = d,
            .hour = hour,
            .minute = minute,
            .second = second,
            .app_name = if (app_name.len > 0) app_name else "Screen",
            .fps = fps,
            .width = width,
            .height = height,
        };
    }
};

/// Replaces invalid filename characters with '_'
pub fn sanitizeFilename(allocator: std.mem.Allocator, input: []const u8) ![]u8 {
    const res = try allocator.alloc(u8, input.len);
    for (input, 0..) |c, i| {
        res[i] = switch (c) {
            '\\', '/', ':', '*', '?', '"', '<', '>', '|', '\r', '\n', '\t' => '_',
            else => c,
        };
    }
    return res;
}

/// Formats a filename according to a pattern:
/// e.g. "{date}_{time}_{app}_{res}_{fps}" -> "2026-10-05_21-00-00_Screen_1920x1080_60fps"
pub fn formatFilename(allocator: std.mem.Allocator, pattern: []const u8, ctx: TemplateContext) ![]u8 {
    var out: std.ArrayListUnmanaged(u8) = .empty;
    defer out.deinit(allocator);

    var i: usize = 0;
    while (i < pattern.len) {
        if (pattern[i] == '{') {
            if (std.mem.indexOfScalar(u8, pattern[i..], '}')) |closing| {
                const tag = pattern[i + 1 .. i + closing];
                if (std.mem.eql(u8, tag, "date")) {
                    var buf: [16]u8 = undefined;
                    const s = std.fmt.bufPrint(&buf, "{d:0>4}-{d:0>2}-{d:0>2}", .{ ctx.year, ctx.month, ctx.day }) catch "";
                    try out.appendSlice(allocator, s);
                } else if (std.mem.eql(u8, tag, "time")) {
                    var buf: [16]u8 = undefined;
                    const s = std.fmt.bufPrint(&buf, "{d:0>2}-{d:0>2}-{d:0>2}", .{ ctx.hour, ctx.minute, ctx.second }) catch "";
                    try out.appendSlice(allocator, s);
                } else if (std.mem.eql(u8, tag, "app")) {
                    const clean = try sanitizeFilename(allocator, ctx.app_name);
                    defer allocator.free(clean);
                    try out.appendSlice(allocator, clean);
                } else if (std.mem.eql(u8, tag, "fps")) {
                    var buf: [16]u8 = undefined;
                    const s = std.fmt.bufPrint(&buf, "{d}fps", .{ctx.fps}) catch "";
                    try out.appendSlice(allocator, s);
                } else if (std.mem.eql(u8, tag, "res")) {
                    var buf: [32]u8 = undefined;
                    const s = std.fmt.bufPrint(&buf, "{d}x{d}", .{ ctx.width, ctx.height }) catch "";
                    try out.appendSlice(allocator, s);
                } else {
                    // Unknown tag, output as literal
                    try out.appendSlice(allocator, pattern[i .. i + closing + 1]);
                }
                i += closing + 1;
                continue;
            }
        }
        try out.append(allocator, pattern[i]);
        i += 1;
    }

    return out.toOwnedSlice(allocator);
}

/// Retrieves the base config directory (%APPDATA%/LifeScreen on Windows)
pub fn getConfigDirPath(allocator: std.mem.Allocator) ![]u8 {
    const environ: std.process.Environ = .{ .block = switch (@import("builtin").os.tag) {
        .windows => .global,
        else => .empty,
    } };
    if (environ.getAlloc(allocator, "APPDATA")) |appdata| {
        defer allocator.free(appdata);
        return std.fs.path.join(allocator, &[_][]const u8{ appdata, "LifeScreen" });
    } else |_| {
        // Fallback for non-windows or missing APPDATA
        if (environ.getAlloc(allocator, "USERPROFILE")) |profile| {
            defer allocator.free(profile);
            return std.fs.path.join(allocator, &[_][]const u8{ profile, ".lifescreen" });
        } else |_| {
            return allocator.dupe(u8, ".lifescreen");
        }
    }
}

/// Retrieves the default user Videos directory (%USERPROFILE%/Videos on Windows)
pub fn getDefaultVideosPath(allocator: std.mem.Allocator) ![]u8 {
    const environ: std.process.Environ = .{ .block = switch (@import("builtin").os.tag) {
        .windows => .global,
        else => .empty,
    } };
    if (environ.getAlloc(allocator, "USERPROFILE")) |profile| {
        defer allocator.free(profile);
        return std.fs.path.join(allocator, &[_][]const u8{ profile, "Videos" });
    } else |_| {
        return allocator.dupe(u8, ".");
    }
}

test "sanitizeFilename removes forbidden characters" {
    const testing = std.testing;
    const input = "Game: Witcher 3 / Special <Edition>? *Awesome* | \"100%\"";
    const cleaned = try sanitizeFilename(testing.allocator, input);
    defer testing.allocator.free(cleaned);

    try testing.expectEqualStrings("Game_ Witcher 3 _ Special _Edition__ _Awesome_ _ _100%_", cleaned);
}

test "formatFilename replaces standard template tags" {
    const testing = std.testing;
    const ctx = TemplateContext{
        .year = 2026,
        .month = 10,
        .day = 5,
        .hour = 21,
        .minute = 15,
        .second = 30,
        .app_name = "Cyberpunk 2077",
        .fps = 120,
        .width = 2560,
        .height = 1440,
    };

    const pattern = "{date}_{time}_{app}_{res}_{fps}";
    const formatted = try formatFilename(testing.allocator, pattern, ctx);
    defer testing.allocator.free(formatted);

    try testing.expectEqualStrings("2026-10-05_21-15-30_Cyberpunk 2077_2560x1440_120fps", formatted);
}

test "formatFilename preserves literal text and unknown tags" {
    const testing = std.testing;
    const ctx = TemplateContext{
        .year = 2026,
        .month = 1,
        .day = 2,
        .hour = 3,
        .minute = 4,
        .second = 5,
        .app_name = "Test",
        .fps = 60,
        .width = 1920,
        .height = 1080,
    };

    const pattern = "Rec_{date}_{unknown}_fixed";
    const formatted = try formatFilename(testing.allocator, pattern, ctx);
    defer testing.allocator.free(formatted);

    try testing.expectEqualStrings("Rec_2026-01-02_{unknown}_fixed", formatted);
}

