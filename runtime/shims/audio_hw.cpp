// The console's audio processor: AcpHal.dll (memory the audio hardware addresses, the processor's command / message
// channel, SHAPE contexts), xapu.dll (the decoder's command queue) and chad.dll (the DSP).
//
// Where the title's sound goes (observed on 1.0.2.1, and as the xcl project records for retail titles): the title
// sends its Opus packets to the decoder (xapu), reads the PCM back, mixes it itself and plays its mix through a
// Windows audio endpoint (Spatial Audio, two static objects: front left / right, mono float 48 kHz each; see
// audio_out.cpp). So the decoder is the one part that has to work; the processor and the DSP only have to answer:
// AcpHal gets one command after Connect, chad none, the SHAPE contexts stay unwritten. (Its mixer keeps time with
// short sleeps: it needs the fine timer run.cpp asks for, or it makes a third of the sound.)
//
//   APU memory    ApuCreateHeap(size, flags); ApuAlloc(&va, &apu, size, alignment, flags) gives process memory and a
//                 32-bit "APU address" for it; ApuMapVirtualAddress(&apu, va, size) for the title's own memory;
//                 ApuMapApuAddress(apu) -> va; ApuFree(va); ApuIsVirtualAddressValid(va, size)
//   AcpHal        AcpHalCreate(&hal): Connect(commands, messages, ...), Disconnect, SubmitCommand(type, id, frame,
//                 data, size), PopMessage(&message) -> bool, GetNumMessages, Release. Messages {u32 type, u32, u64
//                 value, u64 id}: 2 an audio frame (every 128 samples at 48 kHz; value: the frame count), 0x10 a
//                 command done, 0x80 disconnected. AcpHalAllocateShapeContexts(counts + arrays): the arrays exist; the
//                 SHAPE mixer doesn't run.
//   xapu          XApuConnect(input, output, &handle), XApuEnqueueCommand(handle, &command, &waiting): done at once.
//                 Activate (kind 1, with the channel count) starts a stream; Process (kind 3) decodes one Opus packet
//                 (input; 20 ms CELT, TOC 0xF8 mono / 0xFC stereo) into 32-bit float samples (output), `frames` of
//                 them, the first `first_frame` of a stream dropped (312, Opus' pre-skip); Deactivate (kind 2) ends
//                 it. XApuDequeueResult, XApuGetResultCount. RT_AUDIO_TAP=1 also writes what's decoded to
//                 %LOCALAPPDATA%\Flamingo\diag\decoded.wav.
//   chad          connected, commands do nothing
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "shims.h"

#include <opus.h>

