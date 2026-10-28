#include "imgui_bridge.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#ifndef LIFESCREEN_VERSION
#define LIFESCREEN_VERSION "0.1.0"
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static inline const char* tr(int lang, const char* en, const char* uk, const char* zh, const char* de, const char* fr, const char* pl) {
    switch (lang) {
        case 1: return uk;
        case 2: return zh;
        case 3: return de;
        case 4: return fr;
        case 5: return pl;
        default: return en;
    }
}

static int get_monitor_refresh_rate(void) {
    DEVMODEA dm = {0};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        if (dm.dmDisplayFrequency > 0) {
            return (int)dm.dmDisplayFrequency;
        }
    }
    return 60;
}

static bool pick_folder_dialog(char* out_path, size_t max_len, HWND parent_hwnd) {
    BROWSEINFOA bi = {0};
    bi.hwndOwner = parent_hwnd;
    bi.lpszTitle = "Select Destination Folder for Recordings";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl != NULL) {
        char path[MAX_PATH] = {0};
        if (SHGetPathFromIDListA(pidl, path)) {
            strncpy(out_path, path, max_len - 1);
            out_path[max_len - 1] = '\0';
            CoTaskMemFree(pidl);
            return true;
        }
        CoTaskMemFree(pidl);
    }
    return false;
}

// Global DX11 & Window Handles
static HWND                     g_hWnd = NULL;
static ID3D11Device*            g_pd3dDevice = NULL;
static ID3D11DeviceContext*     g_pd3dDeviceContext = NULL;
static IDXGISwapChain*          g_pSwapChain = NULL;
static ID3D11RenderTargetView*  g_mainRenderTargetView = NULL;
static bool                     g_bExitRequested = false;
static bool                     g_hotkey_start_triggered = false;
static bool                     g_hotkey_pause_triggered = false;

// Overlay Window & State
static HWND                     g_hOverlayWnd = NULL;
static int                      g_overlayHours = 0;
static int                      g_overlayMins = 0;
static int                      g_overlaySecs = 0;
static int                      g_overlayFps = 60;
static int                      g_currentHotkeyPreset = -1;

// Live Screen Preview Engine State
static ID3D11Texture2D*          g_pPreviewTexture = NULL;
static ID3D11ShaderResourceView* g_pPreviewSRV = NULL;
static int                       g_previewWidth = 0;
static int                       g_previewHeight = 0;
static bool                      g_previewEnabled = false;
static bool                      g_skipCurrentFrameRender = false;
static bool                      g_previewPaused = false;
static uint64_t                  g_previewFrameCount = 0;
static CRITICAL_SECTION          g_previewCs;
static bool                      g_previewCsInit = false;

extern "C" {
void imgui_set_preview_enabled(bool enabled) {
    g_previewEnabled = enabled;
}

bool imgui_is_preview_enabled(void) {
    return g_previewEnabled;
}

void imgui_update_preview_d3d11(void* d3d11_texture, int width, int height) {
    if (!d3d11_texture || !g_previewEnabled || g_previewPaused || !g_pd3dDevice || !g_pd3dDeviceContext) return;

    // Zero-overhead when minimized or hidden (user is in full-screen or focused on a game)
    if (g_hWnd && (IsIconic(g_hWnd) || !IsWindowVisible(g_hWnd))) return;

    // Throttle preview texture copying to avoid competing with game GPU / CPU bandwidth
    static DWORD s_lastPreviewUpdate = 0;
    DWORD now = GetTickCount();
    DWORD min_interval = (g_hWnd && GetForegroundWindow() != g_hWnd) ? 66 : 33; // 15 FPS when in background, 30 FPS in foreground
    if (now - s_lastPreviewUpdate < min_interval) return;
    s_lastPreviewUpdate = now;

    if (!g_previewCsInit) {
        InitializeCriticalSection(&g_previewCs);
        g_previewCsInit = true;
    }
    EnterCriticalSection(&g_previewCs);

    ID3D11Texture2D* srcTex = (ID3D11Texture2D*)d3d11_texture;

    int req_w = (width > 0) ? width : 1920;
    int req_h = (height > 0) ? height : 1080;

    if (!g_pPreviewTexture || g_previewWidth != req_w || g_previewHeight != req_h) {
        if (g_pPreviewSRV) { g_pPreviewSRV->Release(); g_pPreviewSRV = NULL; }
        if (g_pPreviewTexture) { g_pPreviewTexture->Release(); g_pPreviewTexture = NULL; }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = (UINT)req_w;
        desc.Height = (UINT)req_h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

        HRESULT hr = g_pd3dDevice->CreateTexture2D(&desc, NULL, &g_pPreviewTexture);
        if (SUCCEEDED(hr) && g_pPreviewTexture) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = desc.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            g_pd3dDevice->CreateShaderResourceView(g_pPreviewTexture, &srvDesc, &g_pPreviewSRV);
            g_previewWidth = req_w;
            g_previewHeight = req_h;
        }
    }

    if (g_pPreviewTexture && g_pd3dDeviceContext) {
        ID3D11Device* srcDev = NULL;
        srcTex->GetDevice(&srcDev);
        if (srcDev == g_pd3dDevice) {
            g_pd3dDeviceContext->CopyResource((ID3D11Resource*)g_pPreviewTexture, (ID3D11Resource*)srcTex);
            if (srcDev) srcDev->Release();
        } else {
            if (srcDev) srcDev->Release();
            IDXGIResource* dxgiRes = NULL;
            if (SUCCEEDED(srcTex->QueryInterface(__uuidof(IDXGIResource), (void**)&dxgiRes)) && dxgiRes) {
                HANDLE sharedHandle = NULL;
                if (SUCCEEDED(dxgiRes->GetSharedHandle(&sharedHandle)) && sharedHandle) {
                    ID3D11Texture2D* openedTex = NULL;
                    if (SUCCEEDED(g_pd3dDevice->OpenSharedResource(sharedHandle, __uuidof(ID3D11Texture2D), (void**)&openedTex)) && openedTex) {
                        g_pd3dDeviceContext->CopyResource((ID3D11Resource*)g_pPreviewTexture, (ID3D11Resource*)openedTex);
                        openedTex->Release();
                    }
                }
                dxgiRes->Release();
            }
        }
        g_previewFrameCount++;
    }

    LeaveCriticalSection(&g_previewCs);
}

void imgui_update_preview_bgra(const uint8_t* bgra_data, int width, int height, int stride) {
    if (!bgra_data || !g_previewEnabled || g_previewPaused || !g_pd3dDevice || !g_pd3dDeviceContext) return;
    if (!g_previewCsInit) {
        InitializeCriticalSection(&g_previewCs);
        g_previewCsInit = true;
    }
    EnterCriticalSection(&g_previewCs);

    int req_w = (width > 0) ? width : 1920;
    int req_h = (height > 0) ? height : 1080;

    if (!g_pPreviewTexture || g_previewWidth != req_w || g_previewHeight != req_h) {
        if (g_pPreviewSRV) { g_pPreviewSRV->Release(); g_pPreviewSRV = NULL; }
        if (g_pPreviewTexture) { g_pPreviewTexture->Release(); g_pPreviewTexture = NULL; }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = (UINT)req_w;
        desc.Height = (UINT)req_h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = g_pd3dDevice->CreateTexture2D(&desc, NULL, &g_pPreviewTexture);
        if (SUCCEEDED(hr) && g_pPreviewTexture) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = desc.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            g_pd3dDevice->CreateShaderResourceView(g_pPreviewTexture, &srvDesc, &g_pPreviewSRV);
            g_previewWidth = req_w;
            g_previewHeight = req_h;
        }
    }

    if (g_pPreviewTexture && g_pd3dDeviceContext) {
        g_pd3dDeviceContext->UpdateSubresource((ID3D11Resource*)g_pPreviewTexture, 0, NULL, bgra_data, stride, 0);
        g_previewFrameCount++;
    }

    LeaveCriticalSection(&g_previewCs);
}
}

static void create_render_target(void) {
    ID3D11Texture2D* pBackBuffer = NULL;
    if (g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)) == S_OK && pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static void cleanup_render_target(void) {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = NULL;
    }
}

static void apply_hotkey_preset(int preset) {
    if (g_currentHotkeyPreset == preset && g_hWnd) return;
    if (g_hWnd) {
        UnregisterHotKey(g_hWnd, 101);
        switch (preset) {
            case 0: // F9 (Recommended for gaming)
                RegisterHotKey(g_hWnd, 101, 0, VK_F9);
                break;
            case 1: // F10
                RegisterHotKey(g_hWnd, 101, 0, VK_F10);
                break;
            case 2: // Ctrl+Shift+R
                RegisterHotKey(g_hWnd, 101, MOD_CONTROL | MOD_SHIFT, 'R');
                break;
            case 3: // Alt+F9
                RegisterHotKey(g_hWnd, 101, MOD_ALT, VK_F9);
                break;
            default:
                RegisterHotKey(g_hWnd, 101, 0, VK_F9);
                break;
        }
        g_currentHotkeyPreset = preset;
    }
}

static LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);

            HBRUSH bgBrush = CreateSolidBrush(RGB(18, 20, 24));
            HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(70, 75, 90));
            SelectObject(hdc, bgBrush);
            SelectObject(hdc, borderPen);
            RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 18, 18);

            // Pulsing bright red REC dot
            HBRUSH redBrush = CreateSolidBrush(RGB(235, 40, 40));
            SelectObject(hdc, redBrush);
            SelectObject(hdc, GetStockObject(NULL_PEN));
            Ellipse(hdc, rc.left + 12, rc.top + 8, rc.left + 24, rc.top + 20);
            DeleteObject(redBrush);

            // Text
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            HFONT hFont = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            SelectObject(hdc, hFont);

            char text[64];
            snprintf(text, sizeof(text), "REC %02d:%02d:%02d | %d FPS", g_overlayHours, g_overlayMins, g_overlaySecs, g_overlayFps);
            TextOutA(hdc, rc.left + 30, rc.top + 6, text, (int)strlen(text));

            DeleteObject(hFont);
            DeleteObject(bgBrush);
            DeleteObject(borderPen);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }

    switch (msg) {
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = 940;
            mmi->ptMinTrackSize.y = 620;
            return 0;
        }
        case WM_SIZE:
            if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED) {
                cleanup_render_target();
                g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                create_render_target();
            }
            return 0;
        case WM_HOTKEY:
            if (wParam == 101) g_hotkey_start_triggered = true;
            if (wParam == 102) g_hotkey_pause_triggered = true;
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            g_bExitRequested = true;
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void setup_modern_dark_theme(void) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 5.0f;

    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;

    style.WindowPadding     = ImVec2(16.0f, 14.0f);
    style.FramePadding      = ImVec2(10.0f, 6.0f);
    style.ItemSpacing       = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);

    // Modern Flat Obsidian / Zinc Dark Palette (Strictly flat, no gradients, clean & professional)
    colors[ImGuiCol_Text]                  = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.52f, 0.55f, 0.62f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.12f, 0.13f, 0.16f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.12f, 0.13f, 0.16f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.20f, 0.22f, 0.27f, 0.85f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.16f, 0.17f, 0.21f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.21f, 0.23f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.26f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.09f, 0.10f, 0.12f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.22f, 0.24f, 0.29f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.28f, 0.31f, 0.38f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.35f, 0.38f, 0.46f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.22f, 0.65f, 0.98f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.22f, 0.65f, 0.98f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.38f, 0.75f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.17f, 0.19f, 0.23f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.23f, 0.25f, 0.31f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.28f, 0.31f, 0.38f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.17f, 0.19f, 0.23f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.23f, 0.25f, 0.31f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.28f, 0.31f, 0.38f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.20f, 0.22f, 0.27f, 0.80f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.12f, 0.13f, 0.16f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.19f, 0.21f, 0.26f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    colors[ImGuiCol_PlotHistogram]         = ImVec4(0.16f, 0.72f, 0.40f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.22f, 0.82f, 0.48f, 1.00f);
}

