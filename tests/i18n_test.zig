const std = @import("std");
const lifescreen = @import("lifescreen");
const i18n = lifescreen.i18n;

test "i18n: translations completeness between en and uk" {
    // Verified at compile-time in i18n.zig, also verified in test:
    try std.testing.expect(i18n.en_entries.len > 0);
    try std.testing.expectEqual(i18n.en_entries.len, i18n.uk_entries.len);
}

test "i18n: runtime language switching" {
    var instance = i18n.I18n{ .current_lang = .en };
    try std.testing.expectEqualStrings("Record", instance.get("btn_record"));
    try std.testing.expectEqualStrings("Entire Screen", instance.get("source_screen"));

    instance.setLanguage(.uk);
    try std.testing.expectEqualStrings("Запис", instance.get("btn_record"));
    try std.testing.expectEqualStrings("Весь екран", instance.get("source_screen"));
}

test "i18n: ukrainian three-form pluralization" {
    var instance = i18n.I18n{ .current_lang = .uk };
    const testing = std.testing;

    // 1 -> one
    const s1 = try i18n.format(testing.allocator, instance.getPlural("recorded_files", 1), .{ .n = 1 });
    defer testing.allocator.free(s1);
    try testing.expectEqualStrings("Записано 1 файл", s1);

    // 3 -> few
    const s3 = try i18n.format(testing.allocator, instance.getPlural("recorded_files", 3), .{ .n = 3 });
    defer testing.allocator.free(s3);
    try testing.expectEqualStrings("Записано 3 файли", s3);

    // 5 -> many
    const s5 = try i18n.format(testing.allocator, instance.getPlural("recorded_files", 5), .{ .n = 5 });
    defer testing.allocator.free(s5);
    try testing.expectEqualStrings("Записано 5 файлів", s5);

    // 11 -> many
    const s11 = try i18n.format(testing.allocator, instance.getPlural("recorded_files", 11), .{ .n = 11 });
    defer testing.allocator.free(s11);
    try testing.expectEqualStrings("Записано 11 файлів", s11);

    // 21 -> one
    const s21 = try i18n.format(testing.allocator, instance.getPlural("recorded_files", 21), .{ .n = 21 });
    defer testing.allocator.free(s21);
    try testing.expectEqualStrings("Записано 21 файл", s21);
}

test "i18n: parameter replacement" {
    const testing = std.testing;
    const res = try i18n.format(testing.allocator, "File: {file}, FPS: {fps}, Dropped: {dropped}", .{
        .file = "game_rec.mkv",
        .fps = 60,
        .dropped = 0,
    });
    defer testing.allocator.free(res);
    try testing.expectEqualStrings("File: game_rec.mkv, FPS: 60, Dropped: 0", res);
}
