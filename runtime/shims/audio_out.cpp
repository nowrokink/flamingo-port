// The title's ways to the PC's audio output, watched: XAudio2 (which Bink2Scarlett.dll plays a video's sound
// through) and the COM objects the title creates (its own mix goes to a system audio endpoint).
//
// xaudio2_9.dll #1 / XAudio2Create: Windows' XAudio2 2.9, made with the console-only flags dropped. Its
// IXAudio2 methods are patched in place (all instances share the table): 5 CreateSourceVoice logs each voice's
// format, with console-only voice flags dropped; 7 CreateMasteringVoice drops the console's device ids and flags.
// CoCreateInstance (api-ms-win-core-com): logged (class, interface, result), then Windows' own.
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <propidl.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "shims.h"

namespace {

using CreateFn = HRESULT(WINAPI*)(void**, UINT32, UINT32);
using SourceFn = HRESULT(WINAPI*)(void*, void**, const WAVEFORMATEX*, UINT32, float, void*, const void*, const void*);
using MasterFn = HRESULT(WINAPI*)(void*, void**, UINT32, UINT32, UINT32, LPCWSTR, const void*, UINT32);

constexpr UINT32 kEngineFlags = 0x2000 | 0x8000 | 0x10000;  // what Windows takes: STOP_ENGINE_WHEN_IDLE, 1024_QUANTUM,
                                                            // NO_VIRTUAL_AUDIO_CLIENT
constexpr UINT32 kVoiceFlags = 0x2 | 0x4 | 0x8;              // NOPITCH, NOSRC, USEFILTER
constexpr UINT32 kMasterFlags = 0x10000;                     // NO_VIRTUAL_AUDIO_CLIENT

SourceFn g_source = nullptr;
MasterFn g_master = nullptr;
volatile long g_logged = 0;

bool log_more() { return InterlockedIncrement(&g_logged) <= 64; }

HRESULT WINAPI create_source_voice(void* self, void** voice, const WAVEFORMATEX* f, UINT32 flags, float ratio,
                                   void* callback, const void* sends, const void* effects) {
    HRESULT hr = g_source(self, voice, f, flags & kVoiceFlags, ratio, callback, sends, effects);
    if (log_more() && f) {
        std::fprintf(stderr,
                     "[rt] xaudio2: CreateSourceVoice(tag %#x, %u ch, %lu Hz, %u bits, block %u, extra %u, flags %#x, "
                     "max ratio %g)",
                     f->wFormatTag, f->nChannels, f->nSamplesPerSec, f->wBitsPerSample, f->nBlockAlign, f->cbSize,
                     flags, ratio);
        if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
            auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
            std::fprintf(stderr, " subformat %08lx valid bits %u mask %#lx", x->SubFormat.Data1, x->Samples.wValidBitsPerSample,
                         x->dwChannelMask);
        }
        std::fprintf(stderr, " -> %08lX\n", static_cast<unsigned long>(hr));
    }
    return hr;
}

HRESULT WINAPI create_mastering_voice(void* self, void** voice, UINT32 channels, UINT32 rate, UINT32 flags,
                                      LPCWSTR device, const void* effects, UINT32 category) {
    HRESULT hr = g_master(self, voice, channels, rate, flags & kMasterFlags, nullptr, effects, category);
    if (FAILED(hr)) hr = g_master(self, voice, 0, 0, 0, nullptr, nullptr, 6);  // defaults, game effects
    std::fprintf(stderr, "[rt] xaudio2: CreateMasteringVoice(%u ch, %u Hz, flags %#x, device %ls, category %u) -> %08lX\n",
                 channels, rate, flags, device ? device : L"(default)", category, static_cast<unsigned long>(hr));
    return hr;
}

void patch_slot(void** vtbl, int slot, void* fn, void** original) {
    DWORD old;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old)) return;
    if (!*original) *original = vtbl[slot];
    vtbl[slot] = fn;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
}

