#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "camera_engine.h"

static bool s_mfInitialized = false;
static CameraOverlayConfig s_config = {
    false, 0, 0, 0, 0.80f, 0.75f, 24, 0.22f, 0, 1, 16.0f, 3.0f, 0x0EA5E9FF, 1.0f, false, false, 0x00FF00, 0.40f, 0.10f, {0}
};

static CRITICAL_SECTION s_configCs;
static CRITICAL_SECTION s_frameCs;
static bool s_csInit = false;

#define MAX_CAM_DEVICES 32
static char s_deviceNamesStorage[MAX_CAM_DEVICES][128];
static const char* s_deviceNamesPtrs[MAX_CAM_DEVICES];
static int s_deviceCount = 0;

static HANDLE s_hThread = NULL;
static volatile bool s_captureRunning = false;
static int s_activeSourceType = -1;
static int s_activeDeviceIndex = -1;
static char s_activeWebUrl[512] = {0};

static uint8_t* s_camRawBuffer = NULL;
static int s_camRawW = 0;
static int s_camRawH = 0;
static uint64_t s_camFrameIndex = 0;

static uint8_t* s_scaledBuffer = NULL;
static int s_scaledCapacity = 0;

static ID3D11Texture2D* s_pPreviewTex = NULL;
static ID3D11ShaderResourceView* s_pPreviewSRV = NULL;
static int s_previewTexW = 0;
static int s_previewTexH = 0;

static void ensure_cs(void) {
    if (!s_csInit) {
        InitializeCriticalSection(&s_configCs);
        InitializeCriticalSection(&s_frameCs);
        s_csInit = true;
    }
}

static void ensure_mf(void) {
    if (!s_mfInitialized) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        s_mfInitialized = true;
    }
}

int camera_engine_refresh_devices(void) {
    ensure_cs();
    ensure_mf();

    EnterCriticalSection(&s_configCs);
    s_deviceCount = 0;

    IMFAttributes* pAttributes = NULL;
    HRESULT hr = MFCreateAttributes(&pAttributes, 1);
    if (SUCCEEDED(hr) && pAttributes) {
        pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        IMFActivate** ppDevices = NULL;
        UINT32 count = 0;
        hr = MFEnumDeviceSources(pAttributes, &ppDevices, &count);
        if (SUCCEEDED(hr) && ppDevices) {
            for (UINT32 i = 0; i < count && s_deviceCount < (MAX_CAM_DEVICES - 2); i++) {
                WCHAR* pFriendlyName = NULL;
                UINT32 cchName = 0;
                hr = ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &pFriendlyName, &cchName);
                if (SUCCEEDED(hr) && pFriendlyName) {
                    WideCharToMultiByte(CP_UTF8, 0, pFriendlyName, -1, s_deviceNamesStorage[s_deviceCount], sizeof(s_deviceNamesStorage[0]), NULL, NULL);
                    s_deviceNamesPtrs[s_deviceCount] = s_deviceNamesStorage[s_deviceCount];
                    s_deviceCount++;
                    CoTaskMemFree(pFriendlyName);
                }
                ppDevices[i]->Release();
            }
            CoTaskMemFree(ppDevices);
        }
        pAttributes->Release();
    }

    snprintf(s_deviceNamesStorage[s_deviceCount], sizeof(s_deviceNamesStorage[0]), "Studio Test Pattern (OBS)");
    s_deviceNamesPtrs[s_deviceCount] = s_deviceNamesStorage[s_deviceCount];
    s_deviceCount++;

    snprintf(s_deviceNamesStorage[s_deviceCount], sizeof(s_deviceNamesStorage[0]), "Web / URL Stream Source");
    s_deviceNamesPtrs[s_deviceCount] = s_deviceNamesStorage[s_deviceCount];
    s_deviceCount++;

    LeaveCriticalSection(&s_configCs);
    return s_deviceCount;
}

int camera_engine_get_device_count(void) {
    ensure_cs();
    return s_deviceCount;
}

