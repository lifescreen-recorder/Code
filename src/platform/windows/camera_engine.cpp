#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <winhttp.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "camera_engine.h"

static const GUID LS_CLSID_WICImagingFactory1 =
    { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID LS_CLSID_WICImagingFactory2 =
    { 0x317d06e8, 0x5f24, 0x433d, { 0xbd, 0xf7, 0x79, 0xce, 0x68, 0xd8, 0xab, 0xc2 } };
static const GUID LS_IID_IWICImagingFactory =
    { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID LS_GUID_WICPixelFormat32bppBGRA =
    { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };

static bool s_mfInitialized = false;
static bool s_devicesInitialized = false;
static CameraOverlayConfig s_config = {
    false, 0, 0, 0, 0.80f, 0.75f, 24, 0.22f, 0, 1, 16.0f, 3.0f, 0x0EA5E9FF, 1.0f,
    false, false, false, 0x00FF00, 0.40f, 0.10f,
    0.0f, 1.0f, 1.0f, 0.25f, 0.25f, 0, 0.0f, false, {0}
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
static volatile LONG s_webReloadRequested = 1;
static int s_activeSourceType = -1;
static int s_activeDeviceIndex = -1;
static char s_activeWebUrl[512] = {0};

static uint8_t* s_camRawBuffer = NULL;
static int s_camRawW = 0;
static int s_camRawH = 0;
static uint64_t s_camFrameIndex = 0;

static uint8_t* s_denoiseHistory = NULL;
static int s_denoiseCap = 0;
static uint8_t* s_sharpTemp = NULL;
static int s_sharpCap = 0;

static ID3D11Texture2D* s_pPreviewTex = NULL;
static ID3D11ShaderResourceView* s_pPreviewSRV = NULL;
static int s_previewTexW = 0;
static int s_previewTexH = 0;
static uint64_t s_previewUploadedIdx = 0;

#define MAX_RTV_CACHE 8
struct GpuCompositorState {
    ID3D11Device* device;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11SamplerState* sampler;
    ID3D11BlendState* blend;
    ID3D11RasterizerState* rast;
    ID3D11Buffer* cbuf;
    ID3D11Texture2D* cam_tex;
    ID3D11ShaderResourceView* cam_srv;
    int cam_w;
    int cam_h;
    uint64_t uploaded_idx;
    ID3D11Resource* rtv_keys[MAX_RTV_CACHE];
    ID3D11RenderTargetView* rtv_vals[MAX_RTV_CACHE];
    int rtv_count;
};

static GpuCompositorState s_gpuComp = {0};

struct CompShaderConstants {
    float rect_w;
    float rect_h;
    float radius;
    float border_w;
    float border_r;
    float border_g;
    float border_b;
    float border_a;
    float opacity;
    int shape;
    int mirror_x;
    int flip_y;
    int glow;
    float pad0;
    float pad1;
    float pad2;
};

static void release_gpu_compositor(void) {
    for (int i = 0; i < s_gpuComp.rtv_count; i++) {
        if (s_gpuComp.rtv_vals[i]) s_gpuComp.rtv_vals[i]->Release();
    }
    if (s_gpuComp.cam_srv) s_gpuComp.cam_srv->Release();
    if (s_gpuComp.cam_tex) s_gpuComp.cam_tex->Release();
    if (s_gpuComp.cbuf) s_gpuComp.cbuf->Release();
    if (s_gpuComp.rast) s_gpuComp.rast->Release();
    if (s_gpuComp.blend) s_gpuComp.blend->Release();
    if (s_gpuComp.sampler) s_gpuComp.sampler->Release();
    if (s_gpuComp.ps) s_gpuComp.ps->Release();
    if (s_gpuComp.vs) s_gpuComp.vs->Release();
    memset(&s_gpuComp, 0, sizeof(s_gpuComp));
}

static bool ensure_gpu_compositor(ID3D11Device* dev) {
    if (!dev) return false;
    if (s_gpuComp.device == dev && s_gpuComp.vs && s_gpuComp.ps) return true;

    release_gpu_compositor();

    const char* vs_src =
        "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "VSOut main(uint id : SV_VertexID) {\n"
        "    VSOut o;\n"
        "    float2 uv = float2((id << 1) & 2, id & 2);\n"
        "    o.uv = float2(uv.x * 0.5, uv.y * 0.5);\n"
        "    if (id == 0) { o.pos = float4(-1.0,  1.0, 0.0, 1.0); o.uv = float2(0.0, 0.0); }\n"
        "    else if (id == 1) { o.pos = float4( 1.0,  1.0, 0.0, 1.0); o.uv = float2(1.0, 0.0); }\n"
        "    else if (id == 2) { o.pos = float4(-1.0, -1.0, 0.0, 1.0); o.uv = float2(0.0, 1.0); }\n"
        "    else { o.pos = float4( 1.0, -1.0, 0.0, 1.0); o.uv = float2(1.0, 1.0); }\n"
        "    return o;\n"
        "}\n";

    const char* ps_src =
        "cbuffer Cfg : register(b0) {\n"
        "    float rect_w; float rect_h; float radius; float border_w;\n"
        "    float4 border_col;\n"
        "    float opacity; int shape; int mirror_x; int flip_y;\n"
        "    int glow; float3 pad;\n"
        "};\n"
        "Texture2D camTex : register(t0);\n"
        "SamplerState camSmp : register(s0);\n"
        "struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "float sdRoundBox(float2 p, float2 b, float r) {\n"
        "    float2 q = abs(p) - b + r;\n"
        "    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;\n"
        "}\n"
        "float4 main(PSIn i) : SV_TARGET {\n"
        "    float2 size = float2(rect_w, rect_h);\n"
        "    float2 p = (i.uv - 0.5) * size;\n"
        "    float pad_glow = (glow != 0) ? 10.0 : 0.0;\n"
        "    float2 half_inner = max(size * 0.5 - pad_glow, float2(4.0, 4.0));\n"
        "    float sd = 0.0;\n"
        "    if (shape == 2) {\n"
        "        float rad = min(half_inner.x, half_inner.y);\n"
        "        sd = length(p) - rad;\n"
        "    } else if (shape == 1) {\n"
        "        float r = min(radius, min(half_inner.x, half_inner.y));\n"
        "        sd = sdRoundBox(p, half_inner, r);\n"
        "    } else {\n"
        "        sd = sdRoundBox(p, half_inner, 0.0);\n"
        "    }\n"
        "    if (sd > 0.6) {\n"
        "        if (glow != 0 && sd < 9.5) {\n"
        "            float ga = exp(-sd * 0.38) * 0.55 * opacity;\n"
        "            float3 gc = (border_col.a > 0.05) ? border_col.rgb : float3(0.05, 0.65, 0.92);\n"
        "            return float4(gc, ga);\n"
        "        }\n"
        "        return float4(0.0, 0.0, 0.0, 0.0);\n"
        "    }\n"
        "    float edge_aa = saturate(0.6 - sd);\n"
        "    float2 inner_uv = (p / (half_inner * 2.0)) + 0.5;\n"
        "    if (mirror_x != 0) inner_uv.x = 1.0 - inner_uv.x;\n"
        "    if (flip_y != 0) inner_uv.y = 1.0 - inner_uv.y;\n"
        "    float4 tex_c = camTex.Sample(camSmp, saturate(inner_uv));\n"
        "    float4 out_c = float4(tex_c.rgb, tex_c.a * opacity * edge_aa);\n"
        "    if (border_w > 0.1 && border_col.a > 0.01 && sd >= -border_w) {\n"
        "        float bt = saturate((sd + border_w) / 1.2);\n"
        "        out_c.rgb = lerp(out_c.rgb, border_col.rgb, bt * border_col.a);\n"
        "        out_c.a = max(out_c.a, border_col.a * opacity * edge_aa * bt);\n"
        "    }\n"
        "    return out_c;\n"
        "}\n";

    ID3DBlob* vs_blob = NULL;
    ID3DBlob* ps_blob = NULL;
    if (FAILED(D3DCompile(vs_src, strlen(vs_src), NULL, NULL, NULL, "main", "vs_4_0", 0, 0, &vs_blob, NULL)) || !vs_blob) {
        return false;
    }
    if (FAILED(D3DCompile(ps_src, strlen(ps_src), NULL, NULL, NULL, "main", "ps_4_0", 0, 0, &ps_blob, NULL)) || !ps_blob) {
        vs_blob->Release();
        return false;
    }

    HRESULT hr1 = dev->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), NULL, &s_gpuComp.vs);
    HRESULT hr2 = dev->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), NULL, &s_gpuComp.ps);
    vs_blob->Release();
    ps_blob->Release();
    if (FAILED(hr1) || FAILED(hr2)) {
        release_gpu_compositor();
        return false;
    }

    D3D11_SAMPLER_DESC sdesc = {};
    sdesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sdesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sdesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sdesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    dev->CreateSamplerState(&sdesc, &s_gpuComp.sampler);

    D3D11_BLEND_DESC bdesc = {};
    bdesc.RenderTarget[0].BlendEnable = TRUE;
    bdesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bdesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bdesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bdesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bdesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bdesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bdesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bdesc, &s_gpuComp.blend);

    D3D11_RASTERIZER_DESC rdesc = {};
    rdesc.FillMode = D3D11_FILL_SOLID;
    rdesc.CullMode = D3D11_CULL_NONE;
    dev->CreateRasterizerState(&rdesc, &s_gpuComp.rast);

    D3D11_BUFFER_DESC cdesc = {};
    cdesc.ByteWidth = sizeof(CompShaderConstants);
    cdesc.Usage = D3D11_USAGE_DEFAULT;
    cdesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&cdesc, NULL, &s_gpuComp.cbuf);

    s_gpuComp.device = dev;
    return (s_gpuComp.sampler && s_gpuComp.blend && s_gpuComp.rast && s_gpuComp.cbuf);
}

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

    snprintf(s_deviceNamesStorage[s_deviceCount], sizeof(s_deviceNamesStorage[0]), "Web / URL / File Overlay");
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
    if (!s_devicesInitialized) {
        s_devicesInitialized = true;
        camera_engine_refresh_devices();
    }
}

