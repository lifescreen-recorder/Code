#include "imgui_bridge.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <psapi.h>
#include <math.h>
#include "camera_engine.h"

extern "C" void lifescreen_dsp_test_mic_process_ex(
    float* samples,
    uint32_t frame_count,
    uint32_t channels,
    float sample_rate,
    bool noise_suppression,
    uint32_t ns_mode,
    float ns_strength,
    bool compressor,
    float comp_amount,
    float gain_db,
    float volume
);

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

struct AppAudioSession {
    DWORD pid;
    char name[128];
    float volume;
    bool mute;
    bool valid;
};

#define MAX_AUDIO_SESSIONS 32
static AppAudioSession g_audioSessions[MAX_AUDIO_SESSIONS];
static int g_audioSessionCount = 0;
static DWORD g_lastAudioSessionRefresh = 0;
static bool g_showAppMixerPopup = false;
static bool g_showAudioFiltersPopup = false;
static bool g_showCameraOverlayPopup = false;
static bool g_micTestActive = false;
static volatile float g_micTestLevel = 0.0f;
static volatile LONG g_speakerTestRunning = 0;
static volatile LONG g_micTestRunning = 0;
static HANDLE g_micTestThread = NULL;

struct SpeakerTestParams {
    int device_index;
    float volume;
};

struct MicTestParams {
    int input_index;
    int output_index;
    volatile bool noise_suppression;
    volatile int ns_mode;
    volatile float ns_strength;
    volatile bool compressor;
    volatile float comp_amount;
    volatile float gain_db;
    volatile float volume;
};

static MicTestParams g_micTestParams = {0};

static IMMDevice* get_mmdevice_by_index(IMMDeviceEnumerator* pEnum, EDataFlow flow, int index) {
    if (!pEnum) return NULL;
    IMMDevice* pDevice = NULL;
    if (index <= 0) {
        pEnum->GetDefaultAudioEndpoint(flow, eMultimedia, &pDevice);
        return pDevice;
    }
    IMMDeviceCollection* pCol = NULL;
    if (SUCCEEDED(pEnum->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &pCol)) && pCol) {
        UINT count = 0;
        pCol->GetCount(&count);
        UINT target = (UINT)(index - 1);
        if (target < count) {
            pCol->Item(target, &pDevice);
        }
        pCol->Release();
    }
    if (!pDevice) {
        pEnum->GetDefaultAudioEndpoint(flow, eMultimedia, &pDevice);
    }
    return pDevice;
}

static DWORD WINAPI speaker_test_thread_proc(LPVOID lpParam) {
    SpeakerTestParams* p = (SpeakerTestParams*)lpParam;
    int dev_idx = p ? p->device_index : 0;
    float vol = p ? p->volume : 1.0f;
    if (p) free(p);

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* pEnum = NULL;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum)) && pEnum) {
        IMMDevice* pDev = get_mmdevice_by_index(pEnum, eRender, dev_idx);
        pEnum->Release();
        if (pDev) {
            IAudioClient* pClient = NULL;
            if (SUCCEEDED(pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pClient)) && pClient) {
                WAVEFORMATEX* pwfx = NULL;
                if (SUCCEEDED(pClient->GetMixFormat(&pwfx)) && pwfx) {
                    if (SUCCEEDED(pClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, pwfx, NULL))) {
                        IAudioRenderClient* pRender = NULL;
                        UINT32 bufFrames = 0;
                        pClient->GetBufferSize(&bufFrames);
                        if (SUCCEEDED(pClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRender)) && pRender) {
                            pClient->Start();
                            const double sr = (double)pwfx->nSamplesPerSec;
                            const int ch = pwfx->nChannels;
                            const bool is_float = (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                                (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->wBitsPerSample == 32);
                            const double freqs[3] = { 523.25, 659.25, 783.99 };
                            const UINT32 total_frames = (UINT32)(sr * 0.72);
                            UINT32 written = 0;
                            double phase = 0.0;
                            float gain = (vol > 1.5f ? 1.5f : (vol < 0.05f ? 0.25f : vol)) * 0.22f;

                            while (written < total_frames) {
                                UINT32 pad = 0;
                                pClient->GetCurrentPadding(&pad);
                                UINT32 avail = (bufFrames > pad) ? (bufFrames - pad) : 0;
                                if (avail == 0) {
                                    Sleep(5);
                                    continue;
                                }
                                UINT32 to_write = total_frames - written;
                                if (to_write > avail) to_write = avail;
                                BYTE* pData = NULL;
                                if (SUCCEEDED(pRender->GetBuffer(to_write, &pData)) && pData) {
                                    for (UINT32 i = 0; i < to_write; i++) {
                                        UINT32 idx = written + i;
                                        double t = (double)idx / sr;
                                        int note = (t < 0.24) ? 0 : (t < 0.48 ? 1 : 2);
                                        double nt = t - note * 0.24;
                                        double env = 1.0;
                                        if (nt < 0.015) env = nt / 0.015;
                                        else if (nt > 0.18) env = (0.24 - nt) / 0.06;
                                        if (env < 0.0) env = 0.0;
                                        phase += 2.0 * 3.14159265358979323846 * freqs[note] / sr;
                                        float s = (float)(sin(phase) * env * gain);
                                        if (is_float) {
                                            float* fbuf = (float*)pData;
                                            for (int c = 0; c < ch; c++) fbuf[i * ch + c] = s;
                                        } else {
                                            short* sbuf = (short*)pData;
                                            short sv = (short)(s * 32767.0f);
                                            for (int c = 0; c < ch; c++) sbuf[i * ch + c] = sv;
                                        }
                                    }
                                    pRender->ReleaseBuffer(to_write, 0);
                                    written += to_write;
                                } else {
                                    break;
                                }
                            }
                            Sleep(220);
                            pClient->Stop();
                            pRender->Release();
                        }
                    }
                    CoTaskMemFree(pwfx);
                }
                pClient->Release();
            }
            pDev->Release();
        }
    }
    CoUninitialize();
    InterlockedExchange(&g_speakerTestRunning, 0);
    return 0;
}

static void trigger_speaker_test(int device_index, float volume) {
    if (InterlockedCompareExchange(&g_speakerTestRunning, 1, 0) != 0) return;
    SpeakerTestParams* p = (SpeakerTestParams*)malloc(sizeof(SpeakerTestParams));
    if (!p) {
        InterlockedExchange(&g_speakerTestRunning, 0);
        return;
    }
    p->device_index = device_index;
    p->volume = volume;
    HANDLE h = CreateThread(NULL, 0, speaker_test_thread_proc, p, 0, NULL);
    if (h) CloseHandle(h);
    else {
        free(p);
        InterlockedExchange(&g_speakerTestRunning, 0);
    }
}

static DWORD WINAPI mic_test_thread_proc(LPVOID lpParam) {
    (void)lpParam;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* pEnum = NULL;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum)) && pEnum) {
        IMMDevice* pCapDev = get_mmdevice_by_index(pEnum, eCapture, g_micTestParams.input_index);
        IMMDevice* pRenDev = get_mmdevice_by_index(pEnum, eRender, g_micTestParams.output_index);
        pEnum->Release();

        IAudioClient* pCapClient = NULL;
        IAudioClient* pRenClient = NULL;
        if (pCapDev && pRenDev &&
            SUCCEEDED(pCapDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pCapClient)) &&
            SUCCEEDED(pRenDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pRenClient))) {
            WAVEFORMATEX* capWfx = NULL;
            WAVEFORMATEX* renWfx = NULL;
            if (SUCCEEDED(pCapClient->GetMixFormat(&capWfx)) && SUCCEEDED(pRenClient->GetMixFormat(&renWfx)) && capWfx && renWfx) {
                if (SUCCEEDED(pCapClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, capWfx, NULL)) &&
                    SUCCEEDED(pRenClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, renWfx, NULL))) {
                    IAudioCaptureClient* pCap = NULL;
                    IAudioRenderClient* pRen = NULL;
                    UINT32 renBufFrames = 0;
                    pRenClient->GetBufferSize(&renBufFrames);
                    if (SUCCEEDED(pCapClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCap)) &&
                        SUCCEEDED(pRenClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRen))) {
                        pCapClient->Start();
                        pRenClient->Start();
                        float tmpBuf[4096 * 2];
                        while (InterlockedCompareExchange(&g_micTestRunning, 1, 1) == 1) {
                            UINT32 pktLen = 0;
                            if (FAILED(pCap->GetNextPacketSize(&pktLen)) || pktLen == 0) {
                                Sleep(4);
                                continue;
                            }
                            while (pktLen > 0 && InterlockedCompareExchange(&g_micTestRunning, 1, 1) == 1) {
                                BYTE* capData = NULL;
                                UINT32 numFrames = 0;
                                DWORD flags = 0;
                                if (FAILED(pCap->GetBuffer(&capData, &numFrames, &flags, NULL, NULL))) break;
                                UINT32 procFrames = (numFrames > 4096) ? 4096 : numFrames;
                                UINT32 capCh = capWfx->nChannels ? capWfx->nChannels : 1;
                                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                                    memset(tmpBuf, 0, procFrames * 2 * sizeof(float));
                                } else {
                                    const float* fsrc = (const float*)capData;
                                    for (UINT32 i = 0; i < procFrames; i++) {
                                        float l = fsrc[i * capCh];
                                        float r = (capCh > 1) ? fsrc[i * capCh + 1] : l;
                                        tmpBuf[i * 2] = l;
                                        tmpBuf[i * 2 + 1] = r;
                                    }
                                }
                                pCap->ReleaseBuffer(numFrames);

                                lifescreen_dsp_test_mic_process_ex(
                                    tmpBuf,
                                    procFrames,
                                    2,
                                    (float)capWfx->nSamplesPerSec,
                                    g_micTestParams.noise_suppression,
                                    (uint32_t)g_micTestParams.ns_mode,
                                    g_micTestParams.ns_strength,
                                    g_micTestParams.compressor,
                                    g_micTestParams.comp_amount,
                                    g_micTestParams.gain_db,
                                    g_micTestParams.volume
                                );

                                float peak_after = 0.0f;
                                for (UINT32 i = 0; i < procFrames * 2; i++) {
                                    float av = tmpBuf[i] < 0.0f ? -tmpBuf[i] : tmpBuf[i];
                                    if (av > peak_after) peak_after = av;
                                }
                                if (peak_after > 1.0f) peak_after = 1.0f;
                                g_micTestLevel = peak_after;

                                UINT32 pad = 0;
                                pRenClient->GetCurrentPadding(&pad);
                                UINT32 avail = (renBufFrames > pad) ? (renBufFrames - pad) : 0;
                                UINT32 outFrames = (procFrames < avail) ? procFrames : avail;
                                if (outFrames > 0) {
                                    BYTE* renData = NULL;
                                    if (SUCCEEDED(pRen->GetBuffer(outFrames, &renData)) && renData) {
                                        float* fdst = (float*)renData;
                                        UINT32 renCh = renWfx->nChannels ? renWfx->nChannels : 2;
                                        for (UINT32 i = 0; i < outFrames; i++) {
                                            float l = tmpBuf[i * 2];
                                            float r = tmpBuf[i * 2 + 1];
                                            for (UINT32 c = 0; c < renCh; c++) {
                                                fdst[i * renCh + c] = (c == 0) ? l : r;
                                            }
                                        }
                                        pRen->ReleaseBuffer(outFrames, 0);
                                    }
                                }
                                pCap->GetNextPacketSize(&pktLen);
                            }
                        }
                        pCapClient->Stop();
                        pRenClient->Stop();
                        pCap->Release();
                        pRen->Release();
                    }
                }
                if (capWfx) CoTaskMemFree(capWfx);
                if (renWfx) CoTaskMemFree(renWfx);
            }
        }
        if (pCapClient) pCapClient->Release();
        if (pRenClient) pRenClient->Release();
        if (pCapDev) pCapDev->Release();
        if (pRenDev) pRenDev->Release();
    }
    CoUninitialize();
    return 0;
}

static void update_mic_test_state(bool want_active, const UiState* state) {
    g_micTestParams.input_index = state->selected_audio_input;
    g_micTestParams.output_index = state->selected_audio_output;
    g_micTestParams.noise_suppression = state->mic_noise_suppression;
    g_micTestParams.ns_mode = state->mic_ns_mode;
    g_micTestParams.ns_strength = state->mic_ns_strength;
    g_micTestParams.compressor = state->mic_compressor;
    g_micTestParams.comp_amount = state->mic_comp_amount;
    g_micTestParams.gain_db = state->mic_gain_db;
    g_micTestParams.volume = state->mic_volume;

    if (want_active && !g_micTestThread) {
        InterlockedExchange(&g_micTestRunning, 1);
        g_micTestThread = CreateThread(NULL, 0, mic_test_thread_proc, NULL, 0, NULL);
        if (!g_micTestThread) InterlockedExchange(&g_micTestRunning, 0);
    } else if (!want_active && g_micTestThread) {
        InterlockedExchange(&g_micTestRunning, 0);
        WaitForSingleObject(g_micTestThread, 500);
        CloseHandle(g_micTestThread);
        g_micTestThread = NULL;
        g_micTestLevel = 0.0f;
    }
}

static void refresh_audio_sessions(void) {
    IMMDeviceEnumerator *pEnumerator = NULL;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator
    );
    if (FAILED(hr) || !pEnumerator) return;

    IMMDevice *pDevice = NULL;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
    pEnumerator->Release();
    if (FAILED(hr) || !pDevice) return;

    IAudioSessionManager2 *pSessionManager = NULL;
    hr = pDevice->Activate(
        __uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&pSessionManager
    );
    pDevice->Release();
    if (FAILED(hr) || !pSessionManager) return;

    IAudioSessionEnumerator *pSessionEnum = NULL;
    hr = pSessionManager->GetSessionEnumerator(&pSessionEnum);
    pSessionManager->Release();
    if (FAILED(hr) || !pSessionEnum) return;

    int count = 0;
    pSessionEnum->GetCount(&count);

    g_audioSessionCount = 0;
    DWORD currentPid = GetCurrentProcessId();

    for (int i = 0; i < count && g_audioSessionCount < MAX_AUDIO_SESSIONS; i++) {
        IAudioSessionControl *pBasicControl = NULL;
        if (FAILED(pSessionEnum->GetSession(i, &pBasicControl)) || !pBasicControl) continue;

        IAudioSessionControl2 *pControl2 = NULL;
        hr = pBasicControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pControl2);
        pBasicControl->Release();
        if (FAILED(hr) || !pControl2) continue;

        AudioSessionState state;
        pControl2->GetState(&state);
        if (state == AudioSessionStateExpired) {
            pControl2->Release();
            continue;
        }

        DWORD pid = 0;
        pControl2->GetProcessId(&pid);

        if (pid == currentPid) {
            pControl2->Release();
            continue;
        }

        char appName[128] = "System Sounds";
        if (pControl2->IsSystemSoundsSession() == S_OK) {
            snprintf(appName, sizeof(appName), "System Sounds");
        } else if (pid > 0) {
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProcess) {
                char fullPath[MAX_PATH] = {0};
                DWORD pathLen = MAX_PATH;
                if (QueryFullProcessImageNameA(hProcess, 0, fullPath, &pathLen)) {
                    const char *slash = strrchr(fullPath, '\\');
                    const char *base = slash ? slash + 1 : fullPath;
                    snprintf(appName, sizeof(appName), "%s", base);
                }
                CloseHandle(hProcess);
            } else {
                snprintf(appName, sizeof(appName), "App (PID %lu)", pid);
            }
        }

        ISimpleAudioVolume *pSimpleVolume = NULL;
        hr = pControl2->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pSimpleVolume);
        if (SUCCEEDED(hr) && pSimpleVolume) {
            float vol = 1.0f;
            BOOL mute = FALSE;
            pSimpleVolume->GetMasterVolume(&vol);
            pSimpleVolume->GetMute(&mute);

            bool exists = false;
            for (int s = 0; s < g_audioSessionCount; s++) {
                if (g_audioSessions[s].pid == pid && pid != 0) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                AppAudioSession &sess = g_audioSessions[g_audioSessionCount++];
                sess.pid = pid;
                snprintf(sess.name, sizeof(sess.name), "%s", appName);
                sess.volume = vol;
                sess.mute = mute ? true : false;
                sess.valid = true;
            }
            pSimpleVolume->Release();
        }
        pControl2->Release();
    }

    pSessionEnum->Release();
    g_lastAudioSessionRefresh = GetTickCount();
}

static void apply_app_volume(DWORD pid, float volume, bool mute) {
    if (pid == 0) return;

    IMMDeviceEnumerator *pEnumerator = NULL;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator)) || !pEnumerator) return;

    IMMDevice *pDevice = NULL;
    if (FAILED(pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice)) || !pDevice) {
        pEnumerator->Release();
        return;
    }
    pEnumerator->Release();

    IAudioSessionManager2 *pSessionManager = NULL;
    if (FAILED(pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&pSessionManager)) || !pSessionManager) {
        pDevice->Release();
        return;
    }
    pDevice->Release();

    IAudioSessionEnumerator *pSessionEnum = NULL;
    if (FAILED(pSessionManager->GetSessionEnumerator(&pSessionEnum)) || !pSessionEnum) {
        pSessionManager->Release();
        return;
    }
    pSessionManager->Release();

    int count = 0;
    pSessionEnum->GetCount(&count);

    for (int i = 0; i < count; i++) {
        IAudioSessionControl *pBasicControl = NULL;
        if (FAILED(pSessionEnum->GetSession(i, &pBasicControl)) || !pBasicControl) continue;

        IAudioSessionControl2 *pControl2 = NULL;
        HRESULT hr = pBasicControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pControl2);
        pBasicControl->Release();
        if (FAILED(hr) || !pControl2) continue;

        DWORD sessionPid = 0;
        pControl2->GetProcessId(&sessionPid);

        if (sessionPid == pid) {
            ISimpleAudioVolume *pSimpleVolume = NULL;
            hr = pControl2->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pSimpleVolume);
            if (SUCCEEDED(hr) && pSimpleVolume) {
                pSimpleVolume->SetMasterVolume(volume, NULL);
                pSimpleVolume->SetMute(mute ? TRUE : FALSE, NULL);
                pSimpleVolume->Release();
            }
            pControl2->Release();
            break;
        }
        pControl2->Release();
    }
    pSessionEnum->Release();
}