const char* camera_engine_get_device_name(int index) {
    ensure_cs();
    if (index >= 0 && index < s_deviceCount) {
        return s_deviceNamesPtrs[index];
    }
    return "Unknown Device";
}

const char* const* camera_engine_get_device_names(void) {
    ensure_cs();
    return s_deviceNamesPtrs;
}

void camera_engine_init(void) {
    ensure_cs();
    ensure_mf();
    camera_engine_refresh_devices();
}

void camera_engine_shutdown(void) {
    camera_engine_stop();
    if (s_pPreviewSRV) { s_pPreviewSRV->Release(); s_pPreviewSRV = NULL; }
    if (s_pPreviewTex) { s_pPreviewTex->Release(); s_pPreviewTex = NULL; }
    if (s_camRawBuffer) { free(s_camRawBuffer); s_camRawBuffer = NULL; }
    if (s_scaledBuffer) { free(s_scaledBuffer); s_scaledBuffer = NULL; }
    if (s_mfInitialized) {
        MFShutdown();
        CoUninitialize();
        s_mfInitialized = false;
    }
}

void camera_engine_update_settings(const CameraOverlayConfig* cfg) {
    if (!cfg) return;
    ensure_cs();
    EnterCriticalSection(&s_configCs);
    s_config = *cfg;
    LeaveCriticalSection(&s_configCs);

    if (cfg->enabled) {
        if (!s_captureRunning || s_activeSourceType != cfg->source_type || s_activeDeviceIndex != cfg->device_index) {
            camera_engine_start(cfg->source_type, cfg->device_index, cfg->web_url);
        }
    } else {
        if (s_captureRunning) {
            camera_engine_stop();
        }
    }
}

void camera_engine_get_settings(CameraOverlayConfig* out_cfg) {
    if (!out_cfg) return;
    ensure_cs();
    EnterCriticalSection(&s_configCs);
    *out_cfg = s_config;
    LeaveCriticalSection(&s_configCs);
}

static void generate_test_pattern(uint8_t* buffer, int width, int height, uint64_t tick, const char* label) {
    const uint32_t smpte_colors[7] = {
        0xFFC0C0C0,
        0xFFC0C000,
        0xFF00C0C0,
        0xFF00C000,
        0xFFC000C0,
        0xFFC00000,
        0xFF0000C0
    };

    int bar_h = (height * 2) / 3;
    int bot_h = height - bar_h;
    int bar_w = width / 7;

    for (int y = 0; y < bar_h; y++) {
        uint32_t* row = (uint32_t*)(buffer + y * (width * 4));
        for (int x = 0; x < width; x++) {
            int c_idx = x / bar_w;
            if (c_idx > 6) c_idx = 6;
            row[x] = smpte_colors[c_idx];
        }
    }

    for (int y = bar_h; y < height; y++) {
        uint32_t* row = (uint32_t*)(buffer + y * (width * 4));
        int py = y - bar_h;
        for (int x = 0; x < width; x++) {
            if (py < bot_h / 4) {
                int c_idx = 6 - (x / bar_w);
                if (c_idx < 0) c_idx = 0;
                if (c_idx > 6) c_idx = 6;
                row[x] = smpte_colors[c_idx];
            } else {
                int sweep = (int)(x + tick * 4) % width;
                if (x == sweep || x == (sweep + 1) % width) {
                    row[x] = 0xFFFFFFFF;
                } else {
                    int shade = (x * 255) / width;
                    row[x] = (0xFF << 24) | (shade << 16) | (shade << 8) | shade;
                }
            }
        }
    }

    int wave_y = bar_h - 20 + (int)(sinf((float)(tick * 0.1f)) * 12.0f);
    if (wave_y >= 0 && wave_y < height) {
        uint32_t* row = (uint32_t*)(buffer + wave_y * (width * 4));
        for (int x = 0; x < width; x++) {
            row[x] = 0xFF00FF00;
        }
    }

    int rec_flash = (int)(tick / 15) % 2;
    int dot_cx = 24, dot_cy = 24, dot_r = 8;
    for (int dy = -dot_r; dy <= dot_r; dy++) {
        int py = dot_cy + dy;
        if (py < 0 || py >= height) continue;
        uint32_t* row = (uint32_t*)(buffer + py * (width * 4));
        for (int dx = -dot_r; dx <= dot_r; dx++) {
            int px = dot_cx + dx;
            if (px < 0 || px >= width) continue;
            if (dx * dx + dy * dy <= dot_r * dot_r) {
                row[px] = rec_flash ? 0xFF0000FF : 0xFF303030;
            }
        }
    }

    (void)label;
}

