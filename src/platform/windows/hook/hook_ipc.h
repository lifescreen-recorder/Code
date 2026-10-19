#ifndef HOOK_IPC_H
#define HOOK_IPC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIFESCREEN_HOOK_MAGIC 0x4C534743 // 'LSGC'
#define LIFESCREEN_HOOK_VERSION 1

#pragma pack(push, 8)
typedef struct {
    uint32_t magic;          // LIFESCREEN_HOOK_MAGIC
    uint32_t version;        // LIFESCREEN_HOOK_VERSION
    uint32_t process_id;     // Game process ID
    uint32_t width;          // Texture width
    uint32_t height;         // Texture height
    uint32_t format;         // DXGI_FORMAT (e.g. DXGI_FORMAT_B8G8R8A8_UNORM = 87)
    uint64_t shared_handle;  // Windows HANDLE to shared D3D11 texture
    uint64_t frame_index;    // Monotonically increasing frame counter
    uint64_t timestamp_qpc;  // Performance counter timestamp
    bool active;             // True if game hook is active and producing frames
    bool recording;          // Set to true by LifeScreen when recording is active
} LifeScreenHookIpc;
#pragma pack(pop)

#define LIFESCREEN_IPC_MAP_PREFIX "LifeScreen_GameHook_IPC_"
#define LIFESCREEN_IPC_EVENT_PREFIX "LifeScreen_GameHook_Frame_"

#ifdef __cplusplus
}
#endif

#endif // HOOK_IPC_H
