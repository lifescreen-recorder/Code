#include "wgc_bridge.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>

// GUID definitions
static const GUID IID_IInspectable = 
    { 0xAF86E2E0, 0xB12D, 0x4C6A, { 0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90 } };

static const GUID IID_IGraphicsCaptureItemInterop = 
    { 0x3628E81B, 0x3CAC, 0x4C60, { 0xB7, 0xF4, 0x23, 0xCE, 0x0E, 0x0C, 0x33, 0x56 } };

static const GUID IID_IGraphicsCaptureItem = 
    { 0x79C3F95B, 0x31F7, 0x4EC2, { 0xA4, 0x64, 0x63, 0x2E, 0xF5, 0xD3, 0x07, 0x60 } };

static const GUID IID_IDirect3D11CaptureFramePoolStatics2 = 
    { 0x589B103F, 0x6BBC, 0x5DF5, { 0xA9, 0x91, 0x02, 0xE2, 0x8B, 0x3B, 0x66, 0xD5 } };

static const GUID IID_IDirect3D11CaptureFramePool = 
    { 0x24EB6D22, 0x1975, 0x422E, { 0x82, 0xE7, 0x78, 0x0D, 0xBD, 0x8D, 0xDF, 0x24 } };

static const GUID IID_IGraphicsCaptureSession = 
    { 0x09653835, 0x263B, 0x5B26, { 0xA1, 0xD8, 0x80, 0x9C, 0x0D, 0x34, 0x87, 0x37 } };

static const GUID IID_IGraphicsCaptureSession2 = 
    { 0x2C39AE40, 0x7D2E, 0x5044, { 0x80, 0x4E, 0x8B, 0x67, 0x99, 0xD4, 0xCF, 0x9E } };

static const GUID IID_IGraphicsCaptureSession3 = 
    { 0xFA50C623, 0x38DA, 0x4B32, { 0xAC, 0xF3, 0xFA, 0x97, 0x34, 0xAD, 0x80, 0x0E } };

static const GUID IID_IDirect3D11CaptureFrame = 
    { 0xFA50C627, 0x761D, 0x4537, { 0x87, 0x78, 0x63, 0x8F, 0x8E, 0x0E, 0x16, 0x0A } };

static const GUID IID_IDirect3DDxgiInterfaceAccess = 
    { 0xA9B3D012, 0x3DF2, 0x4EE3, { 0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1 } };

static const GUID IID_ID3D11Texture2D_Local = 
    { 0x6F15AAF2, 0xD208, 0x4E89, { 0x9A, 0xB4, 0x48, 0x95, 0x35, 0xD3, 0x4F, 0x9C } };

static const GUID IID_IDXGIDevice_Local = 
    { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };

typedef struct SizeInt32 {
    int32_t Width;
    int32_t Height;
} SizeInt32;

typedef void* HSTRING;

// WinRT function pointer types
typedef HRESULT (WINAPI *pfn_RoInitialize)(int initType);
typedef void (WINAPI *pfn_RoUninitialize)(void);
typedef HRESULT (WINAPI *pfn_WindowsCreateString)(const wchar_t *sourceString, UINT32 length, HSTRING *string);
typedef HRESULT (WINAPI *pfn_WindowsDeleteString)(HSTRING string);
typedef HRESULT (WINAPI *pfn_RoGetActivationFactory)(HSTRING activatableClassId, REFIID iid, void **factory);
typedef HRESULT (WINAPI *pfn_CreateDirect3D11DeviceFromDXGIDevice)(void* dxgiDevice, void** graphicsDevice);

static pfn_RoInitialize pRoInitialize = NULL;
static pfn_RoUninitialize pRoUninitialize = NULL;
static pfn_WindowsCreateString pWindowsCreateString = NULL;
static pfn_WindowsDeleteString pWindowsDeleteString = NULL;
static pfn_RoGetActivationFactory pRoGetActivationFactory = NULL;
static pfn_CreateDirect3D11DeviceFromDXGIDevice pCreateDirect3D11DeviceFromDXGIDevice = NULL;
static bool s_winrt_initialized = false;