void camera_engine_shutdown(void) {
    camera_engine_stop();
    release_gpu_compositor();
    if (s_pPreviewSRV) { s_pPreviewSRV->Release(); s_pPreviewSRV = NULL; }
    if (s_pPreviewTex) { s_pPreviewTex->Release(); s_pPreviewTex = NULL; }
    if (s_camRawBuffer) { free(s_camRawBuffer); s_camRawBuffer = NULL; }
    if (s_denoiseHistory) { free(s_denoiseHistory); s_denoiseHistory = NULL; }
    if (s_sharpTemp) { free(s_sharpTemp); s_sharpTemp = NULL; }
    if (s_mfInitialized) {
        MFShutdown();
        CoUninitialize();
        s_mfInitialized = false;
    }
}

void camera_engine_reload_web(void) {
    InterlockedExchange(&s_webReloadRequested, 1);
}

void camera_engine_update_settings(const CameraOverlayConfig* cfg) {
    if (!cfg) return;
    ensure_cs();

    bool url_changed = false;
    EnterCriticalSection(&s_configCs);
    if (strcmp(s_config.web_url, cfg->web_url) != 0) {
        url_changed = true;
    }
    s_config = *cfg;
    LeaveCriticalSection(&s_configCs);

    if (cfg->enabled) {
        if (!s_captureRunning || s_activeSourceType != cfg->source_type || s_activeDeviceIndex != cfg->device_index || (cfg->source_type == 2 && url_changed)) {
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

static inline uint8_t clamp_u8(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void apply_camera_filters(uint8_t* pixels, int width, int height, const CameraOverlayConfig* cfg) {
    if (!pixels || width <= 0 || height <= 0 || !cfg) return;
    int total_px = width * height;
    int total_bytes = total_px * 4;

    if (cfg->denoise > 0.02f) {
        if (s_denoiseCap < total_bytes) {
            if (s_denoiseHistory) free(s_denoiseHistory);
            s_denoiseHistory = (uint8_t*)malloc(total_bytes);
            s_denoiseCap = s_denoiseHistory ? total_bytes : 0;
            if (s_denoiseHistory) memcpy(s_denoiseHistory, pixels, total_bytes);
        } else if (s_denoiseHistory) {
            int max_blend = (int)(cfg->denoise * 195.0f);
            int thresh = 12 + (int)(cfg->denoise * 28.0f);
            for (int i = 0; i < total_bytes; i += 4) {
                int db = (int)pixels[i + 0] - (int)s_denoiseHistory[i + 0];
                int dg = (int)pixels[i + 1] - (int)s_denoiseHistory[i + 1];
                int dr = (int)pixels[i + 2] - (int)s_denoiseHistory[i + 2];
                int diff = (abs(dr) + abs(dg) + abs(db)) / 3;
                if (diff < thresh) {
                    int w_hist = (max_blend * (thresh - diff)) / thresh;
                    int w_cur = 256 - w_hist;
                    pixels[i + 0] = (uint8_t)((pixels[i + 0] * w_cur + s_denoiseHistory[i + 0] * w_hist) >> 8);
                    pixels[i + 1] = (uint8_t)((pixels[i + 1] * w_cur + s_denoiseHistory[i + 1] * w_hist) >> 8);
                    pixels[i + 2] = (uint8_t)((pixels[i + 2] * w_cur + s_denoiseHistory[i + 2] * w_hist) >> 8);
                }
                s_denoiseHistory[i + 0] = pixels[i + 0];
                s_denoiseHistory[i + 1] = pixels[i + 1];
                s_denoiseHistory[i + 2] = pixels[i + 2];
                s_denoiseHistory[i + 3] = pixels[i + 3];
            }
        }
    }

    if (cfg->sharpness > 0.02f && width >= 8 && height >= 8) {
        if (s_sharpCap < total_bytes) {
            if (s_sharpTemp) free(s_sharpTemp);
            s_sharpTemp = (uint8_t*)malloc(total_bytes);
            s_sharpCap = s_sharpTemp ? total_bytes : 0;
        }
        if (s_sharpTemp) {
            memcpy(s_sharpTemp, pixels, total_bytes);
            int amount = (int)(cfg->sharpness * 180.0f);
            int stride = width * 4;
            for (int y = 1; y < height - 1; y++) {
                const uint8_t* row_up = s_sharpTemp + (y - 1) * stride;
                const uint8_t* row_c  = s_sharpTemp + y * stride;
                const uint8_t* row_dn = s_sharpTemp + (y + 1) * stride;
                uint8_t* dst = pixels + y * stride;
                for (int x = 1; x < width - 1; x++) {
                    int o = x * 4;
                    for (int c = 0; c < 3; c++) {
                        int center = row_c[o + c];
                        int blur = (row_up[o + c] + row_dn[o + c] + row_c[o - 4 + c] + row_c[o + 4 + c]) >> 2;
                        int detail = center - blur;
                        dst[o + c] = clamp_u8(center + ((detail * amount) >> 8));
                    }
                }
            }
        }
    }

    bool need_color = (fabsf(cfg->brightness) > 0.005f) ||
                      (fabsf(cfg->contrast - 1.0f) > 0.005f) ||
                      (fabsf(cfg->saturation - 1.0f) > 0.005f) ||
                      (cfg->color_filter != 0) ||
                      (cfg->vignette > 0.01f) ||
                      cfg->chroma_key;

    if (!need_color) return;

    int bri_add = (int)(cfg->brightness * 255.0f);
    int con_fp = (int)(cfg->contrast * 256.0f);
    int sat_fp = (int)(cfg->saturation * 256.0f);
    int filter_mode = cfg->color_filter;
    float vig = cfg->vignette;

    uint8_t kr = (uint8_t)((cfg->chroma_color >> 16) & 0xFF);
    uint8_t kg = (uint8_t)((cfg->chroma_color >> 8) & 0xFF);
    uint8_t kb = (uint8_t)(cfg->chroma_color & 0xFF);
    float sim = cfg->chroma_similarity;
    float smth = cfg->chroma_smoothness > 0.01f ? cfg->chroma_smoothness : 0.08f;

    float inv_hw = 2.0f / (float)width;
    float inv_hh = 2.0f / (float)height;

    for (int y = 0; y < height; y++) {
        float ny = ((float)y + 0.5f) * inv_hh - 1.0f;
        float ny2 = ny * ny;
        uint8_t* row = pixels + y * width * 4;

        for (int x = 0; x < width; x++) {
            int o = x * 4;
            int b = row[o + 0];
            int g = row[o + 1];
            int r = row[o + 2];
            int a = row[o + 3];

            if (cfg->chroma_key && a > 0) {
                int dr = r - (int)kr;
                int dg = g - (int)kg;
                int db = b - (int)kb;
                float dist = sqrtf((float)(dr * dr + dg * dg + db * db)) * 0.002264f;
                if (dist < sim) {
                    a = 0;
                } else if (dist < sim + smth) {
                    float f = (dist - sim) / smth;
                    a = (int)((float)a * f);
                }
            }

            r = (((r - 128) * con_fp) >> 8) + 128 + bri_add;
            g = (((g - 128) * con_fp) >> 8) + 128 + bri_add;
            b = (((b - 128) * con_fp) >> 8) + 128 + bri_add;

            int luma = (77 * r + 150 * g + 29 * b) >> 8;
            r = luma + (((r - luma) * sat_fp) >> 8);
            g = luma + (((g - luma) * sat_fp) >> 8);
            b = luma + (((b - luma) * sat_fp) >> 8);

            if (filter_mode == 1) {
                r += 14;
                g += 5;
                b -= 12;
            } else if (filter_mode == 2) {
                r -= 10;
                g += 6;
                b += 18;
            } else if (filter_mode == 3) {
                r = (((r - 128) * 295) >> 8) + 128;
                g = (((g - 128) * 285) >> 8) + 128;
                b = (((b - 128) * 305) >> 8) + 128;
            } else if (filter_mode == 4) {
                int bw = clamp_u8(luma);
                r = bw; g = bw; b = bw;
            } else if (filter_mode == 5) {
                int sr = (r * 101 + g * 197 + b * 48) >> 8;
                int sg = (r * 89 + g * 176 + b * 43) >> 8;
                int sb = (r * 70 + g * 137 + b * 34) >> 8;
                r = sr; g = sg; b = sb;
            }

            if (vig > 0.01f) {
                float nx = ((float)x + 0.5f) * inv_hw - 1.0f;
                float d2 = nx * nx + ny2;
                float vf = 1.0f - vig * d2 * 0.65f;
                if (vf < 0.15f) vf = 0.15f;
                if (vf < 1.0f) {
                    r = (int)((float)r * vf);
                    g = (int)((float)g * vf);
                    b = (int)((float)b * vf);
                }
            }

            row[o + 0] = clamp_u8(b);
            row[o + 1] = clamp_u8(g);
            row[o + 2] = clamp_u8(r);
            row[o + 3] = clamp_u8(a);
        }
    }
}

static void generate_test_pattern(uint8_t* buffer, int width, int height, uint64_t tick) {
    const uint32_t smpte_colors[7] = {
        0xFFC0C0C0, 0xFFC0C000, 0xFF00C0C0, 0xFF00C000, 0xFFC000C0, 0xFFC00000, 0xFF0000C0
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
                    row[x] = (0xFFU << 24) | (shade << 16) | (shade << 8) | shade;
                }
            }
        }
    }

    int wave_y = bar_h - 20 + (int)(sinf((float)(tick * 0.1f)) * 12.0f);
    if (wave_y >= 0 && wave_y < height) {
        uint32_t* row = (uint32_t*)(buffer + wave_y * (width * 4));
        for (int x = 0; x < width; x++) row[x] = 0xFF00FF00;
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
                row[px] = rec_flash ? 0xFFFF2020 : 0xFF303030;
            }
        }
    }
}