static void render_app_audio_mixer_ui(int lang) {
    DWORD now = GetTickCount();
    if (now - g_lastAudioSessionRefresh > 2000 || g_audioSessionCount == 0) {
        refresh_audio_sessions();
    }

    ImGui::TextColored(ImVec4(0.20f, 0.65f, 1.00f, 1.00f), "%s",
        tr(lang,
            "Per-Application Volume Mixer (Adjust before recording):",
            "Гучність активних додатків (налаштування перед записом):",
            "活动应用程序音量独立调节 (录制前设置):",
            "Lautstärke aktiver Anwendungen (Vor Aufnahme anpassen):",
            "Mixeur de volume par application (Ajuster avant l'enregistrement) :",
            "Głośność aktywnych aplikacji (Dostosuj przed nagrywaniem):"));
    ImGui::Spacing();

    if (ImGui::Button(tr(lang, "Refresh Application List", "Оновити список додатків", "刷新应用程序列表", "Anwendungsliste aktualisieren", "Actualiser la liste", "Odśwież listę aplikacji"))) {
        refresh_audio_sessions();
    }

    ImGui::Spacing();

    if (g_audioSessionCount == 0) {
        ImGui::TextDisabled("%s", tr(lang, "No active audio-producing applications detected.", "Немає активних додатків, що відтворюють звук.", "未检测到正在发声的应用程序。", "Keine aktiven Audioanwendungen erkannt.", "Aucune application audio active détectée.", "Nie wykryto aplikacji odtwarzających dźwięk."));
    } else {
        for (int i = 0; i < g_audioSessionCount; i++) {
            AppAudioSession &sess = g_audioSessions[i];
            ImGui::PushID(i * 100);

            bool mute = sess.mute;
            if (ImGui::Checkbox("##mute", &mute)) {
                sess.mute = mute;
                apply_app_volume(sess.pid, sess.volume, sess.mute);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", sess.mute ? tr(lang, "Unmute", "Увімкнути звук", "取消静音", "Stummschaltung aufheben", "Rétablir le son", "Wyłącz wyciszenie") : tr(lang, "Mute", "Вимкнути звук", "静音", "Stummschalten", "Couper le son", "Wycisz"));
            }

            ImGui::SameLine();
            int vol_percent = (int)(sess.volume * 100.0f + 0.5f);
            char label[256];
            snprintf(label, sizeof(label), "%s (%d%%)", sess.name, vol_percent);

            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderInt(label, &vol_percent, 0, 100)) {
                sess.volume = vol_percent / 100.0f;
                apply_app_volume(sess.pid, sess.volume, sess.mute);
            }

            ImGui::PopID();
        }
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
static ID3D11Texture2D*          g_pPreviewSharedOpenedTexture = NULL;
static ID3D11Texture2D*          g_pPreviewBridgeSrcTex = NULL;
static ID3D11Device*             g_pPreviewBridgeSrcDev = NULL;
static int                       g_previewBridgeW = 0;
static int                       g_previewBridgeH = 0;
static HANDLE                    g_previewSharedHandle = NULL;
static int                       g_previewWidth = 0;
static int                       g_previewHeight = 0;
static bool                      g_previewEnabled = false;
static bool                      g_skipCurrentFrameRender = false;
static bool                      g_isCurrentlyRecording = false;
static bool                      g_previewPaused = false;
static uint64_t                  g_previewFrameCount = 0;
static CRITICAL_SECTION          g_previewCs;
static bool                      g_previewCsInit = false;

static void reset_preview_shared_cache(void) {
    if (g_pPreviewSharedOpenedTexture) {
        g_pPreviewSharedOpenedTexture->Release();
        g_pPreviewSharedOpenedTexture = NULL;
    }
    if (g_pPreviewBridgeSrcTex) {
        g_pPreviewBridgeSrcTex->Release();
        g_pPreviewBridgeSrcTex = NULL;
    }
    g_pPreviewBridgeSrcDev = NULL;
    g_previewBridgeW = 0;
    g_previewBridgeH = 0;
    g_previewSharedHandle = NULL;
}

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
        reset_preview_shared_cache();

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
            D3D11_TEXTURE2D_DESC srcDesc = {};
            srcTex->GetDesc(&srcDesc);

            if ((srcDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED) != 0) {
                if (srcDev) srcDev->Release();
                IDXGIResource* dxgiRes = NULL;
                if (SUCCEEDED(srcTex->QueryInterface(__uuidof(IDXGIResource), (void**)&dxgiRes)) && dxgiRes) {
                    HANDLE sharedHandle = NULL;
                    if (SUCCEEDED(dxgiRes->GetSharedHandle(&sharedHandle)) && sharedHandle) {
                        if (g_previewSharedHandle != sharedHandle || !g_pPreviewSharedOpenedTexture) {
                            reset_preview_shared_cache();
                            if (FAILED(g_pd3dDevice->OpenSharedResource(sharedHandle, __uuidof(ID3D11Texture2D), (void**)&g_pPreviewSharedOpenedTexture)) || !g_pPreviewSharedOpenedTexture) {
                                g_previewSharedHandle = NULL;
                            } else {
                                g_previewSharedHandle = sharedHandle;
                            }
                        }
                        if (g_pPreviewSharedOpenedTexture) {
                            g_pd3dDeviceContext->CopyResource((ID3D11Resource*)g_pPreviewTexture, (ID3D11Resource*)g_pPreviewSharedOpenedTexture);
                        }
                    } else {
                        reset_preview_shared_cache();
                    }
                    dxgiRes->Release();
                } else {
                    reset_preview_shared_cache();
                }
            } else if (srcDev) {
                if (!g_pPreviewBridgeSrcTex || g_pPreviewBridgeSrcDev != srcDev || g_previewBridgeW != req_w || g_previewBridgeH != req_h || !g_pPreviewSharedOpenedTexture) {
                    reset_preview_shared_cache();
                    D3D11_TEXTURE2D_DESC bdesc = {};
                    bdesc.Width = (UINT)req_w;
                    bdesc.Height = (UINT)req_h;
                    bdesc.MipLevels = 1;
                    bdesc.ArraySize = 1;
                    bdesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    bdesc.SampleDesc.Count = 1;
                    bdesc.Usage = D3D11_USAGE_DEFAULT;
                    bdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                    bdesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

                    if (SUCCEEDED(srcDev->CreateTexture2D(&bdesc, NULL, &g_pPreviewBridgeSrcTex)) && g_pPreviewBridgeSrcTex) {
                        IDXGIResource* dxgiRes = NULL;
                        if (SUCCEEDED(g_pPreviewBridgeSrcTex->QueryInterface(__uuidof(IDXGIResource), (void**)&dxgiRes)) && dxgiRes) {
                            HANDLE sharedHandle = NULL;
                            if (SUCCEEDED(dxgiRes->GetSharedHandle(&sharedHandle)) && sharedHandle) {
                                if (SUCCEEDED(g_pd3dDevice->OpenSharedResource(sharedHandle, __uuidof(ID3D11Texture2D), (void**)&g_pPreviewSharedOpenedTexture)) && g_pPreviewSharedOpenedTexture) {
                                    g_previewSharedHandle = sharedHandle;
                                    g_pPreviewBridgeSrcDev = srcDev;
                                    g_previewBridgeW = req_w;
                                    g_previewBridgeH = req_h;
                                }
                            }
                            dxgiRes->Release();
                        }
                    }
                }
                if (g_pPreviewBridgeSrcTex && g_pPreviewSharedOpenedTexture) {
                    ID3D11DeviceContext* srcCtx = NULL;
                    srcDev->GetImmediateContext(&srcCtx);
                    if (srcCtx) {
                        srcCtx->CopyResource((ID3D11Resource*)g_pPreviewBridgeSrcTex, (ID3D11Resource*)srcTex);
                        srcCtx->Flush();
                        srcCtx->Release();
                        g_pd3dDeviceContext->CopyResource((ID3D11Resource*)g_pPreviewTexture, (ID3D11Resource*)g_pPreviewSharedOpenedTexture);
                    }
                }
                srcDev->Release();
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

    style.WindowRounding    = 10.0f;
    style.ChildRounding     = 10.0f;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = 8.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 6.0f;

    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;

    style.WindowPadding     = ImVec2(14.0f, 12.0f);
    style.FramePadding      = ImVec2(10.0f, 5.0f);
    style.ItemSpacing       = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 5.0f);

    colors[ImGuiCol_Text]                  = ImVec4(0.94f, 0.96f, 0.99f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.55f, 0.60f, 0.68f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.05f, 0.07f, 0.09f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.08f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.09f, 0.12f, 0.16f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.16f, 0.20f, 0.26f, 0.95f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.12f, 0.15f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.16f, 0.20f, 0.27f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.20f, 0.26f, 0.35f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.09f, 0.12f, 0.16f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.08f, 0.10f, 0.13f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.06f, 0.08f, 0.10f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.20f, 0.25f, 0.32f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.26f, 0.32f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.32f, 0.39f, 0.50f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.24f, 0.66f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.24f, 0.66f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.42f, 0.76f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.14f, 0.18f, 0.24f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.19f, 0.25f, 0.33f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.23f, 0.30f, 0.40f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.15f, 0.20f, 0.27f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.20f, 0.27f, 0.36f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.24f, 0.32f, 0.43f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.16f, 0.20f, 0.26f, 0.90f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.08f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.15f, 0.20f, 0.27f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.14f, 0.19f, 0.26f, 1.00f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.07f, 0.09f, 0.12f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.11f, 0.14f, 0.19f, 1.00f);
    colors[ImGuiCol_PlotHistogram]         = ImVec4(0.18f, 0.78f, 0.45f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.24f, 0.88f, 0.52f, 1.00f);
}