static DWORD WINAPI camera_capture_worker(LPVOID lpParam) {
    (void)lpParam;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    int source_type = s_activeSourceType;
    int device_index = s_activeDeviceIndex;

    int real_cam_count = 0;
    IMFAttributes* pEnumAttr = NULL;
    IMFActivate** ppDevices = NULL;
    UINT32 enumCount = 0;
    if (SUCCEEDED(MFCreateAttributes(&pEnumAttr, 1))) {
        pEnumAttr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (SUCCEEDED(MFEnumDeviceSources(pEnumAttr, &ppDevices, &enumCount))) {
            real_cam_count = (int)enumCount;
        }
        pEnumAttr->Release();
    }

    bool use_mf_cam = (source_type == 0 && device_index < real_cam_count && ppDevices && ppDevices[device_index]);

    IMFMediaSource* pSource = NULL;
    IMFSourceReader* pReader = NULL;

    if (use_mf_cam) {
        HRESULT hr = ppDevices[device_index]->ActivateObject(IID_IMFMediaSource, (void**)&pSource);
        if (SUCCEEDED(hr) && pSource) {
            IMFAttributes* pReaderAttrs = NULL;
            MFCreateAttributes(&pReaderAttrs, 1);
            if (pReaderAttrs) {
                pReaderAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            }
            hr = MFCreateSourceReaderFromMediaSource(pSource, pReaderAttrs, &pReader);
            if (pReaderAttrs) pReaderAttrs->Release();

            if (SUCCEEDED(hr) && pReader) {
                IMFMediaType* pType = NULL;
                MFCreateMediaType(&pType);
                if (pType) {
                    pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                    pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                    pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pType);
                    pType->Release();
                }
            }
        }
    }

    uint64_t tick = 0;
    const int def_w = 640;
    const int def_h = 480;

    while (s_captureRunning) {
        if (pReader) {
            DWORD streamIdx = 0, flags = 0;
            LONGLONG timeStamp = 0;
            IMFSample* pSample = NULL;
            HRESULT hr = pReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIdx, &flags, &timeStamp, &pSample);
            if (SUCCEEDED(hr) && pSample) {
                IMFMediaBuffer* pBuffer = NULL;
                if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuffer))) {
                    BYTE* pData = NULL;
                    DWORD maxLen = 0, curLen = 0;
                    if (SUCCEEDED(pBuffer->Lock(&pData, &maxLen, &curLen))) {
                        IMFMediaType* pCurType = NULL;
                        UINT32 w = def_w, h = def_h;
                        if (SUCCEEDED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurType))) {
                            MFGetAttributeSize(pCurType, MF_MT_FRAME_SIZE, &w, &h);
                            pCurType->Release();
                        }
                        if (w > 0 && h > 0 && curLen >= w * h * 4) {
                            EnterCriticalSection(&s_frameCs);
                            if (s_camRawW != (int)w || s_camRawH != (int)h) {
                                if (s_camRawBuffer) free(s_camRawBuffer);
                                s_camRawBuffer = (uint8_t*)malloc(w * h * 4);
                                s_camRawW = (int)w;
                                s_camRawH = (int)h;
                            }
                            if (s_camRawBuffer) {
                                memcpy(s_camRawBuffer, pData, w * h * 4);
                                s_camFrameIndex++;
                            }
                            LeaveCriticalSection(&s_frameCs);
                        }
                        pBuffer->Unlock();
                    }
                    pBuffer->Release();
                }
                pSample->Release();
            } else {
                Sleep(10);
            }
        } else {
            EnterCriticalSection(&s_frameCs);
            if (s_camRawW != def_w || s_camRawH != def_h || !s_camRawBuffer) {
                if (s_camRawBuffer) free(s_camRawBuffer);
                s_camRawBuffer = (uint8_t*)malloc(def_w * def_h * 4);
                s_camRawW = def_w;
                s_camRawH = def_h;
            }
            if (s_camRawBuffer) {
                generate_test_pattern(s_camRawBuffer, def_w, def_h, tick, s_activeWebUrl);
                s_camFrameIndex++;
            }
            LeaveCriticalSection(&s_frameCs);
            tick++;
            Sleep(33);
        }
    }

    if (pReader) { pReader->Release(); pReader = NULL; }
    if (pSource) { pSource->Shutdown(); pSource->Release(); pSource = NULL; }
    if (ppDevices) {
        for (UINT32 i = 0; i < enumCount; i++) {
            if (ppDevices[i]) ppDevices[i]->Release();
        }
        CoTaskMemFree(ppDevices);
    }

    MFShutdown();
    CoUninitialize();
    return 0;
}

