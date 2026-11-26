// The resource overlay (overlay.h): a thread measures every half second and draws the lines with GDI into a bitmap,
// which the present copies to a texture and draws over the frame.
#include "overlay.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include "uma.h"
#include "window.h"

namespace rt::overlay {
namespace {

std::atomic<bool> g_on{[] {
    char v[4] = {};
    return !(GetEnvironmentVariableA("RT_OVERLAY", v, sizeof v) && v[0] == '0');
}()};

// frame times, written by the presenting thread
std::mutex g_frames_lock;
LARGE_INTEGER g_last_present{};
double g_frame_sum_ms = 0, g_frame_worst_ms = 0;
uint32_t g_frame_count = 0;
// the last frames' times, for the minimum, maximum, average and 1% low (about half a minute at 30 FPS)
constexpr size_t kHistory = 1024;
float g_history[kHistory];
size_t g_history_count = 0, g_history_next = 0;

std::mutex g_runtime_lock;
RuntimeNumbers g_runtime;

std::mutex g_image_lock;
std::vector<uint32_t> g_image;
uint32_t g_w = 0, g_h = 0;
uint64_t g_version = 0;

std::atomic<ID3D12Device*> g_device{nullptr};
std::atomic<bool> g_started{false};
std::atomic<int> g_screen_w{1920};  // the back buffer's width: the top bar spans it

// PROCESS_MEMORY_COUNTERS_EX2 (Windows 10 21H1 on), spelled out: the SDK only declares it for newer targets.
struct MemoryCounters {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
        QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
    SIZE_T PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
};

uint64_t filetime(const FILETIME& f) { return (uint64_t{f.dwHighDateTime} << 32) | f.dwLowDateTime; }

struct Sampler {
    // CPU
    uint64_t proc_last = 0, sys_idle_last = 0, sys_all_last = 0;
    LARGE_INTEGER wall_last{};
    // GPU engines (PDH, English names: the counters are localised on this machine)
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER gpu = nullptr;
    PDH_HCOUNTER disk_idle = nullptr;  // \PhysicalDisk(_Total)\% Idle Time
    // the process's reads and writes
    uint64_t read_last = 0, write_last = 0;
    LARGE_INTEGER io_last{};
    // video memory
    IDXGIAdapter3* adapter = nullptr;
};

struct Numbers {
    double fps = 0, frame_ms = 0, worst_ms = 0;
    double fps_min = 0, fps_max = 0, fps_avg = 0, fps_low1 = 0;  // over the history
    double read_mbs = 0, write_mbs = 0, disk_busy = -1;
    double cpu_game = 0, cpu_system = 0;  // percent of all cores
    uint64_t ram_real = 0, ram_ws = 0, ram_used = 0, ram_total = 0, pool = 0;
    uint64_t vram_used = 0, vram_budget = 0, vram_shared = 0;
    double gpu_3d = -1;
    RuntimeNumbers runtime;
};

void sample(Sampler& s, Numbers& n) {
    {
        std::lock_guard lock(g_frames_lock);
        if (g_frame_count) {
            n.frame_ms = g_frame_sum_ms / g_frame_count;
            n.fps = n.frame_ms > 0 ? 1000.0 / n.frame_ms : 0;
            n.worst_ms = g_frame_worst_ms;
        }
        g_frame_sum_ms = g_frame_worst_ms = 0;
        g_frame_count = 0;
        if (g_history_count) {
            std::vector<float> t(g_history, g_history + g_history_count);
            std::sort(t.begin(), t.end());
            double sum = 0;
            for (float x : t) sum += x;
            n.fps_max = t.front() > 0 ? 1000.0 / t.front() : 0;
            n.fps_min = t.back() > 0 ? 1000.0 / t.back() : 0;
            n.fps_avg = sum > 0 ? 1000.0 * t.size() / sum : 0;
            // 1% low: the frame rate of the slowest 1% of frames (their average time)
            const size_t k = std::max<size_t>(1, t.size() / 100);
            double slow = 0;
            for (size_t i = t.size() - k; i < t.size(); ++i) slow += t[i];
            n.fps_low1 = slow > 0 ? 1000.0 * k / slow : 0;
        }
    }
    // disk: what the process read and wrote, and how busy the disks were
    IO_COUNTERS io{};
    if (GetProcessIoCounters(GetCurrentProcess(), &io)) {
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        if (s.io_last.QuadPart) {
            const double secs = double(t.QuadPart - s.io_last.QuadPart) / f.QuadPart;
            n.read_mbs = (io.ReadTransferCount - s.read_last) / secs / (1 << 20);
            n.write_mbs = (io.WriteTransferCount - s.write_last) / secs / (1 << 20);
        }
        s.read_last = io.ReadTransferCount, s.write_last = io.WriteTransferCount, s.io_last = t;
    }
    {
        std::lock_guard lock(g_runtime_lock);
        n.runtime = g_runtime;
    }
    // CPU: the process's time over the wall time and the cores; the system's from its idle time
    FILETIME c, e, k, u, si, sk, su;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) && GetSystemTimes(&si, &sk, &su)) {
        const uint64_t proc = filetime(k) + filetime(u), idle = filetime(si), all = filetime(sk) + filetime(su);
        if (s.wall_last.QuadPart) {
            const double wall_100ns = (now.QuadPart - s.wall_last.QuadPart) * 1e7 / freq.QuadPart;
            n.cpu_game = 100.0 * (proc - s.proc_last) / (wall_100ns * info.dwNumberOfProcessors);
            const uint64_t d_all = all - s.sys_all_last;  // (kernel time includes idle)
            n.cpu_system = d_all ? 100.0 * (d_all - (idle - s.sys_idle_last)) / d_all : 0;
        }
        s.proc_last = proc, s.sys_idle_last = idle, s.sys_all_last = all, s.wall_last = now;
    }
    // memory
    MemoryCounters m{};
    m.cb = sizeof m;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof m)) {
        n.ram_ws = m.WorkingSetSize;
        n.pool = uma::pool_bytes_used();
        n.ram_real = m.PrivateWorkingSetSize + n.pool;
    } else {
        m.cb = sizeof(PROCESS_MEMORY_COUNTERS_EX);  // an older Windows: no private working set
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), m.cb))
            n.ram_ws = m.WorkingSetSize;
    }
    MEMORYSTATUSEX ms{sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) n.ram_total = ms.ullTotalPhys, n.ram_used = ms.ullTotalPhys - ms.ullAvailPhys;
    // video memory
    if (!s.adapter) {
        if (ID3D12Device* d = g_device.load()) {
            IDXGIFactory4* f = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
                f->EnumAdapterByLuid(d->GetAdapterLuid(), IID_PPV_ARGS(&s.adapter));
                f->Release();
            }
        }
    }
    if (s.adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, other{};
        if (SUCCEEDED(s.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
            n.vram_used = local.CurrentUsage, n.vram_budget = local.Budget;
        if (SUCCEEDED(s.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &other)))
            n.vram_shared = other.CurrentUsage;
    }
    // GPU: the 3D engines' utilisation for this process (instances "pid_<pid>_..._engtype_3D")
    if (!s.query && PdhOpenQueryW(nullptr, 0, &s.query) == ERROR_SUCCESS) {
        if (PdhAddEnglishCounterW(s.query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &s.gpu) != ERROR_SUCCESS)
            s.gpu = nullptr;
        if (PdhAddEnglishCounterW(s.query, L"\\PhysicalDisk(_Total)\\% Idle Time", 0, &s.disk_idle) != ERROR_SUCCESS)
            s.disk_idle = nullptr;
        PdhCollectQueryData(s.query);
    } else if (s.query && PdhCollectQueryData(s.query) == ERROR_SUCCESS) {
        PDH_FMT_COUNTERVALUE idle{};
        if (s.disk_idle && PdhGetFormattedCounterValue(s.disk_idle, PDH_FMT_DOUBLE, nullptr, &idle) == ERROR_SUCCESS)
            n.disk_busy = std::clamp(100.0 - idle.doubleValue, 0.0, 100.0);
        DWORD bytes = 0, count = 0;
        if (s.gpu &&
            PdhGetFormattedCounterArrayW(s.gpu, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr) ==
                static_cast<PDH_STATUS>(PDH_MORE_DATA) && bytes) {
            std::vector<uint8_t> buf(bytes);
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
            if (PdhGetFormattedCounterArrayW(s.gpu, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items) ==
                ERROR_SUCCESS) {
                wchar_t prefix[32];
                swprintf(prefix, 32, L"pid_%lu_", GetCurrentProcessId());
                double sum = 0;
                for (DWORD i = 0; i < count; ++i)
                    if (items[i].szName && std::wcsncmp(items[i].szName, prefix, std::wcslen(prefix)) == 0 &&
                        std::wcsstr(items[i].szName, L"engtype_3D") && items[i].FmtValue.CStatus == ERROR_SUCCESS)
                        sum += items[i].FmtValue.doubleValue;
                n.gpu_3d = std::min(sum, 100.0);
            }
        }
    }
}