static bool init_winrt_functions(void) {
    if (s_winrt_initialized) return true;

    HMODULE hCombase = LoadLibraryA("combase.dll");
    if (!hCombase) return false;

    pRoInitialize = (pfn_RoInitialize)GetProcAddress(hCombase, "RoInitialize");
    pRoUninitialize = (pfn_RoUninitialize)GetProcAddress(hCombase, "RoUninitialize");
    pWindowsCreateString = (pfn_WindowsCreateString)GetProcAddress(hCombase, "WindowsCreateString");
    pWindowsDeleteString = (pfn_WindowsDeleteString)GetProcAddress(hCombase, "WindowsDeleteString");
    pRoGetActivationFactory = (pfn_RoGetActivationFactory)GetProcAddress(hCombase, "RoGetActivationFactory");

    HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
    if (!hD3D11) return false;
    pCreateDirect3D11DeviceFromDXGIDevice = (pfn_CreateDirect3D11DeviceFromDXGIDevice)GetProcAddress(hD3D11, "CreateDirect3D11DeviceFromDXGIDevice");

    if (!pRoInitialize || !pWindowsCreateString || !pWindowsDeleteString || !pRoGetActivationFactory || !pCreateDirect3D11DeviceFromDXGIDevice) {
        return false;
    }

    // Initialize Ro multithreaded (RO_INIT_MULTITHREADED = 1)
    pRoInitialize(1);
    s_winrt_initialized = true;
    return true;
}

bool wgc_is_supported(void) {
    return init_winrt_functions();
}

// Forward COM vtable declarations
typedef struct IInspectable_Local IInspectable_Local;
typedef struct IInspectableVtbl_Local {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IInspectable_Local *This, REFIID riid, void **ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(IInspectable_Local *This);
    ULONG (STDMETHODCALLTYPE *Release)(IInspectable_Local *This);
    HRESULT (STDMETHODCALLTYPE *GetIids)(IInspectable_Local *This, ULONG *iidCount, IID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(IInspectable_Local *This, HSTRING *className);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(IInspectable_Local *This, int *trustLevel);
} IInspectableVtbl_Local;

struct IInspectable_Local {
    const IInspectableVtbl_Local *lpVtbl;
};

// IGraphicsCaptureItemInterop
typedef struct IGraphicsCaptureItemInterop IGraphicsCaptureItemInterop;
typedef struct IGraphicsCaptureItemInteropVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IGraphicsCaptureItemInterop *This, REFIID riid, void **ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(IGraphicsCaptureItemInterop *This);
    ULONG (STDMETHODCALLTYPE *Release)(IGraphicsCaptureItemInterop *This);
    HRESULT (STDMETHODCALLTYPE *CreateForWindow)(IGraphicsCaptureItemInterop *This, HWND appWindow, REFIID riid, void **result);
    HRESULT (STDMETHODCALLTYPE *CreateForMonitor)(IGraphicsCaptureItemInterop *This, HMONITOR monitor, REFIID riid, void **result);
} IGraphicsCaptureItemInteropVtbl;

struct IGraphicsCaptureItemInterop {
    const IGraphicsCaptureItemInteropVtbl *lpVtbl;
};

// IGraphicsCaptureItem
typedef struct IGraphicsCaptureItem IGraphicsCaptureItem;
typedef struct IGraphicsCaptureItemVtbl {
    IInspectableVtbl_Local base;
    HRESULT (STDMETHODCALLTYPE *get_DisplayName)(IGraphicsCaptureItem *This, HSTRING *value);
    HRESULT (STDMETHODCALLTYPE *get_Size)(IGraphicsCaptureItem *This, SizeInt32 *value);
    HRESULT (STDMETHODCALLTYPE *add_Closed)(IGraphicsCaptureItem *This, void *handler, int64_t *token);
    HRESULT (STDMETHODCALLTYPE *remove_Closed)(IGraphicsCaptureItem *This, int64_t token);
} IGraphicsCaptureItemVtbl;

