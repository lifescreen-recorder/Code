#ifndef IMGUI_BRIDGE_H
#define IMGUI_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Current recording state: 0 = Idle, 1 = Starting, 2 = Recording, 3 = Paused, 4 = Stopping
    int state;
    double duration_sec;
    uint64_t file_bytes;
    uint32_t bitrate_kbps;
    uint64_t captured_frames;
    uint64_t encoded_frames;
    uint64_t dropped_frames;
    float current_fps;

    // Audio level meters (0.0f - 1.0f)
    float sys_audio_level;
    float mic_audio_level;

    // Source selection
    int capture_type; // 0 = Entire Screen, 1 = Window, 2 = Game
    int selected_monitor;
    int selected_window;
    bool capture_cursor;

    // Monitor and window labels
    const char* const* monitor_names;
    int monitor_count;

    const char* const* window_names;
    int window_count;

    // Audio device selection & labels
    const char* const* audio_output_names;
    int audio_output_count;
    int selected_audio_output;

    const char* const* audio_input_names;
    int audio_input_count;
    int selected_audio_input;

    // Configuration / Settings
    int language; // 0 = EN, 1 = UK
    int encoder_backend; // 0 = NVENC, 1 = AMF, 2 = QSV, 3 = Software
    int video_codec; // 0 = H264, 1 = HEVC, 2 = AV1
    int rate_control; // 0 = CQP, 1 = CRF, 2 = VBR, 3 = CBR
    int quality_cqp;
    uint32_t target_bitrate_kbps;
    int encoder_preset;
    int keyframe_interval_secs;
    int target_fps;
    int container_format; // 0 = MKV, 1 = MP4, 2 = MOV
    int output_resolution; // 0 = Native/Display, 1 = 1080p, 2 = 720p (Weak PC), 3 = 480p (Ultra Low)
    int color_range;       // 0 = Limited (BT.709 16-235), 1 = Full (0-255)
    char output_dir[260];

    // Hardware Encoder Capabilities Mask
    uint32_t encoder_caps_mask;

    // Overlay indicator
    bool show_overlay_indicator;

    // Hotkey settings (0 = F9, 1 = F10, 2 = Ctrl+Shift+R, 3 = Alt+F9)
    int hotkey_start_stop_preset;

    // Audio configuration
    bool record_system_audio;
    bool record_microphone;
    float system_volume;
    float mic_volume;
    bool separate_audio_tracks;
    bool save_separate_audio_file;

    // Live preview
    int preview_width;
    int preview_height;
    void* preview_texture_srv;

    // Outbound User Action Flags (set by ImGui, read & reset by Zig)
    bool action_start_requested;
    bool action_stop_requested;
    bool action_pause_toggle_requested;
    bool action_refresh_windows_requested;
    bool action_language_changed;
    bool action_open_folder_requested;
    bool action_settings_changed;
} UiState;

// Native Win32 Window & DX11 LifeCycle
bool imgui_window_create(int width, int height, const char* title);
bool imgui_window_poll_events(void);
void imgui_window_begin_frame(void);
void imgui_window_render_ui(UiState* state);
void imgui_window_present(void);
void imgui_window_get_size(int* out_width, int* out_height);
void imgui_window_destroy(void);

// Global Hotkey Pollers
bool imgui_window_poll_start_hotkey(void);
bool imgui_window_poll_pause_hotkey(void);

// Live Screen Preview Engine
void imgui_update_preview_d3d11(void* d3d11_texture, int width, int height);
void imgui_update_preview_bgra(const uint8_t* bgra_data, int width, int height, int stride);
void imgui_set_preview_enabled(bool enabled);
bool imgui_is_preview_enabled(void);

#ifdef __cplusplus
}
#endif

#endif // IMGUI_BRIDGE_H
