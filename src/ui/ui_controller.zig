//! UI Controller coordinating the Win32/DX11/ImGui window with the LifeScreen recording pipeline.
const std = @import("std");
const app_window = @import("app_window.zig");
const types = @import("../core/types.zig");
const stats = @import("../core/stats.zig");
const command = @import("../core/command.zig");
const pipeline = @import("../core/pipeline.zig");
const config = @import("../config/config.zig");
const i18n = @import("../i18n/i18n.zig");
const monitor_list = @import("../platform/windows/monitor_list.zig");
const window_list = @import("../platform/windows/window_list.zig");
const clock = @import("../core/clock.zig");
const encoder = @import("../encoders/ffmpeg_encoder.zig");
const paths = @import("../config/paths.zig");
const wasapi = @import("../platform/windows/wasapi_audio.zig");
const capture_backend = @import("../platform/windows/capture_backend.zig");

extern "shell32" fn ShellExecuteA(hwnd: ?*anyopaque, lpOperation: [*:0]const u8, lpFile: [*:0]const u8, lpParameters: ?[*:0]const u8, lpDirectory: ?[*:0]const u8, nShowCmd: c_int) callconv(.winapi) ?*anyopaque;
extern "shell32" fn SHCreateDirectoryExA(hwnd: ?*anyopaque, pszPath: [*:0]const u8, psa: ?*anyopaque) callconv(.winapi) c_int;

fn ensureDirExists(dir_path: []const u8) void {
    if (dir_path.len == 0) return;
    var buf: [512]u8 = undefined;
    if (dir_path.len >= buf.len - 1) return;
    @memcpy(buf[0..dir_path.len], dir_path);
    buf[dir_path.len] = 0;
    _ = SHCreateDirectoryExA(null, @ptrCast(&buf), null);
}

fn dupeZ(allocator: std.mem.Allocator, s: []const u8) ![:0]u8 {
    const buf = try allocator.allocSentinel(u8, s.len, 0);
    @memcpy(buf, s);
    return buf;
}

const WindowEntries = struct {
    windows: []window_list.WindowInfo,
    names: [][*:0]const u8,
    allocator: std.mem.Allocator,

    fn init(alloc: std.mem.Allocator) !WindowEntries {
        const wins = try window_list.enumerateWindows(alloc);
        errdefer alloc.free(wins);
        const nms = try alloc.alloc([*:0]const u8, wins.len);
        errdefer alloc.free(nms);
        for (wins, 0..) |*win, i| {
            const title = win.getTitle();
            const proc = win.getProcessName();
            const title_preview = if (title.len > 40) title[0..40] else title;
            const s = try std.fmt.allocPrint(alloc, "[{s}] {s}", .{ proc, title_preview });
            defer alloc.free(s);
            const str = try dupeZ(alloc, s);
            nms[i] = str.ptr;
        }
        return .{ .windows = wins, .names = nms, .allocator = alloc };
    }

    fn refresh(self: *WindowEntries) void {
        self.deinit();
        self.* = init(self.allocator) catch .{
            .windows = &[_]window_list.WindowInfo{},
            .names = &[_][*:0]const u8{},
            .allocator = self.allocator,
        };
    }

    fn deinit(self: *WindowEntries) void {
        for (self.names) |ptr| {
            self.allocator.free(std.mem.span(ptr));
        }
        if (self.names.len > 0) self.allocator.free(self.names);
        if (self.windows.len > 0) self.allocator.free(self.windows);
        self.names = &[_][*:0]const u8{};
        self.windows = &[_]window_list.WindowInfo{};
    }
};