void camera_engine_start(int source_type, int device_index, const char* web_url) {
    ensure_cs();
    camera_engine_stop();

    s_activeSourceType = source_type;
    s_activeDeviceIndex = device_index;
    if (web_url) {
        snprintf(s_activeWebUrl, sizeof(s_activeWebUrl), "%s", web_url);
    } else {
        s_activeWebUrl[0] = '\0';
    }

    s_captureRunning = true;
    s_hThread = CreateThread(NULL, 0, camera_capture_worker, NULL, 0, NULL);
}

void camera_engine_stop(void) {
    if (s_captureRunning) {
        s_captureRunning = false;
        if (s_hThread) {
            WaitForSingleObject(s_hThread, 3000);
            CloseHandle(s_hThread);
            s_hThread = NULL;
        }
    }
}

bool camera_engine_is_running(void) {
    return s_captureRunning;
}

void camera_engine_update_d3d11_preview(void* d3d11_device, void* d3d11_context, void** out_srv, int* out_w, int* out_h) {
    if (!out_srv || !d3d11_device || !d3d11_context) return;
    *out_srv = NULL;

    ensure_cs();
    EnterCriticalSection(&s_frameCs);
    if (!s_camRawBuffer || s_camRawW <= 0 || s_camRawH <= 0) {
        LeaveCriticalSection(&s_frameCs);
        return;
    }

    ID3D11Device* pDev = (ID3D11Device*)d3d11_device;
    ID3D11DeviceContext* pCtx = (ID3D11DeviceContext*)d3d11_context;

    if (!s_pPreviewTex || s_previewTexW != s_camRawW || s_previewTexH != s_camRawH) {
        if (s_pPreviewSRV) { s_pPreviewSRV->Release(); s_pPreviewSRV = NULL; }
        if (s_pPreviewTex) { s_pPreviewTex->Release(); s_pPreviewTex = NULL; }

        D3D11_TEXTURE2D_DESC desc = {0};
        desc.Width = s_camRawW;
        desc.Height = s_camRawH;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = pDev->CreateTexture2D(&desc, NULL, &s_pPreviewTex);
        if (SUCCEEDED(hr) && s_pPreviewTex) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
            memset(&srvDesc, 0, sizeof(srvDesc));
            srvDesc.Format = desc.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            pDev->CreateShaderResourceView(s_pPreviewTex, &srvDesc, &s_pPreviewSRV);
            s_previewTexW = s_camRawW;
            s_previewTexH = s_camRawH;
        }
    }

    if (s_pPreviewTex && s_pPreviewSRV) {
        pCtx->UpdateSubresource((ID3D11Resource*)s_pPreviewTex, 0, NULL, s_camRawBuffer, s_camRawW * 4, 0);
        *out_srv = (void*)s_pPreviewSRV;
        if (out_w) *out_w = s_camRawW;
        if (out_h) *out_h = s_camRawH;
    }

    LeaveCriticalSection(&s_frameCs);
}