HRESULT WINAPI XAudio2Create_(void** out, UINT32 flags, UINT32 processor) {
    HMODULE x = LoadLibraryExW(L"xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create = x ? reinterpret_cast<CreateFn>(GetProcAddress(x, "XAudio2Create")) : nullptr;
    if (!create || !out) return E_FAIL;
    HRESULT hr = create(out, flags & kEngineFlags, 0 /* XAUDIO2_USE_DEFAULT_PROCESSOR */);
    std::fprintf(stderr, "[rt] xaudio2: XAudio2Create(flags %#x, processor %#x) -> %08lX (Windows' XAudio2 2.9)\n", flags,
                 processor, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && *out) {
        void** vtbl = *static_cast<void***>(*out);
        patch_slot(vtbl, 5, reinterpret_cast<void*>(&create_source_voice), reinterpret_cast<void**>(&g_source));
        patch_slot(vtbl, 7, reinterpret_cast<void*>(&create_mastering_voice), reinterpret_cast<void**>(&g_master));
    }
    return hr;
}

// ---- the title's audio endpoint, watched down the chain: IMMDeviceEnumerator::GetDefaultAudioEndpoint (slot 4) ->
// IMMDevice::Activate (slot 3) -> IAudioClient::Initialize (slot 3) or ISpatialAudioClient::
// ActivateSpatialAudioStream (slot 10): the format the title's mix goes out in.

void log_format(const char* what, const WAVEFORMATEX* f) {
    if (!f) {
        std::fprintf(stderr, "[rt] audio out: %s: no format\n", what);
        return;
    }
    std::fprintf(stderr, "[rt] audio out: %s: tag %#x, %u ch, %lu Hz, %u bits, block %u", what, f->wFormatTag,
                 f->nChannels, f->nSamplesPerSec, f->wBitsPerSample, f->nBlockAlign);
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
        auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        std::fprintf(stderr, ", subformat %08lx, valid %u bits, mask %#lx", x->SubFormat.Data1,
                     x->Samples.wValidBitsPerSample, x->dwChannelMask);
    }
    std::fprintf(stderr, "\n");
}

using InitFn = HRESULT(WINAPI*)(void*, int, DWORD, LONGLONG, LONGLONG, const WAVEFORMATEX*, const GUID*);
using SpatialFn = HRESULT(WINAPI*)(void*, const PROPVARIANT*, REFIID, void**);
using ActivateFn = HRESULT(WINAPI*)(void*, REFIID, DWORD, PROPVARIANT*, void**);
using EndpointFn = HRESULT(WINAPI*)(void*, int, int, void**);
InitFn g_init = nullptr;
SpatialFn g_spatial = nullptr;
ActivateFn g_activate = nullptr;
EndpointFn g_endpoint = nullptr;

HRESULT WINAPI client_initialize(void* self, int share, DWORD flags, LONGLONG duration, LONGLONG period,
                                 const WAVEFORMATEX* f, const GUID* session) {
    HRESULT hr = g_init(self, share, flags, duration, period, f, session);
    std::fprintf(stderr, "[rt] audio out: IAudioClient::Initialize(%s, flags %#lx, buffer %lld us, period %lld us) -> %08lX\n",
                 share ? "exclusive" : "shared", flags, duration / 10, period / 10, static_cast<unsigned long>(hr));
    log_format("  its format", f);
    return hr;
}

// ---- the stream's objects (ISpatialAudioObjectRenderStream: 8 BeginUpdatingAudioObjects, 9 EndUpdatingAudioObjects,
// 10 ActivateSpatialAudioObject; ISpatialAudioObject: 3 GetBuffer, 6 GetAudioObjectType): the frames Windows asks for
// per update are logged, and with RT_AUDIO_TAP=1 what the title wrote into its front left / right objects is
// appended to %LOCALAPPDATA%\Flamingo\diag\mixed.wav (48 kHz float stereo, up to 60 s) at each update's end.
using BeginFn = HRESULT(WINAPI*)(void*, UINT32*, UINT32*);
using EndFn = HRESULT(WINAPI*)(void*);
using ObjectFn = HRESULT(WINAPI*)(void*, UINT32, void**);
using BufferFn = HRESULT(WINAPI*)(void*, BYTE**, UINT32*);
BeginFn g_begin = nullptr;
EndFn g_end = nullptr;
ObjectFn g_object = nullptr;
BufferFn g_buffer = nullptr;

