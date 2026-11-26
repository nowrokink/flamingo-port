// Playing: the game in a second process of this exe, and the launcher as a splash until its first frame.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <thread>
#include <vector>

#include "core/fs.h"
#include "core/process.h"
#include "core/system.h"
#include "core/text.h"
#include "ui/app.h"

namespace fp::launcher {

// ---------------------------------------------------------------- playing

// The game runs in a second process of this exe (--run): the title takes over the process (its TLS, its memory
// layout), so the launcher stays out of it. The splash stays until the runtime signals the first frame on screen
// (rt::window::frame_shown, through the event named in RT_SPLASH_EVENT), then the launcher hides; it comes back
// when the game ends.
void App::play() {
    if (version_.is_null() || game_proc_) return;
    // not while shaders are being translated (by this window or outside it): the cache is half old, half new
    if (!watching().is_null() || (mode_ == Mode::running && current_ != HEAVY)) {
        MessageBoxW(hwnd_, L"A tradução dos shaders ainda está em andamento. Jogue quando ela terminar.", TITLE,
                    MB_ICONINFORMATION);
        return;
    }
    const std::wstring game_exe = join(game_, w(version_["expected_exe"].str()));
    if (!is_file(game_exe)) {
        MessageBoxW(hwnd_, (w(version_["expected_exe"].str()) + L" não está na pasta da sua cópia.").c_str(), TITLE,
                    MB_ICONERROR);
        return;
    }
    const std::wstring logs = join(out_, L"logs");
    make_dirs(logs);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileW(join(logs, L"game.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    wchar_t event_name[96];
    static int launches = 0;
    swprintf_s(event_name, L"Local\\FlamingoPortFirstFrame-%lu-%d", GetCurrentProcessId(), ++launches);
    HANDLE shown = CreateEventW(nullptr, TRUE, FALSE, event_name);

    Spawn s;
    s.exe = self_path();
    s.args = {L"--run", game_exe, L"--game-dir", game_};
    std::wstring cmd = command_line(s);
    std::wstring block;
    {
        std::vector<std::wstring> vars;
        if (wchar_t* e = GetEnvironmentStringsW()) {
            for (const wchar_t* p = e; *p; p += wcslen(p) + 1) vars.emplace_back(p);
            FreeEnvironmentStringsW(e);
        }
        auto set = [&](const std::wstring& k, const std::wstring& v) {
            std::wstring prefix = k + L"=";
            vars.erase(std::remove_if(vars.begin(), vars.end(),
                                      [&](const std::wstring& x) { return _wcsnicmp(x.c_str(), prefix.c_str(), prefix.size()) == 0; }),
                       vars.end());
            vars.push_back(prefix + v);
        };
        // The console the game is told it runs on: Series X renders 1440p for a 4K output and budgets 13.5 GiB;
        // Series S renders for 1080p and budgets 7.7 GiB, which is what fits a PC with 8 GB of video memory.
        set(L"RT_CONSOLE", flag("full_hd", true) ? L"seriess" : L"seriesx");
        set(L"RT_SHADER_CACHE", join(out_, L"shaders"));
        set(L"RT_SPLASH_EVENT", event_name);
        // the game's window as a child of this one (runtime/shims/window.cpp): one window for the launcher and the game
        set(L"RT_PARENT_WINDOW", std::to_wstring(reinterpret_cast<uintptr_t>(hwnd_)));
        std::sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        for (const auto& v : vars) {
            block += v;
            block += L'\0';
        }
        block += L'\0';
    }
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = log;
    si.hStdError = log;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(s.exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT,
                             block.data(), out_.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        CloseHandle(shown);
        MessageBoxW(hwnd_, L"Não deu para iniciar o jogo.", TITLE, MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    AllowSetForegroundWindow(pi.dwProcessId);
    game_proc_ = pi.hProcess;
    game_running_ = true;
    pipelines_before_ = pipeline_lines();
    add_log(timestamp(), "$ FlamingoPort.exe --run (registro em logs\\game.log)");
    enter_splash();
    HANDLE proc = pi.hProcess;
    const DWORD pid = pi.dwProcessId;
    game_closing_ = false;
    std::thread([this, proc, shown, pid] {
        HANDLE both[2] = {shown, proc};
        DWORD r = WaitForMultipleObjects(2, both, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) {
            // the first frame: the game's window is in ours now (WM_GAME_SHOWN came first); a runtime that made a
            // window of its own instead leaves the launcher hidden meanwhile
            post([this] {
                if (splash_ && !game_child_.load()) {
                    splash_ = false;
                    ShowWindow(hwnd_, SW_HIDE);
                }
            });
            // After its window closes the game process lingers a minute or two handing back ~20 GB of memory: the
            // launcher comes back when the window is gone (two looks in a row), not when the process ends.
            int missing = 0;
            while (WaitForSingleObject(proc, 500) == WAIT_TIMEOUT) {
                const HWND child = game_child_.load();
                const bool there = child ? IsWindow(child) && IsWindowVisible(child) : has_window(pid);
                missing = there ? 0 : missing + 1;
                if (missing == 2) {
                    post([this] {
                        game_closing_ = true;
                        leave_splash();
                        add_log(timestamp(), "a janela do jogo fechou; o processo termina de devolver a memória");
                    });
                    WaitForSingleObject(proc, INFINITE);
                    break;
                }
            }
        }
        DWORD code = 0;
        GetExitCodeProcess(proc, &code);
        CloseHandle(shown);
        post([this, code] {
            CloseHandle(game_proc_);
            game_proc_ = nullptr;
            game_closing_ = false;
            game_running_ = false;
            // pipelines the game asked for that the driver hasn't made ahead yet: offered on the ready card
            pipelines_new_ += std::max<int64_t>(0, pipeline_lines() - pipelines_before_);
            leave_splash();
            if (code) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "o jogo saiu com o código 0x%lx", static_cast<unsigned long>(code));
                add_log(timestamp(), buf);
            }
        });
    }).detach();
}

// Whether process `pid` has a visible top-level window (the game's).
bool App::has_window(DWORD pid) {
    struct Look {
        DWORD pid;
        bool found;
    } look{pid, false};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* l = reinterpret_cast<Look*>(lp);
            DWORD owner = 0;
            GetWindowThreadProcessId(h, &owner);
            if (owner == l->pid && IsWindowVisible(h)) {
                l->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&look));
    return look.found;
}

// Playing in this window: the splash fills it until the game's first frame, then the game's window (a child of
// ours, from the other process) covers it; the launcher's page comes back in the same place when the game ends.
void App::enter_splash() {
    splash_ = true;
    splash_t0_ = GetTickCount64();
    drawer_ = Drawer::none;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// The game's window is on screen: place it, give it the keyboard.
void App::game_shown(HWND child) {
    game_child_ = child;
    splash_ = false;
    layout_child();
    SetFocus(child);
    add_log(timestamp(), "o jogo está na janela do launcher (F11 ou Alt+Enter: tela cheia)");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// The game's window at the largest 16:9 that fits ours, centred (the frame is stretched to it: 1920x1080 or 4K).
// Asynchronous: a game thread that stops answering doesn't hold this one.
void App::layout_child() {
    const HWND child = game_child_.load();
    if (!child || !IsWindow(child)) return;
    RECT cr;
    GetClientRect(hwnd_, &cr);
    const int cw = cr.right, ch = cr.bottom;
    int w = cw, h = cw * 9 / 16;
    if (h > ch) {
        h = ch;
        w = ch * 16 / 9;
    }
    SetWindowPos(child, nullptr, (cw - w) / 2, (ch - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | SWP_NOOWNERZORDER);
}

// Borderless full screen on the window's monitor, and back.
void App::toggle_fullscreen() {
    if (!fullscreen_) {
        saved_place_.length = sizeof saved_place_;
        GetWindowPlacement(hwnd_, &saved_place_);
        saved_style_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (saved_style_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        fullscreen_ = true;
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, saved_style_);
        SetWindowPlacement(hwnd_, &saved_place_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = false;
    }
    layout_child();
}

// The game's window is gone (or never came): the launcher's page again, out of full screen, in front.
void App::leave_splash() {
    splash_ = false;
    game_child_ = nullptr;
    if (fullscreen_) toggle_fullscreen();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::paint_splash() {
    if (!g_.begin()) return;
    RECT cr;
    GetClientRect(hwnd_, &cr);
    const float sw = cr.right / g_.scale(), sh = cr.bottom / g_.scale();
    g_.clear(BG);
    // our own splash art (assets/launcher, in the exe's resources): never the game's
    ID2D1Bitmap* art = g_.image(IDR_SPLASH);
    if (art) {
        const D2D1_SIZE_F s = D2D1::SizeF(static_cast<float>(art->GetPixelSize().width),
                                          static_cast<float>(art->GetPixelSize().height));
        const float scale = std::max(sw / s.width, sh / s.height);
        const float cw = sw / scale, ch = sh / scale;
        g_.bitmap(art, D2D1::RectF(0, 0, sw, sh),
                  D2D1::RectF((s.width - cw) / 2, (s.height - ch) / 2, (s.width + cw) / 2, (s.height + ch) / 2));
    }
    g_.fade(0, sh * 0.72f, sw, sh, BG, 0.f, 0.9f);
    // the title in the dark sky at the top left, the scene's flamingo in the middle
    const Font tf = H(30, DWRITE_FONT_WEIGHT_BOLD, 30 * 0.14f), pf = H(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1.4f);
    const ui::Size ts = g_.measure(L"FLAMINGO", tf), ps = g_.measure(L"//PORT", pf);
    const float tx = 28, ty = 28;
    g_.text(tx, ty, L"FLAMINGO", tf, 0xffffff);
    g_.text(tx + ts.w + 10 - 30 * 0.14f, ty + (ts.h - ps.h) / 2 + 2, L"//PORT", pf, PINK);
    const ULONGLONG secs = (GetTickCount64() - splash_t0_) / 1000;
    std::wstring status = L"Iniciando o jogo…";
    if (secs >= 3) status += L"  " + std::to_wstring(secs) + L" s";
    g_.text(tx, ty + ts.h + 4, status, T(13), HDR_TEXT);
    // an indeterminate bar along the bottom
    const float y0 = sh - 22, x0 = 24, x1 = sw - 24, bh = 4;
    g_.rrect(x0, y0, x1, y0 + bh, 2, TRACK);
    const float span = x1 - x0, band = span / 4;
    const float b0 = x0 + (phase_ / 400.f) * (span + band) - band;
    g_.push_clip(x0, y0, x1, y0 + bh);
    g_.rrect_gradient(std::max(x0, b0), y0, std::min(x1, b0 + band), y0 + bh, 2, VIOLET, PINK);
    g_.pop_clip();
    g_.end();
}

}  // namespace fp::launcher
