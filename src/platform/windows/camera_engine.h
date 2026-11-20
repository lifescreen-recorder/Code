#pragma once
#ifndef LIFESCREEN_CAMERA_ENGINE_H
#define LIFESCREEN_CAMERA_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    int source_type;
    int device_index;
    int position;
    float pos_x_pct;
    float pos_y_pct;
    int margin_px;
    float size_pct;
    int aspect_ratio;
    int shape;
    float corner_radius;
    float border_thickness;
    uint32_t border_color;
    float opacity;
    bool mirror;
    bool chroma_key;
    uint32_t chroma_color;
    float chroma_similarity;
    float chroma_smoothness;
    char web_url[512];
} CameraOverlayConfig;

void camera_engine_init(void);
void camera_engine_shutdown(void);
int camera_engine_refresh_devices(void);
int camera_engine_get_device_count(void);
const char* camera_engine_get_device_name(int index);
const char* const* camera_engine_get_device_names(void);

void camera_engine_start(int source_type, int device_index, const char* web_url);
void camera_engine_stop(void);
bool camera_engine_is_running(void);

void camera_engine_update_settings(const CameraOverlayConfig* cfg);
void camera_engine_get_settings(CameraOverlayConfig* out_cfg);

void camera_engine_update_d3d11_preview(void* d3d11_device, void* d3d11_context, void** out_srv, int* out_w, int* out_h);
void camera_engine_composite_d3d11(void* d3d11_device, void* d3d11_context, void* dest_texture, int screen_w, int screen_h);
void camera_engine_composite_sw_frame(uint8_t* dst_y, uint8_t* dst_uv, int stride_y, int stride_uv, int width, int height, bool is_nv12);

#ifdef __cplusplus
}
#endif

#endif
