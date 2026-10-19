//! LifeScreen - High Performance Screen, Window, and Game Recorder.
const std = @import("std");
const lifescreen = @import("lifescreen");

const log = std.log.scoped(.main);

extern "kernel32" fn GetCommandLineA() callconv(.winapi) [*:0]const u8;
extern "winmm" fn timeBeginPeriod(uPeriod: u32) callconv(.winapi) u32;
extern "winmm" fn timeEndPeriod(uPeriod: u32) callconv(.winapi) u32;
extern "user32" fn SetProcessDPIAware() callconv(.winapi) i32;
extern "user32" fn SetProcessDpiAwarenessContext(value: isize) callconv(.winapi) i32;

pub fn main() !void {
    // Enable Per-Monitor DPI Awareness V2 for 1:1 pixel coords and cursor alignment
    _ = SetProcessDpiAwarenessContext(-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    _ = SetProcessDPIAware();

    _ = timeBeginPeriod(1);
    defer _ = timeEndPeriod(1);

    const allocator = std.heap.smp_allocator;

    var threaded_io = std.Io.Threaded.init(allocator, .{});
    const io = threaded_io.io();

    log.info("==================================================", .{});
    log.info("  LifeScreen v0.3.0-alpha (Zig 0.17.0) - Phase 2 Engine  ", .{});
    log.info("==================================================", .{});

    // 1. Probe available video encoders
    const caps = lifescreen.encoder.EncoderCapabilities.probe();
    log.info("Probing Hardware Encoders:", .{});
    log.info("  * NVIDIA NVENC: H264={any}, HEVC={any}, AV1={any}", .{ caps.nvenc_h264, caps.nvenc_hevc, caps.nvenc_av1 });
    log.info("  * AMD AMF:      H264={any}, HEVC={any}, AV1={any}", .{ caps.amf_h264, caps.amf_hevc, caps.amf_av1 });
    log.info("  * Intel QSV:    H264={any}, HEVC={any}, AV1={any}", .{ caps.qsv_h264, caps.qsv_hevc, caps.qsv_av1 });
    log.info("  * Software:     x264={any}, x265={any}", .{ caps.sw_x264, caps.sw_x265 });

    // 2. Load configuration
    const config_dir = try lifescreen.paths.getConfigDirPath(allocator);
    defer allocator.free(config_dir);

    const config_file = try std.fs.path.join(allocator, &[_][]const u8{ config_dir, "config.json" });
    defer allocator.free(config_file);

    var arena = std.heap.ArenaAllocator.init(allocator);
    defer arena.deinit();

    var cfg = lifescreen.storage.loadFromFile(arena.allocator(), io, config_file);

    // Auto-select fastest hardware encoder if on auto
    if (caps.nvenc_h264) {
        cfg.encoder.preferred_backend = .nvenc;
    } else if (caps.amf_h264) {
        cfg.encoder.preferred_backend = .amf;
    } else if (caps.qsv_h264) {
        cfg.encoder.preferred_backend = .qsv;
    } else {
        cfg.encoder.preferred_backend = .software;
    }

    log.info("Active encoder configuration: backend={s}, codec={s}, rate_control={s}, CQP={d}, FPS={d}", .{
        @tagName(cfg.encoder.preferred_backend),
        @tagName(cfg.encoder.codec),
        @tagName(cfg.encoder.rate_control),
        cfg.encoder.quality_cqp,
        cfg.video.fps,
    });

    // 3. Initialize i18n
    var i18n = &lifescreen.i18n.I18n.global;
    i18n.setLanguage(lifescreen.i18n.Language.fromCode(cfg.language));
    log.info("Locale: {s} - {s}", .{ i18n.current_lang.displayName(), i18n.get("status_recording") });

    // 4. Enumerate Monitors and Test DXGI Capture
    log.info("Enumerating Display Monitors...", .{});
    const monitors = lifescreen.monitor_list.enumerateMonitors(allocator) catch &[_]lifescreen.monitor_list.MonitorInfo{};
    defer if (monitors.len > 0) allocator.free(monitors);
    for (monitors, 0..) |mon, idx| {
        log.info("  [{d}] {s} ({d}x{d}) on {s} (primary={any})", .{
            idx,
            mon.getName(),
            mon.width,
            mon.height,
            mon.getAdapterName(),
            mon.is_primary,
        });
    }

    log.info("Testing DXGI Desktop Duplication on primary monitor...", .{});
    var test_cap = lifescreen.dxgi.DxgiCapture.init(0, true) catch |err| {
        log.warn("DXGI Capture test init failed: {s} (running headless or RDP?)", .{@errorName(err)});
        return;
    };
    log.info("DXGI Desktop Duplication initialized: resolution={d}x{d}", .{ test_cap.width, test_cap.height });
    test_cap.deinit();

    // 5. Enumerate Active Windows for Window Capture
    log.info("Enumerating Active Windows for WGC capture...", .{});
    const windows = lifescreen.window_list.enumerateWindows(allocator) catch &[_]lifescreen.window_list.WindowInfo{};
    defer if (windows.len > 0) allocator.free(windows);
    var shown_wins: usize = 0;
    for (windows) |win| {
        if (shown_wins < 5) {
            log.info("  * [HWND 0x{x}] '{s}' ({s}, {d}x{d})", .{
                win.hwnd,
                win.getTitle(),
                win.getProcessName(),
                win.width,
                win.height,
            });
            shown_wins += 1;
        }
    }
    if (windows.len > 5) {
        log.info("  * ... and {d} more active windows", .{windows.len - 5});
    }

    // 6. Test WASAPI Audio Loopback
    log.info("Testing WASAPI Loopback system audio capture...", .{});
    var test_audio = lifescreen.wasapi.WasapiCapture.initLoopback(null) catch |err| {
        log.warn("WASAPI test init failed: {s}", .{@errorName(err)});
        return;
    };
    log.info("WASAPI Loopback initialized: {d} Hz, {d} channels, float={any}", .{
        test_audio.sample_rate,
        test_audio.channels,
        test_audio.is_float,
    });
    test_audio.deinit();

    // 7. Check launch mode: interactive GUI vs headless test
    const cmd_line_raw = GetCommandLineA();
    const cmd_line = std.mem.span(cmd_line_raw);

    const headless_mode = (std.mem.indexOf(u8, cmd_line, "--headless") != null);
    const ui_test_mode = (std.mem.indexOf(u8, cmd_line, "--ui-test") != null);

    if (ui_test_mode) {
        log.info("Running UI verification mode (2000 ms)...", .{});
        try lifescreen.ui.runUi(allocator, cfg, 2000);
        log.info("UI test run finished successfully!", .{});
        return;
    }

    if (headless_mode) {
        log.info("Running headless automated pipeline test...", .{});
        var global_stats: lifescreen.stats.AtomicStats = .{};
        var cmd_queue = lifescreen.command.CommandQueue.init();
        var recorder_pipeline = lifescreen.pipeline.Pipeline.init(allocator, cfg, &global_stats, &cmd_queue);

        const test_output_filename = "test_screen_recording.mkv";
        log.info("Starting live recording to '{s}' for 2 seconds...", .{test_output_filename});

        try recorder_pipeline.start(.{ .capture_type = .entire_screen }, test_output_filename);
        lifescreen.clock.sleepMs(2000);
        recorder_pipeline.stop();

        const final_stats = global_stats.readSnapshot();
        log.info("==================================================", .{});
        log.info("Recording Finished Successfully!", .{});
        log.info("  * Output file:     {s}", .{test_output_filename});
        log.info("  * Duration:        {d:.2} seconds", .{@as(f64, @floatFromInt(final_stats.duration_nanos)) / 1e9});
        log.info("  * Frames Captured: {d}", .{final_stats.captured_frames});
        log.info("  * Frames Encoded:  {d}", .{final_stats.encoded_frames});
        log.info("  * Dropped Frames:  {d}", .{final_stats.dropped_frames});
        log.info("  * File Bytes:      {d} KB", .{final_stats.file_bytes / 1024});
        log.info("==================================================", .{});
        return;
    }

    // Default: Launch interactive native UI
    log.info("Launching LifeScreen Native UI (Dear ImGui + DirectX 11)...", .{});
    try lifescreen.ui.runUi(allocator, cfg, null);
    log.info("LifeScreen exited cleanly.", .{});
}
