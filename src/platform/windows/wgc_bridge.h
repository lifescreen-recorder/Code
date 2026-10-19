#ifndef WGC_BRIDGE_H
#define WGC_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WgcCapture WgcCapture;

typedef struct {
    uint32_t width;
    uint32_t height;
    bool capture_cursor;
    bool draw_border;
} WgcConfig;

// Returns true if Windows Graphics Capture is supported on this system
bool wgc_is_supported(void);

// Create a WGC capture session for a window (HWND)
WgcCapture* wgc_capture_create_window(void* hwnd, void* d3d11_device, void* d3d11_context, const WgcConfig* config);

// Create a WGC capture session for a monitor (HMONITOR)
WgcCapture* wgc_capture_create_monitor(void* hmonitor, void* d3d11_device, void* d3d11_context, const WgcConfig* config);

// Acquire the next frame directly copying to dest_texture (ID3D11Texture2D)
// Returns true on success, false if timeout
bool wgc_capture_acquire_frame(WgcCapture* cap, void* dest_texture, uint32_t timeout_ms, uint32_t* out_width, uint32_t* out_height);

// Retrieve current capture item dimensions
void wgc_capture_get_size(WgcCapture* cap, uint32_t* out_width, uint32_t* out_height);

// Destroy capture session and release all COM resources
void wgc_capture_destroy(WgcCapture* cap);

#ifdef __cplusplus
}
#endif

#endif // WGC_BRIDGE_H
