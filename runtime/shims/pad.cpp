// The player's controller (see pad.h): a thread reads DualSense HID reports as they come; XInput is polled on demand.
//
// DualSense input reports (offsets from the report ID byte):
//   USB 0x01 (64 bytes)             1-4 sticks LX LY RX RY, 5-6 L2 R2, 8 buttons0, 9 buttons1, 10 buttons2
//   Bluetooth 0x01 (simple, default) 1-4 sticks, 5 buttons0, 6 buttons1, 7 buttons2, 8-9 L2 R2 (Windows pads it to
//                                    78 bytes, the device's longest input report)
//   Bluetooth 0x31 (full)           as USB one byte later (2-5 sticks, 6-7 L2 R2, 9-11 buttons)
// buttons0: low nibble the hat (0 up, clockwise to 7 up-left, 8 none), 0x10 Square, 0x20 Cross, 0x40 Circle,
// 0x80 Triangle; buttons1: 0x01 L1, 0x02 R1, 0x04 L2, 0x08 R2, 0x10 Create, 0x20 Options, 0x40 L3, 0x80 R3;
// buttons2: 0x01 PS, 0x02 touchpad click. Sticks: 0-255, 128 centred, +y down.
#include "pad.h"

#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <Xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace rt::pad {

bool parse_dualsense(const uint8_t* r, size_t n, State& s) {
    if (!r || n < 10) return false;
    const uint8_t *sticks, *b;
    uint8_t l2, r2;
    if (r[0] == 0x01 && n == 64) {  // USB
        sticks = r + 1, l2 = r[5], r2 = r[6], b = r + 8;
    } else if (r[0] == 0x01) {  // Bluetooth, simple
        sticks = r + 1, b = r + 5, l2 = r[8], r2 = r[9];
    } else if (r[0] == 0x31 && n >= 12) {  // Bluetooth, full
        sticks = r + 2, l2 = r[6], r2 = r[7], b = r + 9;
    } else {
        return false;
    }
    auto axis = [](uint8_t v) { return std::clamp((static_cast<int>(v) - 128) / 127.0f, -1.0f, 1.0f); };
    s = {};
    s.left_x = axis(sticks[0]), s.left_y = -axis(sticks[1]);
    s.right_x = axis(sticks[2]), s.right_y = -axis(sticks[3]);
    s.left_trigger = l2 / 255.0f, s.right_trigger = r2 / 255.0f;
    static const uint32_t kHat[8] = {kUp, kUp | kRight, kRight, kDown | kRight, kDown, kDown | kLeft, kLeft, kUp | kLeft};
    uint32_t v = (b[0] & 0x0F) < 8 ? kHat[b[0] & 0x0F] : 0;
    if (b[0] & 0x10) v |= kX;
    if (b[0] & 0x20) v |= kA;
    if (b[0] & 0x40) v |= kB;
    if (b[0] & 0x80) v |= kY;
    if (b[1] & 0x01) v |= kLeftShoulder;
    if (b[1] & 0x02) v |= kRightShoulder;
    if (b[1] & 0x10) v |= kView;
    if (b[1] & 0x20) v |= kMenu;
    if (b[1] & 0x40) v |= kLeftThumb;
    if (b[1] & 0x80) v |= kRightThumb;
    if (b[2] & 0x02) v |= kView;  // touchpad click
    s.buttons = v;
    return true;
}

