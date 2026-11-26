// GameInput.dll: the title imports only ordinal 1, GameInputCreate, and works through the IGameInput objects it
// returns (the v0 API of the PC SDK's GameInput.h, whose vtable order the console's matches). The PC's own GameInput
// can't stand in: it reports a DualSense as a neutral gamepad. Ours is one virtual gamepad fed by pad.h.
//
// IGameInput slots: 3 GetCurrentTimestamp, 4 GetCurrentReading, 5 GetNextReading, 6 GetPreviousReading,
// 7 GetTemporalReading, 8 RegisterReadingCallback, 9 RegisterDeviceCallback, 10 RegisterSystemButtonCallback,
// 11 RegisterKeyboardLayoutCallback, 12 StopCallback, 13 UnregisterCallback, ... 21 SetFocusPolicy.
// IGameInputReading: 3 GetInputKind, 4 GetSequenceNumber, 5 GetTimestamp, 6 GetDevice, 22 GetGamepadState,
// 24 GetUiNavigationState. IGameInputDevice: 3 GetDeviceInfo, 4 GetDeviceStatus, 5 GetBatteryState.
// Anything else stays a probe that logs, so a run shows what to write next.
#include <windows.h>
#include <intrin.h>

#include <GameInput.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "com_probe.h"
#include "pad.h"
#include "shims.h"

namespace {

constexpr HRESULT kReadingNotFound = static_cast<HRESULT>(0x838A0003);  // GAMEINPUT_E_READING_NOT_FOUND
constexpr GameInputKind kGamepadKinds =
    static_cast<GameInputKind>(GameInputKindGamepad | GameInputKindController | GameInputKindControllerAxis |
                               GameInputKindControllerButton | GameInputKindControllerSwitch | GameInputKindUiNavigation);

uint64_t now_us() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<uint64_t>(t.QuadPart / static_cast<double>(f.QuadPart) * 1e6);
}

// ---- the device

struct Device {
    rt::shims::ProbeObject probe;  // first: its table is the object's
    GameInputDeviceInfo info{};
};

// ---- readings: a ring of objects reused (probe objects live for the process), each a snapshot

struct Reading {
    rt::shims::ProbeObject probe;
    uint64_t sequence = 0, timestamp = 0;
    rt::pad::State state;
};
constexpr int kReadings = 256;

struct State {
    std::mutex m;
    void** input_vt = nullptr;
    void** reading_vt = nullptr;
    void** device_vt = nullptr;
    Device* device = nullptr;
    Reading* ring[kReadings] = {};
    int next = 0;
    uint64_t sequence = 0;
    rt::pad::State last{};
    bool have_last = false;
    Reading* latest = nullptr;
    rt::shims::ProbeObject* input = nullptr;
    std::atomic<uint64_t> next_token{1};
    // device callbacks, told when the pad comes and goes
    struct Watch {
        GameInputCallbackToken token;
        GameInputKind kind;
        void* context;
        GameInputDeviceCallback callback;
    };
    std::vector<Watch> watches;
    bool watcher = false;
    bool reported_connected = false;
    std::atomic<int> logged{0};  // which reading methods have been logged once (a bit each)
};
State* g;  // made by GameInputCreate (the runtime has no thread-safe statics)

Device* self_device(void* p) { return reinterpret_cast<Device*>(p); }

// Each implemented method logs its first call (a bit each), so a run shows how far the title gets.
std::atomic<uint64_t> g_seen{0};
void first(int bit, const char* what) {
    if (!(g_seen.fetch_or(uint64_t{1} << bit) & (uint64_t{1} << bit)))
        std::fprintf(stderr, "[rt] gameinput: first %s\n", what);
}
Reading* self_reading(void* p) { return reinterpret_cast<Reading*>(p); }

// The newest snapshot, a new one when the pad's state changed; null when no controller is connected.
Reading* latest_reading() {
    rt::pad::State s;
    bool connected = rt::pad::current(s);
    std::lock_guard lock(g->m);
    if (!connected) {
        g->have_last = false;
        return nullptr;
    }
    if (!g->latest || !g->have_last || std::memcmp(&s, &g->last, sizeof s) != 0) {
        Reading* r = g->ring[g->next];
        g->next = (g->next + 1) % kReadings;
        r->sequence = ++g->sequence;
        r->timestamp = now_us();
        r->state = s;
        g->latest = r;
        g->last = s;
        g->have_last = true;
    }
    return g->latest;
}

