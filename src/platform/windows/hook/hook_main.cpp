#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include "hook_ipc.h"

// Original function pointers
typedef HRESULT (WINAPI *Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
typedef HRESULT (WINAPI *Present1_t)(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
typedef HRESULT (WINAPI *ResizeBuffers_t)(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);

static Present_t g_OrigPresent = NULL;
static Present1_t g_OrigPresent1 = NULL;
static ResizeBuffers_t g_OrigResizeBuffers = NULL;

// IPC state
static HANDLE g_hMapFile = NULL;
static LifeScreenHookIpc* g_pIpc = NULL;
static HANDLE g_hFrameEvent = NULL;

// Shared GPU resources
static CRITICAL_SECTION g_cs;
static ID3D11Texture2D* g_pSharedTexture = NULL;
static uint32_t g_sharedWidth = 0;
static uint32_t g_sharedHeight = 0;
static DXGI_FORMAT g_sharedFormat = DXGI_FORMAT_UNKNOWN;

static void CleanupSharedTexture() {
    EnterCriticalSection(&g_cs);
    if (g_pSharedTexture) {
        g_pSharedTexture->Release();
        g_pSharedTexture = NULL;
    }
    g_sharedWidth = 0;
    g_sharedHeight = 0;
    g_sharedFormat = DXGI_FORMAT_UNKNOWN;
    if (g_pIpc) {
        g_pIpc->shared_handle = 0;
        g_pIpc->active = false;
    }
    LeaveCriticalSection(&g_cs);
}

static void InitIpc() {
    DWORD pid = GetCurrentProcessId();
    char mapName[128];
    char eventName[128];
    snprintf(mapName, sizeof(mapName), "%s%u", LIFESCREEN_IPC_MAP_PREFIX, (unsigned int)pid);
    snprintf(eventName, sizeof(eventName), "%s%u", LIFESCREEN_IPC_EVENT_PREFIX, (unsigned int)pid);

    g_hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(LifeScreenHookIpc), mapName);
    if (g_hMapFile) {
        g_pIpc = (LifeScreenHookIpc*)MapViewOfFile(g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LifeScreenHookIpc));
        if (g_pIpc) {
            ZeroMemory(g_pIpc, sizeof(LifeScreenHookIpc));
            g_pIpc->magic = LIFESCREEN_HOOK_MAGIC;
            g_pIpc->version = LIFESCREEN_HOOK_VERSION;
            g_pIpc->process_id = pid;
            g_pIpc->active = false;
            g_pIpc->recording = false;
        }
    }

    g_hFrameEvent = CreateEventA(NULL, FALSE, FALSE, eventName);
}

static void CloseIpc() {
    CleanupSharedTexture();
    if (g_pIpc) {
        UnmapViewOfFile(g_pIpc);
        g_pIpc = NULL;
    }
    if (g_hMapFile) {
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
    }
    if (g_hFrameEvent) {
        CloseHandle(g_hFrameEvent);
        g_hFrameEvent = NULL;
    }
}

static void CaptureFrame(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain || !g_pIpc) return;

    // Only copy frame if recording is requested or active
    if (!g_pIpc->recording && !g_pIpc->active) return;

    ID3D11Device* pDevice = NULL;
    HRESULT hr = pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&pDevice);
    if (FAILED(hr) || !pDevice) return;

    ID3D11DeviceContext* pContext = NULL;
    pDevice->GetImmediateContext(&pContext);
    if (!pContext) {
        pDevice->Release();
        return;
    }

    ID3D11Texture2D* pBackBuffer = NULL;
    hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
    if (SUCCEEDED(hr) && pBackBuffer) {
        D3D11_TEXTURE2D_DESC desc;
        pBackBuffer->GetDesc(&desc);

        EnterCriticalSection(&g_cs);

        // Check if shared texture needs creation/recreation
        if (!g_pSharedTexture || g_sharedWidth != desc.Width || g_sharedHeight != desc.Height || g_sharedFormat != desc.Format) {
            if (g_pSharedTexture) {
                g_pSharedTexture->Release();
                g_pSharedTexture = NULL;
            }

            D3D11_TEXTURE2D_DESC sharedDesc = desc;
            sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            sharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            sharedDesc.CPUAccessFlags = 0;
            sharedDesc.Usage = D3D11_USAGE_DEFAULT;
            sharedDesc.ArraySize = 1;
            sharedDesc.MipLevels = 1;
            sharedDesc.SampleDesc.Count = 1;
            sharedDesc.SampleDesc.Quality = 0;

            hr = pDevice->CreateTexture2D(&sharedDesc, NULL, &g_pSharedTexture);
            if (SUCCEEDED(hr) && g_pSharedTexture) {
                IDXGIResource* pResource = NULL;
                hr = g_pSharedTexture->QueryInterface(__uuidof(IDXGIResource), (void**)&pResource);
                if (SUCCEEDED(hr) && pResource) {
                    HANDLE hShared = NULL;
                    pResource->GetSharedHandle(&hShared);
                    pResource->Release();

                    g_sharedWidth = desc.Width;
                    g_sharedHeight = desc.Height;
                    g_sharedFormat = desc.Format;

                    g_pIpc->shared_handle = (uint64_t)hShared;
                    g_pIpc->width = desc.Width;
                    g_pIpc->height = desc.Height;
                    g_pIpc->format = (uint32_t)desc.Format;
                    g_pIpc->active = true;
                }
            }
        }

        // Zero-copy GPU-to-GPU copy into shared texture
        if (g_pSharedTexture) {
            pContext->CopyResource(g_pSharedTexture, pBackBuffer);
            g_pIpc->frame_index++;
            QueryPerformanceCounter((LARGE_INTEGER*)&g_pIpc->timestamp_qpc);
            if (g_hFrameEvent) {
                SetEvent(g_hFrameEvent);
            }
        }

        LeaveCriticalSection(&g_cs);
        pBackBuffer->Release();
    }

    pContext->Release();
    pDevice->Release();
}