pub fn runUi(allocator: std.mem.Allocator, cfg: config.Config, headless_test_ms: ?u32) !void {
    var window = try app_window.AppWindow.init("LifeScreen Recorder", 960, 680);
    defer window.deinit();

    var global_stats = stats.AtomicStats{};
    var cmd_queue = command.CommandQueue.init();

    var recorder_pipeline = pipeline.Pipeline.init(allocator, cfg, &global_stats, &cmd_queue);
    defer recorder_pipeline.stop();

    // Enumerate initial monitors
    const monitors = try monitor_list.enumerateMonitors(allocator);
    defer allocator.free(monitors);

    var monitor_names_buf = try allocator.alloc([*:0]const u8, monitors.len);
    defer allocator.free(monitor_names_buf);

    for (monitors, 0..) |*mon, i| {
        const s = try std.fmt.allocPrint(allocator, "[{d}] {s} ({d}x{d})", .{
            mon.id,
            mon.getName(),
            mon.width,
            mon.height,
        });
        defer allocator.free(s);
        const str = try dupeZ(allocator, s);
        monitor_names_buf[i] = str.ptr;
    }
    defer {
        for (monitor_names_buf) |ptr| {
            allocator.free(std.mem.span(ptr));
        }
    }

    // Enumerate initial windows
    var window_entries = try WindowEntries.init(allocator);
    defer window_entries.deinit();

    // Enumerate audio playback devices (speakers, headphones)
    const audio_outputs = wasapi.enumerateDevices(allocator, 0) catch &[_]wasapi.AudioDevice{};
    defer allocator.free(audio_outputs);

    var audio_output_names_buf = try allocator.alloc([*:0]const u8, audio_outputs.len + 1);
    defer allocator.free(audio_output_names_buf);

    audio_output_names_buf[0] = (try dupeZ(allocator, "Default System Device (За замовчуванням)")).ptr;
    for (audio_outputs, 0..) |*dev, i| {
        audio_output_names_buf[i + 1] = (try dupeZ(allocator, dev.getName())).ptr;
    }
    defer {
        for (audio_output_names_buf) |ptr| {
            allocator.free(std.mem.span(ptr));
        }
    }

    // Enumerate audio recording devices (microphones)
    const audio_inputs = wasapi.enumerateDevices(allocator, 1) catch &[_]wasapi.AudioDevice{};
    defer allocator.free(audio_inputs);

    var audio_input_names_buf = try allocator.alloc([*:0]const u8, audio_inputs.len + 1);
    defer allocator.free(audio_input_names_buf);

    audio_input_names_buf[0] = (try dupeZ(allocator, "Default Microphone (За замовчуванням)")).ptr;
    for (audio_inputs, 0..) |*dev, i| {
        audio_input_names_buf[i + 1] = (try dupeZ(allocator, dev.getName())).ptr;
    }
    defer {
        for (audio_input_names_buf) |ptr| {
            allocator.free(std.mem.span(ptr));
        }
    }

    var ui_state = app_window.UiState{};
    ui_state.monitor_names = monitor_names_buf.ptr;
    ui_state.monitor_count = @intCast(monitors.len);
    ui_state.window_names = window_entries.names.ptr;
    ui_state.window_count = @intCast(window_entries.names.len);

    ui_state.audio_output_names = audio_output_names_buf.ptr;
    ui_state.audio_output_count = @intCast(audio_output_names_buf.len);
    ui_state.selected_audio_output = @intCast(cfg.audio.selected_audio_output);

    ui_state.audio_input_names = audio_input_names_buf.ptr;
    ui_state.audio_input_count = @intCast(audio_input_names_buf.len);
    ui_state.selected_audio_input = @intCast(cfg.audio.selected_audio_input);

    ui_state.capture_cursor = cfg.video.capture_cursor;
    ui_state.language = if (std.mem.eql(u8, cfg.language, "uk"))
        1
    else if (std.mem.eql(u8, cfg.language, "zh"))
        2
    else if (std.mem.eql(u8, cfg.language, "de"))
        3
    else if (std.mem.eql(u8, cfg.language, "fr"))
        4
    else if (std.mem.eql(u8, cfg.language, "pl"))
        5
    else
        0;
    ui_state.quality_cqp = @intCast(cfg.encoder.quality_cqp);
    ui_state.target_fps = @intCast(cfg.video.fps);
    ui_state.container_format = switch (cfg.encoder.container) {
        .mkv => 0,
        .mp4 => 1,
        .mov => 2,
    };
    ui_state.rate_control = switch (cfg.encoder.rate_control) {
        .vbr => 0,
        .cbr => 1,
        .cqp => 2,
        .crf => 2,
    };

    // Probed hardware encoder capabilities
    const caps = encoder.EncoderCapabilities.probe();
    var encoder_mask: u32 = 0;
    if (caps.nvenc_h264) encoder_mask |= (1 << 0);
    if (caps.nvenc_hevc) encoder_mask |= (1 << 1);
    if (caps.nvenc_av1)  encoder_mask |= (1 << 2);
    if (caps.amf_h264)   encoder_mask |= (1 << 3);
    if (caps.amf_hevc)   encoder_mask |= (1 << 4);
    if (caps.amf_av1)    encoder_mask |= (1 << 5);
    if (caps.qsv_h264)   encoder_mask |= (1 << 6);
    if (caps.qsv_hevc)   encoder_mask |= (1 << 7);
    if (caps.qsv_av1)    encoder_mask |= (1 << 8);
    if (caps.sw_x264)    encoder_mask |= (1 << 9);
    if (caps.sw_x265)    encoder_mask |= (1 << 10);
    ui_state.encoder_caps_mask = encoder_mask;

    // Default encoder based on detected capabilities
    if (caps.nvenc_h264) {
        ui_state.encoder_backend = 0; // NVENC
    } else if (caps.amf_h264) {
        ui_state.encoder_backend = 1; // AMF
    } else if (caps.qsv_h264) {
        ui_state.encoder_backend = 2; // QSV
    } else {
        ui_state.encoder_backend = 3; // Software
    }

    recorder_pipeline.cfg.encoder.preferred_backend = switch (ui_state.encoder_backend) {
        0 => .nvenc,
        1 => .amf,
        2 => .qsv,
        else => .software,
    };

    ui_state.show_overlay_indicator = true;
    ui_state.hotkey_start_stop_preset = 0; // F9
    ui_state.record_system_audio = cfg.audio.system_audio_enabled;
    ui_state.record_microphone = cfg.audio.mic_enabled;
    ui_state.system_volume = cfg.audio.system_audio_volume;
    ui_state.mic_volume = cfg.audio.mic_volume;
    ui_state.separate_audio_tracks = cfg.audio.separate_tracks;
    ui_state.save_separate_audio_file = cfg.audio.save_separate_audio_file;

    ui_state.output_resolution = if (cfg.video.resolution_width == 1920 and cfg.video.resolution_height == 1080)
        1
    else if (cfg.video.resolution_width == 1280 and cfg.video.resolution_height == 720)
        2
    else if (cfg.video.resolution_width == 854 and cfg.video.resolution_height == 480)
        3
    else
        0;

    ui_state.color_range = switch (cfg.video.color_range) {
        .limited => 0,
        .full => 1,
    };

    ui_state.rate_control = switch (cfg.encoder.rate_control) {
        .vbr => 0,
        .cbr => 1,
        .cqp => 2,
        .crf => 2,
    };
    ui_state.quality_cqp = @intCast(cfg.encoder.quality_cqp);

    ui_state.target_bitrate_kbps = cfg.encoder.bitrate_kbps;
    ui_state.encoder_preset = switch (cfg.encoder.preset) {
        .ultra_fast => 0,
        .performance => 1,
        .balanced => 2,
        .quality => 3,
        else => 2,
    };
    ui_state.keyframe_interval_secs = @intCast(cfg.encoder.keyframe_interval_secs);

    // Copy output directory path into buffer or default to user Videos directory
    var default_dir: []const u8 = cfg.output.output_dir;
    var allocated_dir: ?[]u8 = null;
    defer if (allocated_dir) |d| allocator.free(d);

    if (default_dir.len == 0) {
        if (paths.getDefaultVideosPath(allocator)) |vpath| {
            allocated_dir = vpath;
            default_dir = vpath;
        } else |_| {
            default_dir = ".";
        }
    }

    const out_dir_len = @min(default_dir.len, ui_state.output_dir.len - 1);
    @memcpy(ui_state.output_dir[0..out_dir_len], default_dir[0..out_dir_len]);
    ui_state.output_dir[out_dir_len] = 0;

    var preview_backend: ?capture_backend.CaptureBackend = null;
    var preview_target = types.CaptureTarget{
        .capture_type = .entire_screen,
        .monitor_index = 0,
        .capture_cursor = true,
    };
    var last_preview_ms: u64 = 0;
    defer if (preview_backend) |*pb| pb.deinit();

    const start_time = clock.MonotonicClock.nowNanos() / 1_000_000;

    while (window.pollEvents()) {
        const now_ms = clock.MonotonicClock.nowNanos() / 1_000_000;

        // Check headless timeout if running in automated test mode
        if (headless_test_ms) |timeout| {
            if (now_ms - start_time >= timeout) break;
        }

        // Global Hotkeys Check
        if (window.pollStartHotkey()) {
            if (recorder_pipeline.is_running.load(.monotonic)) {
                recorder_pipeline.stop();
            } else {
                ui_state.action_start_requested = true;
            }
        }
        if (window.pollPauseHotkey()) {
            ui_state.action_pause_toggle_requested = true;
        }

        // Idle Screen Preview Update (30 FPS when not recording)
        if (!recorder_pipeline.is_running.load(.monotonic) and app_window.imgui_is_preview_enabled()) {
            if (now_ms - last_preview_ms >= 33) {
                last_preview_ms = now_ms;

                var current_target = types.CaptureTarget{
                    .capture_type = switch (ui_state.capture_type) {
                        1 => .window,
                        2 => .game,
                        else => .entire_screen,
                    },
                    .capture_cursor = ui_state.capture_cursor,
                };

                if (current_target.capture_type == .entire_screen) {
                    if (monitors.len > 0 and ui_state.selected_monitor < monitors.len) {
                        current_target.monitor_index = @intCast(ui_state.selected_monitor);
                        if (monitors[@intCast(ui_state.selected_monitor)].hmonitor) |hmon| {
                            current_target.monitor_handle = @intFromPtr(hmon);
                        }
                    }
                } else {
                    if (window_entries.windows.len > 0 and ui_state.selected_window < window_entries.windows.len) {
                        current_target.window_handle = window_entries.windows[@intCast(ui_state.selected_window)].hwnd;
                    }
                }

                const target_changed = (current_target.capture_type != preview_target.capture_type) or
                    (current_target.monitor_index != preview_target.monitor_index) or
                    (current_target.monitor_handle != preview_target.monitor_handle) or
                    (current_target.window_handle != preview_target.window_handle) or
                    (current_target.capture_cursor != preview_target.capture_cursor);

                if (target_changed and preview_backend != null) {
                    preview_backend.?.deinit();
                    preview_backend = null;
                }

                preview_target = current_target;

                if (preview_backend == null) {
                    preview_backend = capture_backend.CaptureBackend.init(preview_target) catch null;
                    if (preview_backend) |pb| {
                        ui_state.preview_width = @intCast(pb.width);
                        ui_state.preview_height = @intCast(pb.height);
                    }
                }

                if (preview_backend) |*pb| {
                    const now_ns = clock.MonotonicClock.nowNanos();
                    if (pb.acquireFrame(2, now_ns)) |maybe_frame| {
                        if (maybe_frame) |frame| {
                            app_window.imgui_update_preview_d3d11(frame.gpu_texture_ptr, @intCast(frame.width), @intCast(frame.height));
                        }
                    } else |_| {}
                }
            }
        } else if (preview_backend != null) {
            preview_backend.?.deinit();
            preview_backend = null;
        }

        // Handle Settings changes
        if (ui_state.action_settings_changed) {
            ui_state.action_settings_changed = false;
            recorder_pipeline.cfg.encoder.preferred_backend = switch (ui_state.encoder_backend) {
                0 => .nvenc,
                1 => .amf,
                2 => .qsv,
                else => .software,
            };
            recorder_pipeline.cfg.encoder.codec = switch (ui_state.video_codec) {
                0 => .h264,
                1 => .hevc,
                2 => .av1,
                else => .h264,
            };
            recorder_pipeline.cfg.encoder.rate_control = switch (ui_state.rate_control) {
                0 => .vbr,
                1 => .cbr,
                2 => .cqp,
                else => .vbr,
            };
            recorder_pipeline.cfg.encoder.quality_cqp = @intCast(ui_state.quality_cqp);
            recorder_pipeline.cfg.encoder.bitrate_kbps = ui_state.target_bitrate_kbps;
            recorder_pipeline.cfg.encoder.preset = switch (ui_state.encoder_preset) {
                0 => .ultra_fast,
                1 => .performance,
                2 => .balanced,
                3 => .quality,
                else => .balanced,
            };
            recorder_pipeline.cfg.encoder.keyframe_interval_secs = @intCast(ui_state.keyframe_interval_secs);
            recorder_pipeline.cfg.video.fps = @intCast(ui_state.target_fps);

            switch (ui_state.output_resolution) {
                1 => {
                    recorder_pipeline.cfg.video.resolution_width = 1920;
                    recorder_pipeline.cfg.video.resolution_height = 1080;
                },
                2 => {
                    recorder_pipeline.cfg.video.resolution_width = 1280;
                    recorder_pipeline.cfg.video.resolution_height = 720;
                },
                3 => {
                    recorder_pipeline.cfg.video.resolution_width = 854;
                    recorder_pipeline.cfg.video.resolution_height = 480;
                },
                else => {
                    recorder_pipeline.cfg.video.resolution_width = 0;
                    recorder_pipeline.cfg.video.resolution_height = 0;
                },
            }
            recorder_pipeline.cfg.video.color_range = switch (ui_state.color_range) {
                1 => .full,
                else => .limited,
            };
            recorder_pipeline.cfg.video.capture_cursor = ui_state.capture_cursor;

            recorder_pipeline.cfg.audio.system_audio_enabled = ui_state.record_system_audio;
            recorder_pipeline.cfg.audio.system_audio_volume = ui_state.system_volume;
            recorder_pipeline.cfg.audio.mic_enabled = ui_state.record_microphone;
            recorder_pipeline.cfg.audio.mic_volume = ui_state.mic_volume;
            recorder_pipeline.cfg.audio.separate_tracks = ui_state.separate_audio_tracks;
            recorder_pipeline.cfg.audio.save_separate_audio_file = ui_state.save_separate_audio_file;
            recorder_pipeline.cfg.audio.selected_audio_output = @intCast(ui_state.selected_audio_output);
            recorder_pipeline.cfg.audio.selected_audio_input = @intCast(ui_state.selected_audio_input);
            recorder_pipeline.cfg.encoder.container = switch (ui_state.container_format) {
                1 => .mp4,
                2 => .mov,
                else => .mkv,
            };
        }

        // Handle UI actions
        if (ui_state.action_refresh_windows_requested) {
            ui_state.action_refresh_windows_requested = false;
            window_entries.refresh();
            ui_state.window_names = window_entries.names.ptr;
            ui_state.window_count = @intCast(window_entries.names.len);
            if (ui_state.selected_window >= @as(c_int, @intCast(window_entries.names.len))) {
                ui_state.selected_window = 0;
            }
        }

        if (ui_state.action_start_requested) {
            ui_state.action_start_requested = false;
            if (!recorder_pipeline.is_running.load(.monotonic)) {
                // Free idle screen preview backend so DXGI Desktop Duplication and GPU resources
                // are completely available for the recording pipeline without 0x80070005 (Access Denied) collision
                if (preview_backend) |*pb| {
                    pb.deinit();
                    preview_backend = null;
                }
                // Ensure latest UI options are synchronized to pipeline config
                recorder_pipeline.cfg.video.capture_cursor = ui_state.capture_cursor;
                recorder_pipeline.cfg.video.fps = @intCast(ui_state.target_fps);
                switch (ui_state.output_resolution) {
                    1 => {
                        recorder_pipeline.cfg.video.resolution_width = 1920;
                        recorder_pipeline.cfg.video.resolution_height = 1080;
                    },
                    2 => {
                        recorder_pipeline.cfg.video.resolution_width = 1280;
                        recorder_pipeline.cfg.video.resolution_height = 720;
                    },
                    3 => {
                        recorder_pipeline.cfg.video.resolution_width = 854;
                        recorder_pipeline.cfg.video.resolution_height = 480;
                    },
                    else => {
                        recorder_pipeline.cfg.video.resolution_width = 0;
                        recorder_pipeline.cfg.video.resolution_height = 0;
                    },
                }
                recorder_pipeline.cfg.video.color_range = switch (ui_state.color_range) {
                    1 => .full,
                    else => .limited,
                };
                recorder_pipeline.cfg.audio.system_audio_enabled = ui_state.record_system_audio;
                recorder_pipeline.cfg.audio.system_audio_volume = ui_state.system_volume;
                recorder_pipeline.cfg.audio.mic_enabled = ui_state.record_microphone;
                recorder_pipeline.cfg.audio.mic_volume = ui_state.mic_volume;
                recorder_pipeline.cfg.audio.separate_tracks = ui_state.separate_audio_tracks;
                recorder_pipeline.cfg.audio.save_separate_audio_file = ui_state.save_separate_audio_file;
                recorder_pipeline.cfg.audio.selected_audio_output = @intCast(ui_state.selected_audio_output);
                recorder_pipeline.cfg.audio.selected_audio_input = @intCast(ui_state.selected_audio_input);
                recorder_pipeline.cfg.encoder.container = switch (ui_state.container_format) {
                    1 => .mp4,
                    2 => .mov,
                    else => .mkv,
                };

                var target = types.CaptureTarget{
                    .capture_cursor = ui_state.capture_cursor,
                };

                if (ui_state.capture_type == 0) {
                    target.capture_type = .entire_screen;
                    target.monitor_index = @intCast(ui_state.selected_monitor);
                    if (monitors.len > 0 and ui_state.selected_monitor < monitors.len) {
                        if (monitors[@intCast(ui_state.selected_monitor)].hmonitor) |hmon| {
                            target.monitor_handle = @intFromPtr(hmon);
                        }
                    }
                } else if (ui_state.capture_type == 1) {
                    target.capture_type = .window;
                    if (window_entries.windows.len > 0 and ui_state.selected_window < window_entries.windows.len) {
                        target.window_handle = window_entries.windows[@intCast(ui_state.selected_window)].hwnd;
                    }
                } else {
                    target.capture_type = .game;
                    if (window_entries.windows.len > 0 and ui_state.selected_window < window_entries.windows.len) {
                        target.window_handle = window_entries.windows[@intCast(ui_state.selected_window)].hwnd;
                    }
                }

                var dir_slice: []const u8 = std.mem.sliceTo(&ui_state.output_dir, 0);
                dir_slice = std.mem.trim(u8, dir_slice, " \t\r\n");

                if (dir_slice.len > 0) {
                    ensureDirExists(dir_slice);
                }

                const ext = switch (ui_state.container_format) {
                    1 => "mp4",
                    2 => "mov",
                    else => "mkv",
                };

                var out_file_buf: [512]u8 = undefined;
                const out_file = if (dir_slice.len > 0) blk: {
                    if (dir_slice[dir_slice.len - 1] == '\\' or dir_slice[dir_slice.len - 1] == '/') {
                        break :blk std.fmt.bufPrint(&out_file_buf, "{s}LifeScreen_Recording_{d}.{s}\x00", .{ dir_slice, now_ms, ext }) catch "LifeScreen_Recording.mkv\x00";
                    } else {
                        break :blk std.fmt.bufPrint(&out_file_buf, "{s}\\{s}_{d}.{s}\x00", .{ dir_slice, "LifeScreen_Recording", now_ms, ext }) catch "LifeScreen_Recording.mkv\x00";
                    }
                } else std.fmt.bufPrint(&out_file_buf, "LifeScreen_Recording_{d}.{s}\x00", .{ now_ms, ext }) catch "LifeScreen_Recording.mkv\x00";

                recorder_pipeline.start(target, out_file[0 .. out_file.len - 1]) catch |err| {
                    std.log.err("Failed to start recording: {s}", .{@errorName(err)});
                };
            }
        }

        if (ui_state.action_stop_requested) {
            ui_state.action_stop_requested = false;
            if (recorder_pipeline.is_running.load(.monotonic)) {
                recorder_pipeline.stop();
            }
        }

        if (ui_state.action_pause_toggle_requested) {
            ui_state.action_pause_toggle_requested = false;
            if (recorder_pipeline.is_running.load(.monotonic)) {
                const cur_pause = recorder_pipeline.is_paused.load(.monotonic);
                recorder_pipeline.is_paused.store(!cur_pause, .release);
            }
        }

        if (ui_state.action_language_changed) {
            ui_state.action_language_changed = false;
            const lang: i18n.Language = switch (ui_state.language) {
                1 => .uk,
                2 => .zh,
                3 => .de,
                4 => .fr,
                5 => .pl,
                else => .en,
            };
            recorder_pipeline.cfg.language = lang.code();
            i18n.I18n.global.setLanguage(lang);
        }

        if (ui_state.action_open_folder_requested) {
            ui_state.action_open_folder_requested = false;
            var dir_slice: []const u8 = std.mem.sliceTo(&ui_state.output_dir, 0);
            dir_slice = std.mem.trim(u8, dir_slice, " \t\r\n");
            if (dir_slice.len > 0) {
                ensureDirExists(dir_slice);
            }
            const dir_z: [*:0]const u8 = @ptrCast(&ui_state.output_dir);
            _ = ShellExecuteA(null, "open", dir_z, null, null, 1);
        }

        // Update live stats from pipeline
        const snap = global_stats.readSnapshot();
        ui_state.state = @intFromEnum(snap.state);
        ui_state.duration_sec = @as(f64, @floatFromInt(snap.duration_nanos)) / 1e9;
        ui_state.file_bytes = snap.file_bytes;
        if (ui_state.duration_sec > 0.1) {
            const sec_u64: u64 = @intFromFloat(ui_state.duration_sec);
            if (sec_u64 > 0) {
                ui_state.bitrate_kbps = @intCast((snap.file_bytes * 8) / (sec_u64 * 1000));
            } else {
                ui_state.bitrate_kbps = 0;
            }
            ui_state.current_fps = @as(f32, @floatFromInt(snap.encoded_frames)) / @as(f32, @floatCast(ui_state.duration_sec));
        } else {
            ui_state.bitrate_kbps = 0;
            ui_state.current_fps = 0.0;
        }
        ui_state.captured_frames = snap.captured_frames;
        ui_state.encoded_frames = snap.encoded_frames;
        ui_state.dropped_frames = snap.dropped_frames;

        // Live real-time VU audio meters
        var sys_peak: f32 = 0.0;
        if (ui_state.record_system_audio) {
            if (snap.state == .recording) {
                sys_peak = snap.audio_peak_system;
            } else {
                sys_peak = wasapi.getDevicePeak(0, @intCast(ui_state.selected_audio_output));
            }
        }
        ui_state.sys_audio_level = @min(1.0, sys_peak * ui_state.system_volume);

        var mic_peak: f32 = 0.0;
        if (ui_state.record_microphone) {
            if (snap.state == .recording) {
                mic_peak = snap.audio_peak_mic;
            } else {
                mic_peak = wasapi.getDevicePeak(1, @intCast(ui_state.selected_audio_input));
            }
        }
        ui_state.mic_audio_level = @min(1.0, mic_peak * ui_state.mic_volume);

        // Render Frame
        window.beginFrame();
        window.renderUi(&ui_state);
        window.present();
    }
}
