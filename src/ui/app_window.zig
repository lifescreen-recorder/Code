//! Native Win32 + Direct3D 11 application window with Dear ImGui integration.
const std = @import("std");

pub const UiState = extern struct {
    state: c_int = 0,
    duration_sec: f64 = 0.0,
    file_bytes: u64 = 0,
    bitrate_kbps: u32 = 0,
    captured_frames: u64 = 0,
    encoded_frames: u64 = 0,
    dropped_frames: u64 = 0,
    current_fps: f32 = 0.0,

    sys_audio_level: f32 = 0.0,
    mic_audio_level: f32 = 0.0,

    capture_type: c_int = 0,
    selected_monitor: c_int = 0,
    selected_window: c_int = 0,
    capture_cursor: bool = true,

    monitor_names: ?[*]const [*:0]const u8 = null,
    monitor_count: c_int = 0,

    window_names: ?[*]const [*:0]const u8 = null,
    window_count: c_int = 0,

    audio_output_names: ?[*]const [*:0]const u8 = null,
    audio_output_count: c_int = 0,
    selected_audio_output: c_int = 0,

    audio_input_names: ?[*]const [*:0]const u8 = null,
    audio_input_count: c_int = 0,
    selected_audio_input: c_int = 0,

    language: c_int = 0,
    encoder_backend: c_int = 0,
    video_codec: c_int = 0,
    rate_control: c_int = 0,
    quality_cqp: c_int = 20,
    target_bitrate_kbps: u32 = 15000,
    encoder_preset: c_int = 1,
    keyframe_interval_secs: c_int = 2,
    target_fps: c_int = 60,
    container_format: c_int = 0, // 0 = MKV, 1 = MP4, 2 = MOV
    output_resolution: c_int = 0, // 0 = Native/Display, 1 = 1080p, 2 = 720p, 3 = 480p
    color_range: c_int = 0, // 0 = Limited (BT.709 16-235), 1 = Full (0-255)
    output_dir: [260]u8 = std.mem.zeroes([260]u8),

    encoder_caps_mask: u32 = 0,
    show_overlay_indicator: bool = true,
    hotkey_start_stop_preset: c_int = 0, // 0 = F9

    record_system_audio: bool = true,
    record_microphone: bool = true,
    system_volume: f32 = 1.0,
    mic_volume: f32 = 1.0,
    separate_audio_tracks: bool = false,
    save_separate_audio_file: bool = false,

    preview_width: c_int = 1920,
    preview_height: c_int = 1080,
    preview_texture_srv: ?*anyopaque = null,

    action_start_requested: bool = false,
    action_stop_requested: bool = false,
    action_pause_toggle_requested: bool = false,
    action_refresh_windows_requested: bool = false,
    action_language_changed: bool = false,
    action_open_folder_requested: bool = false,
    action_settings_changed: bool = false,
};

extern fn imgui_window_create(width: c_int, height: c_int, title: [*:0]const u8) callconv(.c) bool;
extern fn imgui_window_poll_events() callconv(.c) bool;
extern fn imgui_window_begin_frame() callconv(.c) void;
extern fn imgui_window_render_ui(state: *UiState) callconv(.c) void;
extern fn imgui_window_present() callconv(.c) void;
extern fn imgui_window_get_size(out_width: *c_int, out_height: *c_int) callconv(.c) void;
extern fn imgui_window_destroy() callconv(.c) void;
extern fn imgui_window_poll_start_hotkey() callconv(.c) bool;
extern fn imgui_window_poll_pause_hotkey() callconv(.c) bool;

pub extern fn imgui_update_preview_d3d11(texture_ptr: ?*anyopaque, width: c_int, height: c_int) callconv(.c) void;
pub extern fn imgui_update_preview_bgra(data: ?[*]const u8, width: c_int, height: c_int, stride: c_int) callconv(.c) void;
pub extern fn imgui_set_preview_enabled(enabled: bool) callconv(.c) void;
pub extern fn imgui_is_preview_enabled() callconv(.c) bool;

pub const AppWindow = struct {
    width: i32,
    height: i32,

    pub fn init(title: [*:0]const u8, width: i32, height: i32) !AppWindow {
        if (!imgui_window_create(width, height, title)) {
            return error.WindowCreationFailed;
        }
        return .{
            .width = width,
            .height = height,
        };
    }

    pub fn pollEvents(self: *AppWindow) bool {
        _ = self;
        return imgui_window_poll_events();
    }

    pub fn beginFrame(self: *AppWindow) void {
        _ = self;
        imgui_window_begin_frame();
    }

    pub fn renderUi(self: *AppWindow, state: *UiState) void {
        _ = self;
        imgui_window_render_ui(state);
    }

    pub fn present(self: *AppWindow) void {
        _ = self;
        imgui_window_present();
    }

    pub fn pollStartHotkey(self: *AppWindow) bool {
        _ = self;
        return imgui_window_poll_start_hotkey();
    }

    pub fn pollPauseHotkey(self: *AppWindow) bool {
        _ = self;
        return imgui_window_poll_pause_hotkey();
    }

    pub fn deinit(self: *AppWindow) void {
        _ = self;
        imgui_window_destroy();
    }
};