static void generate_web_status_card(uint8_t* buffer, int width, int height, uint64_t tick, bool loading) {
    for (int y = 0; y < height; y++) {
        uint32_t* row = (uint32_t*)(buffer + y * width * 4);
        int g = 18 + (y * 18) / height;
        uint32_t base = (0xE8U << 24) | (14 << 16) | (g << 8) | (g + 16);
        for (int x = 0; x < width; x++) {
            row[x] = base;
        }
    }
    int bar_y = height / 2;
    int sweep = (int)((tick * 8) % (uint64_t)width);
    for (int dy = -3; dy <= 3; dy++) {
        int py = bar_y + dy;
        if (py < 0 || py >= height) continue;
        uint32_t* row = (uint32_t*)(buffer + py * width * 4);
        for (int x = 24; x < width - 24; x++) {
            int d = abs(x - sweep);
            if (d < 48) {
                row[x] = loading ? 0xFF0EA5E9 : 0xFFF59E0B;
            } else {
                row[x] = 0xFF1E293B;
            }
        }
    }
}

static bool fetch_http_bytes(const wchar_t* wurl, uint8_t** out_data, size_t* out_size) {
    if (!wurl || !out_data || !out_size) return false;
    *out_data = NULL;
    *out_size = 0;

    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0};
    wchar_t path[1024] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1023;

    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return false;

    HINTERNET hSession = WinHttpOpen(L"LifeScreen/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 2500, 2500, 3000, 3000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD req_flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path[0] ? path : L"/", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (req_flags & WINHTTP_FLAG_SECURE) {
        DWORD sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &sec_flags, sizeof(sec_flags));
    }

    bool ok = false;
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {
        size_t cap = 65536;
        size_t len = 0;
        uint8_t* buf = (uint8_t*)malloc(cap);
        if (buf) {
            while (len < 16 * 1024 * 1024) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
                if (len + avail > cap) {
                    size_t new_cap = (len + avail) * 2;
                    uint8_t* nb = (uint8_t*)realloc(buf, new_cap);
                    if (!nb) break;
                    buf = nb;
                    cap = new_cap;
                }
                DWORD read_bytes = 0;
                if (!WinHttpReadData(hRequest, buf + len, avail, &read_bytes) || read_bytes == 0) break;
                len += read_bytes;
            }
            if (len > 0) {
                *out_data = buf;
                *out_size = len;
                ok = true;
            } else {
                free(buf);
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

struct DecodedMediaFrames {
    uint8_t* frames_bgra;
    int width;
    int height;
    int frame_count;
};

static bool decode_wic_decoder(IWICImagingFactory* pFactory, IWICBitmapDecoder* pDecoder, DecodedMediaFrames* out_media) {
    if (!pFactory || !pDecoder || !out_media) return false;
    UINT cnt = 0;
    if (FAILED(pDecoder->GetFrameCount(&cnt)) || cnt == 0) return false;
    if (cnt > 60) cnt = 60;

    IWICBitmapFrameDecode* pFirst = NULL;
    if (FAILED(pDecoder->GetFrame(0, &pFirst)) || !pFirst) return false;
    UINT w = 0, h = 0;
    pFirst->GetSize(&w, &h);
    pFirst->Release();
    if (w == 0 || h == 0 || w > 3840 || h > 2160) return false;

    size_t frame_bytes = (size_t)w * (size_t)h * 4;
    uint8_t* all_px = (uint8_t*)calloc(cnt, frame_bytes);
    if (!all_px) return false;

    UINT decoded = 0;
    for (UINT i = 0; i < cnt; i++) {
        IWICBitmapFrameDecode* pFrame = NULL;
        if (SUCCEEDED(pDecoder->GetFrame(i, &pFrame)) && pFrame) {
            IWICFormatConverter* pConv = NULL;
            if (SUCCEEDED(pFactory->CreateFormatConverter(&pConv)) && pConv) {
                if (SUCCEEDED(pConv->Initialize(pFrame, LS_GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) {
                    UINT fw = 0, fh = 0;
                    pConv->GetSize(&fw, &fh);
                    if (fw == w && fh == h) {
                        if (SUCCEEDED(pConv->CopyPixels(NULL, w * 4, (UINT)frame_bytes, all_px + decoded * frame_bytes))) {
                            decoded++;
                        }
                    } else if (decoded > 0) {
                        memcpy(all_px + decoded * frame_bytes, all_px + (decoded - 1) * frame_bytes, frame_bytes);
                        pConv->CopyPixels(NULL, fw * 4, fw * fh * 4, all_px + decoded * frame_bytes);
                        decoded++;
                    }
                }
                pConv->Release();
            }
            pFrame->Release();
        }
    }

    if (decoded == 0) {
        free(all_px);
        return false;
    }

    out_media->frames_bgra = all_px;
    out_media->width = (int)w;
    out_media->height = (int)h;
    out_media->frame_count = (int)decoded;
    return true;
}

static bool load_web_or_file_resource(const char* url_utf8, DecodedMediaFrames* out_media) {
    if (!url_utf8 || !url_utf8[0] || !out_media) return false;
    memset(out_media, 0, sizeof(*out_media));

    wchar_t wurl[1024] = {0};
    MultiByteToWideChar(CP_UTF8, 0, url_utf8, -1, wurl, 1023);

    IWICImagingFactory* pFactory = NULL;
    HRESULT hr = CoCreateInstance(LS_CLSID_WICImagingFactory2, NULL, CLSCTX_INPROC_SERVER, LS_IID_IWICImagingFactory, (void**)&pFactory);
    if (FAILED(hr) || !pFactory) {
        hr = CoCreateInstance(LS_CLSID_WICImagingFactory1, NULL, CLSCTX_INPROC_SERVER, LS_IID_IWICImagingFactory, (void**)&pFactory);
    }
    if (FAILED(hr) || !pFactory) return false;

    bool is_http = (_wcsnicmp(wurl, L"http:", 5) == 0) || (_wcsnicmp(wurl, L"https:", 6) == 0);
    bool success = false;

    if (is_http) {
        uint8_t* raw_bytes = NULL;
        size_t raw_len = 0;
        if (fetch_http_bytes(wurl, &raw_bytes, &raw_len) && raw_bytes && raw_len > 0) {
            IWICStream* pStream = NULL;
            if (SUCCEEDED(pFactory->CreateStream(&pStream)) && pStream) {
                if (SUCCEEDED(pStream->InitializeFromMemory(raw_bytes, (DWORD)raw_len))) {
                    IWICBitmapDecoder* pDec = NULL;
                    if (SUCCEEDED(pFactory->CreateDecoderFromStream(pStream, NULL, WICDecodeMetadataCacheOnDemand, &pDec)) && pDec) {
                        success = decode_wic_decoder(pFactory, pDec, out_media);
                        pDec->Release();
                    }
                }
                pStream->Release();
            }
            free(raw_bytes);
        }
    } else {
        const wchar_t* fpath = wurl;
        if (_wcsnicmp(wurl, L"file:", 5) == 0) {
            fpath = wurl + 5;
            while (*fpath == L'/' || *fpath == L'\\') fpath++;
        }
        IWICBitmapDecoder* pDec = NULL;
        if (SUCCEEDED(pFactory->CreateDecoderFromFilename(fpath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDec)) && pDec) {
            success = decode_wic_decoder(pFactory, pDec, out_media);
            pDec->Release();
        }
    }

    pFactory->Release();
    return success;
}

static void select_best_camera_mode(IMFSourceReader* pReader) {
    if (!pReader) return;
    IMFMediaType* pBest = NULL;
    uint64_t best_score = 0;

    for (DWORD idx = 0; idx < 128; idx++) {
        IMFMediaType* pNative = NULL;
        if (FAILED(pReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, idx, &pNative)) || !pNative) {
            break;
        }
        UINT32 w = 0, h = 0;
        UINT32 fps_num = 30, fps_den = 1;
        MFGetAttributeSize(pNative, MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(pNative, MF_MT_FRAME_RATE, &fps_num, &fps_den);
        uint32_t fps = (fps_den > 0) ? (fps_num / fps_den) : 30;

        if (w >= 320 && w <= 1920 && h >= 240 && h <= 1080 && fps >= 15) {
            uint64_t res_score = (w == 1280 && h == 720) ? 3000000ULL :
                                 ((w == 1920 && h == 1080) ? 2800000ULL : (uint64_t)w * (uint64_t)h);
            uint64_t fps_score = (fps >= 60) ? 600000ULL : ((fps >= 30) ? 450000ULL : (uint64_t)fps * 5000ULL);
            uint64_t score = res_score + fps_score;
            if (score > best_score) {
                best_score = score;
                if (pBest) pBest->Release();
                pBest = pNative;
                pNative = NULL;
            }
        }
        if (pNative) pNative->Release();
    }

    if (pBest) {
        pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pBest);
        pBest->Release();
    }

    IMFMediaType* pOutType = NULL;
    if (SUCCEEDED(MFCreateMediaType(&pOutType)) && pOutType) {
        pOutType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pOutType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pOutType);
        pOutType->Release();
    }
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

    if (source_type == 0 && device_index == real_cam_count) {
        source_type = 1;
    } else if (source_type == 0 && device_index == real_cam_count + 1) {
        source_type = 2;
    }

    bool use_mf_cam = (source_type == 0 && device_index >= 0 && device_index < real_cam_count && ppDevices && ppDevices[device_index]);

    IMFMediaSource* pSource = NULL;
    IMFSourceReader* pReader = NULL;

    if (use_mf_cam) {
        HRESULT hr = ppDevices[device_index]->ActivateObject(IID_IMFMediaSource, (void**)&pSource);
        if (SUCCEEDED(hr) && pSource) {
            IMFAttributes* pReaderAttrs = NULL;
            MFCreateAttributes(&pReaderAttrs, 2);
            if (pReaderAttrs) {
                pReaderAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
                pReaderAttrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
            }
            hr = MFCreateSourceReaderFromMediaSource(pSource, pReaderAttrs, &pReader);
            if (pReaderAttrs) pReaderAttrs->Release();

            if (SUCCEEDED(hr) && pReader) {
                select_best_camera_mode(pReader);
            }
        }
    }

    DecodedMediaFrames web_media = {0};
    DWORD last_web_fetch_ms = 0;
    int gif_frame_idx = 0;
    InterlockedExchange(&s_webReloadRequested, 1);

    uint64_t tick = 0;
    const int def_w = 640;
    const int def_h = 360;

    while (s_captureRunning) {
        CameraOverlayConfig cur_cfg;
        EnterCriticalSection(&s_configCs);
        cur_cfg = s_config;
        LeaveCriticalSection(&s_configCs);

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
                        UINT32 w = 640, h = 480;
                        LONG stride = 0;
                        if (SUCCEEDED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurType))) {
                            MFGetAttributeSize(pCurType, MF_MT_FRAME_SIZE, &w, &h);
                            UINT32 raw_stride = 0;
                            if (SUCCEEDED(pCurType->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride))) {
                                stride = (LONG)raw_stride;
                            }
                            pCurType->Release();
                        }
                        if (stride == 0) stride = (LONG)(w * 4);
                        LONG abs_stride = (stride < 0) ? -stride : stride;
                        if (w > 0 && h > 0 && curLen >= (DWORD)(abs_stride * (LONG)h)) {
                            EnterCriticalSection(&s_frameCs);
                            if (s_camRawW != (int)w || s_camRawH != (int)h || !s_camRawBuffer) {
                                if (s_camRawBuffer) free(s_camRawBuffer);
                                s_camRawBuffer = (uint8_t*)malloc((size_t)w * (size_t)h * 4);
                                s_camRawW = (int)w;
                                s_camRawH = (int)h;
                            }
                            if (s_camRawBuffer) {
                                for (UINT32 y = 0; y < h; y++) {
                                    UINT32 src_y = (stride < 0) ? (h - 1 - y) : y;
                                    const uint32_t* src_row = (const uint32_t*)(pData + (size_t)src_y * (size_t)abs_stride);
                                    uint32_t* dst_row = (uint32_t*)(s_camRawBuffer + (size_t)y * (size_t)w * 4);
                                    for (UINT32 x = 0; x < w; x++) {
                                        dst_row[x] = src_row[x] | 0xFF000000U;
                                    }
                                }
                                apply_camera_filters(s_camRawBuffer, (int)w, (int)h, &cur_cfg);
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
                Sleep(8);
            }
        } else if (source_type == 2) {
            DWORD now_ms = GetTickCount();
            bool is_live_snap = (strstr(cur_cfg.web_url, "shot") != NULL) || (strstr(cur_cfg.web_url, "mjpg") != NULL) || (strstr(cur_cfg.web_url, "video") != NULL);
            bool need_reload = (InterlockedExchange(&s_webReloadRequested, 0) != 0) ||
                               (is_live_snap && (now_ms - last_web_fetch_ms >= 80));

            if (need_reload && cur_cfg.web_url[0] != '\0') {
                last_web_fetch_ms = now_ms;
                DecodedMediaFrames next_media = {0};
                if (load_web_or_file_resource(cur_cfg.web_url, &next_media)) {
                    if (web_media.frames_bgra) free(web_media.frames_bgra);
                    web_media = next_media;
                    gif_frame_idx = 0;
                }
            }

            EnterCriticalSection(&s_frameCs);
            if (web_media.frames_bgra && web_media.width > 0 && web_media.height > 0) {
                int w = web_media.width;
                int h = web_media.height;
                if (s_camRawW != w || s_camRawH != h || !s_camRawBuffer) {
                    if (s_camRawBuffer) free(s_camRawBuffer);
                    s_camRawBuffer = (uint8_t*)malloc((size_t)w * (size_t)h * 4);
                    s_camRawW = w;
                    s_camRawH = h;
                }
                if (s_camRawBuffer) {
                    int f_idx = gif_frame_idx % web_media.frame_count;
                    size_t f_bytes = (size_t)w * (size_t)h * 4;
                    memcpy(s_camRawBuffer, web_media.frames_bgra + (size_t)f_idx * f_bytes, f_bytes);
                    apply_camera_filters(s_camRawBuffer, w, h, &cur_cfg);
                    s_camFrameIndex++;
                    if (web_media.frame_count > 1 && (tick % 3 == 0)) {
                        gif_frame_idx = (gif_frame_idx + 1) % web_media.frame_count;
                    }
                }
            } else {
                if (s_camRawW != def_w || s_camRawH != def_h || !s_camRawBuffer) {
                    if (s_camRawBuffer) free(s_camRawBuffer);
                    s_camRawBuffer = (uint8_t*)malloc(def_w * def_h * 4);
                    s_camRawW = def_w;
                    s_camRawH = def_h;
                }
                if (s_camRawBuffer) {
                    generate_web_status_card(s_camRawBuffer, def_w, def_h, tick, cur_cfg.web_url[0] != '\0');
                    s_camFrameIndex++;
                }
            }
            LeaveCriticalSection(&s_frameCs);
            tick++;
            Sleep(33);
        } else {
            EnterCriticalSection(&s_frameCs);
            if (s_camRawW != def_w || s_camRawH != def_h || !s_camRawBuffer) {
                if (s_camRawBuffer) free(s_camRawBuffer);
                s_camRawBuffer = (uint8_t*)malloc(def_w * def_h * 4);
                s_camRawW = def_w;
                s_camRawH = def_h;
            }
            if (s_camRawBuffer) {
                generate_test_pattern(s_camRawBuffer, def_w, def_h, tick);
                apply_camera_filters(s_camRawBuffer, def_w, def_h, &cur_cfg);
                s_camFrameIndex++;
            }
            LeaveCriticalSection(&s_frameCs);
            tick++;
            Sleep(33);
        }
    }

    if (web_media.frames_bgra) free(web_media.frames_bgra);
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
    InterlockedExchange(&s_webReloadRequested, 1);

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
        desc.Width = (UINT)s_camRawW;
        desc.Height = (UINT)s_camRawH;
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
            s_previewUploadedIdx = 0;
        }
    }

    if (s_pPreviewTex && s_pPreviewSRV) {
        if (s_previewUploadedIdx != s_camFrameIndex) {
            pCtx->UpdateSubresource((ID3D11Resource*)s_pPreviewTex, 0, NULL, s_camRawBuffer, (UINT)(s_camRawW * 4), 0);
            s_previewUploadedIdx = s_camFrameIndex;
        }
        *out_srv = (void*)s_pPreviewSRV;
        if (out_w) *out_w = s_camRawW;
        if (out_h) *out_h = s_camRawH;
    }

    LeaveCriticalSection(&s_frameCs);
}