struct IGraphicsCaptureItem {
    const IGraphicsCaptureItemVtbl *lpVtbl;
};

// IDirect3D11CaptureFramePoolStatics2
typedef struct IDirect3D11CaptureFramePoolStatics2 IDirect3D11CaptureFramePoolStatics2;
typedef struct IDirect3D11CaptureFramePoolStatics2Vtbl {
    IInspectableVtbl_Local base;
    HRESULT (STDMETHODCALLTYPE *CreateFreeThreaded)(IDirect3D11CaptureFramePoolStatics2 *This, void *device, int32_t pixelFormat, int32_t numberOfBuffers, SizeInt32 size, void **result);
} IDirect3D11CaptureFramePoolStatics2Vtbl;

struct IDirect3D11CaptureFramePoolStatics2 {
    const IDirect3D11CaptureFramePoolStatics2Vtbl *lpVtbl;
};

// IDirect3D11CaptureFramePool
typedef struct IDirect3D11CaptureFramePool IDirect3D11CaptureFramePool;
typedef struct IDirect3D11CaptureFramePoolVtbl {
    IInspectableVtbl_Local base;
    HRESULT (STDMETHODCALLTYPE *Recreate)(IDirect3D11CaptureFramePool *This, void *device, int32_t pixelFormat, int32_t numberOfBuffers, SizeInt32 size);
    HRESULT (STDMETHODCALLTYPE *TryGetNextFrame)(IDirect3D11CaptureFramePool *This, void **result);
    HRESULT (STDMETHODCALLTYPE *add_FrameArrived)(IDirect3D11CaptureFramePool *This, void *handler, int64_t *token);
    HRESULT (STDMETHODCALLTYPE *remove_FrameArrived)(IDirect3D11CaptureFramePool *This, int64_t token);
    HRESULT (STDMETHODCALLTYPE *CreateCaptureSession)(IDirect3D11CaptureFramePool *This, IGraphicsCaptureItem *item, void **result);
} IDirect3D11CaptureFramePoolVtbl;

struct IDirect3D11CaptureFramePool {
    const IDirect3D11CaptureFramePoolVtbl *lpVtbl;
};

// IGraphicsCaptureSession
typedef struct IGraphicsCaptureSession IGraphicsCaptureSession;
typedef struct IGraphicsCaptureSessionVtbl {
    IInspectableVtbl_Local base;
    HRESULT (STDMETHODCALLTYPE *StartCapture)(IGraphicsCaptureSession *This);
} IGraphicsCaptureSessionVtbl;

struct IGraphicsCaptureSession {
    const IGraphicsCaptureSessionVtbl *lpVtbl;
};

// IGraphicsCaptureSession2
typedef struct IGraphicsCaptureSession2 IGraphicsCaptureSession2;
typedef struct IGraphicsCaptureSession2Vtbl {
    IInspectableVtbl_Local base;
    HRESULT (STDMETHODCALLTYPE *get_IsCursorCaptureEnabled)(IGraphicsCaptureSession2 *This, uint8_t *value);
    HRESULT (STDMETHODCALLTYPE *put_IsCursorCaptureEnabled)(IGraphicsCaptureSession2 *This, uint8_t value);
} IGraphicsCaptureSession2Vtbl;

struct IGraphicsCaptureSession2 {
    const IGraphicsCaptureSession2Vtbl *lpVtbl;
};

// IGraphicsCaptureSession3
typedef struct IGraphicsCaptureSession3 IGraphicsCaptureSession3;
typedef struct IGraphicsCaptureSession3Vtbl {
    IInspectableVtbl_Local base;
    HRESULT (STDMETHODCALLTYPE *get_IsBorderRequired)(IGraphicsCaptureSession3 *This, uint8_t *value);
    HRESULT (STDMETHODCALLTYPE *put_IsBorderRequired)(IGraphicsCaptureSession3 *This, uint8_t value);
} IGraphicsCaptureSession3Vtbl;

struct IGraphicsCaptureSession3 {
    const IGraphicsCaptureSession3Vtbl *lpVtbl;
};