struct Out {
    void* objects[2] = {};      // front left, front right
    const float* buffers[2] = {};
    UINT32 frames = 0;
    uint64_t updates = 0, written = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    bool tried = false;
};
Out g_out;  // the title's audio thread only

void out_header() {
    uint32_t data = static_cast<uint32_t>(g_out.written * 8), riff = 36 + data, rate = 48000, bps = 48000 * 8;
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 3, 0, 2, 0};
    std::memcpy(h + 4, &riff, 4);
    std::memcpy(h + 24, &rate, 4);
    std::memcpy(h + 28, &bps, 4);
    h[32] = 8, h[34] = 32;
    std::memcpy(h + 36, "data", 4);
    std::memcpy(h + 40, &data, 4);
    DWORD n;
    SetFilePointer(g_out.file, 0, nullptr, FILE_BEGIN);
    WriteFile(g_out.file, h, sizeof h, &n, nullptr);
    SetFilePointer(g_out.file, 0, nullptr, FILE_END);
}

HRESULT WINAPI stream_begin(void* self, UINT32* dynamic, UINT32* frames) {
    HRESULT hr = g_begin(self, dynamic, frames);
    g_out.frames = frames ? *frames : 0;
    g_out.buffers[0] = g_out.buffers[1] = nullptr;
    uint64_t n = ++g_out.updates;
    if ((n & (n - 1)) == 0)
        std::fprintf(stderr, "[rt] audio out: update %llu: %u frames a buffer -> %08lX\n", static_cast<unsigned long long>(n),
                     g_out.frames, static_cast<unsigned long>(hr));
    return hr;
}

HRESULT WINAPI object_buffer(void* self, BYTE** buffer, UINT32* bytes) {
    HRESULT hr = g_buffer(self, buffer, bytes);
    for (int i = 0; i < 2; ++i)
        if (self == g_out.objects[i] && SUCCEEDED(hr) && buffer) g_out.buffers[i] = reinterpret_cast<const float*>(*buffer);
    return hr;
}

HRESULT WINAPI stream_end(void* self) {
    if (!g_out.tried) {
        g_out.tried = true;
        char on[4];
        wchar_t local[MAX_PATH];
        if (GetEnvironmentVariableA("RT_AUDIO_TAP", on, sizeof on) && on[0] == '1' &&
            GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
            std::wstring path = std::wstring(local) + L"\\Flamingo\\diag\\mixed.wav";
            g_out.file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
            if (g_out.file != INVALID_HANDLE_VALUE) out_header();
        }
    }
    if (g_out.file != INVALID_HANDLE_VALUE && g_out.frames && g_out.written < 48000ull * 60) {
        std::vector<float> st(size_t{g_out.frames} * 2, 0.0f);
        for (UINT32 i = 0; i < g_out.frames; ++i)
            for (int c = 0; c < 2; ++c)
                if (g_out.buffers[c]) st[i * 2 + c] = g_out.buffers[c][i];
        DWORD n;
        WriteFile(g_out.file, st.data(), static_cast<DWORD>(st.size() * 4), &n, nullptr);
        g_out.written += g_out.frames;
        out_header();
    }
    return g_end(self);
}

HRESULT WINAPI stream_object(void* self, UINT32 type, void** out) {
    HRESULT hr = g_object(self, type, out);
    std::fprintf(stderr, "[rt] audio out: ActivateSpatialAudioObject(type %#x) -> %08lX\n", type,
                 static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && out && *out) {
        if (type == 0x2) g_out.objects[0] = *out;  // AudioObjectType_FrontLeft
        if (type == 0x4) g_out.objects[1] = *out;  // AudioObjectType_FrontRight
        patch_slot(*static_cast<void***>(*out), 3, reinterpret_cast<void*>(&object_buffer),
                   reinterpret_cast<void**>(&g_buffer));
    }
    return hr;
}