void camera_engine_composite_d3d11(void* d3d11_device, void* d3d11_context, void* dest_texture, int screen_w, int screen_h) {
    if (!dest_texture || !d3d11_context || screen_w <= 0 || screen_h <= 0) return;

    ensure_cs();
    CameraOverlayConfig cfg;
    EnterCriticalSection(&s_configCs);
    cfg = s_config;
    LeaveCriticalSection(&s_configCs);

    if (!cfg.enabled) return;

    EnterCriticalSection(&s_frameCs);
    if (!s_camRawBuffer || s_camRawW <= 0 || s_camRawH <= 0) {
        LeaveCriticalSection(&s_frameCs);
        return;
    }

    int src_w = s_camRawW;
    int src_h = s_camRawH;

    float aspect = (float)src_w / (float)src_h;
    if (aspect <= 0.01f) aspect = 1.777f;

    int dst_w = (int)(screen_w * cfg.size_pct);
    if (dst_w < 64) dst_w = 64;
    if (dst_w > screen_w) dst_w = screen_w;

    int dst_h = (cfg.aspect_ratio == 2) ? dst_w : (int)(dst_w / aspect);
    if (dst_h < 48) dst_h = 48;
    if (dst_h > screen_h) dst_h = screen_h;

    int margin = cfg.margin_px;
    int dst_x = 0;
    int dst_y = 0;

    switch (cfg.position) {
        case 0:
            dst_x = screen_w - margin - dst_w;
            dst_y = screen_h - margin - dst_h;
            break;
        case 1:
            dst_x = margin;
            dst_y = screen_h - margin - dst_h;
            break;
        case 2:
            dst_x = screen_w - margin - dst_w;
            dst_y = margin;
            break;
        case 3:
            dst_x = margin;
            dst_y = margin;
            break;
        case 4:
            dst_x = (screen_w - dst_w) / 2;
            dst_y = screen_h - margin - dst_h;
            break;
        case 5:
            dst_x = (screen_w - dst_w) / 2;
            dst_y = margin;
            break;
        case 6:
            dst_x = margin;
            dst_y = (screen_h - dst_h) / 2;
            break;
        case 7:
            dst_x = screen_w - margin - dst_w;
            dst_y = (screen_h - dst_h) / 2;
            break;
        case 8:
        default:
            dst_x = (int)(cfg.pos_x_pct * (float)(screen_w - dst_w));
            dst_y = (int)(cfg.pos_y_pct * (float)(screen_h - dst_h));
            break;
    }

    if (dst_x < 0) dst_x = 0;
    if (dst_y < 0) dst_y = 0;
    if (dst_x + dst_w > screen_w) dst_w = screen_w - dst_x;
    if (dst_y + dst_h > screen_h) dst_h = screen_h - dst_y;
    if (dst_w <= 0 || dst_h <= 0) {
        LeaveCriticalSection(&s_frameCs);
        return;
    }

    int needed_bytes = dst_w * dst_h * 4;
    if (s_scaledCapacity < needed_bytes) {
        if (s_scaledBuffer) free(s_scaledBuffer);
        s_scaledBuffer = (uint8_t*)malloc(needed_bytes);
        s_scaledCapacity = needed_bytes;
    }

    if (!s_scaledBuffer) {
        LeaveCriticalSection(&s_frameCs);
        return;
    }

    uint32_t* dst_px = (uint32_t*)s_scaledBuffer;
    const uint32_t* src_px = (const uint32_t*)s_camRawBuffer;

    float scale_x = (float)src_w / (float)dst_w;
    float scale_y = (float)src_h / (float)dst_h;

    float r_cr = (cfg.shape == 2) ? ((float)dst_w * 0.5f) : cfg.corner_radius;
    float center_x = (float)dst_w * 0.5f;
    float center_y = (float)dst_h * 0.5f;

    uint32_t b_col = cfg.border_color;
    float b_thick = cfg.border_thickness;

    for (int y = 0; y < dst_h; y++) {
        int sy = (int)(y * scale_y);
        if (sy >= src_h) sy = src_h - 1;
        const uint32_t* src_row = src_px + sy * src_w;
        uint32_t* dst_row = dst_px + y * dst_w;

        for (int x = 0; x < dst_w; x++) {
            int sx = cfg.mirror ? (int)((dst_w - 1 - x) * scale_x) : (int)(x * scale_x);
            if (sx >= src_w) sx = src_w - 1;

            uint32_t c = src_row[sx];

            bool is_border = false;
            bool is_outside = false;

            if (cfg.shape == 2) {
                float dx = (float)x - center_x;
                float dy = (float)y - center_y;
                float dist = sqrtf(dx * dx + dy * dy);
                if (dist > r_cr) {
                    is_outside = true;
                } else if (b_thick > 0.0f && dist >= (r_cr - b_thick)) {
                    is_border = true;
                }
            } else if (cfg.shape == 1 && r_cr > 1.0f) {
                float cx = (x < r_cr) ? (r_cr - (float)x) : ((x >= dst_w - r_cr) ? ((float)x - (dst_w - 1 - r_cr)) : 0.0f);
                float cy = (y < r_cr) ? (r_cr - (float)y) : ((y >= dst_h - r_cr) ? ((float)y - (dst_h - 1 - r_cr)) : 0.0f);
                if (cx > 0.0f && cy > 0.0f) {
                    float dist = sqrtf(cx * cx + cy * cy);
                    if (dist > r_cr) {
                        is_outside = true;
                    } else if (b_thick > 0.0f && dist >= (r_cr - b_thick)) {
                        is_border = true;
                    }
                } else if (b_thick > 0.0f) {
                    if (x < b_thick || x >= dst_w - b_thick || y < b_thick || y >= dst_h - b_thick) {
                        is_border = true;
                    }
                }
            } else {
                if (b_thick > 0.0f) {
                    if (x < b_thick || x >= dst_w - b_thick || y < b_thick || y >= dst_h - b_thick) {
                        is_border = true;
                    }
                }
            }

            if (is_outside) {
                dst_row[x] = 0x00000000;
            } else if (is_border) {
                dst_row[x] = b_col;
            } else {
                if (cfg.chroma_key) {
                    uint8_t cr = (uint8_t)((c >> 16) & 0xFF);
                    uint8_t cg = (uint8_t)((c >> 8) & 0xFF);
                    uint8_t cb = (uint8_t)(c & 0xFF);

                    uint8_t kr = (uint8_t)((cfg.chroma_color >> 16) & 0xFF);
                    uint8_t kg = (uint8_t)((cfg.chroma_color >> 8) & 0xFF);
                    uint8_t kb = (uint8_t)(cfg.chroma_color & 0xFF);

                    int dr = (int)cr - (int)kr;
                    int dg = (int)cg - (int)kg;
                    int db = (int)cb - (int)kb;
                    float dist = sqrtf((float)(dr * dr + dg * dg + db * db)) / 441.67f;

                    if (dist < cfg.chroma_similarity) {
                        c = 0x00000000;
                    }
                }
                dst_row[x] = c;
            }
        }
    }

    ID3D11DeviceContext* pCtx = (ID3D11DeviceContext*)d3d11_context;
    D3D11_BOX box;
    box.left = (UINT)dst_x;
    box.top = (UINT)dst_y;
    box.front = 0;
    box.right = (UINT)(dst_x + dst_w);
    box.bottom = (UINT)(dst_y + dst_h);
    box.back = 1;

    pCtx->UpdateSubresource((ID3D11Resource*)dest_texture, 0, &box, s_scaledBuffer, dst_w * 4, 0);

    LeaveCriticalSection(&s_frameCs);
    (void)d3d11_device;
}

