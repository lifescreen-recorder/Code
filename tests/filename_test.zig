const std = @import("std");
const lifescreen = @import("lifescreen");
const paths = lifescreen.paths;

test "filename: sanitizes illegal file path characters" {
    const testing = std.testing;
    const raw = "Game: / \\ * ? < > | \"Title\"";
    const cleaned = try paths.sanitizeFilename(testing.allocator, raw);
    defer testing.allocator.free(cleaned);

    try testing.expectEqualStrings("Game_ _ _ _ _ _ _ _ _Title_", cleaned);
}

test "filename: template expands {date}, {time}, {app}, {res}, {fps}" {
    const testing = std.testing;
    const ctx = paths.TemplateContext{
        .year = 2026,
        .month = 10,
        .day = 5,
        .hour = 21,
        .minute = 30,
        .second = 0,
        .app_name = "Dota2",
        .fps = 144,
        .width = 1920,
        .height = 1080,
    };

    const pattern = "{app}_{date}_{time}_{res}_{fps}";
    const formatted = try paths.formatFilename(testing.allocator, pattern, ctx);
    defer testing.allocator.free(formatted);

    try testing.expectEqualStrings("Dota2_2026-10-05_21-30-00_1920x1080_144fps", formatted);
}
