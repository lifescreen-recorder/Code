//! Validation and sanitization for LifeScreen configuration.
//! Clamps invalid numbers and provides safe defaults. Never crashes on bad input.
const std = @import("std");
const config = @import("config.zig");

pub fn validateAndSanitize(cfg: *config.Config) void {
    // 1. Language validation
    const valid_lang = std.mem.eql(u8, cfg.language, "en") or
        std.mem.eql(u8, cfg.language, "uk") or
        std.mem.eql(u8, cfg.language, "zh") or
        std.mem.eql(u8, cfg.language, "de") or
        std.mem.eql(u8, cfg.language, "fr") or
        std.mem.eql(u8, cfg.language, "pl");
    if (!valid_lang) {
        cfg.language = "en";
    }

    // 2. Video validation
    cfg.video.fps = std.math.clamp(cfg.video.fps, 10, 240);
    if (cfg.video.resolution_width > 0) {
        // Encoders require even dimensions
        cfg.video.resolution_width = (cfg.video.resolution_width + 1) & ~@as(u32, 1);
        cfg.video.resolution_width = std.math.clamp(cfg.video.resolution_width, 320, 7680);
    }
    if (cfg.video.resolution_height > 0) {
        cfg.video.resolution_height = (cfg.video.resolution_height + 1) & ~@as(u32, 1);
        cfg.video.resolution_height = std.math.clamp(cfg.video.resolution_height, 240, 4320);
    }

    // 3. Encoder validation
    cfg.encoder.quality_cqp = std.math.clamp(cfg.encoder.quality_cqp, 0, 51);
    cfg.encoder.bitrate_kbps = std.math.clamp(cfg.encoder.bitrate_kbps, 500, 100_000);
    cfg.encoder.keyframe_interval_secs = std.math.clamp(cfg.encoder.keyframe_interval_secs, 1, 10);

    // 4. Audio validation
    cfg.audio.system_audio_volume = std.math.clamp(cfg.audio.system_audio_volume, 0.0, 1.0);
    cfg.audio.mic_volume = std.math.clamp(cfg.audio.mic_volume, 0.0, 1.0);
    cfg.audio.audio_bitrate_kbps = std.math.clamp(cfg.audio.audio_bitrate_kbps, 64, 512);

    // 5. Output validation
    if (cfg.output.filename_pattern.len == 0) {
        cfg.output.filename_pattern = "{date}_{time}_{app}";
    }

    // 6. Hotkeys
    if (cfg.hotkeys.record_toggle.len == 0) {
        cfg.hotkeys.record_toggle = "F9";
    }
    if (cfg.hotkeys.pause_toggle.len == 0) {
        cfg.hotkeys.pause_toggle = "F10";
    }
}

test "validation clamps out-of-range values safely" {
    var cfg = config.Config.default();
    cfg.video.fps = 999;
    cfg.encoder.quality_cqp = 100;
    cfg.audio.system_audio_volume = 5.5;
    cfg.audio.mic_volume = -1.0;
    cfg.output.filename_pattern = "";
    cfg.language = "invalid_lang";

    validateAndSanitize(&cfg);

    try std.testing.expectEqual(@as(u32, 240), cfg.video.fps);
    try std.testing.expectEqual(@as(u32, 51), cfg.encoder.quality_cqp);
    try std.testing.expectEqual(@as(f32, 1.0), cfg.audio.system_audio_volume);
    try std.testing.expectEqual(@as(f32, 0.0), cfg.audio.mic_volume);
    try std.testing.expectEqualStrings("{date}_{time}_{app}", cfg.output.filename_pattern);
    try std.testing.expectEqualStrings("en", cfg.language);

    cfg.video.fps = 5;
    cfg.language = "de";
    validateAndSanitize(&cfg);
    try std.testing.expectEqual(@as(u32, 10), cfg.video.fps);
    try std.testing.expectEqualStrings("de", cfg.language);
}

test "validation ensures even dimensions for encoder" {
    var cfg = config.Config.default();
    cfg.video.resolution_width = 1921;
    cfg.video.resolution_height = 1079;

    validateAndSanitize(&cfg);

    try std.testing.expectEqual(@as(u32, 1922), cfg.video.resolution_width);
    try std.testing.expectEqual(@as(u32, 1080), cfg.video.resolution_height);
}