HRESULT WINAPI spatial_activate(void* self, const PROPVARIANT* params, REFIID iid, void** out) {
    HRESULT hr = g_spatial(self, params, iid, out);
    if (SUCCEEDED(hr) && out && *out) {
        void** vtbl = *static_cast<void***>(*out);
        patch_slot(vtbl, 8, reinterpret_cast<void*>(&stream_begin), reinterpret_cast<void**>(&g_begin));
        patch_slot(vtbl, 9, reinterpret_cast<void*>(&stream_end), reinterpret_cast<void**>(&g_end));
        patch_slot(vtbl, 10, reinterpret_cast<void*>(&stream_object), reinterpret_cast<void**>(&g_object));
    }
    std::fprintf(stderr, "[rt] audio out: ISpatialAudioClient::ActivateSpatialAudioStream(iid {%08lX-...}) -> %08lX\n",
                 iid.Data1, static_cast<unsigned long>(hr));
    if (params && params->vt == VT_BLOB && params->blob.pBlobData && params->blob.cbSize >= 24) {
        struct Activation {  // SpatialAudioObjectRenderStreamActivationParams
            const WAVEFORMATEX* format;
            UINT32 static_mask, min_dynamic, max_dynamic, category;
        };
        auto* a = reinterpret_cast<const Activation*>(params->blob.pBlobData);
        std::fprintf(stderr, "[rt] audio out:   static objects %#x, dynamic %u-%u, category %u\n", a->static_mask,
                     a->min_dynamic, a->max_dynamic, a->category);
        log_format("  object format", a->format);
    }
    return hr;
}

HRESULT WINAPI device_activate(void* self, REFIID iid, DWORD context, PROPVARIANT* params, void** out) {
    HRESULT hr = g_activate(self, iid, context, params, out);
    std::fprintf(stderr, "[rt] audio out: IMMDevice::Activate(iid {%08lX-%04X-%04X-...}) -> %08lX\n", iid.Data1, iid.Data2,
                 iid.Data3, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && out && *out) {
        void** vtbl = *static_cast<void***>(*out);
        if (iid.Data1 == 0x1CB9AD4C || iid.Data1 == 0x726778CD || iid.Data1 == 0x7ED4EE07)  // IAudioClient 1 / 2 / 3
            patch_slot(vtbl, 3, reinterpret_cast<void*>(&client_initialize), reinterpret_cast<void**>(&g_init));
        else if (iid.Data1 == 0xBBF8E066)  // ISpatialAudioClient
            patch_slot(vtbl, 10, reinterpret_cast<void*>(&spatial_activate), reinterpret_cast<void**>(&g_spatial));
    }
    return hr;
}

HRESULT WINAPI default_endpoint(void* self, int flow, int role, void** out) {
    HRESULT hr = g_endpoint(self, flow, role, out);
    std::fprintf(stderr, "[rt] audio out: GetDefaultAudioEndpoint(flow %d, role %d) -> %08lX\n", flow, role,
                 static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && out && *out)
        patch_slot(*static_cast<void***>(*out), 3, reinterpret_cast<void*>(&device_activate),
                   reinterpret_cast<void**>(&g_activate));
    return hr;
}

HRESULT WINAPI CoCreateInstance_(REFCLSID clsid, IUnknown* outer, DWORD context, REFIID iid, void** out) {
    HRESULT hr = CoCreateInstance(clsid, outer, context, iid, out);
    if (SUCCEEDED(hr) && out && *out && clsid.Data1 == 0xBCDE0395)  // MMDeviceEnumerator
        patch_slot(*static_cast<void***>(*out), 4, reinterpret_cast<void*>(&default_endpoint),
                   reinterpret_cast<void**>(&g_endpoint));
    if (log_more())
        std::fprintf(stderr,
                     "[rt] com: CoCreateInstance({%08lX-%04X-%04X-...}, iid {%08lX-%04X-%04X-...}, context %#lx) -> %08lX\n",
                     clsid.Data1, clsid.Data2, clsid.Data3, iid.Data1, iid.Data2, iid.Data3, context,
                     static_cast<unsigned long>(hr));
    return hr;
}

}  // namespace

SHIM_TABLE(audio_out,
    {"xaudio2_9.dll", "#1", reinterpret_cast<void*>(&XAudio2Create_)},
    {"xaudio2_9.dll", "XAudio2Create", reinterpret_cast<void*>(&XAudio2Create_)},
    {"api-ms-win-core-com-l1-1-0.dll", "CoCreateInstance", reinterpret_cast<void*>(&CoCreateInstance_)},
);
