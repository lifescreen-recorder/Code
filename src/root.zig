//! Root module for LifeScreen library and subsystems.
const std = @import("std");

pub const types = @import("core/types.zig");
pub const stats = @import("core/stats.zig");
pub const command = @import("core/command.zig");
pub const ring_buffer = @import("core/ring_buffer.zig");
pub const clock = @import("core/clock.zig");
pub const pipeline = @import("core/pipeline.zig");

pub const config = @import("config/config.zig");
pub const storage = @import("config/storage.zig");
pub const validation = @import("config/validation.zig");
pub const paths = @import("config/paths.zig");

pub const i18n = @import("i18n/i18n.zig");

pub const d3d11 = @import("platform/windows/d3d11_utils.zig");
pub const dxgi = @import("platform/windows/dxgi_capture.zig");
pub const wasapi = @import("platform/windows/wasapi_audio.zig");
pub const monitor_list = @import("platform/windows/monitor_list.zig");
pub const window_list = @import("platform/windows/window_list.zig");
pub const wgc = @import("platform/windows/wgc_capture.zig");
pub const capture_backend = @import("platform/windows/capture_backend.zig");
pub const game_capture = @import("platform/windows/game_capture.zig");
pub const injector = @import("platform/windows/hook/injector.zig");

pub const platform_windows = struct {
    pub const d3d11 = @import("platform/windows/d3d11_utils.zig");
    pub const dxgi = @import("platform/windows/dxgi_capture.zig");
    pub const wasapi = @import("platform/windows/wasapi_audio.zig");
    pub const monitor_list = @import("platform/windows/monitor_list.zig");
    pub const window_list = @import("platform/windows/window_list.zig");
    pub const wgc = @import("platform/windows/wgc_capture.zig");
    pub const game = @import("platform/windows/game_capture.zig");
    pub const injector = @import("platform/windows/hook/injector.zig");
    pub const capture_backend = @import("platform/windows/capture_backend.zig");
};

pub const encoder = @import("encoders/ffmpeg_encoder.zig");
pub const muxer = @import("encoders/muxer.zig");
pub const wav_writer = @import("encoders/wav_writer.zig");

pub const ui = struct {
    pub const app_window = @import("ui/app_window.zig");
    pub const ui_controller = @import("ui/ui_controller.zig");
    pub const AppWindow = app_window.AppWindow;
    pub const UiState = app_window.UiState;
    pub const runUi = ui_controller.runUi;
};

// Global std_options for logging
pub const std_options: std.Options = .{
    .log_level = .info,
};

test {
    _ = ring_buffer;
    _ = clock;
    _ = paths;
    _ = i18n;
    _ = validation;
    _ = storage;
}