static void compute_overlay_rect(const CameraOverlayConfig* cfg, int src_w, int src_h, int screen_w, int screen_h, int* out_x, int* out_y, int* out_w, int* out_h) {
    float aspect = (src_h > 0) ? ((float)src_w / (float)src_h) : 1.777f;
    if (cfg->aspect_ratio == 1) aspect = 4.0f / 3.0f;
    else if (cfg->aspect_ratio == 2 || cfg->shape == 2) aspect = 1.0f;
    if (aspect <= 0.05f) aspect = 1.777f;

    int dst_w = (int)((float)screen_w * cfg->size_pct);
    if (dst_w < 64) dst_w = 64;
    if (dst_w > screen_w) dst_w = screen_w;

    int dst_h = (int)((float)dst_w / aspect);
    if (dst_h < 48) dst_h = 48;
    if (dst_h > screen_h) dst_h = screen_h;

    int margin = cfg->margin_px;
    int dst_x = 0, dst_y = 0;

    switch (cfg->position) {
        case 0: dst_x = screen_w - margin - dst_w; dst_y = screen_h - margin - dst_h; break;
        case 1: dst_x = margin; dst_y = screen_h - margin - dst_h; break;
        case 2: dst_x = screen_w - margin - dst_w; dst_y = margin; break;
        case 3: dst_x = margin; dst_y = margin; break;
        case 4: dst_x = (screen_w - dst_w) / 2; dst_y = screen_h - margin - dst_h; break;
        case 5: dst_x = (screen_w - dst_w) / 2; dst_y = margin; break;
        case 6: dst_x = margin; dst_y = (screen_h - dst_h) / 2; break;
        case 7: dst_x = screen_w - margin - dst_w; dst_y = (screen_h - dst_h) / 2; break;
        case 8:
        default:
            dst_x = (int)(cfg->pos_x_pct * (float)(screen_w - dst_w));
            dst_y = (int)(cfg->pos_y_pct * (float)(screen_h - dst_h));
            break;
    }

    if (dst_x < 0) dst_x = 0;
    if (dst_y < 0) dst_y = 0;
    if (dst_x + dst_w > screen_w) dst_x = screen_w - dst_w;
    if (dst_y + dst_h > screen_h) dst_y = screen_h - dst_h;
    if (dst_x < 0) dst_x = 0;
    if (dst_y < 0) dst_y = 0;

    *out_x = dst_x;
    *out_y = dst_y;
    *out_w = dst_w;
    *out_h = dst_h;
}