bool imgui_window_create(int width, int height, const char* title) {
    HINSTANCE hInst = GetModuleHandle(NULL);
    HICON hEmbeddedIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    if (!hEmbeddedIcon) {
        hEmbeddedIcon = (HICON)LoadImageW(NULL, L"resources\\icon.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    }
    if (!hEmbeddedIcon) {
        hEmbeddedIcon = (HICON)LoadImageW(NULL, L"..\\resources\\icon.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    }

    WNDCLASSEXW wc = {
        sizeof(WNDCLASSEXW),
        CS_CLASSDC,
        WndProc,
        0L, 0L,
        hInst,
        hEmbeddedIcon,
        LoadCursor(NULL, IDC_ARROW),
        NULL, NULL,
        L"LifeScreenRecorderClass",
        hEmbeddedIcon
    };
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
    sd.SwapEffect = (DXGI_SWAP_EFFECT)4;

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

    if (hr != S_OK) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = D3D11CreateDeviceAndSwapChain(
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
    }

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
        DWORD bg_interval = g_isCurrentlyRecording ? 1000 : 200;
        if (now - s_lastBgFrameTick < bg_interval) {
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

static void draw_led_vu_meter(const char* id, float level, bool enabled, float width, float height) {
    ImGui::PushID(id);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (width <= 0.0f) width = ImGui::GetContentRegionAvail().x;
    if (height <= 0.0f) height = 22.0f;
    ImVec2 p1 = ImVec2(p0.x + width, p0.y + height);
    ImGui::Dummy(ImVec2(width, height));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float clamped = enabled ? (level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level)) : 0.0f;

    dl->AddRectFilled(p0, p1, IM_COL32(15, 20, 27, 255), 5.0f);
    dl->AddRect(p0, p1, IM_COL32(40, 49, 61, 255), 5.0f, 0, 1.0f);

    float pad = 3.0f;
    float inner_w = width - pad * 2.0f;
    float fill_w = inner_w * clamped;
    if (fill_w > 1.0f) {
        float g_end = inner_w * 0.70f;
        float a_end = inner_w * 0.90f;
        float y0 = p0.y + pad;
        float y1 = p1.y - pad;

        float w1 = (fill_w < g_end) ? fill_w : g_end;
        if (w1 > 0.5f) {
            dl->AddRectFilled(ImVec2(p0.x + pad, y0), ImVec2(p0.x + pad + w1, y1), IM_COL32(46, 204, 113, 255), 3.0f);
        }
        if (fill_w > g_end) {
            float x_start = p0.x + pad + g_end;
            float w2 = ((fill_w < a_end) ? fill_w : a_end) - g_end;
            if (w2 > 0.5f) {
                dl->AddRectFilled(ImVec2(x_start, y0), ImVec2(x_start + w2, y1), IM_COL32(241, 196, 15, 255), 2.0f);
            }
        }
        if (fill_w > a_end) {
            float x_start = p0.x + pad + a_end;
            float w3 = fill_w - a_end;
            if (w3 > 0.5f) {
                dl->AddRectFilled(ImVec2(x_start, y0), ImVec2(x_start + w3, y1), IM_COL32(231, 76, 60, 255), 2.0f);
            }
        }
    }

    int segs = 8;
    for (int s = 1; s < segs; s++) {
        float sx = p0.x + pad + (inner_w * (float)s / (float)segs);
        dl->AddLine(ImVec2(sx, p0.y + 2.0f), ImVec2(sx, p1.y - 2.0f), IM_COL32(15, 20, 27, 170), 1.0f);
    }

    char pct_buf[16];
    snprintf(pct_buf, sizeof(pct_buf), "%d%%", (int)(clamped * 100.0f + 0.5f));
    ImVec2 tsz = ImGui::CalcTextSize(pct_buf);
    ImVec2 tpos = ImVec2(p0.x + (width - tsz.x) * 0.5f, p0.y + (height - tsz.y) * 0.5f);
    dl->AddText(tpos, enabled ? IM_COL32(235, 242, 250, 230) : IM_COL32(115, 125, 140, 180), pct_buf);

    ImGui::PopID();
}

static void draw_telemetry_badge(const char* id, const char* label, const char* value, ImVec4 accent_col, float width, float height) {
    ImGui::PushID(id);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1 = ImVec2(p0.x + width, p0.y + height);
    ImGui::Dummy(ImVec2(width, height));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(18, 24, 33, 255), 7.0f);
    dl->AddRect(p0, p1, IM_COL32(42, 52, 66, 230), 7.0f, 0, 1.0f);

    ImU32 acc_u32 = ImGui::ColorConvertFloat4ToU32(accent_col);
    dl->AddRectFilled(ImVec2(p0.x + 2.0f, p0.y + 5.0f), ImVec2(p0.x + 5.5f, p1.y - 5.0f), acc_u32, 2.0f);

    ImVec2 lsz = ImGui::CalcTextSize(label);
    ImVec2 vsz = ImGui::CalcTextSize(value);
    if (lsz.x + vsz.x + 28.0f <= width && height < 44.0f) {
        float tx = p0.x + 12.0f;
        float ty = p0.y + (height - vsz.y) * 0.5f;
        dl->AddText(ImVec2(tx, ty), IM_COL32(145, 158, 178, 255), label);
        dl->AddText(ImVec2(tx + lsz.x + 6.0f, ty), acc_u32, value);
    } else {
        dl->AddText(ImVec2(p0.x + 12.0f, p0.y + 5.0f), IM_COL32(145, 158, 178, 255), label);
        dl->AddText(ImVec2(p0.x + 12.0f, p0.y + height - vsz.y - 5.0f), acc_u32, value);
    }

    ImGui::PopID();
}

extern "C" void imgui_camera_composite_d3d11(void* dest_texture, int screen_w, int screen_h) {
    if (!g_pd3dDevice || !g_pd3dDeviceContext || !dest_texture) return;
    camera_engine_composite_d3d11(g_pd3dDevice, g_pd3dDeviceContext, dest_texture, screen_w, screen_h);
}

extern "C" void imgui_camera_composite_sw_frame(uint8_t* dst_y, uint8_t* dst_uv, int stride_y, int stride_uv, int width, int height, bool is_nv12) {
    camera_engine_composite_sw_frame(dst_y, dst_uv, stride_y, stride_uv, width, height, is_nv12);
}

extern "C" bool imgui_camera_is_enabled(void) {
    CameraOverlayConfig cfg;
    camera_engine_get_settings(&cfg);
    return cfg.enabled;
}

static bool pick_overlay_file_dialog(char* out_path, size_t max_len, HWND owner) {
    wchar_t file_buf[512] = {0};
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = 511;
    ofn.lpstrFilter = L"Images & Overlays (*.png;*.gif;*.jpg;*.jpeg;*.bmp;*.webp)\0*.png;*.gif;*.jpg;*.jpeg;*.bmp;*.webp\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        WideCharToMultiByte(CP_UTF8, 0, file_buf, -1, out_path, (int)max_len, NULL, NULL);
        return true;
    }
    return false;
}

static void render_process_audio_controls(UiState* state, int lang) {
    const char* sys_modes[3] = {
        tr(lang, "All System Audio (Desktop Loopback)", "Весь системний звук", "全部系统声音", "Gesamter Systemton", "Tout l'audio système", "Cały dźwięk systemowy"),
        tr(lang, "Only Target Game / App (No Discord/Browser!)", "Тільки обрана гра / програма (без Discord!)", "仅录制目标游戏/应用 (不含语音软件)", "Nur Ziel-Spiel / App", "Uniquement le jeu / l'application cible", "Tylko wybrana gra / aplikacja"),
        tr(lang, "Exclude Selected App (Mute Discord in Video)", "Усе КРІМ обраної програми (вимкнути Discord у записі)", "排除指定应用 (录制中屏蔽语音软件)", "Ausgewählte App ausschließen", "Exclure l'application choisie", "Wyklucz wybraną aplikację")
    };
    if (ImGui::Combo(tr(lang, "Capture Mode", "Режим захоплення", "捕获模式", "Aufnahmemodus", "Mode capture", "Tryb przechwytywania"), &state->sys_audio_mode, sys_modes, 3)) {
        if (state->sys_audio_mode != 0) refresh_audio_sessions();
        state->action_settings_changed = true;
    }
    if (state->sys_audio_mode != 1 && state->sys_audio_mode != 2) return;
    DWORD now_t = GetTickCount();
    if (now_t - g_lastAudioSessionRefresh > 2500 || g_audioSessionCount == 0) {
        refresh_audio_sessions();
        g_lastAudioSessionRefresh = now_t;
    }
    const char* auto_lbl = tr(lang, "[Auto: Selected Window / Game]", "[Авто: Обране вікно / гра]", "[自动：所选窗口/游戏]", "[Auto: Gewähltes Fenster/Spiel]", "[Auto : Fenêtre / Jeu choisi]", "[Auto: Wybrane okno / gra]");
    char cur_proc_lbl[192];
    if (state->sys_target_pid == 0) {
        snprintf(cur_proc_lbl, sizeof(cur_proc_lbl), "%s", auto_lbl);
    } else {
        snprintf(cur_proc_lbl, sizeof(cur_proc_lbl), "PID %u", (unsigned int)state->sys_target_pid);
        for (int s = 0; s < g_audioSessionCount; s++) {
            if (g_audioSessions[s].valid && g_audioSessions[s].pid == state->sys_target_pid) {
                snprintf(cur_proc_lbl, sizeof(cur_proc_lbl), "%s (PID %u)", g_audioSessions[s].name, (unsigned int)g_audioSessions[s].pid);
                break;
            }
        }
    }
    if (ImGui::BeginCombo(tr(lang, "Target Process", "Програма", "目标进程", "Zielprozess", "Processus cible", "Proces docelowy"), cur_proc_lbl)) {
        if (ImGui::Selectable(auto_lbl, state->sys_target_pid == 0)) {
            state->sys_target_pid = 0;
            state->action_settings_changed = true;
        }
        for (int s = 0; s < g_audioSessionCount; s++) {
            if (!g_audioSessions[s].valid) continue;
            char item_lbl[192];
            snprintf(item_lbl, sizeof(item_lbl), "%s (PID %u)", g_audioSessions[s].name, (unsigned int)g_audioSessions[s].pid);
            if (ImGui::Selectable(item_lbl, state->sys_target_pid == g_audioSessions[s].pid)) {
                state->sys_target_pid = g_audioSessions[s].pid;
                state->action_settings_changed = true;
            }
        }
        ImGui::EndCombo();
    }
}

static void render_camera_controls(UiState* state, int lang) {
    if (!state) return;

    camera_engine_init();
    state->camera_device_names = camera_engine_get_device_names();
    state->camera_device_count = camera_engine_get_device_count();

    float avail_w = ImGui::GetContentRegionAvail().x;
    float col_left_w = (avail_w > 560.0f) ? (avail_w * 0.58f) : avail_w;
    float col_right_w = (avail_w > 560.0f) ? (avail_w - col_left_w - 16.0f) : avail_w;

    ImGui::BeginGroup();
    ImGui::PushItemWidth(col_left_w - 20.0f);

    if (ImGui::Checkbox(
        tr(lang,
            "Enable Camera / Web Overlay",
            "Увімкнути вебкамеру / веб-оверлей",
            "启用摄像头 / 画中画",
            "Kamera- / Web-Overlay aktivieren",
            "Activer l'incrustation Caméra / Web",
            "Włącz nakładkę kamery / web"),
        &state->camera_enabled)) {
        state->action_settings_changed = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const char* source_types[3] = {
        tr(lang, "Webcam (HD Physical / Virtual Camera)", "Вебкамера (HD Фізична / Віртуальна)", "摄像头 (高清物理/虚拟)", "Webcam (HD Kamera)", "Webcam (Caméra HD)", "Kamera internetowa (HD)"),
        tr(lang, "Studio Test Pattern (OBS Card)", "Тестова таблиця студії (OBS)", "演播室测试卡 (OBS 风格)", "Studio-Testbild (OBS-Karte)", "Mire de test studio (OBS)", "Plansza testowa studia (OBS)"),
        tr(lang, "Web URL / PNG / Animated GIF Overlay", "Веб-ресурс URL / PNG / GIF Оверлей", "网页图片 URL / PNG / GIF 动图", "Web-URL / PNG / Animiertes GIF", "URL Web / PNG / GIF animé", "Zasób Web URL / PNG / GIF")
    };

    ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1.00f), "%s",
        tr(lang, "Camera / Media Source:", "Джерело камери / медіа:", "信号源类型:", "Quelle:", "Source :", "Źródło:"));
    if (ImGui::Combo("##CamSourceType", &state->camera_source_type, source_types, 3)) {
        state->action_settings_changed = true;
    }

    if (state->camera_source_type == 0) {
        ImGui::TextDisabled("%s", tr(lang, "Video Capture Device:", "Пристрій захоплення відео:", "视频捕获设备:", "Videoaufnahmegerät:", "Périphérique de capture vidéo :", "Urządzenie przechwytujące wideo:"));
        const char* cur_cam = (state->camera_device_count > 0 && state->camera_device_index < state->camera_device_count)
            ? state->camera_device_names[state->camera_device_index]
            : tr(lang, "No camera detected", "Камер не знайдено", "未检测到摄像头", "Keine Kamera erkannt", "Aucune caméra détectée", "Nie wykryto kamery");

        float ref_btn_w = 78.0f;
        ImGui::SetNextItemWidth(col_left_w - ref_btn_w - 28.0f);
        if (ImGui::BeginCombo("##CamDeviceCombo", cur_cam)) {
            for (int i = 0; i < state->camera_device_count; i++) {
                bool is_sel = (state->camera_device_index == i);
                if (ImGui::Selectable(state->camera_device_names[i], is_sel)) {
                    state->camera_device_index = i;
                    state->action_settings_changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(lang, "Refresh", "Оновити", "刷新", "Aktualis.", "Actualiser", "Odśwież"), ImVec2(ref_btn_w, 24.0f))) {
            camera_engine_refresh_devices();
            state->camera_device_names = camera_engine_get_device_names();
            state->camera_device_count = camera_engine_get_device_count();
        }
    } else if (state->camera_source_type == 2) {
        ImGui::TextDisabled("%s", tr(lang, "HTTP(S) Image/MJPEG URL or Local PNG/GIF Path:", "HTTP(S) URL (картинка/MJPEG-камера) або локальний PNG/GIF:", "HTTP(S) 图片/MJPEG 地址或本地 PNG/GIF 路径:", "HTTP(S)-URL oder lokaler PNG/GIF-Pfad:", "URL HTTP(S) ou fichier PNG/GIF local :", "URL HTTP(S) lub lokalny plik PNG/GIF:"));
        float browse_w = 68.0f;
        float reload_w = 68.0f;
        ImGui::SetNextItemWidth(col_left_w - browse_w - reload_w - 36.0f);
        if (ImGui::InputText("##CamWebUrl", state->camera_web_url, sizeof(state->camera_web_url))) {
            camera_engine_reload_web();
            state->action_settings_changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(lang, "File...", "Файл...", "文件...", "Datei...", "Fichier...", "Plik..."), ImVec2(browse_w, 24.0f))) {
            if (pick_overlay_file_dialog(state->camera_web_url, sizeof(state->camera_web_url), g_hWnd)) {
                camera_engine_reload_web();
                state->action_settings_changed = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(lang, "Reload", "Оновити", "重载", "Laden", "Recharger", "Przeładuj"), ImVec2(reload_w, 24.0f))) {
            camera_engine_reload_web();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1.00f), "%s",
        tr(lang, "Position (Snap or Drag on Live Preview):", "Розташування (Швидкі кнопки або тягни мишкою у Перегляді):", "位置 (快捷对齐或在实时预览中鼠标拖动):", "Position (Schnellwahl oder in Vorschau ziehen):", "Position (Grille rapide ou glisser dans l'aperçu) :", "Pozycja (Siatka lub przeciągnij myszką w Podglądzie):"));

    const int grid_map[3][3] = {
        { 3, 5, 2 },
        { 6, 8, 7 },
        { 1, 4, 0 }
    };
    const char* grid_lbl[3][3] = {
        { "TL##g", "TOP##g", "TR##g" },
        { "L##g",  "XY##g",  "R##g"  },
        { "BL##g", "BOT##g", "BR##g" }
    };
    for (int gy = 0; gy < 3; gy++) {
        for (int gx = 0; gx < 3; gx++) {
            if (gx > 0) ImGui::SameLine();
            int p_val = grid_map[gy][gx];
            bool active_p = (state->camera_position == p_val);
            if (active_p) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.52f, 0.92f, 1.0f));
            if (ImGui::Button(grid_lbl[gy][gx], ImVec2(48.0f, 22.0f))) {
                state->camera_position = p_val;
                if (p_val == 8) {
                    state->camera_pos_x = 0.50f;
                    state->camera_pos_y = 0.50f;
                }
                state->action_settings_changed = true;
            }
            if (active_p) ImGui::PopStyleColor();
        }
    }

    const char* pos_names[9] = {
        tr(lang, "Bottom-Right (Standard OBS)", "Знизу праворуч (Стандарт OBS)", "右下角 (OBS 标准)", "Unten rechts (Standard-OBS)", "En bas à droite (Standard OBS)", "W prawym dolnym rogu (Standard OBS)"),
        tr(lang, "Bottom-Left", "Знизу ліворуч", "左下角", "Unten links", "En bas à gauche", "W lewym dolnym rogu"),
        tr(lang, "Top-Right", "Зверху праворуч", "右上角", "Oben rechts", "En haut à droite", "W prawym górnym rogu"),
        tr(lang, "Top-Left", "Зверху ліворуч", "左上角", "Oben links", "En haut à gauche", "W lewym górnym rogu"),
        tr(lang, "Bottom-Center", "Знизу по центру", "底部居中", "Unten zentriert", "En bas au centre", "Na dole na środku"),
        tr(lang, "Top-Center", "Зверху по центру", "顶部居中", "Oben zentriert", "En haut au centre", "Na górze na środku"),
        tr(lang, "Center-Left", "По центру ліворуч", "左侧居中", "Links zentriert", "À gauche au centre", "Po lewej na środku"),
        tr(lang, "Center-Right", "По центру праворуч", "右侧居中", "Rechts zentriert", "À droite au centre", "Po prawej na środku"),
        tr(lang, "Custom / Mouse Drag (X/Y)", "Власна позиція / Мишкою (X/Y)", "自定义位置 / 鼠标拖动 (X/Y)", "Benutzerdefiniert / Maus (X/Y)", "Personnalisé / Souris (X/Y)", "Własna pozycja / Myszką (X/Y)")
    };

    if (ImGui::Combo("##CamPosition", &state->camera_position, pos_names, 9)) {
        state->action_settings_changed = true;
    }

    if (state->camera_position == 8) {
        int px = (int)(state->camera_pos_x * 100.0f + 0.5f);
        int py = (int)(state->camera_pos_y * 100.0f + 0.5f);
        if (ImGui::SliderInt(tr(lang, "Horizontal X (%)", "По горизонталі X (%)", "水平位置 X (%)", "Horizontale Position X (%)", "Position horizontale X (%)", "Pozioma pozycja X (%)"), &px, 0, 100, "%d%%")) {
            state->camera_pos_x = (float)px / 100.0f;
            state->action_settings_changed = true;
        }
        if (ImGui::SliderInt(tr(lang, "Vertical Y (%)", "По вертикалі Y (%)", "垂直位置 Y (%)", "Vertikale Position Y (%)", "Position verticale Y (%)", "Pionowa pozycja Y (%)"), &py, 0, 100, "%d%%")) {
            state->camera_pos_y = (float)py / 100.0f;
            state->action_settings_changed = true;
        }
    } else {
        if (ImGui::SliderInt(tr(lang, "Edge Margin (px)", "Відступ від краю (px)", "边缘边距 (像素)", "Randabstand (px)", "Marge du bord (px)", "Margines od krawędzi (px)"), &state->camera_margin_px, 0, 80, "%d px")) {
            state->action_settings_changed = true;
        }
    }

    int size_pct = (int)(state->camera_size_pct * 100.0f + 0.5f);
    if (ImGui::SliderInt(tr(lang, "Overlay Size (% of screen)", "Розмір оверлею (% від екрана)", "画中画尺寸 (% 屏幕宽度)", "Overlay-Größe (% des Bildschirms)", "Taille de l'incrustation (% de l'écran)", "Rozmiar nakładki (% ekranu)"), &size_pct, 8, 65, "%d%%")) {
        state->camera_size_pct = (float)size_pct / 100.0f;
        state->action_settings_changed = true;
    }

    const char* shapes[3] = {
        tr(lang, "Rectangle (Standard)", "Прямокутник (Стандарт)", "矩形 (标准)", "Rechteck (Standard)", "Rectangle (Standard)", "Prostokąt (Standard)"),
        tr(lang, "Rounded Corners", "Закруглені кути", "圆角矩形", "Abgerundete Ecken", "Coins arrondis", "Zaokrąglone rogi"),
        tr(lang, "Circle / Streamer Facecam", "Коло (Фейскам стримера)", "圆形 (主播面部摄像头)", "Kreis (Streamer-Facecam)", "Cercle (Facecam de streamer)", "Koło (Kamera twarzy streamera)")
    };

    ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1.00f), "%s",
        tr(lang, "Shape & Border:", "Форма та Рамка:", "形状与边框:", "Form & Rahmen:", "Forme & Bordure :", "Kształt i Ramka:"));
    if (ImGui::Combo("##CamShape", &state->camera_shape, shapes, 3)) {
        state->action_settings_changed = true;
    }

    if (state->camera_shape == 1) {
        if (ImGui::SliderFloat(tr(lang, "Corner Radius (px)", "Радіус кутів (px)", "圆角半径 (像素)", "Eckenradius (px)", "Rayon des coins (px)", "Promień rogów (px)"), &state->camera_corner_radius, 4.0f, 48.0f, "%.0f px")) {
            state->action_settings_changed = true;
        }
    }

    if (ImGui::SliderFloat(tr(lang, "Border Thickness (px)", "Товщина рамки (px)", "边框粗细 (像素)", "Rahmenstärke (px)", "Épaisseur de bordure (px)", "Grubość ramki (px)"), &state->camera_border_thickness, 0.0f, 10.0f, "%.0f px")) {
        state->action_settings_changed = true;
    }

    const char* border_colors[7] = {
        tr(lang, "Cyan (#0EA5E9) [LifeScreen]", "Блакитний (#0EA5E9) [LifeScreen]", "青色 (#0EA5E9) [LifeScreen]", "Cyan (#0EA5E9) [LifeScreen]", "Cyan (#0EA5E9) [LifeScreen]", "Błękitny (#0EA5E9) [LifeScreen]"),
        tr(lang, "Purple (#A855F7)", "Фіолетовий (#A855F7)", "紫色 (#A855F7)", "Lila (#A855F7)", "Violet (#A855F7)", "Fioletowy (#A855F7)"),
        tr(lang, "Green (#10B981)", "Смарагдовий (#10B981)", "绿色 (#10B981)", "Grün (#10B981)", "Vert (#10B981)", "Zielony (#10B981)"),
        tr(lang, "White (#FFFFFF)", "Білий (#FFFFFF)", "白色 (#FFFFFF)", "Weiß (#FFFFFF)", "Blanc (#FFFFFF)", "Biały (#FFFFFF)"),
        tr(lang, "Red (#EF4444)", "Червоний (#EF4444)", "红色 (#EF4444)", "Rot (#EF4444)", "Rouge (#EF4444)", "Czerwony (#EF4444)"),
        tr(lang, "Gold (#F59E0B)", "Золотий (#F59E0B)", "金色 (#F59E0B)", "Gold (#F59E0B)", "Or (#F59E0B)", "Złoty (#F59E0B)"),
        tr(lang, "Transparent / None", "Прозорий / Без рамки", "无边框", "Transparent / Kein", "Transparent / Aucun", "Przezroczysty / Brak")
    };
    const uint32_t border_col_values[7] = {
        0x0EA5E9FF, 0xA855F7FF, 0x10B981FF, 0xFFFFFFFF, 0xEF4444FF, 0xF59E0BFF, 0x00000000
    };

    int current_col_idx = 0;
    for (int ci = 0; ci < 7; ci++) {
        if (state->camera_border_color == border_col_values[ci]) {
            current_col_idx = ci;
            break;
        }
    }
    if (ImGui::Combo(tr(lang, "Border Color", "Колір рамки", "边框颜色", "Rahmenfarbe", "Couleur de bordure", "Kolor ramki"), &current_col_idx, border_colors, 7)) {
        state->camera_border_color = border_col_values[current_col_idx];
        state->action_settings_changed = true;
    }

    int op_pct = (int)(state->camera_opacity * 100.0f + 0.5f);
    if (ImGui::SliderInt(tr(lang, "Opacity (%)", "Прозорість (%)", "不透明度 (%)", "Deckkraft (%)", "Opacité (%)", "Przezroczystość (%)"), &op_pct, 15, 100, "%d%%")) {
        state->camera_opacity = (float)op_pct / 100.0f;
        state->action_settings_changed = true;
    }

    if (ImGui::Checkbox(tr(lang, "Mirror X", "Дзеркало X", "水平镜像", "Spiegeln X", "Miroir X", "Lustro X"), &state->camera_mirror)) {
        state->action_settings_changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(tr(lang, "Flip Y", "Переворот Y", "垂直翻转", "Spiegeln Y", "Miroir Y", "Obrót Y"), &state->camera_flip_vertical)) {
        state->action_settings_changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(tr(lang, "Neon Glow", "Неонове світіння", "霓虹发光", "Neon-Leuchten", "Lueur néon", "Poświata"), &state->camera_glow)) {
        state->action_settings_changed = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.55f, 1.00f), "%s",
        tr(lang, "Quality Enhancer & Studio Filters:", "Покращення якості камери та фільтри:", "画质增强与演播室滤镜:", "Qualitätsverbesserung & Filter:", "Amélioration qualité & Filtres :", "Poprawa jakości kamery i filtry:"));

    const char* color_presets[6] = {
        tr(lang, "Natural (True Color)", "Натуральний (Природні кольори)", "自然原色", "Natürlich", "Naturel", "Naturalny"),
        tr(lang, "Warm Studio (Golden Skin)", "Тепла студія (М'який тон)", "暖调演播室", "Warmes Studio", "Studio chaud", "Ciepłe studio"),
        tr(lang, "Cool Cyber (Cyan Boost)", "Холодний кіберпанк", "冷调赛博", "Kühles Cyber", "Cyber froid", "Chłodny cyber"),
        tr(lang, "Cinema High-Contrast", "Кіноконтраст (Насичений)", "电影高对比", "Kino-Kontrast", "Cinéma contrasté", "Kinowy kontrast"),
        tr(lang, "Noir B&W (Monochrome)", "Чорно-білий нуар", "黑白电影", "Schwarz-Weiß", "Noir & Blanc", "Czarno-biały"),
        tr(lang, "Vintage Sepia", "Вінтажна сепія", "复古怀旧", "Vintage-Sepia", "Sépia vintage", "Sepia vintage")
    };
    if (ImGui::Combo(tr(lang, "Color Filter", "Кольоровий фільтр", "色彩滤镜", "Farbfilter", "Filtre couleur", "Filtr koloru"), &state->camera_color_filter, color_presets, 6)) {
        state->action_settings_changed = true;
    }

    int sharp_pct = (int)(state->camera_sharpness * 100.0f + 0.5f);
    if (ImGui::SliderInt(tr(lang, "HD Sharpness", "Чіткість / Різкість HD", "高清锐化", "HD-Schärfe", "Netteté HD", "Ostrość HD"), &sharp_pct, 0, 100, "%d%%")) {
        state->camera_sharpness = (float)sharp_pct / 100.0f;
        state->action_settings_changed = true;
    }

    int denoise_pct = (int)(state->camera_denoise * 100.0f + 0.5f);
    if (ImGui::SliderInt(tr(lang, "Low-Light Denoise", "Зменшення шуму в темряві", "暗光降噪", "Rauschminderung", "Débruitage basse lumière", "Odszumianie kamery"), &denoise_pct, 0, 100, "%d%%")) {
        state->camera_denoise = (float)denoise_pct / 100.0f;
        state->action_settings_changed = true;
    }

    int bri_pct = (int)(state->camera_brightness * 100.0f + (state->camera_brightness >= 0 ? 0.5f : -0.5f));
    if (ImGui::SliderInt(tr(lang, "Brightness", "Яскравість", "亮度", "Helligkeit", "Luminosité", "Jasność"), &bri_pct, -50, 50, "%+d%%")) {
        state->camera_brightness = (float)bri_pct / 100.0f;
        state->action_settings_changed = true;
    }

    int con_pct = (int)(state->camera_contrast * 100.0f + 0.5f);
    if (ImGui::SliderInt(tr(lang, "Contrast", "Контрастність", "对比度", "Kontrast", "Contraste", "Kontrast"), &con_pct, 50, 180, "%d%%")) {
        state->camera_contrast = (float)con_pct / 100.0f;
        state->action_settings_changed = true;
    }

    int sat_pct = (int)(state->camera_saturation * 100.0f + 0.5f);
    if (ImGui::SliderInt(tr(lang, "Saturation", "Насиченість", "饱和度", "Sättigung", "Saturation", "Nasycenie"), &sat_pct, 0, 200, "%d%%")) {
        state->camera_saturation = (float)sat_pct / 100.0f;
        state->action_settings_changed = true;
    }

    int vig_pct = (int)(state->camera_vignette * 100.0f + 0.5f);
    if (ImGui::SliderInt(tr(lang, "Vignette", "Віньєтка", "暗角", "Vignette", "Vignette", "Winieta"), &vig_pct, 0, 100, "%d%%")) {
        state->camera_vignette = (float)vig_pct / 100.0f;
        state->action_settings_changed = true;
    }

    if (ImGui::SmallButton(tr(lang, "Reset Camera Filters", "Скинути фільтри камери", "重置滤镜", "Filter zurücksetzen", "Réinitialiser filtres", "Resetuj filtry"))) {
        state->camera_color_filter = 0;
        state->camera_sharpness = 0.25f;
        state->camera_denoise = 0.25f;
        state->camera_brightness = 0.0f;
        state->camera_contrast = 1.0f;
        state->camera_saturation = 1.0f;
        state->camera_vignette = 0.0f;
        state->action_settings_changed = true;
    }

    if (ImGui::Checkbox(
        tr(lang, "Chroma Key (Green Screen Removal)", "Хромакей (Видалення фону)", "绿幕抠图 (色度键)", "Chroma-Key (Hintergrund entfernen)", "Chroma Key (Fond vert)", "Chroma Key (Usuwanie tła)"),
        &state->camera_chroma_key)) {
        state->action_settings_changed = true;
    }

    if (state->camera_chroma_key) {
        int ch_idx = (state->camera_chroma_color == 0x0000FF) ? 1 : ((state->camera_chroma_color == 0xFF00FF) ? 2 : 0);
        const char* ch_cols[3] = {
            tr(lang, "Green Screen (#00FF00)", "Зелений фон (#00FF00)", "绿幕 (#00FF00)", "Grün (#00FF00)", "Vert (#00FF00)", "Zielony (#00FF00)"),
            tr(lang, "Blue Screen (#0000FF)", "Синій фон (#0000FF)", "蓝幕 (#0000FF)", "Blau (#0000FF)", "Bleu (#0000FF)", "Niebieski (#0000FF)"),
            tr(lang, "Magenta (#FF00FF)", "Пурпурний (#FF00FF)", "品红 (#FF00FF)", "Magenta (#FF00FF)", "Magenta (#FF00FF)", "Magenta (#FF00FF)")
        };
        if (ImGui::Combo(tr(lang, "Key Color", "Колір фону", "抠图颜色", "Key-Farbe", "Couleur clé", "Kolor tła"), &ch_idx, ch_cols, 3)) {
            state->camera_chroma_color = (ch_idx == 1) ? 0x0000FF : ((ch_idx == 2) ? 0xFF00FF : 0x00FF00);
            state->action_settings_changed = true;
        }
        int sim_pct = (int)(state->camera_chroma_sim * 100.0f + 0.5f);
        if (ImGui::SliderInt(tr(lang, "Similarity (%)", "Чутливість (%)", "相似度 (%)", "Ähnlichkeit (%)", "Similitude (%)", "Podobieństwo (%)"), &sim_pct, 5, 80, "%d%%")) {
            state->camera_chroma_sim = (float)sim_pct / 100.0f;
            state->action_settings_changed = true;
        }
    }

    ImGui::PopItemWidth();
    ImGui::EndGroup();

    if (avail_w > 560.0f) {
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(ImVec4(0.24f, 0.66f, 1.00f, 1.00f), "%s",
            tr(lang, "LIVE CAMERA PREVIEW", "ЖИВИЙ ПЕРЕГЛЯД КАМЕРИ", "实时摄像头预览", "LIVE-KAMERAVORSCHAU", "APERÇU CAMÉRA EN DIRECT", "PODGLĄD KAMERY NA ŻYWO"));
        ImGui::Spacing();

        void* cam_srv = NULL;
        int cam_w = 0, cam_h = 0;
        camera_engine_update_d3d11_preview(g_pd3dDevice, g_pd3dDeviceContext, &cam_srv, &cam_w, &cam_h);

        float p_box_w = col_right_w;
        float p_box_h = (p_box_w * 9.0f) / 16.0f;
        if (p_box_h > 240.0f) p_box_h = 240.0f;

        ImVec2 p_pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        dl->AddRectFilled(p_pos, ImVec2(p_pos.x + p_box_w, p_pos.y + p_box_h), 0xFF14171E, 6.0f);
        dl->AddRect(p_pos, ImVec2(p_pos.x + p_box_w, p_pos.y + p_box_h), 0xFF2A3448, 6.0f);

        if (cam_srv && cam_w > 0 && cam_h > 0) {
            float c_aspect = (state->camera_shape == 2) ? 1.0f : ((float)cam_w / (float)cam_h);
            float dr_w = p_box_w - 16.0f;
            float dr_h = dr_w / c_aspect;
            if (dr_h > p_box_h - 16.0f) {
                dr_h = p_box_h - 16.0f;
                dr_w = dr_h * c_aspect;
            }
            float off_x = p_pos.x + (p_box_w - dr_w) * 0.5f;
            float off_y = p_pos.y + (p_box_h - dr_h) * 0.5f;

            float u0 = state->camera_mirror ? 1.0f : 0.0f;
            float u1 = state->camera_mirror ? 0.0f : 1.0f;
            float v0 = state->camera_flip_vertical ? 1.0f : 0.0f;
            float v1 = state->camera_flip_vertical ? 0.0f : 1.0f;
            ImU32 tint = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, state->camera_opacity));
            float rad = (state->camera_shape == 2) ? (dr_w * 0.5f) : ((state->camera_shape == 1) ? state->camera_corner_radius : 0.0f);

            if (state->camera_glow) {
                dl->AddRect(ImVec2(off_x - 3.0f, off_y - 3.0f), ImVec2(off_x + dr_w + 3.0f, off_y + dr_h + 3.0f), IM_COL32(14, 165, 233, 95), rad + 3.0f, 0, 5.0f);
            }
            dl->AddImageRounded((ImTextureID)(intptr_t)cam_srv, ImVec2(off_x, off_y), ImVec2(off_x + dr_w, off_y + dr_h), ImVec2(u0, v0), ImVec2(u1, v1), tint, rad);
            if (state->camera_border_thickness > 0.0f) {
                ImU32 b_col = IM_COL32((state->camera_border_color >> 24) & 0xFF, (state->camera_border_color >> 16) & 0xFF, (state->camera_border_color >> 8) & 0xFF, state->camera_border_color & 0xFF);
                dl->AddRect(ImVec2(off_x, off_y), ImVec2(off_x + dr_w, off_y + dr_h), b_col, rad, 0, state->camera_border_thickness);
            }
        } else {
            const char* off_text = tr(lang, "Enable overlay above to start live feed", "Увімкніть оверлей вище для запуску потоку", "勾选上方开关以启动画面", "Overlay oben aktivieren", "Activez l'incrustation ci-dessus", "Włącz nakładkę powyżej, aby uruchomić");
            ImVec2 txt_sz = ImGui::CalcTextSize(off_text);
            dl->AddText(ImVec2(p_pos.x + (p_box_w - txt_sz.x) * 0.5f, p_pos.y + (p_box_h - txt_sz.y) * 0.5f), 0xFF64748B, off_text);
        }

        ImGui::Dummy(ImVec2(p_box_w, p_box_h));
        ImGui::Spacing();

        if (cam_w > 0 && cam_h > 0) {
            ImGui::TextColored(ImVec4(0.14f, 0.78f, 0.44f, 1.00f), "%s: %dx%d",
                tr(lang, "Active Feed", "Активний потік", "活动信号", "Aktiver Feed", "Flux actif", "Aktywny strumień"), cam_w, cam_h);
        }
        ImGui::TextWrapped("%s",
            tr(lang,
                "Tip: Open the 'Live Preview' tab to drag and resize the webcam directly with your mouse!",
                "Порада: Відкрий вкладку «Перегляд (Live)», щоб вільно перетягувати і змінювати розмір камери прямо мишкою!",
                "提示：打开“实时预览”标签页即可直接用鼠标拖动和缩放摄像头！",
                "Tipp: Im Tab 'Live-Vorschau' können Sie die Kamera direkt mit der Maus verschieben und skalieren!",
                "Astuce : Ouvrez l'onglet 'Aperçu (Live)' pour déplacer et redimensionner la caméra à la souris !",
                "Wskazówka: Otwórz kartę 'Podgląd (Live)', aby przeciągać i zmieniać rozmiar kamery myszką!"));

        ImGui::EndGroup();
    }
}

