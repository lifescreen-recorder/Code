const std = @import("std");
const lifescreen = @import("lifescreen");
const config = lifescreen.config;
const storage = lifescreen.storage;
const validation = lifescreen.validation;

test "config: safe default values" {
    const cfg = config.Config.default();
    try std.testing.expectEqual(@as(u32, 1), cfg.version);
    try std.testing.expectEqual(@as(u32, 60), cfg.video.fps);
    try std.testing.expectEqual(config.VideoCodec.h264, cfg.encoder.codec);
    try std.testing.expectEqual(config.EncoderBackendType.auto, cfg.encoder.preferred_backend);
    try std.testing.expectEqual(@as(u32, 20), cfg.encoder.quality_cqp);
    try std.testing.expectEqual(config.ContainerFormat.mkv, cfg.encoder.container);
}

test "config: serialization and deserialization roundtrip" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();

    var original = config.Config.default();
    original.video.fps = 120;
    original.encoder.codec = .av1;
    original.encoder.bitrate_kbps = 25000;
    original.language = "uk";

    const json = try storage.serialize(arena.allocator(), original);
    const loaded = try storage.deserialize(arena.allocator(), json);

    try std.testing.expectEqual(@as(u32, 120), loaded.video.fps);
    try std.testing.expectEqual(config.VideoCodec.av1, loaded.encoder.codec);
    try std.testing.expectEqual(@as(u32, 25000), loaded.encoder.bitrate_kbps);
    try std.testing.expectEqualStrings("uk", loaded.language);
}

test "config: validation clamps out-of-range parameters" {
    var cfg = config.Config.default();
    cfg.video.fps = 1000; // clamp to 240
    cfg.encoder.quality_cqp = 99; // clamp to 51
    cfg.audio.system_audio_volume = 10.0; // clamp to 1.0
    cfg.audio.mic_volume = -5.0; // clamp to 0.0

    validation.validateAndSanitize(&cfg);

    try std.testing.expectEqual(@as(u32, 240), cfg.video.fps);
    try std.testing.expectEqual(@as(u32, 51), cfg.encoder.quality_cqp);
    try std.testing.expectEqual(@as(f32, 1.0), cfg.audio.system_audio_volume);
    try std.testing.expectEqual(@as(f32, 0.0), cfg.audio.mic_volume);
}

test "config: resilience against corrupted JSON" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();

    const corrupted = "{\"video\": { incomplete json ...";
    const cfg = try storage.deserialize(arena.allocator(), corrupted);

    // Must not crash and must provide valid defaults
    try std.testing.expectEqual(@as(u32, 60), cfg.video.fps);
    try std.testing.expectEqual(config.CURRENT_CONFIG_VERSION, cfg.version);
}
