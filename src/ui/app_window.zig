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
    encoder_choice: c_int = 0,
    qsv_preset: c_int = 0,
    qsv_low_power: bool = true,
    qsv_async_depth: c_int = 4,
    intel_msdk_codec: c_int = 0,
    ffmpeg_hw_engine: c_int = 0,
    ffmpeg_hw_codec: c_int = 0,
    ffmpeg_hw_preset: c_int = 2,
    ffmpeg_hw_zerolatency: bool = true,
    wmf_codec: c_int = 0,
    wmf_rate_control: c_int = 2,
    wmf_scenario: c_int = 1,
    wmf_quality: c_int = 70,
    wmf_hw_encoding: bool = true,
    openh264_profile: c_int = 0,
    openh264_slices: c_int = 1,
    openh264_rc_mode: c_int = 0,
    x264_preset: c_int = 0,
    x264_tune: c_int = 0,
    x264_profile: c_int = 0,
    x264_threads: c_int = 0,
    mjpeg_quality: c_int = 3,
    mjpeg_huffman: c_int = 1,
    mjpeg_color_range: c_int = 0,

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

    low_end_mode: bool = false,

    record_system_audio2: bool = false,
    selected_audio_output2: c_int = 0,
    system_volume2: f32 = 1.0,
    record_microphone2: bool = false,
    selected_audio_input2: c_int = 0,
    mic_volume2: f32 = 1.0,
    sys_audio_level2: f32 = 0.0,
    mic_audio_level2: f32 = 0.0,

    mic_noise_suppression: bool = false,
    mic_ns_mode: c_int = 0,
    mic_ns_strength: f32 = 0.65,
    mic_compressor: bool = false,
    mic_comp_amount: f32 = 0.5,
    mic_gain_db: f32 = 0.0,

    sys_audio_mode: c_int = 0,
    sys_target_pid: u32 = 0,
    sys_clarity_filter: bool = false,
    sys_compressor: bool = false,
    sys_comp_amount: f32 = 0.5,
    sys_gain_db: f32 = 0.0,

    camera_enabled: bool = false,
    camera_source_type: c_int = 0,
    camera_device_index: c_int = 0,
    camera_device_names: ?[*]const [*:0]const u8 = null,
    camera_device_count: c_int = 0,
    camera_web_url: [512]u8 = std.mem.zeroes([512]u8),
    camera_position: c_int = 0,
    camera_pos_x: f32 = 0.80,
    camera_pos_y: f32 = 0.75,
    camera_margin_px: c_int = 24,
    camera_size_pct: f32 = 0.22,
    camera_aspect_ratio: c_int = 0,
    camera_shape: c_int = 1,
    camera_corner_radius: f32 = 16.0,
    camera_border_thickness: f32 = 3.0,
    camera_border_color: u32 = 0x0EA5E9FF,
    camera_opacity: f32 = 1.0,
    camera_mirror: bool = false,
    camera_flip_vertical: bool = false,
    camera_chroma_key: bool = false,
    camera_chroma_color: u32 = 0x00FF00,
    camera_chroma_sim: f32 = 0.40,
    camera_chroma_smooth: f32 = 0.10,
    camera_brightness: f32 = 0.0,
    camera_contrast: f32 = 1.0,
    camera_saturation: f32 = 1.0,
    camera_sharpness: f32 = 0.25,
    camera_denoise: f32 = 0.25,
    camera_color_filter: c_int = 0,
    camera_vignette: f32 = 0.0,
    camera_glow: bool = false,
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

pub extern fn imgui_camera_composite_d3d11(dest_texture: ?*anyopaque, screen_w: c_int, screen_h: c_int) callconv(.c) void;
pub extern fn imgui_camera_composite_sw_frame(dst_y: ?[*]u8, dst_uv: ?[*]u8, stride_y: c_int, stride_uv: c_int, width: c_int, height: c_int, is_nv12: bool) callconv(.c) void;
pub extern fn imgui_camera_is_enabled() callconv(.c) bool;

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