void imgui_window_render_ui(UiState* state) {
    int lang = state->language;
    bool is_recording = (state->state == 2 || state->state == 3);
    g_isCurrentlyRecording = is_recording;

    camera_engine_init();
    state->camera_device_names = camera_engine_get_device_names();
    state->camera_device_count = camera_engine_get_device_count();

    CameraOverlayConfig cam_cfg;
    cam_cfg.enabled = state->camera_enabled;
    cam_cfg.source_type = state->camera_source_type;
    cam_cfg.device_index = state->camera_device_index;
    cam_cfg.position = state->camera_position;
    cam_cfg.pos_x_pct = state->camera_pos_x;
    cam_cfg.pos_y_pct = state->camera_pos_y;
    cam_cfg.margin_px = state->camera_margin_px;
    cam_cfg.size_pct = state->camera_size_pct;
    cam_cfg.aspect_ratio = state->camera_aspect_ratio;
    cam_cfg.shape = state->camera_shape;
    cam_cfg.corner_radius = state->camera_corner_radius;
    cam_cfg.border_thickness = state->camera_border_thickness;
    cam_cfg.border_color = state->camera_border_color;
    cam_cfg.opacity = state->camera_opacity;
    cam_cfg.mirror = state->camera_mirror;
    cam_cfg.flip_vertical = state->camera_flip_vertical;
    cam_cfg.chroma_key = state->camera_chroma_key;
    cam_cfg.chroma_color = state->camera_chroma_color;
    cam_cfg.chroma_similarity = state->camera_chroma_sim;
    cam_cfg.chroma_smoothness = state->camera_chroma_smooth;
    cam_cfg.brightness = state->camera_brightness;
    cam_cfg.contrast = state->camera_contrast;
    cam_cfg.saturation = state->camera_saturation;
    cam_cfg.sharpness = state->camera_sharpness;
    cam_cfg.denoise = state->camera_denoise;
    cam_cfg.color_filter = state->camera_color_filter;
    cam_cfg.vignette = state->camera_vignette;
    cam_cfg.glow = state->camera_glow;
    snprintf(cam_cfg.web_url, sizeof(cam_cfg.web_url), "%s", state->camera_web_url);
    camera_engine_update_settings(&cam_cfg);

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
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

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

            float spacing_y = ImGui::GetStyle().ItemSpacing.y;
            float spacing_x = ImGui::GetStyle().ItemSpacing.x;
            float total_gaps = spacing_y * 2.0f + 6.0f;
            float usable_h = (avail_h > total_gaps + 480.0f) ? (avail_h - total_gaps) : 500.0f;

            float h_row1 = usable_h * 0.48f;
            if (h_row1 < 252.0f) h_row1 = 252.0f;

            float h_row2 = usable_h * 0.23f;
            if (h_row2 < 122.0f) h_row2 = 122.0f;

            float h_row3 = usable_h - h_row1 - h_row2;
            if (h_row3 < 142.0f) h_row3 = 142.0f;

            float col_w = (total_w - spacing_x) * 0.5f;

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.11f, 0.14f, 1.00f));
            ImGui::BeginChild("SourceCard", ImVec2(col_w, h_row1), true);
            {
                float s_inner_w = ImGui::GetContentRegionAvail().x;
                ImGui::TextColored(ImVec4(0.24f, 0.66f, 1.00f, 1.00f), "%s",
                    tr(lang,
                        "1. VIDEO SOURCE & DISPLAY",
                        "1. ДЖЕРЕЛО ВІДЕО ТА ДИСПЛЕЙ",
                        "1. 视频源与显示器",
                        "1. VIDEOQUELLE & BILDSCHIRM",
                        "1. SOURCE VIDÉO & ÉCRAN",
                        "1. ŹRÓDŁO WIDEO I EKRAN"));
                float hdr_cam_btn_w = 96.0f;
                ImGui::SameLine(s_inner_w - hdr_cam_btn_w + 14.0f);
                if (state->camera_enabled) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.58f, 0.34f, 1.00f));
                else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.28f, 0.48f, 1.00f));
                if (ImGui::Button(tr(lang, "Webcam##Hdr", "Вебкамера##Hdr", "摄像头##Hdr", "Webcam##Hdr", "Caméra##Hdr", "Kamera##Hdr"), ImVec2(hdr_cam_btn_w, 22))) {
                    g_showCameraOverlayPopup = true;
                }
                ImGui::PopStyleColor();
                ImGui::Spacing();

                bool is_screen = (state->capture_type == 0);
                bool is_window = (state->capture_type == 1);
                bool is_game   = (state->capture_type == 2);

                float btn_w = (s_inner_w - spacing_x * 2.0f) / 3.0f;
                float btn_h = (h_row1 > 270.0f) ? 34.0f : 30.0f;
                if (is_screen) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.50f, 0.92f, 1.00f));
                if (ImGui::Button(tr(lang, "Screen", "Екран", "屏幕", "Bildschirm", "Écran", "Ekran"), ImVec2(btn_w, btn_h))) state->capture_type = 0;
                if (is_screen) ImGui::PopStyleColor();

                ImGui::SameLine();
                if (is_window) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.50f, 0.92f, 1.00f));
                if (ImGui::Button(tr(lang, "Window", "Вікно", "窗口", "Fenster", "Fenêtre", "Okno"), ImVec2(btn_w, btn_h))) state->capture_type = 1;
                if (is_window) ImGui::PopStyleColor();

                ImGui::SameLine();
                if (is_game) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.50f, 0.92f, 1.00f));
                if (ImGui::Button(tr(lang, "Game", "Гра", "游戏", "Spiel", "Jeu", "Gra"), ImVec2(btn_w, btn_h))) state->capture_type = 2;
                if (is_game) ImGui::PopStyleColor();

                ImGui::Spacing();

                float refresh_w = 84.0f;
                if (state->capture_type == 0) {
                    ImGui::TextDisabled("%s", tr(lang, "Display / Monitor:", "Монітор / Дисплей:", "显示器 / 屏幕:", "Monitor / Anzeige:", "Moniteur / Écran :", "Monitor / Wyświetlacz:"));
                    const char* current_mon = (state->monitor_count > 0 && state->selected_monitor < state->monitor_count)
                        ? state->monitor_names[state->selected_monitor] : "Default Display";
                    ImGui::SetNextItemWidth(s_inner_w);
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
                    ImGui::SetNextItemWidth(s_inner_w - refresh_w - spacing_x);
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
                    if (ImGui::Button(tr(lang, "Refresh", "Оновити", "刷新", "Aktualis.", "Actualiser", "Odśwież"), ImVec2(refresh_w, 26))) {
                        state->action_refresh_windows_requested = true;
                    }
                } else {
                    ImGui::TextDisabled("%s", tr(lang, "Game Window / App:", "Вікно гри / Додаток:", "游戏窗口 / 应用:", "Spielfenster / App:", "Fenêtre du jeu / App :", "Okno gry / Aplikacja:"));
                    const char* current_win = (state->window_count > 0 && state->selected_window < state->window_count)
                        ? state->window_names[state->selected_window]
                        : tr(lang, "Select game window...", "Виберіть гру зі списку...", "从列表中选择游戏...", "Spielfenster wählen...", "Choisir le jeu...", "Wybierz okno gry...");
                    ImGui::SetNextItemWidth(s_inner_w - refresh_w - spacing_x);
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
                    if (ImGui::Button(tr(lang, "Refresh", "Оновити", "刷新", "Aktualis.", "Actualiser", "Odśwież"), ImVec2(refresh_w, 26))) {
                        state->action_refresh_windows_requested = true;
                    }
                }

                ImGui::Spacing();
                float half_col_x = ImGui::GetCursorPosX() + s_inner_w * 0.50f;
                if (ImGui::Checkbox(tr(lang, "Capture Cursor", "Курсор миші", "捕获鼠标指针", "Mauszeiger", "Curseur souris", "Kursor myszy"), &state->capture_cursor)) {
                    state->action_settings_changed = true;
                }
                ImGui::SameLine(half_col_x);
                if (ImGui::Checkbox(tr(lang, "Overlay (F9)", "Оверлей (F9)", "悬浮窗 (F9)", "Overlay (F9)", "Overlay (F9)", "Nakładka (F9)"), &state->show_overlay_indicator)) {
                    state->action_settings_changed = true;
                }

                if (ImGui::Checkbox(tr(lang, "Weak PC Mode", "Слабкий ПК", "弱电脑模式", "Schwacher PC", "Mode PC faible", "Słaby PC"), &state->low_end_mode)) {
                    state->action_settings_changed = true;
                    if (state->low_end_mode) {
                        state->output_resolution = 1;
                        state->target_fps = 30;
                        state->encoder_preset = 0;
                        if ((state->encoder_caps_mask & (1 << 6)) != 0) {
                            state->encoder_choice = 0;
                            state->qsv_low_power = true;
                            state->qsv_async_depth = 4;
                        } else if ((state->encoder_caps_mask & (1 << 11)) != 0) {
                            state->encoder_choice = 4;
                            state->wmf_hw_encoding = true;
                        } else if ((state->encoder_caps_mask & ((1 << 0) | (1 << 3))) != 0) {
                            state->encoder_choice = 3;
                        }
                    }
                }
                ImGui::SameLine(half_col_x);
                if (ImGui::Checkbox(tr(lang, "Multi-Track", "Мульти-трек", "多音轨", "Multi-Spur", "Multi-piste", "Wielościeżk."), &state->separate_audio_tracks)) {
                    state->action_settings_changed = true;
                }

                ImGui::Spacing();
                const char* cap_engine = (state->capture_type == 2) ? "Game Hook + WGC" : "WGC Zero-Copy";
                const char* enc_names[8] = {
                    "QuickSync H.264", "QuickSync HEVC", "Intel MSDK",
                    "NVENC / AMF HW", "WMF Hardware", "OpenH264",
                    "libx264 CPU", "MJPEG"
                };
                const char* active_enc = (state->encoder_choice >= 0 && state->encoder_choice < 8)
                    ? enc_names[state->encoder_choice] : "Auto";
                char engine_pill[96];
                snprintf(engine_pill, sizeof(engine_pill), "%s  |  %s", cap_engine, active_enc);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.08f, 0.10f, 1.00f));
                ImGui::BeginChild("##EnginePill", ImVec2(s_inner_w, 26.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                ImVec2 esz = ImGui::CalcTextSize(engine_pill);
                ImGui::SetCursorPosY((26.0f - esz.y) * 0.5f);
                ImGui::SetCursorPosX((s_inner_w - esz.x) * 0.5f);
                ImGui::TextColored(ImVec4(0.45f, 0.78f, 0.96f, 1.00f), "%s", engine_pill);
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::SameLine();

            if (is_recording && g_micTestActive) {
                g_micTestActive = false;
            }
            update_mic_test_state(g_micTestActive && !is_recording, state);

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.11f, 0.14f, 1.00f));
            ImGui::BeginChild("AudioCard", ImVec2(col_w, h_row1), true);
            {
                float inner_w = ImGui::GetContentRegionAvail().x;
                float hdr_btn1_w = 96.0f;
                float hdr_btn2_w = 88.0f;

                ImGui::TextColored(ImVec4(0.24f, 0.66f, 1.00f, 1.00f), "%s",
                    tr(lang,
                        "2. AUDIO & MIC",
                        "2. АУДІО ТА МІКРОФОН",
                        "2. 音频与麦克风",
                        "2. AUDIO & MIKRO",
                        "2. AUDIO & MICRO",
                        "2. AUDIO I MIKRO"));
                ImGui::SameLine(inner_w - hdr_btn1_w - hdr_btn2_w - spacing_x + 14.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.28f, 0.48f, 1.00f));
                if (ImGui::Button(tr(lang, "Filters DSP##Hdr", "Фільтри DSP##Hdr", "DSP 滤镜##Hdr", "DSP-Filter##Hdr", "Filtres DSP##Hdr", "Filtry DSP##Hdr"), ImVec2(hdr_btn1_w, 22))) {
                    g_showAudioFiltersPopup = true;
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::Button(tr(lang, "App Mixer##Hdr", "Програми##Hdr", "应用音量##Hdr", "App-Mixer##Hdr", "Mixeur##Hdr", "Aplikacje##Hdr"), ImVec2(hdr_btn2_w, 22))) {
                    g_showAppMixerPopup = true;
                }

                ImGui::Spacing();

                float test_btn_w = 68.0f;
                float meter_w = 118.0f;

                if (ImGui::Checkbox(tr(lang, "System", "Система", "系统", "System", "Système", "System"), &state->record_system_audio)) {
                    state->action_settings_changed = true;
                }
                ImGui::SameLine();
                float avail1 = ImGui::GetContentRegionAvail().x - test_btn_w - spacing_x;
                if (avail1 < 90.0f) avail1 = 90.0f;
                ImGui::SetNextItemWidth(avail1);
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
                ImGui::SameLine();
                if (ImGui::Button(tr(lang, "Test##Spk", "Тест##Spk", "测试##Spk", "Test##Spk", "Test##Spk", "Test##Spk"), ImVec2(test_btn_w, 24))) {
                    trigger_speaker_test(state->selected_audio_output, state->system_volume);
                }

                ImGui::BeginDisabled(!state->record_system_audio);
                float slider_w = inner_w - meter_w - spacing_x;
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
                draw_led_vu_meter("##SysMeter", state->sys_audio_level, state->record_system_audio, meter_w, 22.0f);
                ImGui::EndDisabled();

                ImGui::Spacing();

                if (ImGui::Checkbox(tr(lang, "Mic", "Мікр.", "麦克风", "Mikro", "Micro", "Mikr."), &state->record_microphone)) {
                    state->action_settings_changed = true;
                }
                ImGui::SameLine();
                float avail2 = ImGui::GetContentRegionAvail().x - test_btn_w - spacing_x;
                if (avail2 < 90.0f) avail2 = 90.0f;
                ImGui::SetNextItemWidth(avail2);
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
                ImGui::SameLine();
                ImGui::BeginDisabled(is_recording);
                if (g_micTestActive) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.58f, 0.34f, 1.00f));
                if (ImGui::Button(
                        g_micTestActive
                            ? tr(lang, "Stop##MicT", "Стоп##MicT", "停止##MicT", "Stop##MicT", "Stop##MicT", "Stop##MicT")
                            : tr(lang, "Test##MicT", "Тест##MicT", "测试##MicT", "Test##MicT", "Test##MicT", "Test##MicT"),
                        ImVec2(test_btn_w, 24))) {
                    g_micTestActive = !g_micTestActive;
                }
                if (g_micTestActive) ImGui::PopStyleColor();
                ImGui::EndDisabled();

                ImGui::BeginDisabled(!state->record_microphone && !g_micTestActive);
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
                float shown_mic = g_micTestActive ? g_micTestLevel : state->mic_audio_level;
                draw_led_vu_meter("##MicMeter", shown_mic, state->record_microphone || g_micTestActive, meter_w, 22.0f);
                ImGui::EndDisabled();

                ImGui::Spacing();
                if (ImGui::Checkbox(tr(lang, "Noise Supp.", "Шумоподав", "降噪", "Rauschunt.", "Anti-bruit", "Red. szumu"), &state->mic_noise_suppression)) {
                    state->action_settings_changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Checkbox(tr(lang, "Mic Comp.", "Комп. мікр.", "麦压缩", "Mikro-Komp.", "Comp. micro", "Komp. mikr."), &state->mic_compressor)) {
                    state->action_settings_changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Checkbox(tr(lang, "Spk Night", "Нічн. режим", "夜间音平衡", "Nachtmodus", "Mode nuit", "Tryb nocny"), &state->sys_compressor)) {
                    state->action_settings_changed = true;
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            const char* filters_modal_title = tr(lang,
                "Audio Filters & Effects###AudioFiltersModal",
                "Аудіофільтри та ефекти###AudioFiltersModal",
                "音频滤镜与效果###AudioFiltersModal",
                "Audiofilter & Effekte###AudioFiltersModal",
                "Filtres et Effets Audio###AudioFiltersModal",
                "Filtry i Efekty Audio###AudioFiltersModal");
            if (g_showAudioFiltersPopup) {
                ImGui::OpenPopup(filters_modal_title);
                g_showAudioFiltersPopup = false;
            }
            bool filters_modal_open = true;
            if (ImGui::BeginPopupModal(filters_modal_title, &filters_modal_open, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::PushItemWidth(260.0f);

                ImGui::BeginDisabled(is_recording);
                if (g_micTestActive) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.58f, 0.34f, 1.00f));
                if (ImGui::Button(
                        g_micTestActive
                            ? tr(lang, "Stop Mic Monitor##PopT", "Зупинити тест мікрофона##PopT", "停止麦克风监听##PopT", "Mikro-Test stoppen##PopT", "Arrêter le test micro##PopT", "Zatrzymaj test mikr.##PopT")
                            : tr(lang, "Test Mic (Hear Filters Live)##PopT", "Прослухати мікрофон (з фільтрами)##PopT", "实时试听麦克风效果##PopT", "Mikrofon live testen##PopT", "Tester le micro en direct##PopT", "Testuj mikrofon na żywo##PopT"),
                        ImVec2(240.0f, 26.0f))) {
                    g_micTestActive = !g_micTestActive;
                }
                if (g_micTestActive) ImGui::PopStyleColor();
                ImGui::EndDisabled();
                ImGui::SameLine();
                float modal_mic = g_micTestActive ? g_micTestLevel : state->mic_audio_level;
                draw_led_vu_meter("##ModalMicMeter", modal_mic, state->record_microphone || g_micTestActive, 150.0f, 26.0f);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.24f, 0.66f, 1.00f, 1.00f), "%s",
                    tr(lang, "Microphone DSP:", "Мікрофон (DSP та AI):", "麦克风处理:", "Mikrofon-DSP:", "DSP Microphone :", "DSP Mikrofonu:"));
                if (ImGui::Checkbox(tr(lang, "Noise Suppression##Pop", "Шумозаглушення мікрофона##Pop", "噪声抑制##Pop", "Rauschunterdrückung##Pop", "Réduction du bruit##Pop", "Redukcja szumu##Pop"), &state->mic_noise_suppression)) {
                    state->action_settings_changed = true;
                }
                if (state->mic_noise_suppression) {
                    const char* ns_modes[2] = {
                        tr(lang, "AI Spectral Bark Denoiser (RNNoise-style)", "AI Спектральний шумоподав (прибирає шум під час мовлення)", "AI 频谱降噪 (说话时消除噪音)", "KI-Spektral-Rauschfilter", "Débruiteur IA Spectral", "AI Spektralna redukcja szumu"),
                        tr(lang, "Classic Light Gate (Eco CPU)", "Класичний шумовий гейт (Ультралегкий)", "经典噪声门 (极低占用)", "Klassisches Noise-Gate", "Porte de bruit classique", "Klasyczna bramka szumu")
                    };
                    if (ImGui::Combo(tr(lang, "Denoiser Engine##Pop", "Алгоритм##Pop", "降噪引擎##Pop", "Modus##Pop", "Moteur##Pop", "Tryb##Pop"), &state->mic_ns_mode, ns_modes, 2)) {
                        state->action_settings_changed = true;
                    }
                    int ns_pct = (int)(state->mic_ns_strength * 100.0f + 0.5f);
                    if (ImGui::SliderInt(tr(lang, "Suppression Strength##Pop", "Сила шумозаглушення##Pop", "抑制强度##Pop", "Unterdrückung##Pop", "Intensité##Pop", "Siła redukcji##Pop"), &ns_pct, 5, 100, "%d%%")) {
                        state->mic_ns_strength = (float)ns_pct / 100.0f;
                        state->action_settings_changed = true;
                    }
                }
                if (ImGui::Checkbox(tr(lang, "Voice Compressor##Pop", "Компресор голосу##Pop", "语音压缩器##Pop", "Sprachkompressor##Pop", "Compresseur vocal##Pop", "Kompresor głosu##Pop"), &state->mic_compressor)) {
                    state->action_settings_changed = true;
                }
                if (state->mic_compressor) {
                    int comp_pct = (int)(state->mic_comp_amount * 100.0f + 0.5f);
                    if (ImGui::SliderInt(tr(lang, "Compression Amount##Pop", "Ступінь компресії##Pop", "压缩量##Pop", "Kompressionsgrad##Pop", "Compression##Pop", "Kompresja##Pop"), &comp_pct, 0, 100, "%d%%")) {
                        state->mic_comp_amount = (float)comp_pct / 100.0f;
                        state->action_settings_changed = true;
                    }
                }
                int mic_gdb = (int)(state->mic_gain_db + (state->mic_gain_db >= 0 ? 0.5f : -0.5f));
                if (ImGui::SliderInt(tr(lang, "Mic Gain Boost##Pop", "Підсилення мікрофона##Pop", "麦克风增益##Pop", "Mikrofon-Verstärkung##Pop", "Gain micro##Pop", "Wzmocnienie mikr.##Pop"), &mic_gdb, -12, 24, "%d dB")) {
                    state->mic_gain_db = (float)mic_gdb;
                    state->action_settings_changed = true;
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.24f, 0.66f, 1.00f, 1.00f), "%s",
                    tr(lang, "Speakers / System Audio & Per-Process Isolation:", "Системний звук та Ізоляція окремої програми:", "系统音频与单应用隔离捕获:", "Systemton & App-Isolierung:", "Audio Système & Isolation d'application :", "Dźwięk systemu i Izolacja aplikacji:"));
                ImGui::PushID("PopSysAudio");
                render_process_audio_controls(state, lang);
                ImGui::PopID();
                ImGui::SameLine();
                if (ImGui::SmallButton(tr(lang, "Test Speakers##PopSpk", "Тест динаміків##PopSpk", "测试扬声器##PopSpk", "Lautsprecher testen##PopSpk", "Tester haut-parleurs##PopSpk", "Testuj głośniki##PopSpk"))) {
                    trigger_speaker_test(state->selected_audio_output, state->system_volume);
                }
                if (ImGui::Checkbox(tr(lang, "Clarity Filter (Sub-Rumble Cut)##Pop", "Фільтр чистоти (зріз низьких)##Pop", "清晰度滤波##Pop", "Klarheitsfilter##Pop", "Filtre clarté##Pop", "Filtr czystości##Pop"), &state->sys_clarity_filter)) {
                    state->action_settings_changed = true;
                }
                if (ImGui::Checkbox(tr(lang, "Loudness Compressor (Night Mode)##Pop", "Компресор гучності (нічний режим)##Pop", "响度压缩器##Pop", "Lautheitskompressor##Pop", "Compresseur de niveau##Pop", "Kompresor głośności##Pop"), &state->sys_compressor)) {
                    state->action_settings_changed = true;
                }
                if (state->sys_compressor) {
                    int scomp_pct = (int)(state->sys_comp_amount * 100.0f + 0.5f);
                    if (ImGui::SliderInt(tr(lang, "System Compression##Pop", "Ступінь компресії##Pop", "系统压缩量##Pop", "System-Kompression##Pop", "Compression système##Pop", "Kompresja systemu##Pop"), &scomp_pct, 0, 100, "%d%%")) {
                        state->sys_comp_amount = (float)scomp_pct / 100.0f;
                        state->action_settings_changed = true;
                    }
                }
                int sys_gdb = (int)(state->sys_gain_db + (state->sys_gain_db >= 0 ? 0.5f : -0.5f));
                if (ImGui::SliderInt(tr(lang, "System Gain Boost##Pop", "Підсилення системи##Pop", "系统增益##Pop", "System-Verstärkung##Pop", "Gain système##Pop", "Wzmocnienie syst.##Pop"), &sys_gdb, -12, 18, "%d dB")) {
                    state->sys_gain_db = (float)sys_gdb;
                    state->action_settings_changed = true;
                }

                ImGui::PopItemWidth();
                ImGui::Spacing();
                if (ImGui::Button(tr(lang, "Done##Filters", "Готово##Filters", "完成##Filters", "Fertig##Filters", "Terminé##Filters", "Gotowe##Filters"), ImVec2(140, 28))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            const char* mixer_modal_title = tr(lang,
                "Application Audio Mixer###AppMixerModal",
                "Мікшер гучності програм###AppMixerModal",
                "应用程序音量混合器###AppMixerModal",
                "Anwendungs-Audiomixer###AppMixerModal",
                "Mixeur Audio par Application###AppMixerModal",
                "Mikser Głośności Aplikacji###AppMixerModal");
            if (g_showAppMixerPopup) {
                ImGui::OpenPopup(mixer_modal_title);
                g_showAppMixerPopup = false;
            }
            bool mixer_modal_open = true;
            if (ImGui::BeginPopupModal(mixer_modal_title, &mixer_modal_open, ImGuiWindowFlags_AlwaysAutoResize)) {
                render_app_audio_mixer_ui(lang);
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                if (ImGui::Button(tr(lang, "Done##AppMix", "Готово##AppMix", "完成##AppMix", "Fertig##AppMix", "Terminé##AppMix", "Gotowe##AppMix"), ImVec2(140, 28))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            const char* cam_modal_title = tr(lang,
                "Webcam & Camera Overlay Settings###CamOverlayModal",
                "Налаштування Вебкамери та Оверлею###CamOverlayModal",
                "摄像头画中画与网络信号源设置###CamOverlayModal",
                "Webcam- & Overlay-Einstellungen###CamOverlayModal",
                "Paramètres d'incrustation Caméra###CamOverlayModal",
                "Ustawienia Nakładki Kamery###CamOverlayModal");
            if (g_showCameraOverlayPopup) {
                ImGui::OpenPopup(cam_modal_title);
                g_showCameraOverlayPopup = false;
            }
            bool cam_modal_open = true;
            if (ImGui::BeginPopupModal(cam_modal_title, &cam_modal_open, ImGuiWindowFlags_AlwaysAutoResize)) {
                render_camera_controls(state, lang);
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                if (ImGui::Button(tr(lang, "Done##CamModal", "Готово##CamModal", "完成##CamModal", "Fertig##CamModal", "Terminé##CamModal", "Gotowe##CamModal"), ImVec2(140, 28))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.11f, 0.14f, 1.00f));
            ImGui::BeginChild("QuickSettingsCard", ImVec2(total_w, h_row2), true);
            {
                ImGui::TextColored(ImVec4(0.24f, 0.66f, 1.00f, 1.00f), "%s",
                    tr(lang,
                        "3. OUTPUT & STORAGE",
                        "3. ПАРАМЕТРИ ФАЙЛУ ТА ЗБЕРЕЖЕННЯ",
                        "3. 输出参数与保存位置",
                        "3. AUSGABE & SPEICHERORT",
                        "3. SORTIE & STOCKAGE",
                        "3. PARAMETRY PLIKU I ZAPIS"));
                ImGui::Spacing();

                float card_w = ImGui::GetContentRegionAvail().x;
                float col3_w = (card_w - (spacing_x * 2.0f)) / 3.0f;
                float start_x = ImGui::GetCursorPosX();

                ImGui::TextDisabled("%s", tr(lang, "Format:", "Формат:", "格式:", "Format:", "Format :", "Format:"));
                ImGui::SameLine();
                const char* quick_formats[] = { "MKV", "MP4", "MOV", "WebM", "WMV", "FLV", "F4V", "AVCHD" };
                float fmt_w = (start_x + col3_w) - ImGui::GetCursorPosX() - 4.0f;
                if (fmt_w < 90.0f) fmt_w = 90.0f;
                ImGui::SetNextItemWidth(fmt_w);
                if (ImGui::Combo("##QuickFormat", &state->container_format, quick_formats, 8)) {
                    state->action_settings_changed = true;
                }

                ImGui::SameLine(start_x + col3_w + spacing_x);
                ImGui::TextDisabled("%s", tr(lang, "Quality:", "Якість:", "画质:", "Qualität:", "Qualité :", "Jakość:"));
                ImGui::SameLine();
                const char* quick_res[] = {
                    tr(lang, "Native", "Рідна", "原生", "Nativ", "Natif", "Natywna"),
                    "1080p (Full HD)",
                    "720p (HD)",
                    "480p (SD)"
                };
                float res_w = (start_x + col3_w * 2.0f + spacing_x) - ImGui::GetCursorPosX() - 4.0f;
                if (res_w < 90.0f) res_w = 90.0f;
                ImGui::SetNextItemWidth(res_w);
                if (ImGui::Combo("##QuickRes", &state->output_resolution, quick_res, 4)) {
                    state->action_settings_changed = true;
                }

                ImGui::SameLine(start_x + (col3_w + spacing_x) * 2.0f);
                ImGui::TextDisabled("FPS:");
                ImGui::SameLine();
                float eco_w = 78.0f;
                float fps_w = (start_x + card_w) - ImGui::GetCursorPosX() - eco_w - spacing_x;
                if (fps_w < 75.0f) fps_w = 75.0f;
                ImGui::SetNextItemWidth(fps_w);
                if (ImGui::SliderInt("##QuickFps", &state->target_fps, 10, max_hz, "%d FPS")) {
                    if (state->target_fps < 10) state->target_fps = 10;
                    if (state->target_fps > max_hz) state->target_fps = max_hz;
                    state->action_settings_changed = true;
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.48f, 0.30f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.60f, 0.38f, 1.0f));
                if (ImGui::Button("Eco 30", ImVec2(eco_w, 24))) {
                    state->target_fps = (30 <= max_hz) ? 30 : max_hz;
                    state->encoder_preset = 0;
                    state->rate_control = 2;
                    state->quality_cqp = 23;
                    state->action_settings_changed = true;
                }
                ImGui::PopStyleColor(2);

                ImGui::Spacing();

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

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.09f, 0.12f, 1.00f));
            ImGui::BeginChild("ActionCard", ImVec2(total_w, h_row3), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            {
                float a_inner_w = ImGui::GetContentRegionAvail().x;
                float btn_h = (h_row3 > 200.0f) ? 48.0f : 42.0f;
                if (!is_recording) {
                    ImGui::SetCursorPosX((total_w - 420.0f) * 0.5f);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.86f, 0.16f, 0.25f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.24f, 0.33f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.72f, 0.12f, 0.20f, 1.00f));
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
                ImGui::TextColored(is_recording ? ImVec4(1.0f, 0.32f, 0.32f, 1.0f) : ImVec4(0.40f, 0.85f, 1.0f, 1.0f), "%s", timer_text);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                float tile_w = (a_inner_w - spacing_x * 3.0f) / 4.0f;
                float tile_h = (h_row3 > 200.0f) ? 40.0f : 34.0f;
                double mb = (double)state->file_bytes / (1024.0 * 1024.0);

                char v1[48], v2[48], v3[48], v4[48];
                snprintf(v1, sizeof(v1), "%.1f MB", mb);
                snprintf(v2, sizeof(v2), "%u kbps", state->bitrate_kbps);
                snprintf(v3, sizeof(v3), "%.1f / %d", state->current_fps, state->target_fps);
                snprintf(v4, sizeof(v4), "%llu", (unsigned long long)state->dropped_frames);

                draw_telemetry_badge("tel_size", tr(lang, "Size", "Розмір", "大小", "Größe", "Taille", "Rozmiar"), v1, ImVec4(0.28f, 0.72f, 1.00f, 1.0f), tile_w, tile_h);
                ImGui::SameLine();
                draw_telemetry_badge("tel_br", tr(lang, "Bitrate", "Бітрейт", "码率", "Bitrate", "Débit", "Bitrate"), v2, ImVec4(0.25f, 0.85f, 0.48f, 1.0f), tile_w, tile_h);
                ImGui::SameLine();
                draw_telemetry_badge("tel_fps", "FPS", v3, ImVec4(0.98f, 0.82f, 0.28f, 1.0f), tile_w, tile_h);
                ImGui::SameLine();
                draw_telemetry_badge("tel_drop", tr(lang, "Dropped", "Пропущено", "丢帧", "Verworfen", "Perdues", "Pominięto"), v4, (state->dropped_frames > 0) ? ImVec4(0.98f, 0.32f, 0.32f, 1.0f) : ImVec4(0.55f, 0.65f, 0.78f, 1.0f), tile_w, tile_h);
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
                ImGui::SameLine();
                if (ImGui::Checkbox(tr(lang, "Show Webcam Overlay", "Оверлей вебкамери", "画中画预览", "Webcam anzeigen", "Afficher la cam", "Pokaż nakładkę kamery"), &state->camera_enabled)) {
                    state->action_settings_changed = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(tr(lang, "Config Webcam...", "Налаштувати...", "配置摄像头...", "Webcam konfigurieren...", "Configurer la cam...", "Konfiguruj kamerę..."))) {
                    g_showCameraOverlayPopup = true;
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

                    ImVec2 img_min = ImGui::GetItemRectMin();
                    if (state->camera_enabled) {
                        void* cam_srv = NULL;
                        int cam_w = 0, cam_h = 0;
                        camera_engine_update_d3d11_preview(g_pd3dDevice, g_pd3dDeviceContext, &cam_srv, &cam_w, &cam_h);
                        if (cam_srv && cam_w > 0 && cam_h > 0) {
                            float c_aspect = (state->camera_shape == 2) ? 1.0f : ((float)cam_w / (float)cam_h);
                            float c_w = draw_w * state->camera_size_pct;
                            if (c_w < 32.0f) c_w = 32.0f;
                            if (c_w > draw_w) c_w = draw_w;
                            float c_h = c_w / c_aspect;
                            if (c_h > draw_h) {
                                c_h = draw_h;
                                c_w = c_h * c_aspect;
                            }
                            float m_x = (float)state->camera_margin_px * (draw_w / (float)g_previewWidth);
                            float m_y = (float)state->camera_margin_px * (draw_h / (float)g_previewHeight);
                            if (m_x < 2.0f) m_x = 2.0f;
                            if (m_y < 2.0f) m_y = 2.0f;

                            float c_x = 0.0f, c_y = 0.0f;
                            switch (state->camera_position) {
                                case 0: c_x = img_min.x + draw_w - m_x - c_w; c_y = img_min.y + draw_h - m_y - c_h; break;
                                case 1: c_x = img_min.x + m_x; c_y = img_min.y + draw_h - m_y - c_h; break;
                                case 2: c_x = img_min.x + draw_w - m_x - c_w; c_y = img_min.y + m_y; break;
                                case 3: c_x = img_min.x + m_x; c_y = img_min.y + m_y; break;
                                case 4: c_x = img_min.x + (draw_w - c_w) * 0.5f; c_y = img_min.y + draw_h - m_y - c_h; break;
                                case 5: c_x = img_min.x + (draw_w - c_w) * 0.5f; c_y = img_min.y + m_y; break;
                                case 6: c_x = img_min.x + m_x; c_y = img_min.y + (draw_h - c_h) * 0.5f; break;
                                case 7: c_x = img_min.x + draw_w - m_x - c_w; c_y = img_min.y + (draw_h - c_h) * 0.5f; break;
                                case 8: default:
                                    c_x = img_min.x + state->camera_pos_x * (draw_w - c_w);
                                    c_y = img_min.y + state->camera_pos_y * (draw_h - c_h);
                                    break;
                            }

                            static bool s_draggingCam = false;
                            static bool s_resizingCam = false;
                            static ImVec2 s_dragOffset = ImVec2(0.0f, 0.0f);
                            static float s_resizeStartX = 0.0f;
                            static float s_resizeStartPct = 0.22f;

                            ImGuiIO& pio = ImGui::GetIO();
                            ImVec2 mp = pio.MousePos;
                            bool over_grip = (mp.x >= c_x + c_w - 18.0f && mp.x <= c_x + c_w + 6.0f &&
                                              mp.y >= c_y + c_h - 18.0f && mp.y <= c_y + c_h + 6.0f);
                            bool over_cam = (mp.x >= c_x && mp.x <= c_x + c_w && mp.y >= c_y && mp.y <= c_y + c_h);

                            if (!ImGui::IsPopupOpen(NULL, ImGuiPopupFlags_AnyPopupId)) {
                                if (over_cam && fabsf(pio.MouseWheel) > 0.01f) {
                                    float next_pct = state->camera_size_pct + pio.MouseWheel * 0.02f;
                                    if (next_pct < 0.08f) next_pct = 0.08f;
                                    if (next_pct > 0.65f) next_pct = 0.65f;
                                    state->camera_size_pct = next_pct;
                                    state->action_settings_changed = true;
                                }

                                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                    if (over_grip) {
                                        s_resizingCam = true;
                                        s_resizeStartX = mp.x;
                                        s_resizeStartPct = state->camera_size_pct;
                                    } else if (over_cam) {
                                        s_draggingCam = true;
                                        s_dragOffset = ImVec2(mp.x - c_x, mp.y - c_y);
                                        if (state->camera_position != 8) {
                                            float span_x = (draw_w - c_w > 1.0f) ? (draw_w - c_w) : 1.0f;
                                            float span_y = (draw_h - c_h > 1.0f) ? (draw_h - c_h) : 1.0f;
                                            state->camera_pos_x = (c_x - img_min.x) / span_x;
                                            state->camera_pos_y = (c_y - img_min.y) / span_y;
                                            state->camera_position = 8;
                                        }
                                    }
                                }

                                if (s_resizingCam) {
                                    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                                        float delta_pct = (mp.x - s_resizeStartX) / (draw_w > 1.0f ? draw_w : 1.0f);
                                        float next_pct = s_resizeStartPct + delta_pct;
                                        if (next_pct < 0.08f) next_pct = 0.08f;
                                        if (next_pct > 0.65f) next_pct = 0.65f;
                                        state->camera_size_pct = next_pct;
                                        c_w = draw_w * state->camera_size_pct;
                                        c_h = c_w / c_aspect;
                                        state->action_settings_changed = true;
                                    } else {
                                        s_resizingCam = false;
                                    }
                                } else if (s_draggingCam) {
                                    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                                        float new_cx = mp.x - s_dragOffset.x;
                                        float new_cy = mp.y - s_dragOffset.y;
                                        if (new_cx < img_min.x) new_cx = img_min.x;
                                        if (new_cy < img_min.y) new_cy = img_min.y;
                                        if (new_cx + c_w > img_min.x + draw_w) new_cx = img_min.x + draw_w - c_w;
                                        if (new_cy + c_h > img_min.y + draw_h) new_cy = img_min.y + draw_h - c_h;
                                        float span_x = (draw_w - c_w > 1.0f) ? (draw_w - c_w) : 1.0f;
                                        float span_y = (draw_h - c_h > 1.0f) ? (draw_h - c_h) : 1.0f;
                                        state->camera_pos_x = (new_cx - img_min.x) / span_x;
                                        state->camera_pos_y = (new_cy - img_min.y) / span_y;
                                        c_x = new_cx;
                                        c_y = new_cy;
                                        state->action_settings_changed = true;
                                    } else {
                                        s_draggingCam = false;
                                    }
                                }
                            }

                            ImDrawList* draw_list = ImGui::GetWindowDrawList();
                            ImVec2 p_min(c_x, c_y);
                            ImVec2 p_max(c_x + c_w, c_y + c_h);

                            float u0 = state->camera_mirror ? 1.0f : 0.0f;
                            float u1 = state->camera_mirror ? 0.0f : 1.0f;
                            float v0 = state->camera_flip_vertical ? 1.0f : 0.0f;
                            float v1 = state->camera_flip_vertical ? 0.0f : 1.0f;
                            ImU32 tint_col = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, state->camera_opacity));

                            float r_cr = (state->camera_shape == 2) ? (c_w * 0.5f) : ((state->camera_shape == 1) ? state->camera_corner_radius : 0.0f);
                            if (state->camera_glow) {
                                draw_list->AddRect(ImVec2(p_min.x - 3.0f, p_min.y - 3.0f), ImVec2(p_max.x + 3.0f, p_max.y + 3.0f), IM_COL32(14, 165, 233, 95), r_cr + 3.0f, 0, 5.0f);
                            }
                            draw_list->AddImageRounded((ImTextureID)(intptr_t)cam_srv, p_min, p_max, ImVec2(u0, v0), ImVec2(u1, v1), tint_col, r_cr);

                            if (state->camera_border_thickness > 0.0f) {
                                ImU32 b_col = IM_COL32((state->camera_border_color >> 24) & 0xFF, (state->camera_border_color >> 16) & 0xFF, (state->camera_border_color >> 8) & 0xFF, state->camera_border_color & 0xFF);
                                draw_list->AddRect(p_min, p_max, b_col, r_cr, 0, state->camera_border_thickness);
                            }

                            if (over_cam || over_grip || s_draggingCam || s_resizingCam) {
                                draw_list->AddRect(p_min, p_max, IM_COL32(56, 189, 248, 220), 2.0f, 0, 1.5f);
                                ImVec2 g0(p_max.x - 12.0f, p_max.y - 12.0f);
                                draw_list->AddRectFilled(g0, p_max, over_grip || s_resizingCam ? IM_COL32(56, 189, 248, 255) : IM_COL32(14, 165, 233, 200), 2.0f);
                                if (over_grip || s_resizingCam) {
                                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                                } else {
                                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                                }
                            }
                        }
                    }
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

                        bool low_end_changed = false;
                        if (ImGui::Checkbox(
                                tr(lang,
                                    "Weak PC Mode (Pentium / 2GB RAM)",
                                    "Режим слабкого ПК (Pentium / 2 ГБ ОЗП)",
                                    "弱电脑模式 (奔腾 / 2GB 内存)",
                                    "Schwacher PC Modus (Pentium / 2GB RAM)",
                                    "Mode PC faible (Pentium / 2 Go RAM)",
                                    "Tryb słabego PC (Pentium / 2GB RAM)"),
                                &state->low_end_mode)) {
                            low_end_changed = true;
                        }
                        ImGui::TextDisabled("%s",
                            tr(lang,
                                "  Optimized for 2GB RAM & Pentium iGPU: 1080p + 30 FPS with hardware GPU encoding.",
                                "  Оптимізовано для 2 ГБ ОЗП та Pentium iGPU: 1080p + 30 FPS з апаратним кодуванням.",
                                "  针对 2GB 内存与奔腾核显优化: 1080p + 30 FPS 硬件加速编码。",
                                "  Optimiert für 2GB RAM & Pentium iGPU: 1080p + 30 FPS mit Hardware-GPU-Codierung.",
                                "  Optimisé pour 2 Go RAM & Pentium iGPU : 1080p + 30 FPS avec encodage matériel GPU.",
                                "  Zoptymalizowano dla 2GB RAM i Pentium iGPU: 1080p + 30 FPS ze sprzętowym kodowaniem GPU."));
                        if (low_end_changed) {
                            state->action_settings_changed = true;
                            if (state->low_end_mode) {
                                state->output_resolution = 1;
                                state->target_fps = 30;
                                state->encoder_preset = 0;
                                if ((state->encoder_caps_mask & (1 << 6)) != 0) {
                                    state->encoder_choice = 0;
                                    state->qsv_low_power = true;
                                    state->qsv_async_depth = 4;
                                } else if ((state->encoder_caps_mask & (1 << 11)) != 0) {
                                    state->encoder_choice = 4;
                                    state->wmf_hw_encoding = true;
                                } else if ((state->encoder_caps_mask & ((1 << 0) | (1 << 3))) != 0) {
                                    state->encoder_choice = 3;
                                }
                            }
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
                        static bool s_show_all_encoders = false;
                        bool has_qsv_h264 = (state->encoder_caps_mask & (1 << 6)) != 0;
                        bool has_qsv_hevc = (state->encoder_caps_mask & (1 << 7)) != 0;
                        bool has_nvenc    = (state->encoder_caps_mask & ((1 << 0) | (1 << 1) | (1 << 2))) != 0;
                        bool has_amf      = (state->encoder_caps_mask & ((1 << 3) | (1 << 4) | (1 << 5))) != 0;
                        bool has_wmf_h264 = (state->encoder_caps_mask & (1 << 11)) != 0;

                        struct FilteredEncoderChoice {
                            int id;
                            const char* label_supported;
                            const char* label_unsupported;
                            bool supported;
                        };

                        FilteredEncoderChoice all_choices[8] = {
                            { 0, "QuickSync H.264 (Intel QSV) [Hardware]", "QuickSync H.264 (Intel QSV) [Unsupported]", has_qsv_h264 },
                            { 1, "QuickSync HEVC (Intel QSV) [Hardware]", "QuickSync HEVC (Intel QSV) [Unsupported]", has_qsv_hevc },
                            { 2, "Intel Media SDK (oneVPL / QSV Hardware)", "Intel Media SDK (oneVPL) [Unsupported]", has_qsv_h264 },
                            { 3, "FFmpeg Hardware Encoders (NVENC / AMF)", "FFmpeg Hardware (NVENC/AMF) [No NVENC/AMF GPU]", has_nvenc || has_amf },
                            { 4, "Windows Media Foundation (WMF / IMFTransform)", "Windows Media Foundation (WMF) [Unsupported]", has_wmf_h264 },
                            { 5, "OpenH264 (Cisco Baseline)", "OpenH264 (Cisco Baseline)", true },
                            { 6, "libx264 (Software CPU)", "libx264 (Software CPU)", true },
                            { 7, "Motion JPEG (MJPEG)", "Motion JPEG (MJPEG)", true }
                        };

                        const char* avail_labels[8];
                        int avail_ids[8];
                        int avail_count = 0;
                        int current_filtered_idx = 0;

                        for (int i = 0; i < 8; i++) {
                            if (s_show_all_encoders || all_choices[i].supported) {
                                if (all_choices[i].id == state->encoder_choice) {
                                    current_filtered_idx = avail_count;
                                }
                                avail_ids[avail_count] = all_choices[i].id;
                                avail_labels[avail_count] = all_choices[i].supported ? all_choices[i].label_supported : all_choices[i].label_unsupported;
                                avail_count++;
                            }
                        }

                        if (avail_count == 0) {
                            avail_ids[0] = 6;
                            avail_labels[0] = "libx264 (Software CPU)";
                            avail_count = 1;
                            current_filtered_idx = 0;
                        }

                        if (ImGui::Combo(
                            tr(lang, "Encoder Engine", "Рушій енкодера", "编码引擎与架构", "Encoder-Engine", "Moteur d'encodage", "Silnik enkodera"),
                            &current_filtered_idx, avail_labels, avail_count)) {
                            state->encoder_choice = avail_ids[current_filtered_idx];
                            state->action_settings_changed = true;
                        }

                        bool current_is_supported = false;
                        for (int i = 0; i < 8; i++) {
                            if (all_choices[i].id == state->encoder_choice && all_choices[i].supported) {
                                current_is_supported = true;
                                break;
                            }
                        }

                        if (current_is_supported) {
                            ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.35f, 1.00f), "%s",
                                tr(lang,
                                    "  [OK] Supported by your hardware and verified.",
                                    "  [OK] Повністю підтримується вашим обладнанням та готовий до роботи.",
                                    "  [OK] 经检测受硬件完全支持。",
                                    "  [OK] Von Ihrer Hardware unterstützt und geprüft.",
                                    "  [OK] Pris en charge par votre matériel et vérifié.",
                                    "  [OK] Obsługiwane przez twój sprzęt i zweryfikowane."));
                        } else {
                            ImGui::TextColored(ImVec4(1.00f, 0.40f, 0.20f, 1.00f), "%s",
                                tr(lang,
                                    "  [WARNING] Not supported by this GPU (e.g. MX330 lacks NVENC). Fallback will be used.",
                                    "  [УВАГА] Не підтримується цим GPU (наприклад, у MX330 немає NVENC). Запис перейде на CPU.",
                                    "  [警告] 当前 GPU 不支持此编码器 (例如 MX330 缺少 NVENC 模块)。将使用备用方案。",
                                    "  [WARNUNG] Wird von dieser GPU nicht unterstützt. Fallback wird verwendet.",
                                    "  [AVERTISSEMENT] Non pris en charge par ce GPU. Le repli sera utilisé.",
                                    "  [OSTRZEŻENIE] Nieobsługiwane przez ten GPU. Zostanie użyty fallback."));
                        }

                        if (ImGui::Checkbox(
                            tr(lang, "Show unsupported encoders", "Показати непідтримувані рушії", "显示不支持的编码器", "Nicht unterstützte Encoder anzeigen", "Afficher les encodeurs non pris en charge", "Pokaż nieobsługiwane enkodery"),
                            &s_show_all_encoders)) {
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        ImGui::TextColored(ImVec4(0.20f, 0.65f, 1.00f, 1.00f), "%s",
                            tr(lang,
                                "Encoder Specific Settings:",
                                "Індивідуальні налаштування кодека:",
                                "当前编码器专用设置:",
                                "Encoder-spezifische Einstellungen:",
                                "Paramètres spécifiques à l'encodeur :",
                                "Ustawienia wybranego enkodera:"));
                        ImGui::Spacing();

                        if (state->encoder_choice == 0 || state->encoder_choice == 1) {
                            const char* qsv_presets[] = { "veryfast", "faster", "fast", "medium", "slow" };
                            if (ImGui::Combo(tr(lang, "QuickSync Preset", "Пресет QuickSync", "QuickSync 预设", "QuickSync Preset", "Préréglage QuickSync", "Preset QuickSync"), &state->qsv_preset, qsv_presets, 5)) {
                                state->action_settings_changed = true;
                            }
                            if (ImGui::Checkbox(tr(lang, "Low Power Mode (Intel VDENC Fixed-Function ASIC)", "Режим енергозбереження (Intel VDENC)", "低功耗模式 (Intel VDENC 固定功能硬件)", "Low-Power-Modus (Intel VDENC)", "Mode basse consommation (Intel VDENC)", "Tryb niskiego poboru mocy (Intel VDENC)"), &state->qsv_low_power)) {
                                state->action_settings_changed = true;
                            }
                            if (ImGui::SliderInt(tr(lang, "Async Depth", "Асинхронна черга", "异步深度", "Async-Tiefe", "Profondeur asynchrone", "Głębokość asynchroniczna"), &state->qsv_async_depth, 1, 4)) {
                                state->action_settings_changed = true;
                            }
                        } else if (state->encoder_choice == 2) {
                            const char* msdk_codecs[] = { "H.264 (AVC)", "HEVC (H.265)", "Motion JPEG (MJPEG)" };
                            if (ImGui::Combo(tr(lang, "Intel Media SDK Codec", "Кодек Intel Media SDK", "Intel Media SDK 编解码器", "Intel Media SDK Codec", "Codec Intel Media SDK", "Kodek Intel Media SDK"), &state->intel_msdk_codec, msdk_codecs, 3)) {
                                state->action_settings_changed = true;
                            }
                            if (ImGui::Checkbox(tr(lang, "Low Power Hardware Engine", "Апаратний Low Power рушій", "低功耗硬件引擎", "Low-Power-Hardware-Engine", "Moteur matériel basse consommation", "Silnik sprzętowy Low Power"), &state->qsv_low_power)) {
                                state->action_settings_changed = true;
                            }
                            if (ImGui::SliderInt(tr(lang, "Media SDK Pipeline Depth", "Глибина конвеєра", "流水线深度", "Pipeline-Tiefe", "Profondeur de pipeline", "Głębokość potoku"), &state->qsv_async_depth, 1, 4)) {
                                state->action_settings_changed = true;
                            }
                        } else if (state->encoder_choice == 3) {
                            const char* hw_engines[] = {
                                tr(lang, "Auto-Detect Hardware GPU", "Автовизначення GPU", "自动检测 GPU 硬件", "Hardware-GPU automatisch erkennen", "Détection automatique du GPU matériel", "Automatyczne wykrywanie GPU"),
                                "NVIDIA NVENC",
                                "AMD AMF"
                            };
                            if (ImGui::Combo(tr(lang, "GPU Hardware", "Апаратний GPU", "GPU 硬件", "GPU-Hardware", "Matériel GPU", "Sprzęt GPU"), &state->ffmpeg_hw_engine, hw_engines, 3)) {
                                state->action_settings_changed = true;
                            }
                            const char* hw_codecs[] = { "H.264 (AVC)", "HEVC (H.265)", "AV1" };
                            if (ImGui::Combo(tr(lang, "Hardware Codec", "Апаратний кодек", "硬件编解码器", "Hardware-Codec", "Codec matériel", "Kodek sprzętowy"), &state->ffmpeg_hw_codec, hw_codecs, 3)) {
                                state->action_settings_changed = true;
                            }
                            const char* hw_presets[] = { "P1 (Fastest / Lowest Latency)", "P2 (Fast)", "P4 (Medium / Balanced)", "P6 (High Quality)", "P7 (Max Quality)" };
                            if (ImGui::Combo(tr(lang, "GPU Preset", "Пресет GPU", "GPU 预设", "GPU-Preset", "Préréglage GPU", "Preset GPU"), &state->ffmpeg_hw_preset, hw_presets, 5)) {
                                state->action_settings_changed = true;
                            }
                            if (ImGui::Checkbox(tr(lang, "Zero-Latency Ultra Low Delay Mode", "Режим нульової затримки (Ultra Low Delay)", "零延迟超低延迟模式", "Zero-Latency-Modus", "Mode latence zéro", "Tryb zerowego opóźnienia"), &state->ffmpeg_hw_zerolatency)) {
                                state->action_settings_changed = true;
                            }
                        } else if (state->encoder_choice == 4) {
                            const char* wmf_codecs[] = { "H.264 (via MediaFoundation)", "HEVC (via MediaFoundation)" };
                            if (ImGui::Combo(tr(lang, "WMF Codec", "Кодек WMF", "WMF 编解码器", "WMF-Codec", "Codec WMF", "Kodek WMF"), &state->wmf_codec, wmf_codecs, 2)) {
                                state->action_settings_changed = true;
                            }
                            const char* wmf_rc_modes[] = {
                                "CBR (Constant Bitrate)",
                                "Peak Constrained VBR",
                                "Unconstrained VBR",
                                "Quality Mode",
                                "Low Delay VBR"
                            };
                            if (ImGui::Combo(tr(lang, "WMF Rate Control", "Керування бітрейтом WMF", "WMF 码率控制", "WMF-Bitratensteuerung", "Contrôle du débit WMF", "Kontrola bitrate WMF"), &state->wmf_rate_control, wmf_rc_modes, 5)) {
                                state->action_settings_changed = true;
                            }
                            const char* wmf_scenarios[] = {
                                "Display Remoting (Desktop / Game Capture)",
                                "Live Streaming",
                                "Camera Record",
                                "Archive"
                            };
                            if (ImGui::Combo(tr(lang, "WMF Scenario", "Сценарій використання WMF", "WMF 应用场景", "WMF-Szenario", "Scénario WMF", "Scenariusz WMF"), &state->wmf_scenario, wmf_scenarios, 4)) {
                                state->action_settings_changed = true;
                            }
                            if (ImGui::SliderInt(tr(lang, "WMF Quality Factor (1-100)", "Рівень якості WMF (1-100)", "WMF 画质系数 (1-100)", "WMF-Qualitätsfaktor (1-100)", "Facteur de qualité WMF (1-100)", "Współczynnik якості WMF (1-100)"), &state->wmf_quality, 1, 100)) {
                                state->action_settings_changed = true;
                            }
                            if (ImGui::Checkbox(tr(lang, "Force Hardware Encoding (IMFTransform MFT)", "Примусово апаратний MFT (IMFTransform)", "强制硬件 MFT 编码 (IMFTransform)", "Hardware-MFT erzwingen (IMFTransform)", "Forcer l'encodage matériel MFT (IMFTransform)", "Wymuś sprzętowy MFT (IMFTransform)"), &state->wmf_hw_encoding)) {
                                state->action_settings_changed = true;
                            }
                        } else if (state->encoder_choice == 5) {
                            const char* op_profiles[] = { "Constrained Baseline (Cisco)", "Main Profile", "High Profile" };
                            if (ImGui::Combo(tr(lang, "OpenH264 Profile", "Профіль OpenH264", "OpenH264 配置规范", "OpenH264-Profil", "Profil OpenH264", "Profil OpenH264"), &state->openh264_profile, op_profiles, 3)) {
                                state->action_settings_changed = true;
                            }
                            const char* op_slices[] = { "1 Slice (Single Thread)", "2 Slices (Dual Thread)", "4 Slices (Quad Parallel)" };
                            if (ImGui::Combo(tr(lang, "Slice Count", "Кількість слайсів", "切片数量", "Slice-Anzahl", "Nombre de tranches", "Liczba slice"), &state->openh264_slices, op_slices, 3)) {
                                state->action_settings_changed = true;
                            }
                            const char* op_rc[] = { "Bitrate Mode", "Quality Mode" };
                            if (ImGui::Combo(tr(lang, "Rate Control Mode", "Режим бітрейту", "码率控制模式", "Bitraten-Modus", "Mode de contrôle du débit", "Tryb kontroli bitrate"), &state->openh264_rc_mode, op_rc, 2)) {
                                state->action_settings_changed = true;
                            }
                        } else if (state->encoder_choice == 6) {
                            const char* x264_presets[] = { "Ultrafast", "Superfast", "Veryfast", "Faster", "Fast", "Medium", "Slow" };
                            if (ImGui::Combo(tr(lang, "x264 Preset", "Пресет x264", "x264 速度预设", "x264-Preset", "Préréglage x264", "Preset x264"), &state->x264_preset, x264_presets, 7)) {
                                state->action_settings_changed = true;
                            }
                            const char* x264_tunes[] = { "Zero Latency (Game recording)", "Film", "Animation" };
                            if (ImGui::Combo(tr(lang, "x264 Tune", "Тюнінг x264", "x264 调优", "x264-Abstimmung", "Optimisation x264", "Dostrojenie x264"), &state->x264_tune, x264_tunes, 3)) {
                                state->action_settings_changed = true;
                            }
                            const char* x264_profs[] = { "High", "Main", "Baseline" };
                            if (ImGui::Combo(tr(lang, "x264 Profile", "Профіль x264", "x264 配置规范", "x264-Profil", "Profil x264", "Profil x264"), &state->x264_profile, x264_profs, 3)) {
                                state->action_settings_changed = true;
                            }
                            const char* x264_thr[] = { "Auto CPU Threads", "1 Thread", "2 Threads", "4 Threads", "8 Threads" };
                            if (ImGui::Combo(tr(lang, "CPU Slice Threads", "Потоки CPU", "CPU 切片线程数", "CPU-Slice-Threads", "Threads de découpage CPU", "Wątki CPU"), &state->x264_threads, x264_thr, 5)) {
                                state->action_settings_changed = true;
                            }
                        } else if (state->encoder_choice == 7) {
                            if (ImGui::SliderInt(tr(lang, "JPEG Quality Scale (1 = Best, 31 = Lowest)", "Якість JPEG (1 = Найвища, 31 = Найнижча)", "JPEG 画质系数 (1 = 最高无损, 31 = 最低)", "JPEG-Qualitätsstufe (1 = Beste, 31 = Niedrigste)", "Qualité JPEG (1 = Meilleure, 31 = Plus basse)", "Jakość JPEG (1 = Najlepsza, 31 = Najniższa)"), &state->mjpeg_quality, 1, 31)) {
                                state->action_settings_changed = true;
                            }
                            const char* huff_tables[] = { "Optimal Huffman Table (Better Compression)", "Default Standard Table (Faster)" };
                            if (ImGui::Combo(tr(lang, "Huffman Table", "Таблиця Хаффмана", "哈夫曼表", "Huffman-Tabelle", "Table de Huffman", "Tablica Huffmana"), &state->mjpeg_huffman, huff_tables, 2)) {
                                state->action_settings_changed = true;
                            }
                            const char* mjpeg_ranges[] = { "Full Range PC (0-255)", "Limited TV (16-235)" };
                            if (ImGui::Combo(tr(lang, "MJPEG Color Range", "Колірний діапазон MJPEG", "MJPEG 色彩范围", "MJPEG-Farbbereich", "Plage de couleurs MJPEG", "Zakres kolorów MJPEG"), &state->mjpeg_color_range, mjpeg_ranges, 2)) {
                                state->action_settings_changed = true;
                            }
                            ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.55f, 1.00f), "%s",
                                tr(lang,
                                    "  Intra-Frame Only: Every frame is an independent JPEG. 0% lag in video editors (DaVinci / Premiere).",
                                    "  Intra-Frame: Кожен кадр є окремим JPEG. Нульова затримка монтажу у Premiere / DaVinci.",
                                    "  纯帧内编码: 每一帧均为独立 JPEG 图像。在剪辑软件 (DaVinci / Premiere) 中零延迟快速拖拽。",
                                    "  Nur Intra-Frame: Jeder Frame ist ein separates JPEG. 0% Verzögerung beim Schneiden in DaVinci / Premiere.",
                                    "  Intra-trame uniquement : chaque image est un JPEG indépendant. Zéro latence dans DaVinci / Premiere.",
                                    "  Tylko klatki kluczowe: Każda klatka to niezależny JPEG. 0% opóźnień podczas montażu w DaVinci / Premiere."));
                        }

                        if (state->encoder_choice != 7) {
                            ImGui::Spacing();
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

                            if (state->rate_control == 0 || state->rate_control == 1) {
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
                            } else {
                                if (state->quality_cqp < 10 || state->quality_cqp > 40) state->quality_cqp = 20;
                                if (ImGui::SliderInt(
                                    tr(lang, "Quality Level (CQP/CRF: lower = better)", "Коефіцієнт якості (CQP/CRF: менше = краще)", "画质等级 (CQP/CRF: 数值越小画质越高)", "Qualitätsstufe (CQP/CRF: niedriger = besser)", "Niveau de qualité (CQP/CRF : bas = meilleur)", "Poziom jakości (CQP/CRF: mniej = lepiej)"),
                                    &state->quality_cqp, 14, 32)) {
                                    state->action_settings_changed = true;
                                }
                            }

                            int gop_options[] = { 1, 2, 3, 5 };
                            const char* gop_labels[] = { "1 s", "2 s (Recommended)", "3 s", "5 s" };
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
                        }

