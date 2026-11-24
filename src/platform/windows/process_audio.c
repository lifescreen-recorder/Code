#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <audioclient.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "process_audio.h"

static const GUID IID_IUnknown_Local =
    { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID IID_IAgileObject_Local =
    { 0x94EA2B94, 0xE9CC, 0x49E0, { 0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90 } };
static const GUID IID_IActivateAudioInterfaceCompletionHandler_Local =
    { 0x41D949AB, 0x9862, 0x444A, { 0x80, 0xF6, 0xC2, 0x61, 0x33, 0x4D, 0xA5, 0xEB } };
static const GUID IID_IAudioClient_Local =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID IID_IAudioCaptureClient_Local =
    { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };

typedef struct LS_ActivateAudioInterfaceAsyncOp LS_ActivateAudioInterfaceAsyncOp;
typedef struct LS_ActivateAudioInterfaceAsyncOpVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(LS_ActivateAudioInterfaceAsyncOp *This, REFIID riid, void **ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(LS_ActivateAudioInterfaceAsyncOp *This);
    ULONG (STDMETHODCALLTYPE *Release)(LS_ActivateAudioInterfaceAsyncOp *This);
    HRESULT (STDMETHODCALLTYPE *GetActivateResult)(LS_ActivateAudioInterfaceAsyncOp *This, HRESULT *activateResult, IUnknown **activatedInterface);
} LS_ActivateAudioInterfaceAsyncOpVtbl;

struct LS_ActivateAudioInterfaceAsyncOp {
    const LS_ActivateAudioInterfaceAsyncOpVtbl *lpVtbl;
};

typedef struct LS_CompletionHandler LS_CompletionHandler;
typedef struct LS_CompletionHandlerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(LS_CompletionHandler *This, REFIID riid, void **ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(LS_CompletionHandler *This);
    ULONG (STDMETHODCALLTYPE *Release)(LS_CompletionHandler *This);
    HRESULT (STDMETHODCALLTYPE *ActivateCompleted)(LS_CompletionHandler *This, LS_ActivateAudioInterfaceAsyncOp *activateOperation);
} LS_CompletionHandlerVtbl;

struct LS_CompletionHandler {
    const LS_CompletionHandlerVtbl *lpVtbl;
    volatile LONG ref_count;
    HANDLE done_event;
    HRESULT activate_hr;
    IAudioClient *audio_client;
};

typedef struct LS_ProcessLoopbackParams {
    DWORD TargetProcessId;
    int ProcessLoopbackMode;
} LS_ProcessLoopbackParams;

typedef struct LS_ActivationParams {
    int ActivationType;
    LS_ProcessLoopbackParams ProcessLoopbackParams;
} LS_ActivationParams;

typedef HRESULT (WINAPI *PFN_ActivateAudioInterfaceAsync)(
    LPCWSTR deviceInterfacePath,
    REFIID riid,
    PROPVARIANT *activationParams,
    void *completionHandler,
    LS_ActivateAudioInterfaceAsyncOp **activationOperation
);

static HRESULT STDMETHODCALLTYPE Handler_QueryInterface(LS_CompletionHandler *This, REFIID riid, void **ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown_Local) ||
        IsEqualGUID(riid, &IID_IAgileObject_Local) ||
        IsEqualGUID(riid, &IID_IActivateAudioInterfaceCompletionHandler_Local)) {
        *ppvObject = This;
        InterlockedIncrement(&This->ref_count);
        return S_OK;
    }
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Handler_AddRef(LS_CompletionHandler *This) {
    return (ULONG)InterlockedIncrement(&This->ref_count);
}

static ULONG STDMETHODCALLTYPE Handler_Release(LS_CompletionHandler *This) {
    LONG c = InterlockedDecrement(&This->ref_count);
    return (ULONG)c;
}