void camera_engine_composite_sw_frame(uint8_t* dst_y, uint8_t* dst_uv, int stride_y, int stride_uv, int width, int height, bool is_nv12) {
    if (!dst_y || width <= 0 || height <= 0) return;

    ensure_cs();
    CameraOverlayConfig cfg;
    EnterCriticalSection(&s_configCs);
    cfg = s_config;
    LeaveCriticalSection(&s_configCs);

    if (!cfg.enabled) return;

    EnterCriticalSection(&s_frameCs);
    if (!s_camRawBuffer || s_camRawW <= 0 || s_camRawH <= 0) {
        LeaveCriticalSection(&s_frameCs);
        return;
    }

    int src_w = s_camRawW;
    int src_h = s_camRawH;
    float aspect = (float)src_w / (float)src_h;
    if (aspect <= 0.01f) aspect = 1.777f;

    int dst_w = (int)(width * cfg.size_pct);
    if (dst_w < 64) dst_w = 64;
    int dst_h = (cfg.aspect_ratio == 2) ? dst_w : (int)(dst_w / aspect);
    if (dst_h < 48) dst_h = 48;

    int margin = cfg.margin_px;
    int dst_x = width - margin - dst_w;
    int dst_y_pos = height - margin - dst_h;

    switch (cfg.position) {
        case 0: dst_x = width - margin - dst_w; dst_y_pos = height - margin - dst_h; break;
        case 1: dst_x = margin; dst_y_pos = height - margin - dst_h; break;
        case 2: dst_x = width - margin - dst_w; dst_y_pos = margin; break;
        case 3: dst_x = margin; dst_y_pos = margin; break;
        case 4: dst_x = (width - dst_w) / 2; dst_y_pos = height - margin - dst_h; break;
        case 5: dst_x = (width - dst_w) / 2; dst_y_pos = margin; break;
        case 6: dst_x = margin; dst_y_pos = (height - dst_h) / 2; break;
        case 7: dst_x = width - margin - dst_w; dst_y_pos = (height - dst_h) / 2; break;
        case 8: default:
            dst_x = (int)(cfg.pos_x_pct * (float)(width - dst_w));
            dst_y_pos = (int)(cfg.pos_y_pct * (float)(height - dst_h));
            break;
    }

    if (dst_x < 0) dst_x = 0;
    if (dst_y_pos < 0) dst_y_pos = 0;
    if (dst_x + dst_w > width) dst_w = width - dst_x;
    if (dst_y_pos + dst_h > height) dst_h = height - dst_y_pos;

    float scale_x = (float)src_w / (float)dst_w;
    float scale_y = (float)src_h / (float)dst_h;
    const uint32_t* src_px = (const uint32_t*)s_camRawBuffer;

    for (int y = 0; y < dst_h; y++) {
        int sy = (int)(y * scale_y);
        if (sy >= src_h) sy = src_h - 1;
        const uint32_t* src_row = src_px + sy * src_w;

        int ty = dst_y_pos + y;
        uint8_t* row_y = dst_y + ty * stride_y;

        for (int x = 0; x < dst_w; x++) {
            int sx = cfg.mirror ? (int)((dst_w - 1 - x) * scale_x) : (int)(x * scale_x);
            if (sx >= src_w) sx = src_w - 1;

            uint32_t c = src_row[sx];
            uint8_t r = (uint8_t)((c >> 16) & 0xFF);
            uint8_t g = (uint8_t)((c >> 8) & 0xFF);
            uint8_t b = (uint8_t)(c & 0xFF);

            int y_val = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            if (y_val < 16) y_val = 16;
            if (y_val > 235) y_val = 235;

            row_y[dst_x + x] = (uint8_t)y_val;

            if (dst_uv && (y % 2 == 0) && (x % 2 == 0)) {
                int u_val = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int v_val = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                if (u_val < 16) u_val = 16; if (u_val > 240) u_val = 240;
                if (v_val < 16) v_val = 16; if (v_val > 240) v_val = 240;

                int uv_y = ty / 2;
                int uv_x = (dst_x + x) / 2;

                if (is_nv12) {
                    uint8_t* row_uv = dst_uv + uv_y * stride_uv;
                    row_uv[uv_x * 2] = (uint8_t)u_val;
                    row_uv[uv_x * 2 + 1] = (uint8_t)v_val;
                }
            }
        }
    }

    LeaveCriticalSection(&s_frameCs);
}