// IDirect3D11CaptureFrame
typedef struct IDirect3D11CaptureFrame IDirect3D11CaptureFrame;
typedef struct IDirect3D11CaptureFrameVtbl {
    IInspectableVtbl_Local base;
    HRESULT (STDMETHODCALLTYPE *get_Surface)(IDirect3D11CaptureFrame *This, void **value);
    HRESULT (STDMETHODCALLTYPE *get_SystemRelativeTime)(IDirect3D11CaptureFrame *This, int64_t *value);
    HRESULT (STDMETHODCALLTYPE *get_ContentSize)(IDirect3D11CaptureFrame *This, SizeInt32 *value);
} IDirect3D11CaptureFrameVtbl;

struct IDirect3D11CaptureFrame {
    const IDirect3D11CaptureFrameVtbl *lpVtbl;
};

// IDirect3DDxgiInterfaceAccess
typedef struct IDirect3DDxgiInterfaceAccess IDirect3DDxgiInterfaceAccess;
typedef struct IDirect3DDxgiInterfaceAccessVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDirect3DDxgiInterfaceAccess *This, REFIID riid, void **ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(IDirect3DDxgiInterfaceAccess *This);
    ULONG (STDMETHODCALLTYPE *Release)(IDirect3DDxgiInterfaceAccess *This);
    HRESULT (STDMETHODCALLTYPE *GetInterface)(IDirect3DDxgiInterfaceAccess *This, REFIID riid, void **p);
} IDirect3DDxgiInterfaceAccessVtbl;

struct IDirect3DDxgiInterfaceAccess {
    const IDirect3DDxgiInterfaceAccessVtbl *lpVtbl;
};

struct WgcCapture {
    IGraphicsCaptureItem *item;
    IDirect3D11CaptureFramePool *frame_pool;
    IGraphicsCaptureSession *session;
    ID3D11Device *d3d11_device;
    ID3D11DeviceContext *d3d11_context;
    void *winrt_device;
    uint32_t width;
    uint32_t height;
};