static HRESULT STDMETHODCALLTYPE Handler_ActivateCompleted(LS_CompletionHandler *This, LS_ActivateAudioInterfaceAsyncOp *op) {
    if (op) {
        HRESULT hrAct = E_FAIL;
        IUnknown *pUnk = NULL;
        HRESULT hr = op->lpVtbl->GetActivateResult(op, &hrAct, &pUnk);
        if (SUCCEEDED(hr) && SUCCEEDED(hrAct) && pUnk) {
            IAudioClient *pClient = NULL;
            if (SUCCEEDED(pUnk->lpVtbl->QueryInterface(pUnk, &IID_IAudioClient_Local, (void**)&pClient))) {
                This->audio_client = pClient;
                This->activate_hr = S_OK;
            }
            pUnk->lpVtbl->Release(pUnk);
        } else {
            This->activate_hr = FAILED(hrAct) ? hrAct : hr;
        }
    }
    if (This->done_event) {
        SetEvent(This->done_event);
    }
    return S_OK;
}

static const LS_CompletionHandlerVtbl g_HandlerVtbl = {
    Handler_QueryInterface,
    Handler_AddRef,
    Handler_Release,
    Handler_ActivateCompleted
};

typedef struct ProcessAudioCapture {
    IAudioClient *client;
    IAudioCaptureClient *capture_client;
    bool com_init;
} ProcessAudioCapture;

void* process_audio_open(uint32_t target_pid, bool exclude_process) {
    if (target_pid == 0) return NULL;

    HMODULE hMmdev = GetModuleHandleW(L"Mmdevapi.dll");
    if (!hMmdev) {
        hMmdev = LoadLibraryW(L"Mmdevapi.dll");
    }
    if (!hMmdev) return NULL;

    PFN_ActivateAudioInterfaceAsync pActivate =
        (PFN_ActivateAudioInterfaceAsync)GetProcAddress(hMmdev, "ActivateAudioInterfaceAsync");
    if (!pActivate) return NULL;

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool did_com = SUCCEEDED(hrCom);

    LS_CompletionHandler handler;
    handler.lpVtbl = &g_HandlerVtbl;
    handler.ref_count = 1;
    handler.done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    handler.activate_hr = E_FAIL;
    handler.audio_client = NULL;

    if (!handler.done_event) {
        if (did_com) CoUninitialize();
        return NULL;
    }

    LS_ActivationParams params;
    memset(&params, 0, sizeof(params));
    params.ActivationType = 1;
    params.ProcessLoopbackParams.TargetProcessId = (DWORD)target_pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = exclude_process ? 1 : 0;

    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = (BYTE*)&params;

    LS_ActivateAudioInterfaceAsyncOp *pAsyncOp = NULL;
    HRESULT hr = pActivate(L"VAD\\Process_Loopback", &IID_IAudioClient_Local, &pv, &handler, &pAsyncOp);
    if (FAILED(hr)) {
        CloseHandle(handler.done_event);
        if (did_com) CoUninitialize();
        return NULL;
    }

    WaitForSingleObject(handler.done_event, 2000);
    if (pAsyncOp) {
        pAsyncOp->lpVtbl->Release(pAsyncOp);
    }
    CloseHandle(handler.done_event);

    if (FAILED(handler.activate_hr) || !handler.audio_client) {
        if (handler.audio_client) handler.audio_client->lpVtbl->Release(handler.audio_client);
        if (did_com) CoUninitialize();
        return NULL;
    }

    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 48000;
    wfx.wBitsPerSample = 32;
    wfx.nBlockAlign = 8;
    wfx.nAvgBytesPerSec = 48000 * 8;
    wfx.cbSize = 0;

    DWORD stream_flags = 0x00020000 | 0x80000000 | 0x08000000;
    hr = handler.audio_client->lpVtbl->Initialize(
        handler.audio_client,
        AUDCLNT_SHAREMODE_SHARED,
        stream_flags,
        200000,
        0,
        &wfx,
        NULL
    );
    if (FAILED(hr)) {
        handler.audio_client->lpVtbl->Release(handler.audio_client);
        if (did_com) CoUninitialize();
        return NULL;
    }

    IAudioCaptureClient *pCap = NULL;
    hr = handler.audio_client->lpVtbl->GetService(handler.audio_client, &IID_IAudioCaptureClient_Local, (void**)&pCap);
    if (FAILED(hr) || !pCap) {
        handler.audio_client->lpVtbl->Release(handler.audio_client);
        if (did_com) CoUninitialize();
        return NULL;
    }

    hr = handler.audio_client->lpVtbl->Start(handler.audio_client);
    if (FAILED(hr)) {
        pCap->lpVtbl->Release(pCap);
        handler.audio_client->lpVtbl->Release(handler.audio_client);
        if (did_com) CoUninitialize();
        return NULL;
    }

    ProcessAudioCapture *inst = (ProcessAudioCapture*)calloc(1, sizeof(ProcessAudioCapture));
    if (!inst) {
        handler.audio_client->lpVtbl->Stop(handler.audio_client);
        pCap->lpVtbl->Release(pCap);
        handler.audio_client->lpVtbl->Release(handler.audio_client);
        if (did_com) CoUninitialize();
        return NULL;
    }
    inst->client = handler.audio_client;
    inst->capture_client = pCap;
    inst->com_init = did_com;
    return inst;
}

