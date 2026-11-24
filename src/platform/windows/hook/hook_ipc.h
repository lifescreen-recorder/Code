#ifndef HOOK_IPC_H
#define HOOK_IPC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIFESCREEN_HOOK_MAGIC 0x4C534743
#define LIFESCREEN_HOOK_VERSION 2

#define LIFESCREEN_API_UNKNOWN 0
#define LIFESCREEN_API_D3D11   1
#define LIFESCREEN_API_D3D12   2
#define LIFESCREEN_API_OPENGL  3
#define LIFESCREEN_API_VULKAN  4

#pragma pack(push, 8)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t process_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t shared_handle;
    uint64_t frame_index;
    uint64_t timestamp_qpc;
    bool active;
    bool recording;
    uint8_t pad[2];
    uint32_t graphics_api;
} LifeScreenHookIpc;
#pragma pack(pop)

#define LIFESCREEN_IPC_MAP_PREFIX "LifeScreen_GameHook_IPC_"
#define LIFESCREEN_IPC_EVENT_PREFIX "LifeScreen_GameHook_Frame_"

#ifdef __cplusplus
}
#endif

#endif