bool imgui_window_create(int width, int height, const char* title) {
    WNDCLASSEXW wc = {
        sizeof(WNDCLASSEXW),
        CS_CLASSDC,
        WndProc,
        0L, 0L,
        GetModuleHandle(NULL),
        NULL, NULL, NULL, NULL,
        L"LifeScreenRecorderClass",
        NULL
    };
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    RECT wr = { 0, 0, width, height };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int win_w = wr.right - wr.left;
    int win_h = wr.bottom - wr.top;
    int pos_x = (screen_w - win_w) / 2;
    int pos_y = (screen_h - win_h) / 2;

    char full_title[128] = { 0 };
    snprintf(full_title, sizeof(full_title), "%s v" LIFESCREEN_VERSION, title ? title : "LifeScreen Recorder");
    wchar_t title_w[128] = { 0 };
    MultiByteToWideChar(CP_UTF8, 0, full_title, -1, title_w, 127);

    g_hWnd = CreateWindowW(
        wc.lpszClassName,
        title_w,
        WS_OVERLAPPEDWINDOW,
        pos_x, pos_y, win_w, win_h,
        NULL, NULL, wc.hInstance, NULL
    );

    if (!g_hWnd) return false;

    // Load custom LifeScreen Cat Icon from resources if present
    HICON hIcon = (HICON)LoadImageW(NULL, L"resources\\icon.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (!hIcon) {
        hIcon = (HICON)LoadImageW(NULL, L"..\\resources\\icon.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    }
    if (hIcon) {
        SendMessageW(g_hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessageW(g_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    }

    // Create On-Screen Recording Overlay Window (Topmost, Layered, Click-Through)
    WNDCLASSA owc = { 0 };
    owc.lpfnWndProc = OverlayWndProc;
    owc.hInstance = GetModuleHandle(NULL);
    owc.lpszClassName = "LifeScreenOverlayClass";
    RegisterClassA(&owc);

    int overlay_w = 210;
    int overlay_h = 32;
    int overlay_x = screen_w - overlay_w - 20;
    int overlay_y = 20;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

    g_hOverlayWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        "LifeScreenOverlayClass", "LifeScreen Overlay",
        WS_POPUP,
        overlay_x, overlay_y, overlay_w, overlay_h,
        NULL, NULL, owc.hInstance, NULL
    );
    if (g_hOverlayWnd) {
        SetLayeredWindowAttributes(g_hOverlayWnd, 0, 235, LWA_ALPHA);
        SetWindowDisplayAffinity(g_hOverlayWnd, WDA_EXCLUDEFROMCAPTURE);
    }

    // Direct3D 11 & SwapChain Setup
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL,
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        createDeviceFlags,
        featureLevelArray, 2,
        D3D11_SDK_VERSION,
        &sd,
        &g_pSwapChain,
        &g_pd3dDevice,
        &featureLevel,
        &g_pd3dDeviceContext
    );

    if (hr != S_OK) return false;

    create_render_target();

    // Default hotkeys: F9 (preset 0) and Ctrl+Shift+P for pause
    apply_hotkey_preset(0);
    RegisterHotKey(g_hWnd, 102, MOD_CONTROL | MOD_SHIFT, 'P');

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Load primary UI font (Latin, Extended Latin for DE/FR/PL, Cyrillic for UK)
    // ImGui 1.92+ uses dynamic on-demand glyph baking when glyph_ranges is NULL.
    const char* font_paths[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
    };

    bool font_loaded = false;
    for (int i = 0; i < 3; i++) {
        if (GetFileAttributesA(font_paths[i]) != INVALID_FILE_ATTRIBUTES) {
            if (io.Fonts->AddFontFromFileTTF(font_paths[i], 18.0f, NULL, NULL)) {
                font_loaded = true;
                break;
            }
        }
    }
    if (!font_loaded) {
        io.Fonts->AddFontDefault();
    } else {
        // Merge Simplified Chinese (CJK) fallback font so Chinese UI renders seamlessly
        const char* cjk_paths[] = {
            "C:\\Windows\\Fonts\\msyh.ttc",
            "C:\\Windows\\Fonts\\msyh.ttf",
            "C:\\Windows\\Fonts\\simsun.ttc",
            "C:\\Windows\\Fonts\\simhei.ttf",
        };
        for (int i = 0; i < 4; i++) {
            if (GetFileAttributesA(cjk_paths[i]) != INVALID_FILE_ATTRIBUTES) {
                ImFontConfig merge_cfg;
                merge_cfg.MergeMode = true;
                if (io.Fonts->AddFontFromFileTTF(cjk_paths[i], 18.0f, &merge_cfg, NULL)) {
                    break;
                }
            }
        }
    }

    setup_modern_dark_theme();

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ShowWindow(g_hWnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hWnd);

    return true;
}

bool imgui_window_poll_events(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0U, 0U, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) {
            g_bExitRequested = true;
        }
    }
    return !g_bExitRequested;
}

void imgui_window_begin_frame(void) {
    if (g_hWnd && IsIconic(g_hWnd)) {
        g_skipCurrentFrameRender = true;
        return;
    }
    if (g_hWnd && GetForegroundWindow() != g_hWnd) {
        static DWORD s_lastBgFrameTick = 0;
        DWORD now = GetTickCount();
        if (now - s_lastBgFrameTick < 150) {
            g_skipCurrentFrameRender = true;
            return;
        }
        s_lastBgFrameTick = now;
    }
    g_skipCurrentFrameRender = false;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

static void format_duration(double sec, char* buf, size_t buf_len) {
    int total_s = (int)sec;
    int h = total_s / 3600;
    int m = (total_s % 3600) / 60;
    int s = total_s % 60;
    snprintf(buf, buf_len, "%02d:%02d:%02d", h, m, s);
}

struct UiLogEntry {
    char time_str[16];
    char text[256];
    ImVec4 color;
};

#define MAX_UI_LOGS 128
static UiLogEntry g_uiLogs[MAX_UI_LOGS];
static int g_uiLogCount = 0;
static bool g_uiLogsInitialized = false;
static int g_lastUiState = -1;

static void add_ui_log(const char* msg, ImVec4 color = ImVec4(0.85f, 0.88f, 0.92f, 1.0f)) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (g_uiLogCount >= MAX_UI_LOGS) {
        memmove(&g_uiLogs[0], &g_uiLogs[1], sizeof(UiLogEntry) * (MAX_UI_LOGS - 1));
        g_uiLogCount = MAX_UI_LOGS - 1;
    }
    snprintf(g_uiLogs[g_uiLogCount].time_str, sizeof(g_uiLogs[g_uiLogCount].time_str),
             "[%02d:%02d:%02d]", st.wHour, st.wMinute, st.wSecond);
    snprintf(g_uiLogs[g_uiLogCount].text, sizeof(g_uiLogs[g_uiLogCount].text), "%s", msg);
    g_uiLogs[g_uiLogCount].color = color;
    g_uiLogCount++;
}

static void clear_session_logs(void) {
    g_uiLogCount = 0;
}