// ---- IGameInputReading

GameInputKind WINAPI reading_kind(void*) { first(10, "IGameInputReading::GetInputKind"); return kGamepadKinds; }
uint64_t WINAPI reading_sequence(void* self, GameInputKind) { first(11, "IGameInputReading::GetSequenceNumber"); return self_reading(self)->sequence; }
uint64_t WINAPI reading_timestamp(void* self) { return self_reading(self)->timestamp; }
void WINAPI reading_device(void*, IGameInputDevice** out) { first(12, "IGameInputReading::GetDevice");
    if (out) *out = reinterpret_cast<IGameInputDevice*>(g->device);
}
bool WINAPI reading_gamepad(void* self, GameInputGamepadState* out) { first(13, "IGameInputReading::GetGamepadState");
    if (!out) return false;
    const rt::pad::State& s = self_reading(self)->state;
    out->buttons = static_cast<GameInputGamepadButtons>(s.buttons);
    out->leftTrigger = s.left_trigger, out->rightTrigger = s.right_trigger;
    out->leftThumbstickX = s.left_x, out->leftThumbstickY = s.left_y;
    out->rightThumbstickX = s.right_x, out->rightThumbstickY = s.right_y;
    return true;
}
bool WINAPI reading_ui_navigation(void* self, GameInputUiNavigationState* out) { first(14, "IGameInputReading::GetUiNavigationState");
    if (!out) return false;
    const uint32_t b = self_reading(self)->state.buttons;
    uint32_t n = 0;
    using namespace rt::pad;
    if (b & kMenu) n |= GameInputUiNavigationMenu;
    if (b & kView) n |= GameInputUiNavigationView;
    if (b & kA) n |= GameInputUiNavigationAccept;
    if (b & kB) n |= GameInputUiNavigationCancel;
    if (b & kUp) n |= GameInputUiNavigationUp;
    if (b & kDown) n |= GameInputUiNavigationDown;
    if (b & kLeft) n |= GameInputUiNavigationLeft;
    if (b & kRight) n |= GameInputUiNavigationRight;
    out->buttons = static_cast<GameInputUiNavigationButtons>(n);
    return true;
}

// ---- IGameInputDevice

const GameInputDeviceInfo* WINAPI device_info(void* self) {
    if (!(g_seen.load() & (uint64_t{1} << 15)))
        std::fprintf(stderr, "[rt] gameinput: GetDeviceInfo called from %p\n", _ReturnAddress());
    first(15, "IGameInputDevice::GetDeviceInfo");
    return &self_device(self)->info;
}
GameInputDeviceStatus WINAPI device_status(void*) { first(16, "IGameInputDevice::GetDeviceStatus");
    rt::pad::State s;
    return rt::pad::current(s) ? static_cast<GameInputDeviceStatus>(GameInputDeviceConnected | GameInputDeviceInputEnabled)
                               : GameInputDeviceNoStatus;
}
void WINAPI device_battery(void*, GameInputBatteryState* out) { first(17, "IGameInputDevice::GetBatteryState");
    if (out) *out = {};
}

// ---- IGameInput

uint64_t WINAPI input_timestamp(void*) { first(18, "IGameInput::GetCurrentTimestamp"); return now_us(); }

void log_once(int bit, const char* what, GameInputKind kind, void* device) {
    if (g->logged.fetch_or(1 << bit) & (1 << bit)) return;
    std::fprintf(stderr, "[rt] gameinput: first %s(kind %#x, device %p)\n", what, kind, device);
}

constexpr GameInputDeviceStatus kOn =
    static_cast<GameInputDeviceStatus>(GameInputDeviceConnected | GameInputDeviceInputEnabled);