const char* container_formats[] = {
                            tr(lang, "MKV (*.mkv - Recommended, crash-resilient)", "MKV (*.mkv - Рекомендовано, стійкий до збоїв)", "MKV (*.mkv - 推荐，防崩溃损坏)", "MKV (*.mkv - Empfohlen, absturzsicher)", "MKV (*.mkv - Recommandé, résistant aux crashs)", "MKV (*.mkv - Zalecany, odporny na awarie)"),
                            tr(lang, "MP4 (*.mp4 - Universal compatibility)", "MP4 (*.mp4 - Сумісний з усіма плеєрами)", "MP4 (*.mp4 - 通用兼容格式)", "MP4 (*.mp4 - Universelle Kompatibilität)", "MP4 (*.mp4 - Compatibilité universelle)", "MP4 (*.mp4 - Uniwersalna kompatybilność)"),
tr(lang, "MOV (*.mov - Video Editing / QuickTime)", "MOV (*.mov - Монтаж у DaVinci / Premiere)", "MOV (*.mov - 适合视频剪辑)", "MOV (*.mov - Videoschnitt / QuickTime)", "MOV (*.mov - Montage vidéo / QuickTime)", "MOV (*.mov - Montaż wideo / QuickTime)"),
                            tr(lang, "WebM (*.webm - Web & Browser streaming)", "WebM (*.webm - Для вебу та браузерів)", "WebM (*.webm - 网页与浏览器流媒体)", "WebM (*.webm - Web- & Browser-Streaming)", "WebM (*.webm - Web et streaming navigateur)", "WebM (*.webm - Format do sieci i przeglądarek)"),
                            tr(lang, "WMV (*.wmv - Windows Media Video)", "WMV (*.wmv - Класичний Windows Media)", "WMV (*.wmv - Windows 媒体视频)", "WMV (*.wmv - Windows Media Video)", "WMV (*.wmv - Windows Media Video)", "WMV (*.wmv - Format Windows Media)"),
                            tr(lang, "FLV (*.flv - Flash Video streaming)", "FLV (*.flv - Flash Video стрім)", "FLV (*.flv - Flash 视频流)", "FLV (*.flv - Flash-Video)", "FLV (*.flv - Vidéo Flash)", "FLV (*.flv - Flash Video)"),
                            tr(lang, "F4V (*.f4v - Flash MP4 video)", "F4V (*.f4v - Flash MP4 формат)", "F4V (*.f4v - Flash MP4 视频)", "F4V (*.f4v - Flash MP4-Video)", "F4V (*.f4v - Vidéo Flash MP4)", "F4V (*.f4v - Flash MP4)"),
                            tr(lang, "AVCHD (*.m2ts - High-definition camcorder)", "AVCHD (*.m2ts - Відеокамери високої чіткості)", "AVCHD (*.m2ts - 高清摄像机格式)", "AVCHD (*.m2ts - HD-Camcorder-Format)", "AVCHD (*.m2ts - Format caméscope haute définition)", "AVCHD (*.m2ts - Format kamer wysokiej rozdzielczości)")
                        };
                        if (ImGui::Combo(
                            tr(lang, "Container Format", "Формат відеофайлу", "视频容器格式", "Container-Format", "Format du conteneur", "Format pliku wideo"),
                            &state->container_format, container_formats, 8)) {
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
                            ImGui::PushID("SetSysAudio");
                            render_process_audio_controls(state, lang);
                            ImGui::PopID();
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

                        ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1.00f), "%s",
                            tr(lang,
                                "Additional Audio Sources:",
                                "Додаткові джерела аудіо:",
                                "附加音频源:",
                                "Zusätzliche Audioquellen:",
                                "Sources audio supplémentaires :",
                                "Dodatkowe źródła dźwięku:"));

                        if (ImGui::Checkbox(
                            tr(lang,
                                "2nd System Audio Source",
                                "2-й системний звук",
                                "第2系统声音",
                                "2. Systemton-Quelle",
                                "2e son système",
                                "2. źródło dźwięku systemowego"),
                            &state->record_system_audio2)) {
                            state->action_settings_changed = true;
                        }
                        if (state->record_system_audio2) {
                            if (state->audio_output_count > 0 && state->audio_output_names != NULL) {
                                if (ImGui::Combo(
                                    tr(lang,
                                        "2nd Playback Device",
                                        "2-й пристрій виводу",
                                        "第2播放设备",
                                        "2. Wiedergabegerät",
                                        "2e périphérique de lecture",
                                        "2. urządzenie odtwarzania"),
                                    &state->selected_audio_output2, state->audio_output_names, state->audio_output_count)) {
                                    state->action_settings_changed = true;
                                }
                            }
                            int sys2_vol_percent = (int)(state->system_volume2 * 100.0f + 0.5f);
                            if (ImGui::SliderInt(
                                tr(lang,
                                    "2nd System Volume (%)",
                                    "Гучність 2-ї системи (%)",
                                    "第2系统音量 (%)",
                                    "2. Systemlautstärke (%)",
                                    "Volume 2e système (%)",
                                    "Głośność 2. systemu (%)"),
                                &sys2_vol_percent, 0, 200, "%d%%")) {
                                state->system_volume2 = (float)sys2_vol_percent / 100.0f;
                                state->action_settings_changed = true;
                            }
                            ImGui::SameLine();
                            ImGui::ProgressBar(state->record_system_audio2 ? state->sys_audio_level2 : 0.0f, ImVec2(90, 16), "");
                        }

                        if (ImGui::Checkbox(
                            tr(lang,
                                "2nd Microphone Source",
                                "2-й мікрофон",
                                "第2麦克风",
                                "2. Mikrofon-Quelle",
                                "2e microphone",
                                "2. źródło mikrofonu"),
                            &state->record_microphone2)) {
                            state->action_settings_changed = true;
                        }
                        if (state->record_microphone2) {
                            if (state->audio_input_count > 0 && state->audio_input_names != NULL) {
                                if (ImGui::Combo(
                                    tr(lang,
                                        "2nd Recording Device",
                                        "2-й пристрій вводу",
                                        "第2录音设备",
                                        "2. Aufnahmegerät",
                                        "2e périphérique d'entrée",
                                        "2. urządzenie wejściowe"),
                                    &state->selected_audio_input2, state->audio_input_names, state->audio_input_count)) {
                                    state->action_settings_changed = true;
                                }
                            }
                            int mic2_vol_percent = (int)(state->mic_volume2 * 100.0f + 0.5f);
                            if (ImGui::SliderInt(
                                tr(lang,
                                    "2nd Microphone Volume (%)",
                                    "Гучність 2-го мікрофона (%)",
                                    "第2麦克风音量 (%)",
                                    "2. Mikrofonlautstärke (%)",
                                    "Volume 2e microphone (%)",
                                    "Głośność 2. mikrofonu (%)"),
                                &mic2_vol_percent, 0, 200, "%d%%")) {
                                state->mic_volume2 = (float)mic2_vol_percent / 100.0f;
                                state->action_settings_changed = true;
                            }
                            ImGui::SameLine();
                            ImGui::ProgressBar(state->record_microphone2 ? state->mic_audio_level2 : 0.0f, ImVec2(90, 16), "");
                        }

                        ImGui::TextDisabled("%s",
                            tr(lang,
                                "  Track 0: Mix | Track 1-4: each source on its own track (when Separate Tracks is on)",
                                "  Доріжка 0: Змішана | Доріжки 1-4: кожне джерело на своїй доріжці (коли ввімкнено окремі доріжки)",
                                "  音轨 0: 混音 | 音轨 1-4: 每个源单独音轨 (开启独立音轨时)",
                                "  Spur 0: Mix | Spur 1-4: jede Quelle auf eigener Spur (bei getrennten Spuren)",
                                "  Piste 0 : Mix | Pistes 1-4 : chaque source sur sa propre piste (si pistes séparées)",
                                "  Ścieżka 0: Miks | Ścieżki 1-4: każde źródło na osobnej ścieżce (przy osobnych ścieżkach)"));

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1.00f), "%s",
                            tr(lang,
                                "Voice Processing (Microphone):",
                                "Обробка голосу (мікрофон):",
                                "语音处理 (麦克风):",
                                "Sprachverarbeitung (Mikrofon):",
                                "Traitement de la voix (Microphone) :",
                                "Przetwarzanie głosu (Mikrofon):"));
                        ImGui::Spacing();

                        if (ImGui::Checkbox(
                            tr(lang,
                                "Noise Suppression",
                                "Шумозаглушення",
                                "噪声抑制",
                                "Rauschunterdrückung",
                                "Réduction du bruit",
                                "Redukcja szumu"),
                            &state->mic_noise_suppression)) {
                            state->action_settings_changed = true;
                        }
                        if (state->mic_noise_suppression) {
                            const char* set_ns_modes[2] = {
                                tr(lang, "AI Spectral Bark Denoiser (RNNoise-style)", "AI Спектральний шумоподав (прибирає шум під час мовлення)", "AI 频谱降噪 (说话时消除噪音)", "KI-Spektral-Rauschfilter", "Débruiteur IA Spectral", "AI Spektralna redukcja szumu"),
                                tr(lang, "Classic Light Gate (Eco CPU)", "Класичний шумовий гейт (Ультралегкий)", "经典噪声门 (极低占用)", "Klassisches Noise-Gate", "Porte de bruit classique", "Klasyczna bramka szumu")
                            };
                            if (ImGui::Combo(tr(lang, "Denoiser Engine##Set", "Алгоритм##Set", "降噪引擎##Set", "Modus##Set", "Moteur##Set", "Tryb##Set"), &state->mic_ns_mode, set_ns_modes, 2)) {
                                state->action_settings_changed = true;
                            }
                            int ns_pct = (int)(state->mic_ns_strength * 100.0f + 0.5f);
                            if (ImGui::SliderInt(
                                tr(lang,
                                    "Suppression Strength (%)",
                                    "Сила шумозаглушення (%)",
                                    "抑制强度 (%)",
                                    "Unterdrückungsstärke (%)",
                                    "Intensité de la réduction (%)",
                                    "Siła redukcji (%)"),
                                &ns_pct, 0, 100, "%d%%")) {
                                state->mic_ns_strength = (float)ns_pct / 100.0f;
                                state->action_settings_changed = true;
                            }
                            ImGui::TextDisabled("%s",
                                tr(lang,
                                    "  Removes hum, rumble and constant background noise (high-pass + noise gate).",
                                    "  Прибирає гул, низькочастотний шум і постійний фоновий шум (HP-фільтр + шумовий гейт).",
                                    "  消除嗡嗡声、隆隆声和持续背景噪声 (高通滤波器 + 噪声门)。",
                                    "  Entfernt Brummen, Rumpeln und konstantes Hintergrundrauschen (Hochpass + Noise-Gate).",
                                    "  Supprime la rumeur, les basses fréquences et le bruit de fond constant (passe-haut + porte de bruit).",
                                    "  Usuwa brzęczenie, dudnienie i stały szum tła (filtr górnoprzepustowy + bramka szumu)."));
                        }

                        if (ImGui::Checkbox(
                            tr(lang,
                                "Compressor (Levels Out Loud Peaks)",
                                "Компресор (вирівнює голосні піки)",
                                "压缩器 (平衡响亮峰值)",
                                "Kompressor (Gleicht laute Spitzen aus)",
                                "Compresseur (Égalise les pics sonores)",
                                "Kompresor (Wyrównuje głośne piki)"),
                            &state->mic_compressor)) {
                            state->action_settings_changed = true;
                        }
                        if (state->mic_compressor) {
                            int comp_pct = (int)(state->mic_comp_amount * 100.0f + 0.5f);
                            if (ImGui::SliderInt(
                                tr(lang,
                                    "Compression Amount (%)",
                                    "Ступінь компресії (%)",
                                    "压缩量 (%)",
                                    "Kompressionsgrad (%)",
                                    "Degré de compression (%)",
                                    "Stopień kompresji (%)"),
                                &comp_pct, 0, 100, "%d%%")) {
                                state->mic_comp_amount = (float)comp_pct / 100.0f;
                                state->action_settings_changed = true;
                            }
                        }

                        {
                            int gain_db = (int)(state->mic_gain_db + (state->mic_gain_db >= 0 ? 0.5f : -0.5f));
                            if (ImGui::SliderInt(
                                tr(lang,
                                    "Volume Boost (dB)",
                                    "Підсилення гучності (дБ)",
                                    "音量增益 (分贝)",
                                    "Lautstärke-Verstärkung (dB)",
                                    "Amplification du volume (dB)",
                                    "Wzmocnienie głośności (dB)"),
                                &gain_db, -12, 24, "%d dB")) {
                                state->mic_gain_db = (float)gain_db;
                                state->action_settings_changed = true;
                            }
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1.00f), "%s",
                            tr(lang,
                                "Speakers / System Audio Filters:",
                                "Фільтри динаміків / системного звуку:",
                                "扬声器 / 系统音频滤镜:",
                                "Lautsprecher / Systemton-Filter:",
                                "Filtres haut-parleurs / système :",
                                "Filtry głośników / systemu:"));
                        ImGui::Spacing();

                        if (ImGui::Checkbox(
                            tr(lang,
                                "Clarity Filter (Sub-Rumble Cut)",
                                "Фільтр чистоти (зріз низького гулу)",
                                "清晰度滤波 (切除低频轰鸣)",
                                "Klarheitsfilter (Sub-Bass-Cut)",
                                "Filtre de clarté (Coupe-bas)",
                                "Filtr czystości (Odcięcie basu)"),
                            &state->sys_clarity_filter)) {
                            state->action_settings_changed = true;
                        }
                        if (ImGui::Checkbox(
                            tr(lang,
                                "Loudness Compressor (Night Mode)",
                                "Компресор гучності (вирівнює вибухи та діалоги)",
                                "响度压缩器 (夜间模式)",
                                "Lautheitskompressor (Nachtmodus)",
                                "Compresseur de niveau (Mode nuit)",
                                "Kompresor głośności (Tryb nocny)"),
                            &state->sys_compressor)) {
                            state->action_settings_changed = true;
                        }
                        if (state->sys_compressor) {
                            int scomp_pct = (int)(state->sys_comp_amount * 100.0f + 0.5f);
                            if (ImGui::SliderInt(
                                tr(lang,
                                    "System Compression (%)",
                                    "Ступінь компресії системи (%)",
                                    "系统压缩量 (%)",
                                    "System-Kompression (%)",
                                    "Compression système (%)",
                                    "Kompresja systemu (%)"),
                                &scomp_pct, 0, 100, "%d%%")) {
                                state->sys_comp_amount = (float)scomp_pct / 100.0f;
                                state->action_settings_changed = true;
                            }
                        }
                        {
                            int sgain_db = (int)(state->sys_gain_db + (state->sys_gain_db >= 0 ? 0.5f : -0.5f));
                            if (ImGui::SliderInt(
                                tr(lang,
                                    "System Volume Boost (dB)",
                                    "Підсилення системи (дБ)",
                                    "系统音量增益 (分贝)",
                                    "System-Verstärkung (dB)",
                                    "Gain système (dB)",
                                    "Wzmocnienie systemu (dB)"),
                                &sgain_db, -12, 18, "%d dB")) {
                                state->sys_gain_db = (float)sgain_db;
                                state->action_settings_changed = true;
                            }
                        }

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
                                "  Track 0: Mix of all sources | Tracks 1-4: each source on its own track (ideal for editing)",
                                "  Доріжка 0: мікс усіх джерел | Доріжки 1-4: кожне джерело окремо (зручно для монтажу)",
                                "  音轨 0: 所有源混音 | 音轨 1-4: 每个源单独音轨 (方便后期剪辑)",
                                "  Spur 0: Mix aller Quellen | Spuren 1-4: jede Quelle einzeln (Ideal für Videoschnitt)",
                                "  Piste 0 : Mix de toutes les sources | Pistes 1-4 : chaque source séparément (Idéal pour le montage)",
                                "  Ścieżka 0: Miks wszystkich źródeł | Ścieżki 1-4: każde źródło osobno (Wygodne do montażu)"));

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
                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                        render_app_audio_mixer_ui(lang);
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(tr(lang, "Camera & Overlay", "Вебкамера та Оверлей", "摄像头与画中画", "Kamera & Overlay", "Caméra & Incrustation", "Kamera i Nakładka"))) {
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.14f, 0.17f, 1.00f));
                    ImGui::BeginChild("CameraSettingsCard", ImVec2(0, 0), true);
                    render_camera_controls(state, lang);
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                    ImGui::EndTabItem();
                }

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
        reset_preview_shared_cache();
        g_previewWidth = 0;
        g_previewHeight = 0;
        if (g_previewCsInit) LeaveCriticalSection(&g_previewCs);
    }
    g_previewEnabled = should_enable_preview;
}

void imgui_window_present(void) {
    if (g_skipCurrentFrameRender || (g_hWnd && IsIconic(g_hWnd))) {
        Sleep(25);
        return;
    }

    ImGui::Render();
    const float clear_color[4] = { 0.10f, 0.11f, 0.13f, 1.00f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    HRESULT hr = g_pSwapChain->Present(1, 0);

    if (hr == DXGI_STATUS_OCCLUDED) {
        Sleep(30);
    } else if (g_hWnd && GetForegroundWindow() != g_hWnd) {
        Sleep(25);
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
    if (g_micTestThread) {
        InterlockedExchange(&g_micTestRunning, 0);
        WaitForSingleObject(g_micTestThread, 500);
        CloseHandle(g_micTestThread);
        g_micTestThread = NULL;
    }
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
    reset_preview_shared_cache();
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