static WgcCapture* wgc_create_internal(IGraphicsCaptureItem *item, void* d3d_device, void* d3d_context, const WgcConfig* config) {
    if (!item || !d3d_device || !d3d_context) {
        if (item) item->lpVtbl->base.Release((IInspectable_Local*)item);
        return NULL;
    }

    ID3D11Device *pDevice = (ID3D11Device*)d3d_device;
    ID3D11DeviceContext *pContext = (ID3D11DeviceContext*)d3d_context;

    // Get IDXGIDevice from ID3D11Device
    IDXGIDevice *pDxgiDevice = NULL;
    HRESULT hr = pDevice->lpVtbl->QueryInterface(pDevice, &IID_IDXGIDevice_Local, (void**)&pDxgiDevice);
    if (FAILED(hr) || !pDxgiDevice) {
        item->lpVtbl->base.Release((IInspectable_Local*)item);
        return NULL;
    }

    void *winrt_device = NULL;
    hr = pCreateDirect3D11DeviceFromDXGIDevice(pDxgiDevice, &winrt_device);
    pDxgiDevice->lpVtbl->Release(pDxgiDevice);
    if (FAILED(hr) || !winrt_device) {
        item->lpVtbl->base.Release((IInspectable_Local*)item);
        return NULL;
    }

    // Get item size
    SizeInt32 item_size = { 0, 0 };
    item->lpVtbl->get_Size(item, &item_size);
    if (item_size.Width <= 0 || item_size.Height <= 0) {
        item_size.Width = (config && config->width > 0) ? (int32_t)config->width : 1920;
        item_size.Height = (config && config->height > 0) ? (int32_t)config->height : 1080;
    }

    // Activate FramePoolStatics2
    HSTRING framePoolClass = NULL;
    const wchar_t framePoolName[] = L"Windows.Graphics.Capture.Direct3D11CaptureFramePool";
    hr = pWindowsCreateString(framePoolName, (UINT32)wcslen(framePoolName), &framePoolClass);
    if (FAILED(hr)) {
        item->lpVtbl->base.Release((IInspectable_Local*)item);
        ((IInspectable_Local*)winrt_device)->lpVtbl->Release((IInspectable_Local*)winrt_device);
        return NULL;
    }

    IDirect3D11CaptureFramePoolStatics2 *poolStatics = NULL;
    hr = pRoGetActivationFactory(framePoolClass, &IID_IDirect3D11CaptureFramePoolStatics2, (void**)&poolStatics);
    pWindowsDeleteString(framePoolClass);
    if (FAILED(hr) || !poolStatics) {
        item->lpVtbl->base.Release((IInspectable_Local*)item);
        ((IInspectable_Local*)winrt_device)->lpVtbl->Release((IInspectable_Local*)winrt_device);
        return NULL;
    }

    // Create FreeThreaded frame pool (DirectXPixelFormat_B8G8R8A8UIntNormalized = 87)
    // Use 4 buffers so WGC never stalls the application or DWM compositor!
    IDirect3D11CaptureFramePool *framePool = NULL;
    hr = poolStatics->lpVtbl->CreateFreeThreaded(poolStatics, winrt_device, 87, 4, item_size, (void**)&framePool);
    poolStatics->lpVtbl->base.Release((IInspectable_Local*)poolStatics);
    if (FAILED(hr) || !framePool) {
        fprintf(stderr, "[WGC] CreateFreeThreaded failed: 0x%08X (size: %dx%d)\n", (unsigned int)hr, (int)item_size.Width, (int)item_size.Height);
        item->lpVtbl->base.Release((IInspectable_Local*)item);
        ((IInspectable_Local*)winrt_device)->lpVtbl->Release((IInspectable_Local*)winrt_device);
        return NULL;
    }

    // Create capture session
    IGraphicsCaptureSession *session = NULL;
    hr = framePool->lpVtbl->CreateCaptureSession(framePool, item, (void**)&session);
    if (FAILED(hr) || !session) {
        fprintf(stderr, "[WGC] CreateCaptureSession failed: 0x%08X\n", (unsigned int)hr);
        framePool->lpVtbl->base.Release((IInspectable_Local*)framePool);
        item->lpVtbl->base.Release((IInspectable_Local*)item);
        ((IInspectable_Local*)winrt_device)->lpVtbl->Release((IInspectable_Local*)winrt_device);
        return NULL;
    }

    // Configure cursor if requested
    if (config) {
        IGraphicsCaptureSession2 *session2 = NULL;
        hr = session->lpVtbl->base.QueryInterface((IInspectable_Local*)session, &IID_IGraphicsCaptureSession2, (void**)&session2);
        if (SUCCEEDED(hr) && session2) {
            session2->lpVtbl->put_IsCursorCaptureEnabled(session2, config->capture_cursor ? 1 : 0);
            session2->lpVtbl->base.Release((IInspectable_Local*)session2);
        }

        // Configure yellow border (disable it by default for clean recordings)
        IGraphicsCaptureSession3 *session3 = NULL;
        hr = session->lpVtbl->base.QueryInterface((IInspectable_Local*)session, &IID_IGraphicsCaptureSession3, (void**)&session3);
        if (SUCCEEDED(hr) && session3) {
            session3->lpVtbl->put_IsBorderRequired(session3, config->draw_border ? 1 : 0);
            session3->lpVtbl->base.Release((IInspectable_Local*)session3);
        }
    }

    // Start capture session
    hr = session->lpVtbl->StartCapture(session);
    if (FAILED(hr)) {
        fprintf(stderr, "[WGC] StartCapture failed: 0x%08X\n", (unsigned int)hr);
        session->lpVtbl->base.Release((IInspectable_Local*)session);
        framePool->lpVtbl->base.Release((IInspectable_Local*)framePool);
        item->lpVtbl->base.Release((IInspectable_Local*)item);
        ((IInspectable_Local*)winrt_device)->lpVtbl->Release((IInspectable_Local*)winrt_device);
        return NULL;
    }

    WgcCapture *cap = (WgcCapture*)malloc(sizeof(WgcCapture));
    if (!cap) return NULL;

    pDevice->lpVtbl->AddRef(pDevice);
    pContext->lpVtbl->AddRef(pContext);

    cap->item = item;
    cap->frame_pool = framePool;
    cap->session = session;
    cap->d3d11_device = pDevice;
    cap->d3d11_context = pContext;
    cap->winrt_device = winrt_device;
    cap->width = (uint32_t)item_size.Width;
    cap->height = (uint32_t)item_size.Height;

    return cap;
}