// Tells every device callback that the pad came or went.
void announce(bool connected) {
    std::vector<State::Watch> w;
    {
        std::lock_guard lock(g->m);
        if (g->reported_connected == connected) return;
        g->reported_connected = connected;
        w = g->watches;
    }
    std::fprintf(stderr, "[rt] gameinput: gamepad %s (%zu callback(s) told)\n", connected ? "connected" : "gone",
                 w.size());
    for (auto& x : w)
        if (x.kind & kGamepadKinds)
            x.callback(x.token, x.context, reinterpret_cast<IGameInputDevice*>(g->device), now_us(),
                       connected ? kOn : GameInputDeviceNoStatus, connected ? GameInputDeviceNoStatus : kOn);
}

DWORD WINAPI watch_pad(void*) {
    for (;;) {
        rt::pad::State s;
        announce(rt::pad::current(s));
        Sleep(250);
    }
}

// The pad's state now, waiting up to `ms` for a controller to show up (the HID reader starts on first use).
bool pad_present(DWORD ms) {
    rt::pad::State s;
    DWORD start = GetTickCount();
    for (;;) {
        if (rt::pad::current(s)) return true;
        if (GetTickCount() - start >= ms) return false;
        Sleep(50);
    }
}

bool wants_pad(GameInputKind kind, void* device) {
    return (kind & kGamepadKinds) && (!device || device == g->device);
}

HRESULT WINAPI input_current(void*, GameInputKind kind, void* device, IGameInputReading** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    log_once(0, "GetCurrentReading", kind, device);
    if (!wants_pad(kind, device)) return kReadingNotFound;
    Reading* r = latest_reading();
    if (!r) return kReadingNotFound;
    *out = reinterpret_cast<IGameInputReading*>(r);
    return S_OK;
}

// The next reading after `ref`: the newest one, when it's newer.
HRESULT WINAPI input_next(void*, void* ref, GameInputKind kind, void* device, IGameInputReading** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    log_once(1, "GetNextReading", kind, device);
    if (!ref || !wants_pad(kind, device)) return kReadingNotFound;
    Reading* r = latest_reading();
    if (!r || r->sequence <= self_reading(ref)->sequence) return kReadingNotFound;
    *out = reinterpret_cast<IGameInputReading*>(r);
    return S_OK;
}
HRESULT WINAPI input_previous(void*, void*, GameInputKind, void*, IGameInputReading** out) {
    if (out) *out = nullptr;
    return kReadingNotFound;
}

// RegisterDeviceCallback(device, kind, statusFilter, enumerationKind, context, callback, token): an enumeration
// reports the virtual gamepad if a controller is there (waiting up to 2 s for the HID reader to find one), and the
// callback is kept to be told when it comes and goes later (a thread watches the pad).
HRESULT WINAPI input_register_device(void*, void* device, GameInputKind kind, GameInputDeviceStatus,
                                     GameInputEnumerationKind enumeration, void* context,
                                     GameInputDeviceCallback callback, GameInputCallbackToken* token) {
    GameInputCallbackToken t = g->next_token++;
    if (token) *token = t;
    std::fprintf(stderr, "[rt] gameinput: RegisterDeviceCallback(kind %#x, enumeration %d)\n", kind, enumeration);
    if (!callback || !wants_pad(kind, device)) return S_OK;
    bool present = pad_present(2000);
    bool start = false;
    {
        std::lock_guard lock(g->m);
        g->watches.push_back({t, kind, context, callback});
        if (present) g->reported_connected = true;
        start = !g->watcher;
        g->watcher = true;
    }
    std::fprintf(stderr, "[rt] gameinput: controller %s; %s\n", present ? "present" : "not found yet",
                 present && enumeration != GameInputNoEnumeration ? "telling the callback" : "the callback waits");
    if (present && enumeration != GameInputNoEnumeration) callback(t, context, reinterpret_cast<IGameInputDevice*>(g->device), now_us(), kOn, GameInputDeviceNoStatus);
    if (start)
        if (HANDLE h = CreateThread(nullptr, 0, &watch_pad, nullptr, 0, nullptr)) CloseHandle(h);
    return S_OK;
}
HRESULT WINAPI input_register_other(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    GameInputCallbackToken* token) {
    if (token) *token = g->next_token++;
    return S_OK;
}
void WINAPI input_stop_callback(void*, GameInputCallbackToken) {}
bool WINAPI input_unregister(void*, GameInputCallbackToken token, uint64_t) {
    std::lock_guard lock(g->m);
    std::erase_if(g->watches, [&](const State::Watch& w) { return w.token == token; });
    return true;
}
void WINAPI input_focus_policy(void*, GameInputFocusPolicy) {}