namespace {

constexpr HRESULT kNoResults = static_cast<HRESULT>(0x8AC81001);  // XApuDequeueResult: none waiting

// ---- APU memory

struct Region {
    uintptr_t va;
    uint32_t size, apu;
    bool owned;
};
std::mutex g_apu_lock;
std::vector<Region>* g_regions = new std::vector<Region>;
uint32_t g_next_apu = 0x10000;

uint32_t new_apu_address(uintptr_t va, uint32_t size, bool owned) {
    uint32_t a = g_next_apu;
    g_next_apu += (size + 0xFFFF) & ~0xFFFFu;
    g_regions->push_back({va, size, a, owned});
    return a;
}

HRESULT WINAPI ApuCreateHeap(uint32_t size, uint32_t flags) {
    std::fprintf(stderr, "[rt] audio: ApuCreateHeap(%#x, %#x): process memory\n", size, flags);
    return S_OK;
}

HRESULT WINAPI ApuAlloc(void** va, uint32_t* apu, uint32_t size, uint32_t alignment, uint32_t) {
    if (!va) return E_INVALIDARG;
    // VirtualAlloc is 64 KiB aligned, more than any alignment seen (0x800)
    void* p = VirtualAlloc(nullptr, size ? size : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p || (alignment > 0x10000 && reinterpret_cast<uintptr_t>(p) % alignment)) {
        if (p) VirtualFree(p, 0, MEM_RELEASE);
        *va = nullptr;
        return E_OUTOFMEMORY;
    }
    std::lock_guard lock(g_apu_lock);
    uint32_t a = new_apu_address(reinterpret_cast<uintptr_t>(p), size, true);
    *va = p;
    if (apu) *apu = a;
    return S_OK;
}

HRESULT WINAPI ApuFree(void* va) {
    if (!va) return E_INVALIDARG;
    bool owned = false;
    {
        std::lock_guard lock(g_apu_lock);
        for (size_t i = 0; i < g_regions->size(); ++i)
            if ((*g_regions)[i].va == reinterpret_cast<uintptr_t>(va)) {
                owned = (*g_regions)[i].owned;
                g_regions->erase(g_regions->begin() + i);
                break;
            }
    }
    if (owned) VirtualFree(va, 0, MEM_RELEASE);
    return S_OK;
}

HRESULT WINAPI ApuMapVirtualAddress(uint32_t* apu, void* va, uint32_t size) {
    if (!apu || !va) return E_INVALIDARG;
    std::lock_guard lock(g_apu_lock);
    *apu = new_apu_address(reinterpret_cast<uintptr_t>(va), size, false);
    return S_OK;
}

void* WINAPI ApuMapApuAddress(uint32_t apu) {
    std::lock_guard lock(g_apu_lock);
    for (const Region& r : *g_regions)
        if (apu >= r.apu && apu - r.apu < r.size) return reinterpret_cast<void*>(r.va + (apu - r.apu));
    return nullptr;
}

BOOL WINAPI ApuIsVirtualAddressValid(const void*, uint32_t) { return TRUE; }

// ---- SHAPE contexts

struct ShapeContexts {
    uint32_t counts[5];
    uint32_t reserved;
    void* contexts[5];
};

HRESULT WINAPI AcpHalAllocateShapeContexts(ShapeContexts* c) {
    if (!c) return E_INVALIDARG;
    for (int i = 0; i < 5; ++i) {
        c->contexts[i] = nullptr;
        if (!c->counts[i]) continue;
        uint32_t apu;
        if (FAILED(ApuAlloc(&c->contexts[i], &apu, c->counts[i] * 64, 0x100, 0))) return E_OUTOFMEMORY;
    }
    std::fprintf(stderr, "[rt] audio: SHAPE contexts %u %u %u %u %u (the mixer doesn't run)\n", c->counts[0],
                 c->counts[1], c->counts[2], c->counts[3], c->counts[4]);
    return S_OK;
}
HRESULT WINAPI AcpHalReleaseShapeContexts() { return S_OK; }

// ---- the processor's channel

struct Message {
    uint32_t type, pad;
    uint64_t value, id;
};
constexpr uint32_t kFrame = 2, kCommandDone = 0x10, kDisconnected = 0x80;
constexpr auto kFramePeriod = std::chrono::microseconds(2667);  // 128 samples at 48 kHz

struct Hal {
    void** vtbl;
    std::mutex m;
    std::deque<Message> messages;
    bool connected = false;
    std::chrono::steady_clock::time_point next_frame;
    uint64_t frames = 0;   // audio frames so far: a frame message's value
    uint64_t popped = 0;   // messages handed out (logged as they grow)
};

bool frame_due(Hal* h, std::chrono::steady_clock::time_point now) { return h->connected && now >= h->next_frame; }

HRESULT WINAPI hal_connect(Hal* h, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    std::fprintf(stderr, "[rt] audio: AcpHal Connect(%llx, %llx, %llx, %llx)\n", (unsigned long long)a,
                 (unsigned long long)b, (unsigned long long)c, (unsigned long long)d);
    std::lock_guard lock(h->m);
    h->connected = true;
    h->next_frame = std::chrono::steady_clock::now() + kFramePeriod;
    return S_OK;
}
HRESULT WINAPI hal_disconnect(Hal* h) {
    std::lock_guard lock(h->m);
    h->connected = false;
    h->messages.push_back({kDisconnected, 0, 0, 0});
    return S_OK;
}
// Being worked out: the first commands of each type are logged with their first bytes.
HRESULT WINAPI hal_submit(Hal* h, uint32_t type, uint64_t id, uint32_t frame, const void* data, uint32_t size) {
    static int seen[64];  // per type, the hal's lock held below
    {
        std::lock_guard lock(h->m);
        if (type < 64 && seen[type]++ < 4) {
            std::fprintf(stderr, "[rt] audio: AcpHal command type %u id %llx frame %u, %u bytes:", type,
                         static_cast<unsigned long long>(id), frame, size);
            const auto* b = static_cast<const uint8_t*>(data);
            for (uint32_t i = 0; b && i < size && i < 48; ++i) std::fprintf(stderr, "%s%02x", i % 4 ? "" : " ", b[i]);
            std::fprintf(stderr, "\n");
        }
    }
    std::lock_guard lock(h->m);
    if (!h->connected) return E_UNEXPECTED;
    if (id) h->messages.push_back({kCommandDone, 0, 0, id});
    return S_OK;
}
bool WINAPI hal_pop(Hal* h, Message* out) {
    static volatile long first = 0;
    if (!InterlockedExchange(&first, 1)) std::fprintf(stderr, "[rt] audio: AcpHal PopMessage first called\n");
    std::lock_guard lock(h->m);
    if (!h->messages.empty()) {
        if (out) *out = h->messages.front();
        h->messages.pop_front();
        return true;
    }
    auto now = std::chrono::steady_clock::now();
    if (!frame_due(h, now)) return false;
    // a frame late by more than 8 isn't made up for
    h->next_frame = std::max(h->next_frame + kFramePeriod, now - 8 * kFramePeriod);
    ++h->frames;
    if (out) *out = {kFrame, 0, h->frames, 0};
    if ((++h->popped & (h->popped - 1)) == 0)
        std::fprintf(stderr, "[rt] audio: %llu frame messages taken\n", static_cast<unsigned long long>(h->popped));
    return true;
}
uint32_t WINAPI hal_count(Hal* h) {
    static volatile long first = 0;
    if (!InterlockedExchange(&first, 1)) std::fprintf(stderr, "[rt] audio: AcpHal GetNumMessages first called\n");
    std::lock_guard lock(h->m);
    return static_cast<uint32_t>(h->messages.size()) + (frame_due(h, std::chrono::steady_clock::now()) ? 1 : 0);
}
void WINAPI hal_release(Hal*) {}

void* g_hal_vtbl[] = {reinterpret_cast<void*>(&hal_connect), reinterpret_cast<void*>(&hal_disconnect),
                      reinterpret_cast<void*>(&hal_submit),  reinterpret_cast<void*>(&hal_pop),
                      reinterpret_cast<void*>(&hal_count),   reinterpret_cast<void*>(&hal_release)};

HRESULT WINAPI AcpHalCreate(Hal** out) {
    if (!out) return E_INVALIDARG;
    auto* h = new Hal{};
    h->vtbl = g_hal_vtbl;
    *out = h;
    return S_OK;
}

// ---- RT_AUDIO_TAP=1: the decoder's output, appended as decoded (mono duplicated to stereo), to
// %LOCALAPPDATA%\Flamingo\diag\decoded.wav (48 kHz float stereo), up to 60 s, for listening to what the title gets.
struct Tap {
    std::mutex m;
    bool tried = false;
    HANDLE file = INVALID_HANDLE_VALUE;
    uint64_t frames = 0;
};
Tap g_tap;

void wav_header(HANDLE f, uint64_t frames) {
    uint32_t data = static_cast<uint32_t>(frames * 8), riff = 36 + data;
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 3, 0, 2, 0};
    uint32_t rate = 48000, bytes_per_s = 48000 * 8;
    std::memcpy(h + 4, &riff, 4);
    std::memcpy(h + 24, &rate, 4);
    std::memcpy(h + 28, &bytes_per_s, 4);
    h[32] = 8, h[34] = 32;
    std::memcpy(h + 36, "data", 4);
    std::memcpy(h + 40, &data, 4);
    DWORD n;
    SetFilePointer(f, 0, nullptr, FILE_BEGIN);
    WriteFile(f, h, sizeof h, &n, nullptr);
    SetFilePointer(f, 0, nullptr, FILE_END);
}