WgcCapture* wgc_capture_create_window(void* hwnd, void* d3d11_device, void* d3d11_context, const WgcConfig* config) {
    if (!init_winrt_functions() || !hwnd) {
        fprintf(stderr, "[WGC] init_winrt_functions failed or hwnd is NULL (0x%p)\n", hwnd);
        return NULL;
    }
    if (pRoInitialize) pRoInitialize(1);

    // Get IGraphicsCaptureItemInterop
    HSTRING itemClass = NULL;
    const wchar_t itemName[] = L"Windows.Graphics.Capture.GraphicsCaptureItem";
    HRESULT hr = pWindowsCreateString(itemName, (UINT32)wcslen(itemName), &itemClass);
    if (FAILED(hr)) {
        fprintf(stderr, "[WGC] WindowsCreateString failed: 0x%08X\n", (unsigned int)hr);
        return NULL;
    }

    IGraphicsCaptureItemInterop *interop = NULL;
    hr = pRoGetActivationFactory(itemClass, &IID_IGraphicsCaptureItemInterop, (void**)&interop);
    pWindowsDeleteString(itemClass);
    if (FAILED(hr) || !interop) {
        fprintf(stderr, "[WGC] RoGetActivationFactory for ItemInterop failed: 0x%08X\n", (unsigned int)hr);
        return NULL;
    }

    IGraphicsCaptureItem *item = NULL;
    hr = interop->lpVtbl->CreateForWindow(interop, (HWND)hwnd, &IID_IGraphicsCaptureItem, (void**)&item);
    interop->lpVtbl->Release(interop);
    if (FAILED(hr) || !item) {
        fprintf(stderr, "[WGC] CreateForWindow failed: 0x%08X for HWND 0x%p\n", (unsigned int)hr, hwnd);
        return NULL;
    }

    return wgc_create_internal(item, d3d11_device, d3d11_context, config);
}

WgcCapture* wgc_capture_create_monitor(void* hmonitor, void* d3d11_device, void* d3d11_context, const WgcConfig* config) {
    if (!init_winrt_functions() || !hmonitor) return NULL;
    if (pRoInitialize) pRoInitialize(1);

    HSTRING itemClass = NULL;
    const wchar_t itemName[] = L"Windows.Graphics.Capture.GraphicsCaptureItem";
    HRESULT hr = pWindowsCreateString(itemName, (UINT32)wcslen(itemName), &itemClass);
    if (FAILED(hr)) return NULL;

    IGraphicsCaptureItemInterop *interop = NULL;
    hr = pRoGetActivationFactory(itemClass, &IID_IGraphicsCaptureItemInterop, (void**)&interop);
    pWindowsDeleteString(itemClass);
    if (FAILED(hr) || !interop) {
        fprintf(stderr, "[WGC] RoGetActivationFactory for ItemInterop failed: 0x%08X\n", (unsigned int)hr);
        return NULL;
    }

    IGraphicsCaptureItem *item = NULL;
    hr = interop->lpVtbl->CreateForMonitor(interop, (HMONITOR)hmonitor, &IID_IGraphicsCaptureItem, (void**)&item);
    interop->lpVtbl->Release(interop);
    if (FAILED(hr) || !item) {
        fprintf(stderr, "[WGC] CreateForMonitor failed: 0x%08X for HMONITOR 0x%p\n", (unsigned int)hr, hmonitor);
        return NULL;
    }

    return wgc_create_internal(item, d3d11_device, d3d11_context, config);
}