// One line, in columns: the label in its colour, the number in white (right-aligned), then its unit and a note in
// grey and a smaller type.
struct Line {
    const wchar_t* label;
    COLORREF color;
    std::wstring value, unit;
};

double gib(uint64_t b) { return b / double(1ull << 30); }

std::wstring format(const wchar_t* f, double a, double b = 0) {
    wchar_t v[96];
    swprintf(v, 96, f, a, b);
    return v;
}

void draw(const Numbers& n, int screen_w) {
    const COLORREF fps_c = RGB(255, 200, 60), cpu_c = RGB(80, 170, 255), gpu_c = RGB(90, 220, 110),
                   mem_c = RGB(230, 140, 255), disk_c = RGB(200, 200, 200), unit_c = RGB(165, 165, 165);
    std::vector<Line> lines;
    lines.push_back({L"FPS", fps_c, format(L"%.0f", n.fps), format(L"fps   %.1f ms", n.frame_ms)});
    lines.push_back({L"M\u00c9DIA", fps_c, format(L"%.1f", n.fps_avg), L"fps"});
    lines.push_back({L"1% LOW", fps_c, format(L"%.1f", n.fps_low1), L"fps"});
    lines.push_back({L"M\u00cdN", fps_c, format(L"%.1f", n.fps_min), L"fps"});
    lines.push_back({L"M\u00c1X", fps_c, format(L"%.1f", n.fps_max), L"fps"});
    lines.push_back({L"CPU", cpu_c, format(L"%.0f", n.cpu_game), L"%"});
    lines.push_back({L"GPU", gpu_c, n.gpu_3d >= 0 ? format(L"%.0f", n.gpu_3d) : L"-", L"%"});
    lines.push_back({L"RAM", mem_c, format(L"%.1f", gib(n.ram_real)), L"GB"});
    lines.push_back({L"VRAM", gpu_c, format(L"%.1f", gib(n.vram_used)), format(L"GB   de %.1f", gib(n.vram_budget))});
    lines.push_back({L"DISCO", disk_c, format(L"%.1f", n.read_mbs + n.write_mbs), L"MB/s"});
    // the top bar: the details, one line across the screen
    wchar_t bar[512];
    swprintf(bar, 512,
             L"PORT   espera GPU %.1f ms  ·  draws %.1f ms  ·  pipelines compilando %llu  ·  "
             L"pior quadro %.0f ms  ·  CPU sistema %.0f%%  ·  RAM sistema %.1f/%.0f GB  ·  "
             L"console %.1f GB  ·  Gerenciador %.1f GB  ·  VRAM compart. %.1f GB",
             n.runtime.gpu_wait_ms, n.runtime.draw_ms, static_cast<unsigned long long>(n.runtime.pipelines_making),
             n.worst_ms, n.cpu_system, gib(n.ram_used), gib(n.ram_total), gib(n.pool), gib(n.ram_ws),
             gib(n.vram_shared));

    const int font_h = 24, unit_h = 17, bar_font_h = 16, pad = 10, gap = 14, line_h = font_h + 4;
    const int bar_h = bar_font_h + 10, margin = 12;
    HDC dc = CreateCompatibleDC(nullptr);
    auto make_font = [](int height, int weight) {
        return CreateFontW(-height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    HFONT font = make_font(font_h, FW_BOLD), font_unit = make_font(unit_h, FW_SEMIBOLD),
          font_bar = make_font(bar_font_h, FW_SEMIBOLD);
    HGDIOBJ old_font = SelectObject(dc, font);
    auto extent = [&](const std::wstring& t) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, t.c_str(), static_cast<int>(t.size()), &sz);
        return static_cast<int>(sz.cx);
    };
    // the panel's columns: the widest label, the widest number (fits "000.0", so it doesn't jump), the units
    int label_w = 0, value_w = extent(L"000.0"), unit_w = 0;
    for (const Line& l : lines) {
        label_w = std::max(label_w, extent(l.label));
        value_w = std::max(value_w, extent(l.value));
    }
    SelectObject(dc, font_unit);
    for (const Line& l : lines) unit_w = std::max(unit_w, extent(l.unit));
    SelectObject(dc, font_bar);
    const int bar_text_w = extent(bar);
    // panel, at (margin, bar_h + margin)
    const int panel_x = margin, panel_y = bar_h + margin;
    const int value_right = panel_x + pad + label_w + gap + value_w, unit_x = value_right + 6;
    const int panel_w = unit_x + unit_w + pad - panel_x, panel_h = pad * 2 + line_h * static_cast<int>(lines.size());
    const int w = (std::max({screen_w, bar_text_w + 2 * pad, panel_x + panel_w}) + 3) & ~3;
    const int h = panel_y + panel_h;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteObject(font_unit);
        DeleteObject(font_bar);
        DeleteDC(dc);
        return;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    std::memset(bits, 0, size_t{4} * w * h);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, font_bar);
    SetTextColor(dc, RGB(240, 120, 200));
    TextOutW(dc, pad, (bar_h - bar_font_h) / 2 - 1, L"PORT", 4);
    SetTextColor(dc, RGB(225, 225, 225));
    TextOutW(dc, pad + extent(L"PORT   "), (bar_h - bar_font_h) / 2 - 1, bar + 7,
             static_cast<int>(std::wcslen(bar + 7)));
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line& l = lines[i];
        const int y = panel_y + pad + static_cast<int>(i) * line_h;
        SelectObject(dc, font);
        SetTextColor(dc, l.color);
        TextOutW(dc, panel_x + pad, y, l.label, static_cast<int>(std::wcslen(l.label)));
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, value_right - extent(l.value), y, l.value.c_str(), static_cast<int>(l.value.size()));
        SelectObject(dc, font_unit);
        SetTextColor(dc, unit_c);
        TextOutW(dc, unit_x, y + (font_h - unit_h) - 1, l.unit.c_str(), static_cast<int>(l.unit.size()));
    }
    GdiFlush();
    // Text drawn over black: its coverage is the brightest channel. Straight alpha out: the colour undone from the
    // coverage, over the translucent dark bar and panel; clear everywhere else.
    std::vector<uint32_t> img(size_t{1} * w * h);
    const uint32_t* px = static_cast<const uint32_t*>(bits);
    constexpr uint32_t kPanel = 150, kBar = 185;  // the backgrounds' alpha
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t{1} * y * w + x;
            const bool in_panel = x >= panel_x && x < panel_x + panel_w && y >= panel_y;
            const uint32_t base = y < bar_h ? kBar : in_panel ? kPanel : 0;
            const uint32_t p = px[i], b = p & 255, g = (p >> 8) & 255, r = (p >> 16) & 255;
            const uint32_t cov = std::max({r, g, b});
            if (!cov) {
                img[i] = base << 24;
                continue;
            }
            const uint32_t a = base + (255 - base) * cov / 255;
            // the text's colour, its coverage taken out, then mixed with the black background by the alpha
            const auto ch = [&](uint32_t c) { return std::min<uint32_t>(255, (c * 255 / cov) * cov / a); };
            img[i] = a << 24 | ch(r) << 16 | ch(g) << 8 | ch(b);
        }
    SelectObject(dc, old_bmp);
    SelectObject(dc, old_font);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteObject(font_unit);
    DeleteObject(font_bar);
    DeleteDC(dc);
    std::lock_guard lock(g_image_lock);
    g_image = std::move(img);
    g_w = static_cast<uint32_t>(w), g_h = static_cast<uint32_t>(h);
    ++g_version;
}

