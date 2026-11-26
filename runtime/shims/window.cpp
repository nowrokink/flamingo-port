// The window frames are presented in: made on first use, on a thread of its own that runs its message loop (the
// title never pumps messages: on the console there are none). Closing it ends the process.
//
// Started by the launcher (FlamingoPort.exe), RT_PARENT_WINDOW names the launcher's window: ours is then a child of
// it, hidden until the first frame, sized and placed by the launcher (one window and one taskbar button for both).
// The two processes' input queues are joined by Windows for a parent and child; this thread only pumps messages, so
// it never holds the launcher's up. Alt+Enter and F11 go to the launcher, which owns the full-screen switch; if the
// launcher's window goes, ours goes with it and the game ends.
#include "window.h"

#include "shims.h"

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace rt::window {
namespace {

std::atomic<HWND> g_window{nullptr};
std::atomic<HWND> g_parent{nullptr};
HANDLE g_ready = nullptr;

// The launcher's window from RT_PARENT_WINDOW (a number, as the launcher writes it), or null.
HWND parent_from_environment() {
    wchar_t v[32] = {};
    DWORD n = GetEnvironmentVariableW(L"RT_PARENT_WINDOW", v, 32);
    if (!n || n >= 32) return nullptr;
    HWND h = reinterpret_cast<HWND>(static_cast<uintptr_t>(wcstoull(v, nullptr, 0)));
    return IsWindow(h) ? h : nullptr;
}

LRESULT CALLBACK proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (HWND parent = g_parent.load()) {
        // the launcher owns full screen: Alt+Enter (no beep for the Alt) and F11 are passed up
        if ((msg == WM_SYSKEYDOWN && wp == VK_RETURN) || (msg == WM_KEYDOWN && wp == VK_F11)) {
            PostMessageW(parent, WM_GAME_FULLSCREEN, 0, 0);
            return 0;
        }
        if (msg == WM_SYSCHAR && wp == VK_RETURN) return 0;
        if (msg == WM_DESTROY) {  // the launcher's window went (it closed, or its process ended): so does the game
            std::fprintf(stderr, "[rt] window: the launcher's window is gone: ending\n");
            std::fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
    if (msg == WM_CLOSE) {
        std::fprintf(stderr, "[rt] window closed: ending\n");
        std::fflush(stderr);
        // TerminateProcess, not ExitProcess: nothing of ours needs the DLL detach ExitProcess runs, and it runs with
        // the other threads ended where they stood (pipeline makers inside the driver's compiler among them). Saves
        // are safe: each is committed by one rename (gamesave.cpp).
        // Either way the process stays listed for about two minutes after a world load (measured with Stop-Process
        // too: one thread left, 20 GB of private memory going back), holding the exe meanwhile.
        TerminateProcess(GetCurrentProcess(), 0);
    }
    return DefWindowProcW(w, msg, wp, lp);
}

struct Size {
    int width, height;
};

DWORD WINAPI thread(void* p) {
    auto* s = static_cast<Size*>(p);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FlamingoPortWindow";
    RegisterClassExW(&wc);
    if (HWND parent = g_parent.load()) {
        // inside the launcher: a hidden child at the frame's size; the launcher places it when it shows. Per-monitor
        // DPI aware as the launcher's window is (a DPI-unaware child would be stretched, blurred, by Windows).
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        HWND w = CreateWindowExW(0, wc.lpszClassName, L"Flamingo Port", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, s->width,
                                 s->height, parent, nullptr, wc.hInstance, nullptr);
        delete s;
        if (w) {
            std::fprintf(stderr, "[rt] window: inside the launcher's window\n");
        } else {
            std::fprintf(stderr, "[rt] window: can't be a child of the launcher's window (error %lu): one of its own\n",
                         GetLastError());
            g_parent = nullptr;
            s = new Size{1280, 720};
        }
        if (w) {
            g_window = w;
            SetEvent(g_ready);
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            return 0;
        }
    }
    RECT r{0, 0, s->width, s->height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    // Centred in the primary monitor's work area, where the launcher's splash was.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    const int aw = work.right - work.left, ah = work.bottom - work.top;
    const int x = aw > 0 ? work.left + std::max(0, (aw - ww) / 2) : CW_USEDEFAULT;
    const int y = ah > 0 ? work.top + std::max(0, (ah - wh) / 2) : CW_USEDEFAULT;
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"Flamingo Port", WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y, ww, wh,
                             nullptr, nullptr, wc.hInstance, nullptr);
    delete s;
    g_window = w;
    SetEvent(g_ready);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

}  // namespace

HWND get(int width, int height) {
    if (HWND w = g_window.load()) return w;
    static std::atomic<bool> starting{false};
    if (!starting.exchange(true)) {
        g_parent = parent_from_environment();
        g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        CloseHandle(CreateThread(nullptr, 0, &thread, new Size{width, height}, 0, nullptr));
    }
    while (!g_window.load()) Sleep(1);
    return g_window.load();
}

void frame_shown() {
    static std::atomic<bool> signalled{false};
    if (signalled.load(std::memory_order_relaxed) || signalled.exchange(true)) return;
    HWND w = g_window.load();
    if (HWND parent = g_parent.load(); parent && w) {
        // inside the launcher: on screen now, and the launcher told which window to place (before the event, so its
        // message arrives first)
        ShowWindow(w, SW_SHOWNA);
        PostMessageW(parent, WM_GAME_SHOWN, 0, reinterpret_cast<LPARAM>(w));
    }
    wchar_t name[128] = {};
    DWORD n = GetEnvironmentVariableW(L"RT_SPLASH_EVENT", name, 128);
    if (n && n < 128) {
        if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, name)) {
            SetEvent(e);
            CloseHandle(e);
        }
    }
    if (w && !g_parent.load()) SetForegroundWindow(w);
    std::fprintf(stderr, "[rt] window: first frame shown, the launcher's splash told\n");
}

bool embedded() { return g_parent.load() != nullptr; }

}  // namespace rt::window

// ---- the title's own window: it makes one (CreateWindowExA, title "America") as the console's runtime expects, and
// shows it; on the PC that's an empty white window beside ours. It's made hidden and stays hidden (its messages still
// work: the title never pumps them anyway).
namespace {

std::atomic<HWND> g_title_window{nullptr};

HWND WINAPI CreateWindowExA_(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent,
                             HMENU menu, HINSTANCE inst, LPVOID param) {
    HWND hwnd = CreateWindowExA(ex, cls, name, style & ~WS_VISIBLE, x, y, w, h, parent, menu, inst, param);
    if (!parent) g_title_window = hwnd;
    std::fprintf(stderr, "[rt] window: the title's window \"%s\" made hidden\n", name ? name : "");
    return hwnd;
}

BOOL WINAPI ShowWindow_(HWND hwnd, int cmd) {
    if (hwnd && hwnd == g_title_window.load()) return FALSE;  // stays hidden
    return ShowWindow(hwnd, cmd);
}

}  // namespace

SHIM_TABLE(title_window,
    {"ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "CreateWindowExA", reinterpret_cast<void*>(&CreateWindowExA_)},
    {"ext-ms-win-rtcore-ntuser-window-l1-1-0.dll", "ShowWindow", reinterpret_cast<void*>(&ShowWindow_)},
);