namespace {

constexpr USHORT kSony = 0x054C;
constexpr USHORT kDualSense = 0x0CE6, kDualSenseEdge = 0x0DF2;

struct Reader {
    std::mutex m;
    bool started = false;
    bool hid_connected = false;
    State hid;
    std::atomic<uint64_t> sequence{0};
    HMODULE xinput = nullptr;
    DWORD(WINAPI* xget)(DWORD, XINPUT_STATE*) = nullptr;
    DWORD xinput_next_try[4] = {};  // XInputGetState on an empty port is slow: retry those every 2 s
};
Reader g_reader;  // a global: the runtime has no thread-safe statics

// The first DualSense's device path, or empty.
std::wstring find_dualsense() {
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    SP_DEVICE_INTERFACE_DATA iface{sizeof iface};
    for (DWORD i = 0; found.empty() && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::string buf(need, '\0');
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof *detail;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{sizeof a};
        if (HidD_GetAttributes(h, &a) && a.VendorID == kSony && (a.ProductID == kDualSense || a.ProductID == kDualSenseEdge))
            found = detail->DevicePath;
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

DWORD WINAPI read_hid(void*) {
    for (;;) {
        std::wstring path = find_dualsense();
        HANDLE h = path.empty() ? INVALID_HANDLE_VALUE
                                : CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                              OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            Sleep(2000);
            continue;
        }
        std::fprintf(stderr, "[rt] pad: DualSense connected\n");
        uint8_t report[128];
        DWORD got = 0;
        bool first = true;
        while (ReadFile(h, report, sizeof report, &got, nullptr)) {
            State s;
            if (!parse_dualsense(report, got, s)) continue;
            if (first) {
                std::fprintf(stderr, "[rt] pad: DualSense reports %#x of %lu bytes\n", report[0], got);
                first = false;
            }
            std::lock_guard lock(g_reader.m);
            g_reader.hid = s;
            g_reader.hid_connected = true;
            ++g_reader.sequence;
        }
        CloseHandle(h);
        {
            std::lock_guard lock(g_reader.m);
            g_reader.hid_connected = false;
            g_reader.hid = {};
            ++g_reader.sequence;
        }
        std::fprintf(stderr, "[rt] pad: DualSense disconnected\n");
    }
}

float stick(SHORT v) { return std::clamp(v / 32767.0f, -1.0f, 1.0f); }

// ---- the virtual pad: commands on the named pipe \.\pipe\FlamingoPad, one a line (tools/pad.py sends them), added
// to the real controllers' input. Times in milliseconds; a press without a time is 120 ms.
//   press <button> [ms]      a, b, x, y, up, down, left, right, lb, rb, ls, rs, menu, view
//   hold <button> / release <button>
//   stick l|r <x> <y> [ms]   -1..1, +y up (no time: until the next stick command)
//   trigger l|r <0..1> [ms]
struct Virtual {
    std::mutex m;
    bool used = false;  // a command came: the pad counts as connected from then on
    uint64_t held = 0;
    struct Timed {
        uint32_t button;
        ULONGLONG until;
    };
    std::vector<Timed> presses;
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    ULONGLONG sticks_until = 0, triggers_until = 0;
};
Virtual g_virtual;

uint32_t button_named(const std::string& n) {
    static const std::pair<const char*, uint32_t> kNames[] = {
        {"a", kA}, {"b", kB}, {"x", kX}, {"y", kY}, {"up", kUp}, {"down", kDown}, {"left", kLeft}, {"right", kRight},
        {"lb", kLeftShoulder}, {"rb", kRightShoulder}, {"ls", kLeftThumb}, {"rs", kRightThumb}, {"menu", kMenu},
        {"start", kMenu}, {"view", kView}, {"back", kView}};
    for (auto& [name, b] : kNames)
        if (n == name) return b;
    return 0;
}

void virtual_command(const std::string& line) {
    char verb[16] = {}, a[16] = {};
    float f1 = 0, f2 = 0, f3 = 0;
    int n = sscanf_s(line.c_str(), "%15s %15s %f %f %f", verb, static_cast<unsigned>(sizeof verb), a,
                     static_cast<unsigned>(sizeof a), &f1, &f2, &f3);
    if (n < 2) return;
    std::string v = verb, arg = a;
    ULONGLONG now = GetTickCount64();
    std::lock_guard lock(g_virtual.m);
    g_virtual.used = true;
    if (v == "press") {
        if (uint32_t b = button_named(arg)) g_virtual.presses.push_back({b, now + static_cast<ULONGLONG>(n >= 3 ? f1 : 120)});
    } else if (v == "hold") {
        g_virtual.held |= button_named(arg);
    } else if (v == "release") {
        g_virtual.held &= ~uint64_t{button_named(arg)};
    } else if (v == "stick" && n >= 4) {
        (arg == "r" ? g_virtual.rx : g_virtual.lx) = std::clamp(f1, -1.0f, 1.0f);
        (arg == "r" ? g_virtual.ry : g_virtual.ly) = std::clamp(f2, -1.0f, 1.0f);
        g_virtual.sticks_until = n >= 5 ? now + static_cast<ULONGLONG>(f3) : ~0ull;
    } else if (v == "trigger" && n >= 3) {
        (arg == "r" ? g_virtual.rt : g_virtual.lt) = std::clamp(f1, 0.0f, 1.0f);
        g_virtual.triggers_until = n >= 4 ? now + static_cast<ULONGLONG>(f2) : ~0ull;
    }
    std::fprintf(stderr, "[rt] pad: virtual %s\n", line.c_str());
}

DWORD WINAPI virtual_pipe(void*) {
    for (;;) {
        HANDLE p = CreateNamedPipeW(L"\\\\.\\pipe\\FlamingoPad", PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 0,
                                    4096, 0, nullptr);
        if (p == INVALID_HANDLE_VALUE) return 0;
        if (ConnectNamedPipe(p, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            std::string pending;
            char buf[512];
            DWORD got = 0;
            while (ReadFile(p, buf, sizeof buf, &got, nullptr) && got) {
                pending.append(buf, got);
                for (size_t nl; (nl = pending.find('\n')) != std::string::npos; pending.erase(0, nl + 1))
                    virtual_command(pending.substr(0, nl));
            }
            if (!pending.empty()) virtual_command(pending);
        }
        DisconnectNamedPipe(p);
        CloseHandle(p);
    }
}

// The virtual pad's state now; false until a command has come.
bool virtual_state(State& s) {
    std::lock_guard lock(g_virtual.m);
    if (!g_virtual.used) return false;
    ULONGLONG now = GetTickCount64();
    s = {};
    s.buttons = static_cast<uint32_t>(g_virtual.held);
    std::erase_if(g_virtual.presses, [&](const Virtual::Timed& t) { return t.until <= now; });
    for (auto& t : g_virtual.presses) s.buttons |= t.button;
    if (now < g_virtual.sticks_until) s.left_x = g_virtual.lx, s.left_y = g_virtual.ly, s.right_x = g_virtual.rx, s.right_y = g_virtual.ry;
    if (now < g_virtual.triggers_until) s.left_trigger = g_virtual.lt, s.right_trigger = g_virtual.rt;
    return true;
}

// Sticks and triggers: the one pushed further wins; buttons: either.
void combine(State& into, const State& s) {
    into.buttons |= s.buttons;
    auto further = [](float a, float b) { return std::fabs(b) > std::fabs(a) ? b : a; };
    into.left_x = further(into.left_x, s.left_x), into.left_y = further(into.left_y, s.left_y);
    into.right_x = further(into.right_x, s.right_x), into.right_y = further(into.right_y, s.right_y);
    into.left_trigger = std::max(into.left_trigger, s.left_trigger);
    into.right_trigger = std::max(into.right_trigger, s.right_trigger);
}

}  // namespace

bool current(State& out, uint64_t* sequence) {
    Reader& r = g_reader;
    bool any = false;
    out = {};
    {
        std::lock_guard lock(r.m);
        if (!r.started) {
            r.started = true;
            if (HANDLE t = CreateThread(nullptr, 0, &read_hid, nullptr, 0, nullptr)) CloseHandle(t);
            if (HANDLE t = CreateThread(nullptr, 0, &virtual_pipe, nullptr, 0, nullptr)) CloseHandle(t);
            r.xinput = LoadLibraryW(L"xinput1_4.dll");
            if (r.xinput)
                r.xget = reinterpret_cast<DWORD(WINAPI*)(DWORD, XINPUT_STATE*)>(GetProcAddress(r.xinput, "XInputGetState"));
        }
        if (r.hid_connected) {
            out = r.hid;
            any = true;
        }
    }
    for (DWORD i = 0; r.xget && i < 4; ++i) {
        DWORD now = GetTickCount();
        if (r.xinput_next_try[i] && static_cast<int>(now - r.xinput_next_try[i]) < 0) continue;
        XINPUT_STATE x{};
        if (r.xget(i, &x) != ERROR_SUCCESS) {
            r.xinput_next_try[i] = now + 2000;
            continue;
        }
        r.xinput_next_try[i] = 0;
        const XINPUT_GAMEPAD& g = x.Gamepad;
        State s;
        // XINPUT_GAMEPAD_* bits: dpad 0x1-0x8, start 0x10, back 0x20, thumbs 0x40/0x80, shoulders 0x100/0x200,
        // A B X Y 0x1000-0x8000
        struct Bit {
            WORD xinput;
            uint32_t button;
        };
        static const Bit kMap[] = {
            {0x0001, kUp}, {0x0002, kDown}, {0x0004, kLeft}, {0x0008, kRight}, {0x0010, kMenu}, {0x0020, kView},
            {0x0040, kLeftThumb}, {0x0080, kRightThumb}, {0x0100, kLeftShoulder}, {0x0200, kRightShoulder},
            {0x1000, kA}, {0x2000, kB}, {0x4000, kX}, {0x8000, kY}};
        for (auto& [bit, b] : kMap)
            if (g.wButtons & bit) s.buttons |= b;
        s.left_trigger = g.bLeftTrigger / 255.0f, s.right_trigger = g.bRightTrigger / 255.0f;
        s.left_x = stick(g.sThumbLX), s.left_y = stick(g.sThumbLY);
        s.right_x = stick(g.sThumbRX), s.right_y = stick(g.sThumbRY);
        combine(out, s);
        any = true;
    }
    State v;
    if (virtual_state(v)) {
        combine(out, v);
        any = true;
    }
    if (sequence) *sequence = r.sequence.load();
    return any;
}

}  // namespace rt::pad