DWORD WINAPI overlay_thread(void*) {
    Sampler s;
    for (;;) {
        Numbers n;
        sample(s, n);
        if (g_on.load()) draw(n, g_screen_w.load());
        Sleep(500);
    }
}

}  // namespace

bool enabled() { return g_on.load(); }

void poll_toggle() {
    static std::atomic<bool> was_down{false};
    const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (down == was_down.exchange(down) || !down) return;
    // only with the keyboard on the game (its window, or the launcher's around it)
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    HWND ours = window::get(1280, 720);
    if (pid != GetCurrentProcessId() && fg != GetAncestor(ours, GA_ROOT)) return;
    g_on = !g_on.load();
}

void frame_presented(ID3D12Device* device, uint32_t width) {
    if (device && !g_device.load()) g_device = device;
    if (width) g_screen_w = static_cast<int>(width);
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    {
        std::lock_guard lock(g_frames_lock);
        if (g_last_present.QuadPart) {
            const double ms = (now.QuadPart - g_last_present.QuadPart) * 1000.0 / freq.QuadPart;
            g_frame_sum_ms += ms;
            g_frame_worst_ms = std::max(g_frame_worst_ms, ms);
            ++g_frame_count;
            g_history[g_history_next] = static_cast<float>(ms);
            g_history_next = (g_history_next + 1) % kHistory;
            g_history_count = std::min(g_history_count + 1, kHistory);
        }
        g_last_present = now;
    }
    if (!g_started.exchange(true))
        if (HANDLE t = CreateThread(nullptr, 0, &overlay_thread, nullptr, 0, nullptr)) {
            SetThreadDescription(t, L"rt overlay");
            CloseHandle(t);
        }
}

void set_runtime(const RuntimeNumbers& n) {
    std::lock_guard lock(g_runtime_lock);
    g_runtime = n;
}

bool copy_image(std::vector<uint32_t>& out, uint32_t* w, uint32_t* h, uint64_t* version) {
    std::lock_guard lock(g_image_lock);
    if (g_image.empty()) return false;
    out = g_image;
    *w = g_w, *h = g_h, *version = g_version;
    return true;
}

}  // namespace rt::overlay