bool wgc_capture_acquire_frame(WgcCapture* cap, void* dest_texture, uint32_t timeout_ms, uint32_t* out_width, uint32_t* out_height) {
    if (!cap || !dest_texture) return false;

    IDirect3D11CaptureFrame *frame = NULL;
    HRESULT hr = cap->frame_pool->lpVtbl->TryGetNextFrame(cap->frame_pool, (void**)&frame);
    if (FAILED(hr) || !frame) {
        if (timeout_ms == 0) return false;

        LARGE_INTEGER freq, start, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        int64_t timeout_ticks = (int64_t)timeout_ms * freq.QuadPart / 1000;
        while (1) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart - start.QuadPart >= timeout_ticks) break;
            Sleep(0); // Yield CPU slice without sleeping 1-2 ms
            hr = cap->frame_pool->lpVtbl->TryGetNextFrame(cap->frame_pool, (void**)&frame);
            if (SUCCEEDED(hr) && frame) break;
        }
        if (!frame) return false;
    }

    // Get ContentSize
    SizeInt32 contentSize = { 0, 0 };
    frame->lpVtbl->get_ContentSize(frame, &contentSize);
    if (contentSize.Width > 0 && contentSize.Height > 0) {
        cap->width = (uint32_t)contentSize.Width;
        cap->height = (uint32_t)contentSize.Height;
    }
    if (out_width) *out_width = cap->width;
    if (out_height) *out_height = cap->height;

    // Get surface
    IInspectable_Local *surface = NULL;
    hr = frame->lpVtbl->get_Surface(frame, (void**)&surface);
    if (FAILED(hr) || !surface) {
        frame->lpVtbl->base.Release((IInspectable_Local*)frame);
        return false;
    }

    // Query IDirect3DDxgiInterfaceAccess
    IDirect3DDxgiInterfaceAccess *access = NULL;
    hr = surface->lpVtbl->QueryInterface(surface, &IID_IDirect3DDxgiInterfaceAccess, (void**)&access);
    surface->lpVtbl->Release(surface);
    if (FAILED(hr) || !access) {
        frame->lpVtbl->base.Release((IInspectable_Local*)frame);
        return false;
    }

    // Extract underlying ID3D11Texture2D
    ID3D11Texture2D *captured_tex = NULL;
    hr = access->lpVtbl->GetInterface(access, &IID_ID3D11Texture2D_Local, (void**)&captured_tex);
    access->lpVtbl->Release(access);
    if (FAILED(hr) || !captured_tex) {
        frame->lpVtbl->base.Release((IInspectable_Local*)frame);
        return false;
    }

    // Direct GPU-to-GPU copy
    cap->d3d11_context->lpVtbl->CopyResource(
        cap->d3d11_context,
        (ID3D11Resource*)dest_texture,
        (ID3D11Resource*)captured_tex
    );

    captured_tex->lpVtbl->Release(captured_tex);
    frame->lpVtbl->base.Release((IInspectable_Local*)frame);

    return true;
}

void wgc_capture_get_size(WgcCapture* cap, uint32_t* out_width, uint32_t* out_height) {
    if (!cap) return;
    if (out_width) *out_width = cap->width;
    if (out_height) *out_height = cap->height;
}

void wgc_capture_destroy(WgcCapture* cap) {
    if (!cap) return;

    if (cap->session) {
        cap->session->lpVtbl->base.Release((IInspectable_Local*)cap->session);
    }
    if (cap->frame_pool) {
        cap->frame_pool->lpVtbl->base.Release((IInspectable_Local*)cap->frame_pool);
    }
    if (cap->item) {
        cap->item->lpVtbl->base.Release((IInspectable_Local*)cap->item);
    }
    if (cap->winrt_device) {
        ((IInspectable_Local*)cap->winrt_device)->lpVtbl->Release((IInspectable_Local*)cap->winrt_device);
    }
    if (cap->d3d11_context) {
        cap->d3d11_context->lpVtbl->Release(cap->d3d11_context);
    }
    if (cap->d3d11_device) {
        cap->d3d11_device->lpVtbl->Release(cap->d3d11_device);
    }

    free(cap);
}