void make_state() {
    using rt::shims::set_probe_method;
    g = new State;
    g->input_vt = rt::shims::make_probe_vtable("IGameInput");
    set_probe_method(g->input_vt, 3, reinterpret_cast<void*>(&input_timestamp));
    set_probe_method(g->input_vt, 4, reinterpret_cast<void*>(&input_current));
    set_probe_method(g->input_vt, 5, reinterpret_cast<void*>(&input_next));
    set_probe_method(g->input_vt, 6, reinterpret_cast<void*>(&input_previous));
    set_probe_method(g->input_vt, 9, reinterpret_cast<void*>(&input_register_device));
    set_probe_method(g->input_vt, 10, reinterpret_cast<void*>(&input_register_other));  // system buttons
    set_probe_method(g->input_vt, 12, reinterpret_cast<void*>(&input_stop_callback));
    set_probe_method(g->input_vt, 13, reinterpret_cast<void*>(&input_unregister));
    set_probe_method(g->input_vt, 21, reinterpret_cast<void*>(&input_focus_policy));
    g->reading_vt = rt::shims::make_probe_vtable("IGameInputReading");
    set_probe_method(g->reading_vt, 3, reinterpret_cast<void*>(&reading_kind));
    set_probe_method(g->reading_vt, 4, reinterpret_cast<void*>(&reading_sequence));
    set_probe_method(g->reading_vt, 5, reinterpret_cast<void*>(&reading_timestamp));
    set_probe_method(g->reading_vt, 6, reinterpret_cast<void*>(&reading_device));
    set_probe_method(g->reading_vt, 22, reinterpret_cast<void*>(&reading_gamepad));
    set_probe_method(g->reading_vt, 24, reinterpret_cast<void*>(&reading_ui_navigation));
    g->device_vt = rt::shims::make_probe_vtable("IGameInputDevice");
    set_probe_method(g->device_vt, 3, reinterpret_cast<void*>(&device_info));
    set_probe_method(g->device_vt, 4, reinterpret_cast<void*>(&device_status));
    set_probe_method(g->device_vt, 5, reinterpret_cast<void*>(&device_battery));

    g->device = new Device{{g->device_vt, "IGameInputDevice", nullptr}, {}};
    GameInputDeviceInfo& i = g->device->info;
    i.infoSize = sizeof i;
    i.vendorId = 0x045E;  // a Microsoft gamepad, as the title knows them
    i.productId = 0x0B12;
    i.deviceFamily = GameInputFamilyXboxOne;
    i.supportedInput = kGamepadKinds;
    // The title keys controllers by deviceId (it copies the 32 bytes at +0x20 after GetDeviceInfo) and ignores a
    // device whose id is all zeros: a fixed, non-zero id for ours.
    for (int k = 0; k < 32; ++k) i.deviceId.value[k] = static_cast<uint8_t>(0xF1 + k);
    i.deviceRootId = i.deviceId;
    i.usage = {0x01, 0x05};  // generic desktop, game pad
    i.capabilities = GameInputDeviceCapabilityNone;
    for (Reading*& r : g->ring) r = new Reading{{g->reading_vt, "IGameInputReading", nullptr}};
    g->input = rt::shims::make_probe_object(g->input_vt, "IGameInput");
}

HRESULT WINAPI GameInputCreate_(IGameInput** out) {
    if (!out) return E_POINTER;
    if (!g) make_state();
    *out = reinterpret_cast<IGameInput*>(g->input);
    std::fprintf(stderr, "[rt] gameinput: GameInputCreate -> the runtime's gamepad (DualSense over HID, XInput)\n");
    return S_OK;
}

}  // namespace

SHIM_TABLE(gameinput,
    {"gameinput.dll", "#1", reinterpret_cast<void*>(&GameInputCreate_)},
    {"gameinput.dll", "GameInputCreate", reinterpret_cast<void*>(&GameInputCreate_)},
);