void imgui_window_render_ui(UiState* state) {
    int lang = state->language;
    bool is_recording = (state->state == 2 || state->state == 3);

    int max_hz = get_monitor_refresh_rate();
    if (max_hz < 10) max_hz = 60;
    if (state->target_fps < 10) {
        state->target_fps = 10;
        state->action_settings_changed = true;
    } else if (state->target_fps > max_hz) {
        state->target_fps = max_hz;
        state->action_settings_changed = true;
    }

    // Apply hotkey preset if changed
    apply_hotkey_preset(state->hotkey_start_stop_preset);

    // Update On-Screen Overlay window (strictly 1 Hz repaint when second changes to never interrupt game DWM flip)
    if (g_hOverlayWnd) {
        if (is_recording && state->show_overlay_indicator) {
            int total_s = (int)state->duration_sec;
            int disp_fps = (int)(state->current_fps > 0.0f ? state->current_fps : (float)state->target_fps);
            static int s_lastOverlaySec = -1;
            static int s_lastOverlayFps = -1;
            if (total_s != s_lastOverlaySec || disp_fps != s_lastOverlayFps) {
                s_lastOverlaySec = total_s;
                s_lastOverlayFps = disp_fps;
                g_overlayHours = total_s / 3600;
                g_overlayMins = (total_s % 3600) / 60;
                g_overlaySecs = total_s % 60;
                g_overlayFps = disp_fps;
                InvalidateRect(g_hOverlayWnd, NULL, FALSE);
            }
            if (!IsWindowVisible(g_hOverlayWnd)) {
                SetWindowDisplayAffinity(g_hOverlayWnd, WDA_EXCLUDEFROMCAPTURE);
                ShowWindow(g_hOverlayWnd, SW_SHOWNOACTIVATE);
            }
        } else {
            if (IsWindowVisible(g_hOverlayWnd)) {
                ShowWindow(g_hOverlayWnd, SW_HIDE);
            }
        }
    }

    if (g_skipCurrentFrameRender) {
        g_previewEnabled = false;
        return;
    }

    bool preview_tab_active = false;
    int win_w = 960, win_h = 700;
    imgui_window_get_size(&win_w, &win_h);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)win_w, (float)win_h));

    // Dynamic Activity Logger state transitions
    if (!g_uiLogsInitialized) {
        g_uiLogsInitialized = true;
        add_ui_log("LifeScreen v" LIFESCREEN_VERSION " initialized. Zero-Copy GPU Pipeline ready.", ImVec4(0.3f, 0.85f, 0.45f, 1.0f));
    }

    if (g_lastUiState != state->state) {
        if (state->state == 2 && g_lastUiState != 3) {
            add_ui_log(tr(lang,
                "[REC] Video recording started (Zero-Copy GPU Pipeline)!",
                "[REC] Запис відео розпочато (Zero-Copy GPU Pipeline)!",
                "[REC] 视频录制已开始 (Zero-Copy GPU 管线)！",
                "[REC] Videoaufnahme gestartet (Zero-Copy GPU Pipeline)!",
                "[REC] Enregistrement vidéo démarré (Pipeline GPU Zero-Copy) !",
                "[REC] Nagrywanie wideo rozpoczęte (Zero-Copy GPU Pipeline)!"),
                ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        } else if (state->state == 3) {
            add_ui_log(tr(lang,
                "[PAUSE] Recording paused.",
                "[PAUSE] Запис тимчасово призупинено.",
                "[PAUSE] 录制已暂停。",
                "[PAUSE] Aufnahme pausiert.",
                "[PAUSE] Enregistrement en pause.",
                "[PAUSE] Nagrywanie wstrzymane."),
                ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
        } else if (state->state == 2 && g_lastUiState == 3) {
            add_ui_log(tr(lang,
                "[RESUME] Recording resumed.",
                "[RESUME] Запис відновлено.",
                "[RESUME] 录制已继续。",
                "[RESUME] Aufnahme fortgesetzt.",
                "[RESUME] Enregistrement repris.",
                "[RESUME] Nagrywanie wznowione."),
                ImVec4(0.3f, 0.85f, 0.45f, 1.0f));
        } else if (state->state == 0 && (g_lastUiState == 2 || g_lastUiState == 3)) {
            char stop_msg[256];
            snprintf(stop_msg, sizeof(stop_msg),
                tr(lang,
                    "[STOP] Recording saved successfully! Size: %.1f MB",
                    "[STOP] Запис успішно збережено! Розмір: %.1f MB",
                    "[STOP] 录制已成功保存！大小: %.1f MB",
                    "[STOP] Aufnahme erfolgreich gespeichert! Größe: %.1f MB",
                    "[STOP] Enregistrement sauvegardé ! Taille : %.1f MB",
                    "[STOP] Nagranie zapisane pomyślnie! Rozmiar: %.1f MB"),
                (double)state->file_bytes / (1024.0 * 1024.0));
            add_ui_log(stop_msg, ImVec4(0.3f, 0.85f, 0.45f, 1.0f));
        }
        g_lastUiState = state->state;
    }

    ImGuiWindowFlags window_flags = 
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::Begin("LifeScreenMainWindow", NULL, window_flags);

    // Header Bar
    {
        ImGui::TextColored(ImVec4(0.22f, 0.65f, 0.98f, 1.00f), "LifeScreen");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.55f, 1.00f), "v" LIFESCREEN_VERSION);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr(lang,
            "| Zero-Copy GPU & Eco 1GB RAM Engine",
            "| Zero-Copy GPU та Eco 1ГБ ОЗУ Рушій",
            "| 零拷贝 GPU 与 1GB 内存轻量引擎",
            "| Zero-Copy GPU & Eco 1GB RAM Engine",
            "| Moteur GPU Zero-Copy & Éco 1 Go RAM",
            "| Silnik Zero-Copy GPU i Eco 1 GB RAM"));

        const char* status_badge_text = is_recording
            ? (state->state == 3
                ? tr(lang, "[PAUSED]", "[ПАУЗА]", "[已暂停]", "[PAUSIERT]", "[PAUSE]", "[PAUZA]")
                : tr(lang, "[RECORDING]", "[ЗАПИС ТРИВАЄ]", "[正在录制]", "[AUFNAHME]", "[ENREGISTREMENT]", "[NAGRYWANIE]"))
            : tr(lang, "[READY]", "[ГОТОВИЙ]", "[就绪]", "[BEREIT]", "[PRÊT]", "[GOTOWY]");
        ImVec4 status_badge_col = is_recording
            ? (state->state == 3 ? ImVec4(0.96f, 0.70f, 0.15f, 1.0f) : ImVec4(0.96f, 0.25f, 0.30f, 1.0f))
            : ImVec4(0.22f, 0.80f, 0.45f, 1.0f);

        ImVec2 badge_size = ImGui::CalcTextSize(status_badge_text);
        ImGui::SameLine((float)win_w - badge_size.x - 22.0f);
        ImGui::TextColored(status_badge_col, "%s", status_badge_text);
    }

    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("MainTabBar")) {
        // TAB 1: MAIN RECORDER DASHBOARD
        if (ImGui::BeginTabItem(tr(lang, "  Recorder  ", "  Головна  ", "  录制主页  ", "  Aufnahme  ", "  Enregistreur  ", "  Nagrywanie  "))) {
            ImGui::Spacing();

            float total_w = ImGui::GetContentRegionAvail().x;
            float avail_h = ImGui::GetContentRegionAvail().y;

            // Compute proportional heights to fill 100% of vertical space with ZERO scrollbars!
            float spacing_y = ImGui::GetStyle().ItemSpacing.y;
            float total_gaps = spacing_y * 2.0f + 6.0f;
            float usable_h = (avail_h > total_gaps + 400.0f) ? (avail_h - total_gaps) : 440.0f;

            // 3 Rows:
            // Row 1 (Source & Audio): 40% of height (min 200px)
            // Row 2 (Quick Settings): 28% of height (min 135px)
            // Row 3 (Action Deck): remaining 32% of height (min 165px)
            float h_row1 = usable_h * 0.40f;
            if (h_row1 < 200.0f) h_row1 = 200.0f;

            float h_row2 = usable_h * 0.28f;
            if (h_row2 < 135.0f) h_row2 = 135.0f;

            float h_row3 = usable_h - h_row1 - h_row2;
            if (h_row3 < 165.0f) h_row3 = 165.0f;

            float col_w = (total_w - 16.0f) * 0.5f;

            // ==========================================
            // TOP ROW: [1. VIDEO SOURCE] and [2. AUDIO & MICROPHONE]
            // ==========================================

            // LEFT CARD: 1. Video Source
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.15f, 0.18f, 1.00f));
            ImGui::BeginChild("SourceCard", ImVec2(col_w, h_row1), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            {
                ImGui::TextColored(ImVec4(0.20f, 0.65f, 1.00f, 1.00f), "%s",
                    tr(lang,
                        "1. VIDEO SOURCE & DISPLAY",
                        "1. ДЖЕРЕЛО ВІДЕО ТА ДИСПЛЕЙ",
                        "1. 视频源与显示器",
                        "1. VIDEOQUELLE & BILDSCHIRM",
                        "1. SOURCE VIDÉO & ÉCRAN",
                        "1. ŹRÓDŁO WIDEO I EKRAN"));
                ImGui::Spacing();
                if (h_row1 > 260.0f) ImGui::Spacing();

                bool is_screen = (state->capture_type == 0);
                bool is_window = (state->capture_type == 1);
                bool is_game   = (state->capture_type == 2);

                float btn_w = (col_w - 38.0f) / 3.0f;
                float btn_h = (h_row1 > 260.0f) ? 36.0f : 30.0f;
                if (is_screen) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.53f, 0.90f, 1.00f));
                if (ImGui::Button(tr(lang, "Screen", "Екран", "屏幕", "Bildschirm", "Écran", "Ekran"), ImVec2(btn_w, btn_h))) state->capture_type = 0;
                if (is_screen) ImGui::PopStyleColor();

                ImGui::SameLine();
                if (is_window) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.53f, 0.90f, 1.00f));
                if (ImGui::Button(tr(lang, "Window", "Вікно", "窗口", "Fenster", "Fenêtre", "Okno"), ImVec2(btn_w, btn_h))) state->capture_type = 1;
                if (is_window) ImGui::PopStyleColor();

                ImGui::SameLine();
                if (is_game) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.53f, 0.90f, 1.00f));
                if (ImGui::Button(tr(lang, "Game", "Гра", "游戏", "Spiel", "Jeu", "Gra"), ImVec2(btn_w, btn_h))) state->capture_type = 2;
                if (is_game) ImGui::PopStyleColor();

                ImGui::Spacing();

                if (state->capture_type == 0) {
                    ImGui::TextDisabled("%s", tr(lang, "Display / Monitor:", "Монітор / Дисплей:", "显示器 / 屏幕:", "Monitor / Anzeige:", "Moniteur / Écran :", "Monitor / Wyświetlacz:"));
                    const char* current_mon = (state->monitor_count > 0 && state->selected_monitor < state->monitor_count)
                        ? state->monitor_names[state->selected_monitor] : "Default Display";
                    ImGui::SetNextItemWidth(col_w - 28.0f);
                    if (ImGui::BeginCombo("##MonitorSelect", current_mon)) {
                        for (int i = 0; i < state->monitor_count; i++) {
                            bool is_selected = (state->selected_monitor == i);
                            if (ImGui::Selectable(state->monitor_names[i], is_selected)) {
                                state->selected_monitor = i;
                            }
                        }
                        ImGui::EndCombo();
                    }
                } else if (state->capture_type == 1) {
                    ImGui::TextDisabled("%s", tr(lang, "Target Window:", "Цільове вікно:", "目标窗口:", "Zielfenster:", "Fenêtre cible :", "Docelowe okno:"));
                    const char* current_win = (state->window_count > 0 && state->selected_window < state->window_count)
                        ? state->window_names[state->selected_window]
                        : tr(lang, "Select window...", "Виберіть вікно...", "选择窗口...", "Fenster wählen...", "Choisir une fenêtre...", "Wybierz okno...");
                    ImGui::SetNextItemWidth(col_w - 110.0f);
                    if (ImGui::BeginCombo("##WindowSelect", current_win)) {
                        for (int i = 0; i < state->window_count; i++) {
                            bool is_selected = (state->selected_window == i);
                            if (ImGui::Selectable(state->window_names[i], is_selected)) {
                                state->selected_window = i;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(tr(lang, "Refresh", "Оновити", "刷新", "Aktualis.", "Actualiser", "Odśwież"), ImVec2(74, 26))) {
                        state->action_refresh_windows_requested = true;
                    }
                } else {
                    ImGui::TextDisabled("%s", tr(lang, "Game Window / App:", "Вікно гри / Додаток:", "游戏窗口 / 应用:", "Spielfenster / App:", "Fenêtre du jeu / App :", "Okno gry / Aplikacja:"));
                    const char* current_win = (state->window_count > 0 && state->selected_window < state->window_count)
                        ? state->window_names[state->selected_window]
                        : tr(lang, "Select game window...", "Виберіть гру зі списку...", "从列表中选择游戏...", "Spielfenster wählen...", "Choisir le jeu...", "Wybierz okno gry...");
                    ImGui::SetNextItemWidth(col_w - 110.0f);
                    if (ImGui::BeginCombo("##GameSelect", current_win)) {
                        for (int i = 0; i < state->window_count; i++) {
                            bool is_selected = (state->selected_window == i);
                            if (ImGui::Selectable(state->window_names[i], is_selected)) {
                                state->selected_window = i;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(tr(lang, "Refresh", "Оновити", "刷新", "Aktualis.", "Actualiser", "Odśwież"), ImVec2(74, 26))) {
                        state->action_refresh_windows_requested = true;
                    }
                }

                ImGui::Spacing();
                if (h_row1 > 260.0f) ImGui::Spacing();

                if (ImGui::Checkbox(tr(lang, "Capture Cursor", "Курсор миші", "捕获鼠标指针", "Mauszeiger", "Curseur souris", "Kursor myszy"), &state->capture_cursor)) {
                    state->action_settings_changed = true;
                }
                ImGui::SameLine(col_w * 0.48f);
                if (ImGui::Checkbox(tr(lang, "Overlay (F9)", "Оверлей (F9)", "悬浮窗 (F9)", "Overlay (F9)", "Overlay (F9)", "Nakładka (F9)"), &state->show_overlay_indicator)) {
                    state->action_settings_changed = true;
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::SameLine();

            // RIGHT CARD: 2. Audio & Sound Controls
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.15f, 0.18f, 1.00f));
            ImGui::BeginChild("AudioCard", ImVec2(col_w, h_row1), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            {
                ImGui::TextColored(ImVec4(0.20f, 0.65f, 1.00f, 1.00f), "%s",
                    tr(lang,
                        "2. AUDIO & MICROPHONE",
                        "2. АУДІО ТА МІКРОФОН",
                        "2. 系统音频与麦克风",
                        "2. AUDIO & MIKROFON",
                        "2. AUDIO & MICROPHONE",
                        "2. DŹWIĘK I MIKROFON"));
                ImGui::Spacing();
                if (h_row1 > 260.0f) ImGui::Spacing();

                // Channel 1: System Audio Checkbox & Device Dropdown
                if (ImGui::Checkbox(tr(lang, "System Audio", "Звук системи", "系统声音", "Systemton", "Son système", "Dźwięk systemu"), &state->record_system_audio)) {
                    state->action_settings_changed = true;
                }
                ImGui::SameLine();
                float cur_x1 = ImGui::GetCursorPosX();
                float dev_w1 = col_w - cur_x1 - 14.0f;
                if (dev_w1 < 120.0f) dev_w1 = 120.0f;
                ImGui::SetNextItemWidth(dev_w1);
                const char* cur_out = (state->audio_output_count > 0 && state->selected_audio_output < state->audio_output_count)
                    ? state->audio_output_names[state->selected_audio_output] : "Default Device";
                if (ImGui::BeginCombo("##AudioOutDev", cur_out)) {
                    for (int i = 0; i < state->audio_output_count; i++) {
                        bool is_sel = (state->selected_audio_output == i);
                        if (ImGui::Selectable(state->audio_output_names[i], is_sel)) {
                            state->selected_audio_output = i;
                            state->action_settings_changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }

                // Volume slider + VU Meter
                ImGui::BeginDisabled(!state->record_system_audio);
                float meter_w = 90.0f;
                float slider_w = col_w - 28.0f - meter_w - 8.0f;
                if (slider_w < 100.0f) slider_w = 100.0f;
                ImGui::SetNextItemWidth(slider_w);
                int sys_pct = (int)(state->system_volume * 100.0f + 0.5f);
                char sys_vol_label[48];
                snprintf(sys_vol_label, sizeof(sys_vol_label), "%s: %%d%%%%",
                    tr(lang, "Volume", "Гучність", "音量", "Lautstärke", "Volume", "Głośność"));
                if (ImGui::SliderInt("##SysVol", &sys_pct, 0, 150, sys_vol_label)) {
                    state->system_volume = (float)sys_pct / 100.0f;
                    state->action_settings_changed = true;
                }
                ImGui::SameLine();
                ImGui::ProgressBar(state->record_system_audio ? state->sys_audio_level : 0.0f, ImVec2(meter_w, 20), "");
                ImGui::EndDisabled();

                ImGui::Spacing();
                if (h_row1 > 260.0f) ImGui::Spacing();

                // Channel 2: Microphone Checkbox & Device Dropdown
                if (ImGui::Checkbox(tr(lang, "Microphone", "Мікрофон", "麦克风", "Mikrofon", "Microphone", "Mikrofon"), &state->record_microphone)) {
                    state->action_settings_changed = true;
                }
                ImGui::SameLine();
                float cur_x2 = ImGui::GetCursorPosX();
                float dev_w2 = col_w - cur_x2 - 14.0f;
                if (dev_w2 < 120.0f) dev_w2 = 120.0f;
                ImGui::SetNextItemWidth(dev_w2);
                const char* cur_in = (state->audio_input_count > 0 && state->selected_audio_input < state->audio_input_count)
                    ? state->audio_input_names[state->selected_audio_input] : "Default Microphone";
                if (ImGui::BeginCombo("##AudioInDev", cur_in)) {
                    for (int i = 0; i < state->audio_input_count; i++) {
                        bool is_sel = (state->selected_audio_input == i);
                        if (ImGui::Selectable(state->audio_input_names[i], is_sel)) {
                            state->selected_audio_input = i;
                            state->action_settings_changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }

                // Mic Volume slider + VU Meter
                ImGui::BeginDisabled(!state->record_microphone);
                ImGui::SetNextItemWidth(slider_w);
                int mic_pct = (int)(state->mic_volume * 100.0f + 0.5f);
                char mic_vol_label[48];
                snprintf(mic_vol_label, sizeof(mic_vol_label), "%s: %%d%%%%",
                    tr(lang, "Volume", "Гучність", "音量", "Lautstärke", "Volume", "Głośność"));
                if (ImGui::SliderInt("##MicVol", &mic_pct, 0, 200, mic_vol_label)) {
                    state->mic_volume = (float)mic_pct / 100.0f;
                    state->action_settings_changed = true;
                }
                ImGui::SameLine();
                ImGui::ProgressBar(state->record_microphone ? state->mic_audio_level : 0.0f, ImVec2(meter_w, 20), "");
                ImGui::EndDisabled();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::Spacing();

            // ==========================================
            // MIDDLE ROW: [3. OUTPUT & STORAGE]
            // ==========================================
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.15f, 0.18f, 1.00f));
            ImGui::BeginChild("QuickSettingsCard", ImVec2(total_w, h_row2), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            {
                ImGui::TextColored(ImVec4(0.20f, 0.65f, 1.00f, 1.00f), "%s",
                    tr(lang,
                        "3. OUTPUT & STORAGE",
                        "3. ПАРАМЕТРИ ФАЙЛУ ТА ЗБЕРЕЖЕННЯ",
                        "3. 输出参数与保存位置",
                        "3. AUSGABE & SPEICHERORT",
                        "3. SORTIE & STOCKAGE",
                        "3. PARAMETRY PLIKU I ZAPIS"));
                ImGui::Spacing();
                if (h_row2 > 160.0f) ImGui::Spacing();

                float card_w = ImGui::GetContentRegionAvail().x;
                float spacing_x = ImGui::GetStyle().ItemSpacing.x;
                float col3_w = (card_w - (spacing_x * 2.0f)) / 3.0f;

                // Column 1: Format
                ImGui::TextDisabled("%s", tr(lang, "Format:", "Формат:", "格式:", "Format:", "Format :", "Format:"));
                ImGui::SameLine();
                const char* quick_formats[] = { "MKV (Crash-Resistant)", "MP4 (Universal)", "MOV" };
                ImGui::SetNextItemWidth(col3_w - 65.0f);
                if (ImGui::Combo("##QuickFormat", &state->container_format, quick_formats, 3)) {
                    state->action_settings_changed = true;
                }

                // Column 2: Resolution
                ImGui::SameLine(col3_w + spacing_x);
                ImGui::TextDisabled("%s", tr(lang, "Quality:", "Якість:", "画质:", "Qualität:", "Qualité :", "Jakość:"));
                ImGui::SameLine();
                const char* quick_res[] = {
                    tr(lang, "Native", "Рідна (Екран)", "原生 (屏幕)", "Nativ", "Natif", "Natywna"),
                    "1080p (Full HD)",
                    "720p (HD)",
                    "480p (SD)"
                };
                ImGui::SetNextItemWidth(col3_w - 65.0f);
                if (ImGui::Combo("##QuickRes", &state->output_resolution, quick_res, 4)) {
                    state->action_settings_changed = true;
                }

                // Column 3: FPS Slider (from 10 to Monitor Refresh Rate) + 1-Click Eco 30 FPS Mode
                ImGui::SameLine((col3_w + spacing_x) * 2.0f);
                ImGui::TextDisabled("FPS:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(col3_w * 0.52f);
                if (ImGui::SliderInt("##QuickFps", &state->target_fps, 10, max_hz, "%d FPS")) {
                    if (state->target_fps < 10) state->target_fps = 10;
                    if (state->target_fps > max_hz) state->target_fps = max_hz;
                    state->action_settings_changed = true;
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.48f, 0.30f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.60f, 0.38f, 1.0f));
                if (ImGui::Button("Eco 30FPS", ImVec2(col3_w * 0.32f, 24))) {
                    state->target_fps = (30 <= max_hz) ? 30 : max_hz;
                    state->encoder_preset = 0; // Ultrafast
                    state->rate_control = 2;   // CQP
                    state->quality_cqp = 23;
                    state->action_settings_changed = true;
                }
                ImGui::PopStyleColor(2);

                ImGui::Spacing();
                if (h_row2 > 160.0f) ImGui::Spacing();

                // Destination Folder Row
                ImGui::TextDisabled("%s", tr(lang, "Folder:", "Папка:", "目录:", "Ordner:", "Dossier :", "Folder:"));
                ImGui::SameLine();

                float btn1_w = 96.0f;
                float btn2_w = 88.0f;
                float remaining_row_w = ImGui::GetContentRegionAvail().x;
                float input_w = remaining_row_w - btn1_w - btn2_w - (spacing_x * 2.0f);
                if (input_w < 100.0f) input_w = 100.0f;

                ImGui::SetNextItemWidth(input_w);
                ImGui::InputText("##QuickOutputDir", state->output_dir, sizeof(state->output_dir));
                ImGui::SameLine();
                if (ImGui::Button(tr(lang, "Browse...", "Огляд...", "浏览...", "Durchs...", "Parcourir...", "Przeglądaj..."), ImVec2(btn1_w, 26))) {
                    if (pick_folder_dialog(state->output_dir, sizeof(state->output_dir), g_hWnd)) {
                        state->action_settings_changed = true;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(tr(lang, "Open", "Відкрити", "打开", "Öffnen", "Ouvrir", "Otwórz"), ImVec2(btn2_w, 26))) {
                    state->action_open_folder_requested = true;
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::Spacing();

            // ==========================================
            // LOWER-MIDDLE ROW: [4. ACTION DECK & TELEMETRY]
            // ==========================================
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.13f, 0.15f, 1.00f));
            ImGui::BeginChild("ActionCard", ImVec2(total_w, h_row3), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            {
                ImGui::Spacing();
                if (h_row3 > 200.0f) ImGui::Spacing();

                // Big Centered Action Buttons
                float btn_h = (h_row3 > 220.0f) ? 54.0f : 46.0f;
                if (!is_recording) {
                    ImGui::SetCursorPosX((total_w - 420.0f) * 0.5f);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.88f, 0.14f, 0.24f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.96f, 0.22f, 0.32f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.74f, 0.10f, 0.18f, 1.00f));
                    if (ImGui::Button(
                        tr(lang,
                            "START RECORDING (F9)",
                            "ПОЧАТИ ЗАПИС (F9)",
                            "开始录制 (F9)",
                            "AUFNAHME STARTEN (F9)",
                            "DÉMARRER L'ENREGISTREMENT (F9)",
                            "ROZPOCZNIJ NAGRYWANIE (F9)"),
                        ImVec2(420, btn_h))) {
                        state->action_start_requested = true;
                    }
                    ImGui::PopStyleColor(3);
                } else {
                    ImGui::SetCursorPosX((total_w - 470.0f) * 0.5f);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.24f, 0.29f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.31f, 0.38f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.20f, 0.24f, 1.00f));
                    if (ImGui::Button(
                        tr(lang, "STOP (F9)", "ЗУПИНИТИ (F9)", "停止录制 (F9)", "STOPP (F9)", "ARRÊTER (F9)", "ZATRZYMAJ (F9)"),
                        ImVec2(270, btn_h))) {
                        state->action_stop_requested = true;
                    }
                    ImGui::PopStyleColor(3);

                    ImGui::SameLine();
                    bool is_paused = (state->state == 3);
                    ImGui::PushStyleColor(ImGuiCol_Button, is_paused ? ImVec4(0.16f, 0.62f, 0.35f, 1.0f) : ImVec4(0.22f, 0.26f, 0.34f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, is_paused ? ImVec4(0.20f, 0.70f, 0.40f, 1.0f) : ImVec4(0.28f, 0.32f, 0.42f, 1.0f));
                    if (ImGui::Button(
                        is_paused
                            ? tr(lang, "RESUME", "ПРОДОВЖИТИ", "继续", "FORTSETZEN", "REPRENDRE", "WZNÓW")
                            : tr(lang, "PAUSE", "ПАУЗА", "暂停", "PAUSE", "PAUSE", "PAUZA"),
                        ImVec2(190, btn_h))) {
                        state->action_pause_toggle_requested = true;
                    }
                    ImGui::PopStyleColor(2);
                }

                ImGui::Spacing();

                // Big Centered Timer & Status
                char time_buf[32];
                format_duration(state->duration_sec, time_buf, sizeof(time_buf));
                char timer_text[128];
                snprintf(timer_text, sizeof(timer_text), "%s   |   %s", time_buf,
                    is_recording
                        ? (state->state == 3
                            ? tr(lang, "[PAUSED]", "[ПАУЗА]", "[已暂停]", "[PAUSIERT]", "[PAUSE]", "[PAUZA]")
                            : tr(lang, "[RECORDING]", "[ЗАПИС ТРИВАЄ]", "[正在录制]", "[AUFNAHME LÄUFT]", "[ENREGISTREMENT]", "[NAGRYWANIE]"))
                        : tr(lang, "[READY]", "[ГОТОВИЙ ДО ЗАПИСУ]", "[准备就绪]", "[AUFNAHMEBEREIT]", "[PRÊT À ENREGISTRER]", "[GOTOWY DO NAGRYWANIA]"));
                ImVec2 t_sz = ImGui::CalcTextSize(timer_text);
                ImGui::SetCursorPosX((total_w - t_sz.x) * 0.5f);
                ImGui::TextColored(is_recording ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f) : ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "%s", timer_text);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Live Telemetry Tiles (4 distinct stylish badges)
                float tile_w = (total_w - 48.0f) / 4.0f;
                float tile_h = (h_row3 > 220.0f) ? 40.0f : 32.0f;
                double mb = (double)state->file_bytes / (1024.0 * 1024.0);

                char b1[64], b2[64], b3[64], b4[64];
                snprintf(b1, sizeof(b1), "%s: %.1f MB", tr(lang, "Size", "Розмір", "大小", "Größe", "Taille", "Rozmiar"), mb);
                snprintf(b2, sizeof(b2), "%s: %u kbps", tr(lang, "Bitrate", "Бітрейт", "码率", "Bitrate", "Débit", "Bitrate"), state->bitrate_kbps);
                snprintf(b3, sizeof(b3), "FPS: %.1f / %d", state->current_fps, state->target_fps);
                snprintf(b4, sizeof(b4), "%s: %llu", tr(lang, "Dropped", "Пропущено", "丢帧", "Verworfen", "Perdues", "Pominięto"), (unsigned long long)state->dropped_frames);

                const char* tile_texts[] = { b1, b2, b3, b4 };
                ImVec4 tile_colors[] = {
                    ImVec4(0.3f, 0.75f, 1.0f, 1.0f),
                    ImVec4(0.3f, 0.85f, 0.45f, 1.0f),
                    ImVec4(1.0f, 0.85f, 0.35f, 1.0f),
                    (state->dropped_frames > 0) ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f) : ImVec4(0.6f, 0.65f, 0.75f, 1.0f)
                };

                for (int t = 0; t < 4; t++) {
                    if (t > 0) ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.17f, 0.20f, 0.80f));
                    char tile_id[32];
                    snprintf(tile_id, sizeof(tile_id), "TelemetryTile%d", t);
                    ImGui::BeginChild(tile_id, ImVec2(tile_w, tile_h), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                    {
                        ImVec2 sz = ImGui::CalcTextSize(tile_texts[t]);
                        ImGui::SetCursorPosX((tile_w - sz.x) * 0.5f);
                        ImGui::SetCursorPosY((tile_h - sz.y) * 0.5f);
                        ImGui::TextColored(tile_colors[t], "%s", tile_texts[t]);
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::EndTabItem();
        }

        // TAB 2: ON-DEMAND LIVE PREVIEW (Zero RAM/VRAM when tab is closed)
        if (ImGui::BeginTabItem(tr(lang, "  Live Preview  ", "  Перегляд (Live)  ", "  实时预览 (Live)  ", "  Live-Vorschau  ", "  Aperçu (Live)  ", "  Podgląd (Live)  "))) {
            preview_tab_active = true;
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.13f, 0.16f, 1.00f));
            ImGui::BeginChild("LivePreviewCard", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar);
            {
                ImGui::TextColored(ImVec4(0.25f, 0.75f, 1.0f, 1.0f), "%s",
                    tr(lang,
                        "LIVE CAPTURE PREVIEW (Auto-suspended when tab is closed to save RAM/VRAM)",
                        "ЖИВИЙ ПЕРЕГЛЯД ЗАХОПЛЕННЯ (Вимикається поза вкладкою для економії ОЗУ/VRAM)",
                        "实时捕获预览（关闭此标签页时自动释放内存/显存）",
                        "LIVE-AUFNAHMEVORSCHAU (Wird beim Schließen des Tabs pausiert, um RAM/VRAM zu sparen)",
                        "APERÇU EN DIRECT (Suspendu hors de l'onglet pour économiser la RAM/VRAM)",
                        "PODGLĄD NA ŻYWO (Wstrzymywany po zamknięciu karty w celu oszczędzania RAM/VRAM)"));
                ImGui::SameLine();
                if (ImGui::SmallButton(g_previewPaused
                    ? tr(lang, "Resume Preview", "Відновити", "继续预览", "Fortsetzen", "Reprendre", "Wznów")
                    : tr(lang, "Pause Preview", "Пауза", "暂停预览", "Pausieren", "Pause", "Wstrzymaj"))) {
                    g_previewPaused = !g_previewPaused;
                }

                ImGui::Separator();
                ImGui::Spacing();

                ImVec2 avail = ImGui::GetContentRegionAvail();
                if (g_pPreviewSRV && g_previewWidth > 0 && g_previewHeight > 0 && !g_previewPaused) {
                    float aspect = (float)g_previewWidth / (float)g_previewHeight;
                    float draw_w = avail.x;
                    float draw_h = draw_w / aspect;
                    if (draw_h > avail.y) {
                        draw_h = avail.y;
                        draw_w = draw_h * aspect;
                    }
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - draw_w) * 0.5f);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (avail.y - draw_h) * 0.5f);
                    ImGui::Image((ImTextureID)(intptr_t)g_pPreviewSRV, ImVec2(draw_w, draw_h));
                } else {
                    const char* msg = is_recording
                        ? tr(lang,
                            "Recording is active in Zero-Copy GPU mode (Preview paused to maximize game FPS).",
                            "Запис триває у режимі Zero-Copy GPU (Прев'ю вимкнено для максимального FPS у грі).",
                            "正在以零拷贝 GPU 模式录制（已暂停预览以保证游戏最高帧率）。",
                            "Aufnahme läuft im Zero-Copy-GPU-Modus (Vorschau pausiert für maximale Spiel-FPS).",
                            "Enregistrement actif en mode GPU Zero-Copy (Aperçu en pause pour maximiser les FPS).",
                            "Nagrywanie trwa w trybie Zero-Copy GPU (Podgląd wstrzymany dla maksymalnego FPS w grze).")
                        : tr(lang,
                            "Initializing live screen preview...",
                            "Ініціалізація живого перегляду екрана...",
                            "正在初始化实时屏幕预览...",
                            "Live-Bildschirmvorschau wird initialisiert...",
                            "Initialisation de l'aperçu en direct...",
                            "Inicjalizacja podglądu ekranu na żywo...");
                    ImVec2 tsz = ImGui::CalcTextSize(msg);
                    ImGui::SetCursorPosX((avail.x - tsz.x) * 0.5f);
                    ImGui::SetCursorPosY((avail.y - tsz.y) * 0.5f);
                    ImGui::TextDisabled("%s", msg);
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::EndTabItem();
        }

        // TAB 3: SETTINGS (Dynamic Hardware Encoders & Audio & Hotkeys & Language)
        if (ImGui::BeginTabItem(tr(lang, "  Settings  ", "  Налаштування  ", "  设置  ", "  Einstellungen  ", "  Paramètres  ", "  Ustawienia  "))) {
            ImGui::Spacing();

            if (ImGui::BeginTabBar("SettingsSubTabs")) {
                // SUB-TAB 1: Video & Hardware Encoders
                if (ImGui::BeginTabItem(tr(lang, "Video & Encoder", "Відео та Енкодер", "视频与编码器", "Video & Encoder", "Vidéo & Encodeur", "Wideo i Enkoder"))) {
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.14f, 0.17f, 1.00f));
                    ImGui::BeginChild("VideoSettingsCard", ImVec2(0, 0), true);
                    {
                        ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1.00f), "%s",
                            tr(lang,
                                "Video & Resolution Settings:",
                                "Параметри відео та роздільна здатність:",
                                "视频与分辨率设置:",
                                "Video- & Auflösungseinstellungen:",
                                "Paramètres vidéo et résolution :",
                                "Ustawienia wideo i rozdzielczości:"));
                        ImGui::Spacing();

                        // Mouse Cursor Capture toggle
                        if (ImGui::Checkbox(
                            tr(lang,
                                "Capture Mouse Cursor in Video",
                                "Захоплювати курсор миші у відео",
                                "在视频中捕获鼠标指针",
                                "Mauszeiger im Video aufnehmen",
                                "Capturer le curseur de la souris",
                                "Przechwytuj kursor myszy w wideo"),
                            &state->capture_cursor)) {
                            state->action_settings_changed = true;
                        }
                        ImGui::Spacing();

                        // Output Resolution Scaler
                        const char* res_labels[] = {
                            tr(lang, "Native (Match screen)", "Рідна (як на екрані)", "原生分辨率 (与屏幕一致)", "Nativ (Wie Bildschirm)", "Natif (Comme l'écran)", "Natywna (jak ekran)"),
                            "1080p (1920x1080) Full HD",
                            "720p (1280x720) HD",
                            "480p (854x480) SD"
                        };
                        if (ImGui::Combo(
                            tr(lang, "Output Resolution", "Роздільна здатність запису", "输出分辨率", "Ausgabeauflösung", "Résolution de sortie", "Rozdzielczość wyjściowa"),
                            &state->output_resolution, res_labels, 4)) {
                            state->action_settings_changed = true;
                        }

                        // Color Range Selector
                        const char* color_range_labels[] = {
                            tr(lang,
                                "Limited (BT.709 TV 16-235) - Standard Broadcast & Players",
                                "Обмежений (BT.709 TV 16-235) - Стандарт відео, точні насичені кольори",
                                "有限范围 (BT.709 TV 16-235) - 播放器标准色彩",
                                "Begrenzt (BT.709 TV 16-235) - Standard für Player",
                                "Limité (BT.709 TV 16-235) - Standard vidéo",
                                "Ograniczony (BT.709 TV 16-235) - Standard wideo"),
                            tr(lang,
                                "Full (Full Range PC 0-255) - PC Full Dynamic Range",
                                "Повний (Full Range PC 0-255) - Повний діапазон ПК",
                                "完全范围 (Full Range PC 0-255) - PC 全动态范围",
                                "Voll (Full Range PC 0-255) - Voller PC-Farbbereich",
                                "Complet (Full Range PC 0-255) - Plage complète PC",
                                "Pełny (Full Range PC 0-255) - Pełny zakres PC")
                        };
                        if (ImGui::Combo(
                            tr(lang, "Color Range", "Колірний діапазон", "色彩范围", "Farbbereich", "Plage de couleurs", "Zakres kolorów"),
                            &state->color_range, color_range_labels, 2)) {
                            state->action_settings_changed = true;
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1.00f), "%s",
                            tr(lang,
                                "GPU Hardware Capabilities:",
                                "Апаратні можливості GPU:",
                                "GPU 硬件加速编码:",
                                "GPU-Hardwarebeschleunigung:",
                                "Capacités matérielles GPU :",
                                "Możliwości sprzętowe GPU:"));
                        ImGui::Spacing();

                        // 1. Dynamic filtered backends based on detected hardware
                        bool has_nvenc = (state->encoder_caps_mask & ((1 << 0) | (1 << 1) | (1 << 2))) != 0;
                        bool has_amf   = (state->encoder_caps_mask & ((1 << 3) | (1 << 4) | (1 << 5))) != 0;
                        bool has_qsv   = (state->encoder_caps_mask & ((1 << 6) | (1 << 7) | (1 << 8))) != 0;

                        struct BackendItem {
                            int id;
                            const char* label;
                        };
                        BackendItem avail_backends[4];
                        int avail_count = 0;
                        if (has_nvenc) avail_backends[avail_count++] = { 0, "NVIDIA NVENC (Hardware)" };
                        if (has_amf)   avail_backends[avail_count++] = { 1, "AMD AMF (Hardware)" };
                        if (has_qsv)   avail_backends[avail_count++] = { 2, "Intel QuickSync (Hardware)" };
                        avail_backends[avail_count++] = { 3, tr(lang, "Software (CPU - libx264)", "Програмний (CPU - libx264)", "软件编码 (CPU - libx264)", "Software (CPU - libx264)", "Logiciel (CPU - libx264)", "Programowy (CPU - libx264)") };

                        int current_backend_idx = 0;
                        for (int i = 0; i < avail_count; i++) {
                            if (avail_backends[i].id == state->encoder_backend) {
                                current_backend_idx = i;
                                break;
                            }
                        }

                        const char* backend_labels[4];
                        for (int i = 0; i < avail_count; i++) backend_labels[i] = avail_backends[i].label;

                        if (ImGui::Combo(
                            tr(lang, "Encoder Backend", "Бекенд енкодера", "编码器后端", "Encoder-Backend", "Backend d'encodage", "Backend enkodera"),
                            &current_backend_idx, backend_labels, avail_count)) {
                            state->encoder_backend = avail_backends[current_backend_idx].id;
                            state->action_settings_changed = true;
                        }

                        // 2. Dynamic filtered codecs based on selected backend
                        bool can_h264 = true;
                        bool can_hevc = false;
                        bool can_av1  = false;

                        if (state->encoder_backend == 0) {
                            can_h264 = (state->encoder_caps_mask & (1 << 0)) != 0;
                            can_hevc = (state->encoder_caps_mask & (1 << 1)) != 0;
                            can_av1  = (state->encoder_caps_mask & (1 << 2)) != 0;
                        } else if (state->encoder_backend == 1) {
                            can_h264 = (state->encoder_caps_mask & (1 << 3)) != 0;
                            can_hevc = (state->encoder_caps_mask & (1 << 4)) != 0;
                            can_av1  = (state->encoder_caps_mask & (1 << 5)) != 0;
                        } else if (state->encoder_backend == 2) {
                            can_h264 = (state->encoder_caps_mask & (1 << 6)) != 0;
                            can_hevc = (state->encoder_caps_mask & (1 << 7)) != 0;
                            can_av1  = (state->encoder_caps_mask & (1 << 8)) != 0;
                        } else {
                            can_h264 = (state->encoder_caps_mask & (1 << 9)) != 0;
                            can_hevc = (state->encoder_caps_mask & (1 << 10)) != 0;
                            can_av1  = false;
                        }

                        struct CodecItem {
                            int id;
                            const char* label;
                        };
                        CodecItem avail_codecs[3];
                        int codec_count = 0;
                        if (can_h264) avail_codecs[codec_count++] = { 0, "H.264 (AVC)" };
                        if (can_hevc) avail_codecs[codec_count++] = { 1, "HEVC (H.265)" };
                        if (can_av1)  avail_codecs[codec_count++] = { 2, "AV1 (Next-Gen 2026)" };

                        int current_codec_idx = 0;
                        for (int i = 0; i < codec_count; i++) {
                            if (avail_codecs[i].id == state->video_codec) {
                                current_codec_idx = i;
                                break;
                            }
                        }

                        const char* codec_labels[3];
                        for (int i = 0; i < codec_count; i++) codec_labels[i] = avail_codecs[i].label;

                        if (ImGui::Combo(
                            tr(lang, "Video Codec", "Відеокодек", "视频编解码器", "Videocodec", "Codec vidéo", "Kodek wideo"),
                            &current_codec_idx, codec_labels, codec_count)) {
                            state->video_codec = avail_codecs[current_codec_idx].id;
                            state->action_settings_changed = true;
                        }

                        // Rate Control Mode & Parameters
                        const char* rate_modes[] = {
                            tr(lang, "VBR (Variable Bitrate - Recommended)", "VBR (Змінний бітрейт - Рекомендовано)", "VBR (动态码率 - 推荐)", "VBR (Variable Bitrate - Empfohlen)", "VBR (Débit variable - Recommandé)", "VBR (Zmienny bitrate - Zalecane)"),
                            tr(lang, "CBR (Constant Bitrate - Fixed)", "CBR (Постійний бітрейт - Стабільний)", "CBR (固定码率)", "CBR (Konstante Bitrate)", "CBR (Débit constant)", "CBR (Stały bitrate)"),
                            tr(lang, "CQP / CRF (Constant Quality)", "CQP / CRF (Стала якість)", "CQP / CRF (恒定画质)", "CQP / CRF (Konstante Qualität)", "CQP / CRF (Qualité constante)", "CQP / CRF (Stała jakość)")
                        };
                        if (ImGui::Combo(
                            tr(lang, "Rate Control Mode", "Керування бітрейтом", "码率控制模式", "Bitratensteuerung", "Contrôle du débit", "Kontrola bitrate"),
                            &state->rate_control, rate_modes, 3)) {
                            state->action_settings_changed = true;
                        }

                        if (state->rate_control == 0 || state->rate_control == 1) { // VBR or CBR
                            int mbps = (int)(state->target_bitrate_kbps / 1000);
                            if (mbps < 2) mbps = 15;
                            const char* bitrate_slider_title = (state->rate_control == 1)
                                ? tr(lang, "Constant Bitrate (Mbps)", "Постійний бітрейт (Мбіт/с)", "固定码率 (Mbps)", "Konstante Bitrate (Mbps)", "Débit constant (Mbps)", "Stały bitrate (Mbps)")
                                : tr(lang, "Target Bitrate (Mbps)", "Цільовий бітрейт (Мбіт/с)", "目标码率 (Mbps)", "Ziel-Bitrate (Mbps)", "Débit cible (Mbps)", "Docelowy bitrate (Mbps)");
                            if (ImGui::SliderInt(bitrate_slider_title, &mbps, 2, 80, "%d Mbps")) {
                                state->target_bitrate_kbps = (uint32_t)(mbps * 1000);
                                state->action_settings_changed = true;
                            }

                            ImGui::TextDisabled("%s", tr(lang, "  Quick Bitrate Presets:", "  Швидкі пресети бітрейту:", "  快速码率预设:", "  Schnellwahl-Bitrate:", "  Préréglages rapides :", "  Szybkie presety bitrate:"));
                            ImGui::SameLine();
                            if (ImGui::SmallButton("6M (720p)")) { state->target_bitrate_kbps = 6000; state->action_settings_changed = true; }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("12M (1080p 30fps)")) { state->target_bitrate_kbps = 12000; state->action_settings_changed = true; }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("18M (1080p 60fps)")) { state->target_bitrate_kbps = 18000; state->action_settings_changed = true; }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("30M (High)")) { state->target_bitrate_kbps = 30000; state->action_settings_changed = true; }
                        } else { // CQP or CRF
                            if (state->quality_cqp < 10 || state->quality_cqp > 40) state->quality_cqp = 20;
                            if (ImGui::SliderInt(
                                tr(lang, "Quality Level (CQP/CRF: lower = better)", "Коефіцієнт якості (CQP/CRF: менше = краще)", "画质等级 (CQP/CRF: 数值越小画质越高)", "Qualitätsstufe (CQP/CRF: niedriger = besser)", "Niveau de qualité (CQP/CRF : bas = meilleur)", "Poziom jakości (CQP/CRF: mniej = lepiej)"),
                                &state->quality_cqp, 14, 32)) {
                                state->action_settings_changed = true;
                            }
                        }

                        // Encoder Preset (Speed vs Quality)
                        const char* preset_labels[] = {
                            tr(lang, "Ultrafast (Recommended for games - 0 lag)", "Ultrafast (Надшвидкий - Рекомендовано для ігор)", "Ultrafast (极速 - 游戏录制推荐，零卡顿)", "Ultrafast (Empfohlen für Spiele - 0 Lag)", "Ultrafast (Recommandé pour les jeux - 0 lag)", "Ultrafast (Zalecany do gier - 0 przycięć)"),
                            tr(lang, "Superfast / Fast (Low load)", "Superfast / Fast (Швидкий)", "Superfast / Fast (快速 - 低负载)", "Superfast / Fast (Geringe Last)", "Superfast / Fast (Faible charge)", "Superfast / Fast (Niskie obciążenie)"),
                            tr(lang, "Balanced / Medium (Balanced)", "Balanced / Medium (Збалансований)", "Balanced / Medium (均衡)", "Balanced / Medium (Ausgewogen)", "Balanced / Medium (Équilibré)", "Balanced / Medium (Zbalansowany)"),
                            tr(lang, "Slow (High Quality - Heavy CPU load)", "Slow (Висока якість - Велике навантаження)", "Slow (高画质 - 高 CPU 负载)", "Slow (Hohe Qualität - Hohe CPU-Last)", "Slow (Haute qualité - Charge CPU élevée)", "Slow (Wysoka jakość - Duże obciążenie CPU)")
                        };
                        if (ImGui::Combo(
                            tr(lang, "Encoder Preset", "Профіль швидкодії (Пресет)", "编码预设 (速度与画质)", "Encoder-Preset", "Préréglage de l'encodeur", "Profil szybkości (Preset)"),
                            &state->encoder_preset, preset_labels, 4)) {
                            state->action_settings_changed = true;
                        }

                        // Keyframe / GOP interval
                        int gop_options[] = { 1, 2, 3, 5 };
                        const char* gop_labels[] = {
                            "1 s",
                            "2 s (Recommended)",
                            "3 s",
                            "5 s"
                        };
                        int current_gop_idx = 1;
                        for (int i = 0; i < 4; i++) {
                            if (gop_options[i] == state->keyframe_interval_secs) current_gop_idx = i;
                        }
                        if (ImGui::Combo(
                            tr(lang, "Keyframe Interval (GOP)", "Інтервал ключових кадрів (GOP)", "关键帧间隔 (GOP)", "Keyframe-Intervall (GOP)", "Intervalle d'images clés (GOP)", "Interwał klatek kluczowych (GOP)"),
                            &current_gop_idx, gop_labels, 4)) {
                            state->keyframe_interval_secs = gop_options[current_gop_idx];
                            state->action_settings_changed = true;
                        }

                        // Container Format Selection
                        const char* container_formats[] = {
                            tr(lang, "MKV (*.mkv - Recommended, crash-resilient)", "MKV (*.mkv - Рекомендовано, стійкий до збоїв)", "MKV (*.mkv - 推荐，防崩溃损坏)", "MKV (*.mkv - Empfohlen, absturzsicher)", "MKV (*.mkv - Recommandé, résistant aux crashs)", "MKV (*.mkv - Zalecany, odporny na awarie)"),
                            tr(lang, "MP4 (*.mp4 - Universal compatibility)", "MP4 (*.mp4 - Сумісний з усіма плеєрами)", "MP4 (*.mp4 - 通用兼容格式)", "MP4 (*.mp4 - Universelle Kompatibilität)", "MP4 (*.mp4 - Compatibilité universelle)", "MP4 (*.mp4 - Uniwersalna kompatybilność)"),
                            tr(lang, "MOV (*.mov - Video Editing / QuickTime)", "MOV (*.mov - Монтаж у DaVinci / Premiere)", "MOV (*.mov - 适合视频剪辑)", "MOV (*.mov - Videoschnitt / QuickTime)", "MOV (*.mov - Montage vidéo / QuickTime)", "MOV (*.mov - Montaż wideo / QuickTime)")
                        };
                        if (ImGui::Combo(
                            tr(lang, "Container Format", "Формат відеофайлу", "视频容器格式", "Container-Format", "Format du conteneur", "Format pliku wideo"),
                            &state->container_format, container_formats, 3)) {
                            state->action_settings_changed = true;
                        }

                        // Frame Rate: Full range from 10 FPS up to user's monitor refresh rate (max_hz)
                        if (ImGui::SliderInt(
                            tr(lang, "Frame Rate (FPS)", "Частота кадрів (FPS)", "帧率 (FPS)", "Bildrate (FPS)", "Fréquence d'images (FPS)", "Liczba klatek (FPS)"),
                            &state->target_fps, 10, max_hz, "%d FPS")) {
                            if (state->target_fps < 10) state->target_fps = 10;
                            if (state->target_fps > max_hz) state->target_fps = max_hz;
                            state->action_settings_changed = true;
                        }

                        ImGui::TextDisabled(
                            tr(lang,
                                "  Range: 10 - %d FPS (Monitor Refresh Rate: %d Hz) | Quick:",
                                "  Діапазон: від 10 до %d FPS (Частота монітора: %d Гц) | Швидкий вибір:",
                                "  范围: 10 - %d FPS (显示器刷新率: %d Hz) | 快速选择:",
                                "  Bereich: 10 - %d FPS (Monitor-Bildwiederholrate: %d Hz) | Schnellwahl:",
                                "  Plage : 10 - %d FPS (Taux du moniteur : %d Hz) | Rapide :",
                                "  Zakres: 10 - %d FPS (Odświeżanie monitora: %d Hz) | Szybki wybór:"),
                            max_hz, max_hz);
                        ImGui::SameLine();
                        int preset_fps[] = { 10, 20, 30, 60, 120 };
                        for (int i = 0; i < 5; i++) {
                            if (preset_fps[i] < max_hz) {
                                char pbuf[24];
                                snprintf(pbuf, sizeof(pbuf), "%d FPS", preset_fps[i]);
                                if (ImGui::SmallButton(pbuf)) {
                                    state->target_fps = preset_fps[i];
                                    state->action_settings_changed = true;
                                }
                                ImGui::SameLine();
                            }
                        }
                        {
                            char max_buf[32];
                            snprintf(max_buf, sizeof(max_buf), "%d FPS (Max)", max_hz);
                            if (ImGui::SmallButton(max_buf)) {
                                state->target_fps = max_hz;
                                state->action_settings_changed = true;
                            }
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        ImGui::Text("%s", tr(lang, "Output Directory:", "Папка для збереження записів:", "录制保存目录:", "Ausgabeordner:", "Dossier de sortie :", "Folder zapisu nagrań:"));
                        ImGui::InputText("##OutputDir", state->output_dir, sizeof(state->output_dir));
                        ImGui::SameLine();
                        if (ImGui::Button(tr(lang, "Browse...", "Огляд...", "浏览...", "Durchsuchen...", "Parcourir...", "Przeglądaj..."))) {
                            if (pick_folder_dialog(state->output_dir, sizeof(state->output_dir), g_hWnd)) {
                                state->action_settings_changed = true;
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(tr(lang, "Open Folder", "Відкрити", "打开文件夹", "Ordner öffnen", "Ouvrir le dossier", "Otwórz folder"))) {
                            state->action_open_folder_requested = true;
                        }
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                    ImGui::EndTabItem();
                }

                // SUB-TAB 2: Audio Configuration
                if (ImGui::BeginTabItem(tr(lang, "Audio", "Аудіо", "音频", "Audio", "Audio", "Dźwięk"))) {
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.14f, 0.17f, 1.00f));
                    ImGui::BeginChild("AudioSettingsCard", ImVec2(0, 0), true);
                    {
                        ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1.00f), "%s",
                            tr(lang,
                                "Audio Sources & Devices:",
                                "Джерела аудіо та вибір пристроїв:",
                                "音频源与设备选择:",
                                "Audioquellen & Geräte:",
                                "Sources et périphériques audio :",
                                "Źródła dźwięku i urządzenia:"));
                        ImGui::Spacing();

                        // System Audio & Output Device
                        if (ImGui::Checkbox(
                            tr(lang,
                                "Record System Audio (WASAPI Loopback)",
                                "Записувати системний звук (WASAPI Loopback)",
                                "录制系统声音 (WASAPI Loopback)",
                                "Systemton aufnehmen (WASAPI Loopback)",
                                "Enregistrer le son système (WASAPI Loopback)",
                                "Nagrywaj dźwięk systemowy (WASAPI Loopback)"),
                            &state->record_system_audio)) {
                            state->action_settings_changed = true;
                        }
                        if (state->record_system_audio) {
                            if (state->audio_output_count > 0 && state->audio_output_names != NULL) {
                                if (ImGui::Combo(
                                    tr(lang,
                                        "Playback Device (Headphones / Speakers)",
                                        "Пристрій виводу (навушники / динаміки)",
                                        "播放设备 (耳机 / 扬声器)",
                                        "Wiedergabegerät (Kopfhörer / Lautsprecher)",
                                        "Périphérique de lecture (Casque / Haut-parleurs)",
                                        "Urządzenie odtwarzania (Słuchawki / Głośniki)"),
                                    &state->selected_audio_output, state->audio_output_names, state->audio_output_count)) {
                                    state->action_settings_changed = true;
                                }
                            }
                            int sys_vol_percent = (int)(state->system_volume * 100.0f + 0.5f);
                            if (ImGui::SliderInt(
                                tr(lang, "System Volume (%)", "Гучність системи (%)", "系统音量 (%)", "Systemlautstärke (%)", "Volume système (%)", "Głośność systemu (%)"),
                                &sys_vol_percent, 0, 200, "%d%%")) {
                                state->system_volume = (float)sys_vol_percent / 100.0f;
                                state->action_settings_changed = true;
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("100%##sys")) {
                                state->system_volume = 1.0f;
                                state->action_settings_changed = true;
                            }
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        // Microphone & Input Device
                        if (ImGui::Checkbox(
                            tr(lang,
                                "Record Microphone (Microphone Input)",
                                "Записувати мікрофон (Microphone Input)",
                                "录制麦克风 (Microphone Input)",
                                "Mikrofon aufnehmen (Mikrofoneingang)",
                                "Enregistrer le microphone",
                                "Nagrywaj mikrofon"),
                            &state->record_microphone)) {
                            state->action_settings_changed = true;
                        }
                        if (state->record_microphone) {
                            if (state->audio_input_count > 0 && state->audio_input_names != NULL) {
                                if (ImGui::Combo(
                                    tr(lang,
                                        "Recording Device (Microphone)",
                                        "Пристрій вводу (мікрофон)",
                                        "录音设备 (麦克风)",
                                        "Aufnahmegerät (Mikrofon)",
                                        "Périphérique d'entrée (Microphone)",
                                        "Urządzenie wejściowe (Mikrofon)"),
                                    &state->selected_audio_input, state->audio_input_names, state->audio_input_count)) {
                                    state->action_settings_changed = true;
                                }
                            }
                            int mic_vol_percent = (int)(state->mic_volume * 100.0f + 0.5f);
                            if (ImGui::SliderInt(
                                tr(lang, "Microphone Volume (%)", "Гучність мікрофона (%)", "麦克风音量 (%)", "Mikrofonlautstärke (%)", "Volume du microphone (%)", "Głośność mikrofonu (%)"),
                                &mic_vol_percent, 0, 200, "%d%%")) {
                                state->mic_volume = (float)mic_vol_percent / 100.0f;
                                state->action_settings_changed = true;
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("100%##mic")) {
                                state->mic_volume = 1.0f;
                                state->action_settings_changed = true;
                            }
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        // Separate Tracks
                        if (ImGui::Checkbox(
                            tr(lang,
                                "Record Separate Audio Tracks (Multi-Track Audio)",
                                "Записувати на окремі доріжки (Multi-Track Audio)",
                                "录制独立音轨 (多轨音频)",
                                "Getrennte Audiospuren aufnehmen (Multi-Track)",
                                "Pistes audio séparées (Multi-pistes)",
                                "Nagrywaj osobne ścieżki audio (Multi-Track)"),
                            &state->separate_audio_tracks)) {
                            state->action_settings_changed = true;
                        }
                        ImGui::TextDisabled("%s",
                            tr(lang,
                                "  Track 1: System Audio | Track 2: Microphone (Ideal for editing)",
                                "  Доріжка 1: Системний звук | Доріжка 2: Мікрофон (зручно для монтажу)",
                                "  音轨 1: 系统声音 | 音轨 2: 麦克风 (方便后期剪辑)",
                                "  Spur 1: Systemton | Spur 2: Mikrofon (Ideal für Videoschnitt)",
                                "  Piste 1 : Son système | Piste 2 : Microphone (Idéal pour le montage)",
                                "  Ścieżka 1: Dźwięk systemu | Ścieżka 2: Mikrofon (Wygodne do montażu)"));

                        ImGui::Spacing();

                        // Standalone Audio File (.wav)
                        if (ImGui::Checkbox(
                            tr(lang,
                                "Save Audio as Separate File (.wav)",
                                "Зберігати звук як окремий файл (.wav)",
                                "将音频另存为独立文件 (.wav)",
                                "Audio als separate Datei speichern (.wav)",
                                "Enregistrer l'audio dans un fichier séparé (.wav)",
                                "Zapisz dźwięk jako osobny plik (.wav)"),
                            &state->save_separate_audio_file)) {
                            state->action_settings_changed = true;
                        }
                        ImGui::TextDisabled("%s",
                            tr(lang,
                                "  Writes uncompressed studio 48 kHz WAV audio alongside the video file.",
                                "  Створює безкомпресійний студійний 48 кГц WAV файл поруч із відеофайлом.",
                                "  在视频文件旁生成无损 48 kHz WAV 音频文件。",
                                "  Erstellt eine unkomprimierte 48-kHz-WAV-Audiodatei neben dem Video.",
                                "  Crée un fichier WAV 48 kHz non compressé à côté du fichier vidéo.",
                                "  Tworzy nieskompresowany plik WAV 48 kHz obok pliku wideo."));
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                    ImGui::EndTabItem();
                }

                // SUB-TAB 3: Hotkeys & Overlay Indicator
                if (ImGui::BeginTabItem(tr(lang, "Hotkeys & Overlay", "Гарячі клавіші та Оверлей", "快捷键与悬浮窗", "Hotkeys & Overlay", "Raccourcis & Overlay", "Skróty i Nakładka"))) {
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.14f, 0.17f, 1.00f));
                    ImGui::BeginChild("HotkeysSettingsCard", ImVec2(0, 0), true);
                    {
                        ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1.00f), "%s",
                            tr(lang, "Global Hotkeys:", "Глобальні комбінації клавіш:", "全局快捷键:", "Globale Tastenkürzel:", "Raccourcis globaux :", "Globalne skróty klawiszowe:"));
                        ImGui::Spacing();

                        const char* hotkey_presets[] = {
                            tr(lang, "F9 (Recommended for gaming)", "F9 (Рекомендовано для ігор)", "F9 (游戏录制推荐)", "F9 (Empfohlen für Spiele)", "F9 (Recommandé pour les jeux)", "F9 (Zalecane do gier)"),
                            "F10",
                            "Ctrl + Shift + R",
                            "Alt + F9 (NVIDIA ShadowPlay)"
                        };

                        if (ImGui::Combo(
                            tr(lang, "Start / Stop Recording", "Старт / Зупинка запису", "开始 / 停止录制", "Aufnahme Start / Stopp", "Démarrer / Arrêter l'enregistrement", "Start / Stop nagrywania"),
                            &state->hotkey_start_stop_preset,
                            hotkey_presets, 4)) {
                            state->action_settings_changed = true;
                        }

                        ImGui::TextDisabled("%s",
                            tr(lang,
                                "Pause / Resume Recording: Ctrl + Shift + P",
                                "Пауза / Відновлення запису: Ctrl + Shift + P",
                                "暂停 / 继续录制: Ctrl + Shift + P",
                                "Aufnahme Pausieren / Fortsetzen: Ctrl + Shift + P",
                                "Pause / Reprendre l'enregistrement : Ctrl + Shift + P",
                                "Wstrzymaj / Wznów nagrywanie: Ctrl + Shift + P"));

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1.00f), "%s",
                            tr(lang, "On-Screen Recording Overlay:", "Екранний оверлей-індикатор:", "屏幕录制状态悬浮窗:", "Bildschirm-Aufnahme-Overlay:", "Indicateur d'enregistrement à l'écran :", "Ekranowy wskaźnik nagrywania:"));
                        ImGui::Spacing();

                        if (ImGui::Checkbox(
                            tr(lang,
                                "Show On-Screen Recording Indicator Overlay",
                                "Відображати індикатор запису поверх усіх вікон",
                                "在屏幕角落显示录制状态悬浮窗",
                                "Aufnahme-Indikator auf dem Bildschirm anzeigen",
                                "Afficher l'indicateur d'enregistrement à l'écran",
                                "Wyświetlaj wskaźnik nagrywania na ekranie"),
                            &state->show_overlay_indicator)) {
                            state->action_settings_changed = true;
                        }
                        ImGui::TextDisabled("%s",
                            tr(lang,
                                "Displays a transparent floating badge [REC 00:01:23 | 60 FPS] in the screen corner with full click-through.",
                                "Показує прозорий плаваючий бейдж [REC 00:01:23 | 60 FPS] у кутку екрана без блокування кліків миші.",
                                "在屏幕右上角显示透明穿透状态栏 [REC 00:01:23 | 60 FPS]，不遮挡鼠标点击与录制画面。",
                                "Zeigt ein transparentes Badge [REC 00:01:23 | 60 FPS] in der Bildschirmecke an (klickdurchlässig).",
                                "Affiche un badge flottant transparent [REC 00:01:23 | 60 FPS] dans le coin de l'écran sans bloquer les clics.",
                                "Wyświetla przezroczystą plakietkę [REC 00:01:23 | 60 FPS] w rogu ekranu bez blokowania kliknięć myszy."));
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                    ImGui::EndTabItem();
                }

                // SUB-TAB 4: Language & Version
                if (ImGui::BeginTabItem(tr(lang, "Language", "Мова", "语言", "Sprache", "Langue", "Język"))) {
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.14f, 0.17f, 1.00f));
                    ImGui::BeginChild("LangSettingsCard", ImVec2(0, 380), true);
                    {
                        ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1.00f), "%s",
                            tr(lang,
                                "Interface Language:",
                                "Мова інтерфейсу:",
                                "界面语言 (Interface Language):",
                                "Oberflächensprache:",
                                "Langue de l'interface :",
                                "Język interfejsu:"));
                        ImGui::Spacing();

                        const char* languages[] = {
                            "English",
                            "Українська",
                            "中文 (简体)",
                            "Deutsch",
                            "Français",
                            "Polski"
                        };
                        if (ImGui::Combo("##LanguageCombo", &state->language, languages, 6)) {
                            state->action_language_changed = true;
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.55f, 1.00f),
                            "%s: v" LIFESCREEN_VERSION,
                            tr(lang,
                                "Current Application Version",
                                "Поточна версія програми",
                                "当前软件版本",
                                "Aktuelle Programmversion",
                                "Version actuelle de l'application",
                                "Aktualna wersja programu"));
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();

    bool should_enable_preview = preview_tab_active && !g_previewPaused && !(g_hWnd && IsIconic(g_hWnd));
    if (!should_enable_preview && g_previewEnabled) {
        if (g_previewCsInit) EnterCriticalSection(&g_previewCs);
        if (g_pPreviewSRV) { g_pPreviewSRV->Release(); g_pPreviewSRV = NULL; }
        if (g_pPreviewTexture) { g_pPreviewTexture->Release(); g_pPreviewTexture = NULL; }
        g_previewWidth = 0;
        g_previewHeight = 0;
        if (g_previewCsInit) LeaveCriticalSection(&g_previewCs);
    }
    g_previewEnabled = should_enable_preview;
}