void tap(const float* pcm, uint32_t frames, uint32_t channels) {
    std::lock_guard lock(g_tap.m);
    if (!g_tap.tried) {
        g_tap.tried = true;
        char on[4];
        wchar_t local[MAX_PATH];
        if (GetEnvironmentVariableA("RT_AUDIO_TAP", on, sizeof on) && on[0] == '1' &&
            GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
            std::wstring dir = std::wstring(local) + L"\\Flamingo\\diag";
            CreateDirectoryW(dir.c_str(), nullptr);
            g_tap.file = CreateFileW((dir + L"\\decoded.wav").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (g_tap.file != INVALID_HANDLE_VALUE) wav_header(g_tap.file, 0);
            std::fprintf(stderr, "[rt] audio: tapping the decoder's output into decoded.wav\n");
        }
    }
    if (g_tap.file == INVALID_HANDLE_VALUE || g_tap.frames >= 48000ull * 60) return;
    std::vector<float> st(size_t{frames} * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        st[i * 2] = pcm[i * channels];
        st[i * 2 + 1] = pcm[i * channels + (channels > 1 ? 1 : 0)];
    }
    DWORD n;
    WriteFile(g_tap.file, st.data(), static_cast<DWORD>(st.size() * 4), &n, nullptr);
    g_tap.frames += frames;
    wav_header(g_tap.file, g_tap.frames);
}

// ---- xapu

#pragma pack(push, 1)
struct XApuCommand {
    uint32_t kind, stream, sequence, options;
    void* context;
};
struct XApuProcess {
    XApuCommand id;
    float ramp, pitch;
    uint32_t first_frame, frames, input_bytes;
    void* input;
    uint32_t output_bytes;
    void* output;
};
struct XApuActivate {
    XApuCommand id;
    uint32_t channels;
};
struct XApuResult {
    XApuCommand id;
    uint32_t state;
    uint64_t state_data, time;
    HRESULT error;
    uint32_t frames, output_bytes;
    void* output;
    float pitch;
    uint32_t input_bytes;
    void* input;
};
struct XApuConnectInput {
    uint32_t process, streams, commands_per_stream, data_bytes, frames_per_output, options;
};
struct XApuConnectOutput {
    void* data;
    void* ready;
};
#pragma pack(pop)

struct XApu {
    std::mutex m;
    std::deque<XApuResult> results;
    std::vector<uint32_t> channels;  // per stream, from Activate
    std::vector<OpusDecoder*> decoders;  // per stream, made on Activate (for its channel count)
    void* data = nullptr;
    HANDLE ready = nullptr;
    uint32_t frames_per_output = 0;
};

HRESULT WINAPI XApuConnect(const XApuConnectInput* in, XApuConnectOutput* out, XApu** handle) {
    if (!in || !out || !handle) return E_INVALIDARG;
    auto* x = new XApu{};
    x->channels.assign(in->streams, 2);
    x->decoders.assign(in->streams, nullptr);
    x->frames_per_output = in->frames_per_output;
    if (in->data_bytes) x->data = VirtualAlloc(nullptr, in->data_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (in->options & 8) x->ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    out->data = x->data;
    out->ready = x->ready;
    *handle = x;
    std::fprintf(stderr, "[rt] audio: XApuConnect(%u streams, %u commands each)\n", in->streams,
                 in->commands_per_stream);
    return S_OK;
}

HRESULT WINAPI XApuDisconnect(XApu* x) {
    if (!x) return E_INVALIDARG;
    if (x->data) VirtualFree(x->data, 0, MEM_RELEASE);
    if (x->ready) CloseHandle(x->ready);
    for (OpusDecoder* d : x->decoders)
        if (d) opus_decoder_destroy(d);
    delete x;
    return S_OK;
}

HRESULT WINAPI XApuEnqueueCommand(XApu* x, const XApuCommand* command, uint32_t* waiting) {
    if (!x || !command) return E_INVALIDARG;
    XApuCommand c;
    std::memcpy(&c, command, sizeof c);
    XApuResult r{};
    r.id = c;
    r.pitch = 1.0f;
    std::lock_guard lock(x->m);
    {  // being worked out: the first commands of each kind, and a count every so often
        static int seen[16];
        static uint64_t total = 0;
        if (c.kind < 16 && seen[c.kind]++ < 3) {
            std::fprintf(stderr, "[rt] audio: xapu command kind %u stream %u sequence %u options %#x:", c.kind, c.stream,
                         c.sequence, c.options);
            const auto* b = reinterpret_cast<const uint8_t*>(command);
            for (int i = 0; i < 72; ++i) std::fprintf(stderr, "%s%02x", i % 4 ? "" : " ", b[i]);
            std::fprintf(stderr, "\n");
            if (c.kind == 3) {
                XApuProcess p;
                std::memcpy(&p, command, sizeof p);
                std::fprintf(stderr, "[rt] audio:   process: %u frames from %u, %u input bytes at %p (", p.frames,
                             p.first_frame, p.input_bytes, p.input);
                const auto* in = static_cast<const uint8_t*>(p.input);
                for (uint32_t i = 0; in && i < p.input_bytes && i < 24; ++i) std::fprintf(stderr, "%02x", in[i]);
                std::fprintf(stderr, "), %u output bytes, pitch %g\n", p.output_bytes, p.pitch);
            }
        }
        if ((++total & (total - 1)) == 0)
            std::fprintf(stderr, "[rt] audio: %llu xapu commands\n", static_cast<unsigned long long>(total));
    }
    if (c.kind == 1 && c.stream < x->channels.size()) {
        XApuActivate a;
        std::memcpy(&a, command, sizeof a);
        uint32_t ch = a.channels == 1 ? 1 : 2;
        OpusDecoder*& d = x->decoders[c.stream];
        if (d && x->channels[c.stream] != ch) {
            opus_decoder_destroy(d);
            d = nullptr;
        }
        x->channels[c.stream] = ch;
        int err = OPUS_OK;
        if (!d) d = opus_decoder_create(48000, static_cast<int>(ch), &err);
        else opus_decoder_ctl(d, OPUS_RESET_STATE);
        if (err != OPUS_OK) {
            std::fprintf(stderr, "[rt] audio: no Opus decoder (%s)\n", opus_strerror(err));
            d = nullptr;
        }
    } else if (c.kind == 3) {
        XApuProcess p;
        std::memcpy(&p, command, sizeof p);
        uint32_t channels = c.stream < x->channels.size() ? x->channels[c.stream] : 2;
        uint32_t frame_bytes = channels * 4;
        uint32_t asked = p.frames ? p.frames : x->frames_per_output ? x->frames_per_output : 960;
        uint32_t frames = std::min(asked, p.output_bytes / frame_bytes);
        OpusDecoder* d = c.stream < x->decoders.size() ? x->decoders[c.stream] : nullptr;
        int got = -1;
        if (d && p.output && frames) {
            auto* out = static_cast<float*>(p.output);
            got = p.input && p.input_bytes
                      ? opus_decode_float(d, static_cast<const unsigned char*>(p.input),
                                          static_cast<opus_int32>(p.input_bytes), out, static_cast<int>(frames), 0)
                      : opus_decode_float(d, nullptr, 0, out, static_cast<int>(frames), 0);  // a lost packet
            if (got > 0 && p.first_frame) {  // the stream's first samples dropped (pre-skip)
                uint32_t skip = std::min<uint32_t>(p.first_frame, static_cast<uint32_t>(got));
                std::memmove(out, out + size_t{skip} * channels, (size_t{static_cast<uint32_t>(got)} - skip) * frame_bytes);
                got -= static_cast<int>(skip);
            }
            static volatile long errors = 0;
            if (got < 0 && InterlockedIncrement(&errors) <= 8)
                std::fprintf(stderr, "[rt] audio: Opus packet of %u bytes not decoded (%s)\n", p.input_bytes,
                             opus_strerror(got));
        }
        if (got > 0) tap(static_cast<const float*>(p.output), static_cast<uint32_t>(got), channels);
        if (got < 0) {  // no decoder, or a bad packet: silence of the length asked for
            if (p.output && frames) std::memset(p.output, 0, size_t{frames} * frame_bytes);
        } else {
            frames = static_cast<uint32_t>(got);
        }
        r.state = 1 | 2;  // output ready, more input wanted
        r.frames = frames;
        r.output_bytes = frames * frame_bytes;
        r.output = p.output;
        r.pitch = p.pitch;
        r.input_bytes = p.input_bytes;
        r.input = p.input;
    }
    x->results.push_back(r);
    if (waiting) *waiting = static_cast<uint32_t>(x->results.size());
    if (x->ready && (c.options & 1)) SetEvent(x->ready);
    return S_OK;
}

HRESULT WINAPI XApuDequeueResult(XApu* x, XApuResult* out) {
    if (!x || !out) return E_INVALIDARG;
    std::lock_guard lock(x->m);
    if (x->results.empty()) return kNoResults;
    std::memcpy(out, &x->results.front(), sizeof *out);
    x->results.pop_front();
    return S_OK;
}

uint32_t WINAPI XApuGetResultCount(XApu* x, uint32_t* total) {
    if (!x) return 0;
    std::lock_guard lock(x->m);
    auto n = static_cast<uint32_t>(x->results.size());
    if (total) *total = n;
    return n;
}

// ---- chad (the DSP)

// Being worked out: the first calls of each are logged with their arguments (and 48 bytes behind pointer ones).
void dsp_log(const char* what, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d = 0) {
    static volatile long n = 0;
    if (InterlockedIncrement(&n) > 40) return;
    std::fprintf(stderr, "[rt] audio: chad %s(%llx, %llx, %llx, %llx)", what, (unsigned long long)a, (unsigned long long)b,
                 (unsigned long long)c, (unsigned long long)d);
    for (uintptr_t p : {b, c}) {
        if (p < 0x10000) continue;
        __try {
            const auto* q = reinterpret_cast<const uint32_t*>(p);
            std::fprintf(stderr, " [%llx:", (unsigned long long)p);
            for (int i = 0; i < 12; ++i) std::fprintf(stderr, " %08x", q[i]);
            std::fprintf(stderr, "]");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::fprintf(stderr, " [%llx: unreadable]", (unsigned long long)p);
        }
    }
    std::fprintf(stderr, "\n");
}

HRESULT WINAPI XDspConnectWithMaximumStreamLimit(uintptr_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e,
                                                 void** handle) {
    std::fprintf(stderr, "[rt] audio: chad XDspConnectWithMaximumStreamLimit(%llx, %u, %u, %u, %u)\n",
                 (unsigned long long)a, b, c, d, e);
    if (!handle) return E_INVALIDARG;
    *handle = new uint32_t{1};
    return S_OK;
}
HRESULT WINAPI XDspDisconnect(void*) { return S_OK; }
// HRESULT XDspActivate(connection, desc, status**, stream**): a DSP stream for one of the title's effects (the open
// world's first ones are 0x400-sample blocks over buffers of 0x23800-0x30000 bytes). On success the title keeps the
// two pointers it is given back (a status block it polls at +4 and +0xC, and the stream it submits commands to) and
// reads through them. The DSP isn't modelled, so activation fails: 1.0.2.1 then leaves that effect off (its object
// stays in its first state and its update does nothing). Saying S_OK with the pointers unset made it read null.
HRESULT WINAPI XDspActivate(void* h, uintptr_t a, uintptr_t b, uintptr_t c) {
    dsp_log("XDspActivate", reinterpret_cast<uintptr_t>(h), a, b, c);
    return E_FAIL;
}
HRESULT WINAPI XDspDeactivate(void* h, uintptr_t a, uintptr_t b, uintptr_t c) {
    dsp_log("XDspDeactivate", reinterpret_cast<uintptr_t>(h), a, b, c);
    return S_OK;
}
HRESULT WINAPI XDspSubmitCommand(void* h, uintptr_t a, uintptr_t b, uintptr_t c) {
    dsp_log("XDspSubmitCommand", reinterpret_cast<uintptr_t>(h), a, b, c);
    return S_OK;
}

void* f(auto* p) { return reinterpret_cast<void*>(p); }

}  // namespace

SHIM_TABLE(audio_hw,
    {"acphal.dll", "ApuCreateHeap", f(&ApuCreateHeap)},
    {"acphal.dll", "ApuAlloc", f(&ApuAlloc)},
    {"acphal.dll", "ApuFree", f(&ApuFree)},
    {"acphal.dll", "ApuMapVirtualAddress", f(&ApuMapVirtualAddress)},
    {"acphal.dll", "ApuMapApuAddress", f(&ApuMapApuAddress)},
    {"acphal.dll", "ApuIsVirtualAddressValid", f(&ApuIsVirtualAddressValid)},
    {"acphal.dll", "AcpHalCreate", f(&AcpHalCreate)},
    {"acphal.dll", "AcpHalAllocateShapeContexts", f(&AcpHalAllocateShapeContexts)},
    {"acphal.dll", "AcpHalReleaseShapeContexts", f(&AcpHalReleaseShapeContexts)},
    {"xapu.dll", "XApuConnect", f(&XApuConnect)},
    {"xapu.dll", "XApuDisconnect", f(&XApuDisconnect)},
    {"xapu.dll", "XApuEnqueueCommand", f(&XApuEnqueueCommand)},
    {"xapu.dll", "XApuDequeueResult", f(&XApuDequeueResult)},
    {"xapu.dll", "XApuGetResultCount", f(&XApuGetResultCount)},
    {"chad.dll", "XDspConnectWithMaximumStreamLimit", f(&XDspConnectWithMaximumStreamLimit)},
    {"chad.dll", "XDspDisconnect", f(&XDspDisconnect)},
    {"chad.dll", "XDspActivate", f(&XDspActivate)},
    {"chad.dll", "XDspDeactivate", f(&XDspDeactivate)},
    {"chad.dll", "XDspSubmitCommand", f(&XDspSubmitCommand)},
);