void camera_engine_composite_d3d11(void* d3d11_device, void* d3d11_context, void* dest_texture, int screen_w, int screen_h) {
    (void)d3d11_device;
    (void)d3d11_context;
    if (!dest_texture || screen_w <= 0 || screen_h <= 0) return;

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

    ID3D11Texture2D* pDestTex = (ID3D11Texture2D*)dest_texture;
    ID3D11Device* pOwnerDev = NULL;
    pDestTex->GetDevice(&pOwnerDev);
    if (!pOwnerDev) {
        LeaveCriticalSection(&s_frameCs);
        return;
    }

    ID3D11DeviceContext* pOwnerCtx = NULL;
    pOwnerDev->GetImmediateContext(&pOwnerCtx);
    if (!pOwnerCtx) {
        pOwnerDev->Release();
        LeaveCriticalSection(&s_frameCs);
        return;
    }

    if (ensure_gpu_compositor(pOwnerDev)) {
        if (!s_gpuComp.cam_tex || s_gpuComp.cam_w != s_camRawW || s_gpuComp.cam_h != s_camRawH) {
            if (s_gpuComp.cam_srv) { s_gpuComp.cam_srv->Release(); s_gpuComp.cam_srv = NULL; }
            if (s_gpuComp.cam_tex) { s_gpuComp.cam_tex->Release(); s_gpuComp.cam_tex = NULL; }

            D3D11_TEXTURE2D_DESC td = {};
            td.Width = (UINT)s_camRawW;
            td.Height = (UINT)s_camRawH;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            if (SUCCEEDED(pOwnerDev->CreateTexture2D(&td, NULL, &s_gpuComp.cam_tex)) && s_gpuComp.cam_tex) {
                D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
                sd.Format = td.Format;
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                pOwnerDev->CreateShaderResourceView(s_gpuComp.cam_tex, &sd, &s_gpuComp.cam_srv);
                s_gpuComp.cam_w = s_camRawW;
                s_gpuComp.cam_h = s_camRawH;
                s_gpuComp.uploaded_idx = 0;
            }
        }

        if (s_gpuComp.cam_tex && s_gpuComp.cam_srv) {
            if (s_gpuComp.uploaded_idx != s_camFrameIndex) {
                pOwnerCtx->UpdateSubresource((ID3D11Resource*)s_gpuComp.cam_tex, 0, NULL, s_camRawBuffer, (UINT)(s_camRawW * 4), 0);
                s_gpuComp.uploaded_idx = s_camFrameIndex;
            }

            ID3D11RenderTargetView* rtv = NULL;
            for (int i = 0; i < s_gpuComp.rtv_count; i++) {
                if (s_gpuComp.rtv_keys[i] == (ID3D11Resource*)pDestTex) {
                    rtv = s_gpuComp.rtv_vals[i];
                    break;
                }
            }
            if (!rtv) {
                if (SUCCEEDED(pOwnerDev->CreateRenderTargetView((ID3D11Resource*)pDestTex, NULL, &rtv)) && rtv) {
                    if (s_gpuComp.rtv_count < MAX_RTV_CACHE) {
                        s_gpuComp.rtv_keys[s_gpuComp.rtv_count] = (ID3D11Resource*)pDestTex;
                        s_gpuComp.rtv_vals[s_gpuComp.rtv_count] = rtv;
                        s_gpuComp.rtv_count++;
                    } else {
                        if (s_gpuComp.rtv_vals[0]) s_gpuComp.rtv_vals[0]->Release();
                        s_gpuComp.rtv_keys[0] = (ID3D11Resource*)pDestTex;
                        s_gpuComp.rtv_vals[0] = rtv;
                    }
                }
            }

            if (rtv) {
                int dst_x = 0, dst_y = 0, dst_w = 0, dst_h = 0;
                compute_overlay_rect(&cfg, s_camRawW, s_camRawH, screen_w, screen_h, &dst_x, &dst_y, &dst_w, &dst_h);

                int pad_glow = cfg.glow ? 10 : 0;
                int vp_x = dst_x - pad_glow;
                int vp_y = dst_y - pad_glow;
                int vp_w = dst_w + pad_glow * 2;
                int vp_h = dst_h + pad_glow * 2;

                CompShaderConstants cb = {};
                cb.rect_w = (float)vp_w;
                cb.rect_h = (float)vp_h;
                cb.radius = cfg.corner_radius * ((float)dst_w / 320.0f);
                cb.border_w = cfg.border_thickness;
                cb.border_r = (float)((cfg.border_color >> 24) & 0xFF) / 255.0f;
                cb.border_g = (float)((cfg.border_color >> 16) & 0xFF) / 255.0f;
                cb.border_b = (float)((cfg.border_color >> 8) & 0xFF) / 255.0f;
                cb.border_a = (float)(cfg.border_color & 0xFF) / 255.0f;
                cb.opacity = cfg.opacity;
                cb.shape = cfg.shape;
                cb.mirror_x = cfg.mirror ? 1 : 0;
                cb.flip_y = cfg.flip_vertical ? 1 : 0;
                cb.glow = cfg.glow ? 1 : 0;

                pOwnerCtx->UpdateSubresource((ID3D11Resource*)s_gpuComp.cbuf, 0, NULL, &cb, 0, 0);

                D3D11_VIEWPORT vp = {};
                vp.TopLeftX = (FLOAT)vp_x;
                vp.TopLeftY = (FLOAT)vp_y;
                vp.Width = (FLOAT)vp_w;
                vp.Height = (FLOAT)vp_h;
                vp.MinDepth = 0.0f;
                vp.MaxDepth = 1.0f;

                float blend_factor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                pOwnerCtx->OMSetRenderTargets(1, &rtv, NULL);
                pOwnerCtx->OMSetBlendState(s_gpuComp.blend, blend_factor, 0xFFFFFFFF);
                pOwnerCtx->RSSetState(s_gpuComp.rast);
                pOwnerCtx->RSSetViewports(1, &vp);
                pOwnerCtx->IASetInputLayout(NULL);
                pOwnerCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                pOwnerCtx->VSSetShader(s_gpuComp.vs, NULL, 0);
                pOwnerCtx->PSSetShader(s_gpuComp.ps, NULL, 0);
                pOwnerCtx->PSSetConstantBuffers(0, 1, &s_gpuComp.cbuf);
                pOwnerCtx->PSSetShaderResources(0, 1, &s_gpuComp.cam_srv);
                pOwnerCtx->PSSetSamplers(0, 1, &s_gpuComp.sampler);

                pOwnerCtx->Draw(4, 0);

                ID3D11ShaderResourceView* null_srv = NULL;
                ID3D11RenderTargetView* null_rtv = NULL;
                pOwnerCtx->PSSetShaderResources(0, 1, &null_srv);
                pOwnerCtx->OMSetRenderTargets(1, &null_rtv, NULL);
            }
        }
    }

    pOwnerCtx->Release();
    pOwnerDev->Release();
    LeaveCriticalSection(&s_frameCs);
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
    int dst_x = 0, dst_y_pos = 0, dst_w = 0, dst_h = 0;
    compute_overlay_rect(&cfg, src_w, src_h, width, height, &dst_x, &dst_y_pos, &dst_w, &dst_h);

    float scale_x = (float)src_w / (float)dst_w;
    float scale_y = (float)src_h / (float)dst_h;
    const uint32_t* src_px = (const uint32_t*)s_camRawBuffer;

    for (int y = 0; y < dst_h; y++) {
        int sy = cfg.flip_vertical ? (int)((dst_h - 1 - y) * scale_y) : (int)(y * scale_y);
        if (sy >= src_h) sy = src_h - 1;
        const uint32_t* src_row = src_px + sy * src_w;

        int ty = dst_y_pos + y;
        if (ty < 0 || ty >= height) continue;
        uint8_t* row_y = dst_y + ty * stride_y;

        for (int x = 0; x < dst_w; x++) {
            int tx = dst_x + x;
            if (tx < 0 || tx >= width) continue;
            int sx = cfg.mirror ? (int)((dst_w - 1 - x) * scale_x) : (int)(x * scale_x);
            if (sx >= src_w) sx = src_w - 1;

            uint32_t c = src_row[sx];
            uint8_t a = (uint8_t)((c >> 24) & 0xFF);
            if (a < 16) continue;
            uint8_t r = (uint8_t)((c >> 16) & 0xFF);
            uint8_t g = (uint8_t)((c >> 8) & 0xFF);
            uint8_t b = (uint8_t)(c & 0xFF);

            int y_val = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            if (y_val < 16) y_val = 16;
            if (y_val > 235) y_val = 235;

            row_y[tx] = (uint8_t)y_val;

            if (dst_uv && (y % 2 == 0) && (x % 2 == 0)) {
                int u_val = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int v_val = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                if (u_val < 16) u_val = 16;
                if (u_val > 240) u_val = 240;
                if (v_val < 16) v_val = 16;
                if (v_val > 240) v_val = 240;

                int uv_y = ty / 2;
                int uv_x = tx / 2;

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