void imgui_window_present(void) {
    if (g_skipCurrentFrameRender || (g_hWnd && IsIconic(g_hWnd))) {
        Sleep(30);
        return;
    }

    ImGui::Render();
    const float clear_color[4] = { 0.10f, 0.11f, 0.13f, 1.00f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    HRESULT hr = g_pSwapChain->Present(1, 0); // V-Sync enabled

    if (hr == DXGI_STATUS_OCCLUDED) {
        // Window is occluded (covered by game or other app): Present does not wait for V-Sync.
        // Sleep to avoid high-frequency unthrottled loop!
        Sleep(30);
    } else if (g_hWnd && GetForegroundWindow() != g_hWnd) {
        // Window in background while playing game: throttle to 30 FPS
        Sleep(16);
    }
}

void imgui_window_get_size(int* out_width, int* out_height) {
    RECT rect;
    if (GetClientRect(g_hWnd, &rect)) {
        *out_width = rect.right - rect.left;
        *out_height = rect.bottom - rect.top;
    }
}

void imgui_window_destroy(void) {
    if (g_hOverlayWnd) {
        DestroyWindow(g_hOverlayWnd);
        g_hOverlayWnd = NULL;
    }
    UnregisterClassA("LifeScreenOverlayClass", GetModuleHandle(NULL));

    UnregisterHotKey(g_hWnd, 101);
    UnregisterHotKey(g_hWnd, 102);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    cleanup_render_target();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }

    if (g_hWnd) {
        DestroyWindow(g_hWnd);
        g_hWnd = NULL;
    }
}

bool imgui_window_poll_start_hotkey(void) {
    bool triggered = g_hotkey_start_triggered;
    g_hotkey_start_triggered = false;

    // Asynchronous key-state fallback: ensures F9 (or F10) triggers recording even if
    // another app or game intercepted RegisterHotKey, or when focus is elsewhere.
    static bool s_f9_was_down = false;
    static bool s_f10_was_down = false;

    if (g_currentHotkeyPreset == 0 || g_currentHotkeyPreset == -1) {
        SHORT keyState = GetAsyncKeyState(VK_F9);
        bool is_down = (keyState & 0x8000) != 0;
        if (is_down && !s_f9_was_down) {
            triggered = true;
        }
        s_f9_was_down = is_down;
    } else if (g_currentHotkeyPreset == 1) {
        SHORT keyState = GetAsyncKeyState(VK_F10);
        bool is_down = (keyState & 0x8000) != 0;
        if (is_down && !s_f10_was_down) {
            triggered = true;
        }
        s_f10_was_down = is_down;
    }

    return triggered;
}

bool imgui_window_poll_pause_hotkey(void) {
    bool triggered = g_hotkey_pause_triggered;
    g_hotkey_pause_triggered = false;
    return triggered;
}
