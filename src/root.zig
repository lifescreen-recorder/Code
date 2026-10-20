//! Root module for LifeScreen library and subsystems.
const std = @import("std");
const builtin = @import("builtin");

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

pub const encoder = @import("encoders/ffmpeg_encoder.zig");
pub const muxer = @import("encoders/muxer.zig");
pub const wav_writer = @import("encoders/wav_writer.zig");

pub const is_windows = builtin.os.tag == .windows;
pub const is_linux = builtin.os.tag == .linux;
pub const is_darwin = builtin.os.tag.isDarwin();

// Windows-specific subsystems
pub const d3d11 = if (is_windows) @import("platform/windows/d3d11_utils.zig") else struct {};
pub const dxgi = if (is_windows) @import("platform/windows/dxgi_capture.zig") else struct {};
pub const wasapi = if (is_windows) @import("platform/windows/wasapi_audio.zig") else struct {};
pub const monitor_list = if (is_windows) @import("platform/windows/monitor_list.zig") else struct {};
pub const window_list = if (is_windows) @import("platform/windows/window_list.zig") else struct {};
pub const wgc = if (is_windows) @import("platform/windows/wgc_capture.zig") else struct {};
pub const capture_backend = if (is_windows) @import("platform/windows/capture_backend.zig") else struct {};
pub const game_capture = if (is_windows) @import("platform/windows/game_capture.zig") else struct {};
pub const injector = if (is_windows) @import("platform/windows/hook/injector.zig") else struct {};

pub const platform_windows = if (is_windows) struct {
    pub const d3d11 = @import("platform/windows/d3d11_utils.zig");
    pub const dxgi = @import("platform/windows/dxgi_capture.zig");
    pub const wasapi = @import("platform/windows/wasapi_audio.zig");
    pub const monitor_list = @import("platform/windows/monitor_list.zig");
    pub const window_list = @import("platform/windows/window_list.zig");
    pub const wgc = @import("platform/windows/wgc_capture.zig");
    pub const game = @import("platform/windows/game_capture.zig");
    pub const injector = @import("platform/windows/hook/injector.zig");
    pub const capture_backend = @import("platform/windows/capture_backend.zig");
} else struct {};

pub const platform_linux = if (is_linux) struct {
    pub const pipewire = @import("platform/linux/pipewire_capture.zig");
    pub const x11 = @import("platform/linux/x11_capture.zig");
    pub const pulseaudio = @import("platform/linux/pulseaudio.zig");
    pub const monitor_list = @import("platform/linux/monitor_list.zig");
    pub const capture_backend = @import("platform/linux/capture_backend.zig");
} else struct {};

pub const platform_macos = if (is_darwin) struct {
    pub const sck = @import("platform/macos/screencapturekit.zig");
    pub const coreaudio = @import("platform/macos/coreaudio.zig");
    pub const display_list = @import("platform/macos/display_list.zig");
    pub const capture_backend = @import("platform/macos/capture_backend.zig");
} else struct {};

pub const ui = if (is_windows) struct {
    pub const app_window = @import("ui/app_window.zig");
    pub const ui_controller = @import("ui/ui_controller.zig");
    pub const AppWindow = app_window.AppWindow;
    pub const UiState = app_window.UiState;
    pub const runUi = ui_controller.runUi;
} else struct {};

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