uint32_t process_audio_read(void* handle, float* out_samples, uint32_t max_floats, float* out_peak) {
    if (!handle || !out_samples || max_floats < 2) {
        if (out_peak) *out_peak = 0.0f;
        return 0;
    }

    ProcessAudioCapture *inst = (ProcessAudioCapture*)handle;
    UINT32 packet_len = 0;
    if (FAILED(inst->capture_client->lpVtbl->GetNextPacketSize(inst->capture_client, &packet_len)) || packet_len == 0) {
        if (out_peak) *out_peak = 0.0f;
        return 0;
    }

    uint32_t total_written = 0;
    float peak = 0.0f;

    while (packet_len > 0 && total_written + 2 <= max_floats) {
        BYTE *pData = NULL;
        UINT32 numFrames = 0;
        DWORD flags = 0;

        HRESULT hr = inst->capture_client->lpVtbl->GetBuffer(
            inst->capture_client,
            &pData,
            &numFrames,
            &flags,
            NULL,
            NULL
        );
        if (FAILED(hr) || numFrames == 0) break;

        uint32_t frames_to_copy = numFrames;
        if (total_written + frames_to_copy * 2 > max_floats) {
            frames_to_copy = (max_floats - total_written) / 2;
        }

        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !pData) {
            memset(out_samples + total_written, 0, frames_to_copy * 2 * sizeof(float));
        } else {
            const float *src = (const float*)pData;
            uint32_t count = frames_to_copy * 2;
            memcpy(out_samples + total_written, src, count * sizeof(float));
            for (uint32_t i = 0; i < count; i++) {
                float a = fabsf(src[i]);
                if (a > peak) peak = a;
            }
        }

        total_written += frames_to_copy * 2;
        inst->capture_client->lpVtbl->ReleaseBuffer(inst->capture_client, numFrames);

        if (total_written + 2 > max_floats) break;
        if (FAILED(inst->capture_client->lpVtbl->GetNextPacketSize(inst->capture_client, &packet_len))) break;
    }

    if (out_peak) *out_peak = peak > 1.0f ? 1.0f : peak;
    return total_written;
}

void process_audio_close(void* handle) {
    if (!handle) return;
    ProcessAudioCapture *inst = (ProcessAudioCapture*)handle;
    if (inst->client) {
        inst->client->lpVtbl->Stop(inst->client);
    }
    if (inst->capture_client) {
        inst->capture_client->lpVtbl->Release(inst->capture_client);
    }
    if (inst->client) {
        inst->client->lpVtbl->Release(inst->client);
    }
    if (inst->com_init) {
        CoUninitialize();
    }
    free(inst);
}
