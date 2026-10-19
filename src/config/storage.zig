//! Configuration serialization, deserialization, migration, and storage.
//! Guarantees safe fallbacks: never crashes on corrupted files or missing paths.
const std = @import("std");
const config = @import("config.zig");
const validation = @import("validation.zig");

const log = std.log.scoped(.config);

/// Serializes configuration struct into formatted JSON bytes
pub fn serialize(allocator: std.mem.Allocator, cfg: config.Config) ![]u8 {
    var out: std.Io.Writer.Allocating = .init(allocator);
    defer out.deinit();

    try std.json.Stringify.value(cfg, .{ .whitespace = .indent_2 }, &out.writer);
    return allocator.dupe(u8, out.written());
}

/// Deserializes JSON string into a validated Config struct with version migration
/// ООООО СЕНПАЙ, ТУТ ДОВБО"ОБИ СИДЯТЬ, ТР"ХНИ ЇХ РАЗОК !
/// Memory allocated for dynamic fields is backed by the passed allocator (recommended: ArenaAllocator).
pub fn deserialize(allocator: std.mem.Allocator, json_str: []const u8) !config.Config {
    const parsed = std.json.parseFromSlice(config.Config, allocator, json_str, .{
        .ignore_unknown_fields = true,
        .allocate = .alloc_always,
    }) catch |err| {
        log.warn("Corrupted configuration JSON ({s}), falling back to defaults", .{@errorName(err)});
        return config.Config.default();
    };
    // Note: If using an arena allocator, we do not call parsed.deinit(),
    // allowing parsed.value slices to remain valid for the lifetime of the arena!
    var result = parsed.value;

    // Check version and run migrations if needed
    if (result.version < config.CURRENT_CONFIG_VERSION) {
        result = migrate(result, result.version);
    }

    // Always sanitize and clamp to safe operational bounds
    validation.validateAndSanitize(&result);
    return result;
}

/// Upgrades old config structures to current version
pub fn migrate(old_cfg: config.Config, old_version: u32) config.Config {
    var updated = old_cfg;
    if (old_version == 0) {
        // Migration from unversioned to v1: set proper defaults
        updated.version = config.CURRENT_CONFIG_VERSION;
    }
    updated.version = config.CURRENT_CONFIG_VERSION;
    return updated;
}

/// Saves config to disk.
pub fn saveToFile(allocator: std.mem.Allocator, io: std.Io, cfg: config.Config, file_path: []const u8) !void {
    const json_bytes = try serialize(allocator, cfg);
    defer allocator.free(json_bytes);

    // Ensure parent directory exists
    if (std.fs.path.dirname(file_path)) |dir_path| {
        std.Io.Dir.cwd().createDirPath(io, dir_path) catch {};
    }

    const file = try std.Io.Dir.cwd().createFile(io, file_path, .{ .truncate = true });
    defer file.close(io);

    var writer = file.writer(io, &.{});
    try writer.writeAll(json_bytes);
}

/// Loads config from disk, returning defaults on any failure without crashing
pub fn loadFromFile(allocator: std.mem.Allocator, io: std.Io, file_path: []const u8) config.Config {
    const file_contents = std.Io.Dir.cwd().readFileAlloc(io, file_path, allocator, std.Io.Limit.limited(1024 * 1024)) catch |err| {
        if (err != error.FileNotFound) {
            log.warn("Failed to read config file {s}: {s}, using defaults", .{ file_path, @errorName(err) });
        }
        return config.Config.default();
    };
    defer allocator.free(file_contents);

    return deserialize(allocator, file_contents) catch {
        return config.Config.default();
    };
}

test "serialize and deserialize roundtrip" {
    const testing = std.testing;
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();

    var original = config.Config.default();
    original.video.fps = 144;
    original.encoder.codec = .hevc;
    original.encoder.quality_cqp = 18;
    original.language = "uk";

    const json = try serialize(arena.allocator(), original);

    const loaded = try deserialize(arena.allocator(), json);

    try testing.expectEqual(@as(u32, 144), loaded.video.fps);
    try testing.expectEqual(config.VideoCodec.hevc, loaded.encoder.codec);
    try testing.expectEqual(@as(u32, 18), loaded.encoder.quality_cqp);
    try testing.expectEqualStrings("uk", loaded.language);
}

test "deserialize handles corrupted JSON gracefully" {
    const testing = std.testing;
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();

    const bad_json = "{ bad syntax: true, missing quotes, ...";

    const cfg = try deserialize(arena.allocator(), bad_json);

    // Must return safe defaults without failing
    try testing.expectEqual(@as(u32, 60), cfg.video.fps);
    try testing.expectEqual(config.CURRENT_CONFIG_VERSION, cfg.version);
}

test "deserialize runs migration on older config version" {
    const testing = std.testing;
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();

    const v0_json = "{\"version\": 0, \"video\": {\"fps\": 30}}";

    const cfg = try deserialize(arena.allocator(), v0_json);
    try testing.expectEqual(config.CURRENT_CONFIG_VERSION, cfg.version);
    try testing.expectEqual(@as(u32, 30), cfg.video.fps);
}