static HRESULT WINAPI Hooked_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    CaptureFrame(pSwapChain);
    if (g_OrigPresent) {
        return g_OrigPresent(pSwapChain, SyncInterval, Flags);
    }
    return S_OK;
}

static HRESULT WINAPI Hooked_Present1(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    CaptureFrame((IDXGISwapChain*)pSwapChain);
    if (g_OrigPresent1) {
        return g_OrigPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }
    return S_OK;
}

static HRESULT WINAPI Hooked_ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    CleanupSharedTexture();
    if (g_OrigResizeBuffers) {
        return g_OrigResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    return S_OK;
}

static DWORD WINAPI HookInitThread(LPVOID lpParam) {
    (void)lpParam;
    InitIpc();

    // Create a temporary dummy window to initialize a dummy DXGI swapchain
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "LifeScreenDummyHookClass";
    RegisterClassA(&wc);

    HWND dummyHwnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
    if (!dummyHwnd) return 1;

    DXGI_SWAP_CHAIN_DESC scd = {0};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width = 64;
    scd.BufferDesc.Height = 64;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummyHwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    ID3D11Device* dummyDevice = NULL;
    ID3D11DeviceContext* dummyContext = NULL;
    IDXGISwapChain* dummySwapChain = NULL;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        NULL, 0, D3D11_SDK_VERSION, &scd,
        &dummySwapChain, &dummyDevice, &featureLevel, &dummyContext
    );

    if (FAILED(hr)) {
        // Fallback to WARP software rasterizer if hardware device creation fails
        hr = D3D11CreateDeviceAndSwapChain(
            NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
            NULL, 0, D3D11_SDK_VERSION, &scd,
            &dummySwapChain, &dummyDevice, &featureLevel, &dummyContext
        );
    }

    if (SUCCEEDED(hr) && dummySwapChain) {
        void** vtable = *(void***)dummySwapChain;
        g_OrigPresent = (Present_t)vtable[8];
        g_OrigResizeBuffers = (ResizeBuffers_t)vtable[13];

        IDXGISwapChain1* dummySwapChain1 = NULL;
        void** vtable1 = NULL;
        if (SUCCEEDED(dummySwapChain->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&dummySwapChain1))) {
            vtable1 = *(void***)dummySwapChain1;
            g_OrigPresent1 = (Present1_t)vtable1[22];
            dummySwapChain1->Release();
        }

        // Install VMT hooks on DXGI class vtables
        DWORD oldProtect;
        if (VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            vtable[8] = (void*)Hooked_Present;
            VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &oldProtect);
        }

        if (VirtualProtect(&vtable[13], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            vtable[13] = (void*)Hooked_ResizeBuffers;
            VirtualProtect(&vtable[13], sizeof(void*), oldProtect, &oldProtect);
        }

        if (vtable1 && g_OrigPresent1) {
            if (VirtualProtect(&vtable1[22], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                vtable1[22] = (void*)Hooked_Present1;
                VirtualProtect(&vtable1[22], sizeof(void*), oldProtect, &oldProtect);
            }
        }

        dummySwapChain->Release();
        dummyContext->Release();
        dummyDevice->Release();
    }

    DestroyWindow(dummyHwnd);
    UnregisterClassA("LifeScreenDummyHookClass", wc.hInstance);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            InitializeCriticalSection(&g_cs);
            CreateThread(NULL, 0, HookInitThread, NULL, 0, NULL);
            break;
        case DLL_PROCESS_DETACH:
            CloseIpc();
            DeleteCriticalSection(&g_cs);
            break;
    }
    return TRUE;
}
