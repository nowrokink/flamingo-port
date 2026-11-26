// The launcher's window: its plumbing and messages, the start, and its size: resizable down to MIN_W x MIN_H,
// first opened in proportion to the screen it's on (from a 1366x768 notebook to 4K), scaled per monitor (DPI v2).
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "prep/versions.h"
#include "ui/app.h"
#include "ui/launcher.h"

namespace fp::launcher {

App* g_app = nullptr;

// ---------------------------------------------------------------- window plumbing

LRESULT CALLBACK App::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        g_app->hwnd_ = h;
        return DefWindowProcW(h, msg, wp, lp);
    }
    return g_app && g_app->hwnd_ ? g_app->handle_message(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

void App::post(std::function<void()> fn) {
    {
        std::lock_guard lock(queue_m_);
        queue_.push_back(std::move(fn));
    }
    PostMessageW(hwnd_, WM_RUN, 0, 0);
}

LRESULT App::handle_message(UINT msg, WPARAM wp, LPARAM lp) {
    const float sc = g_.scale();
    switch (msg) {
        case WM_RUN: {
            std::deque<std::function<void()>> q;
            {
                std::lock_guard lock(queue_m_);
                q.swap(queue_);
            }
            for (auto& fn : q) fn();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            if (wp == 1) {  // animation
                phase_ = (phase_ + 5) % 400;
                bool animate = (splash_ && !game_child_.load()) || (mode_ == Mode::running && !game_child_.load());
                if (animate) InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (wp == 2) {
                poll_status();
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            if (game_child_.load()) {  // the game covers the window: only the bars beside its 16:9
                hits_.clear();
                if (g_.begin()) {
                    g_.clear(0x000000);
                    g_.end();
                }
            } else if (splash_) {
                hits_.clear();
                paint_splash();
            } else {
                paint();
            }
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            g_.resize();
            layout_child();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_GAME_SHOWN:
            if (game_proc_) game_shown(reinterpret_cast<HWND>(lp));
            return 0;
        case WM_GAME_FULLSCREEN:
            if (game_child_.load()) toggle_fullscreen();
            return 0;
        case WM_SETFOCUS:
            if (HWND child = game_child_.load()) SetFocus(child);  // the keyboard stays with the game
            return 0;
        case WM_SYSKEYDOWN:
            if (wp == VK_RETURN && game_child_.load()) {
                toggle_fullscreen();
                return 0;
            }
            break;
        case WM_DPICHANGED: {
            g_.set_dpi(static_cast<float>(HIWORD(wp)));
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
            set_icons();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            if (!splash_) {
                RECT r{0, 0, static_cast<LONG>(MIN_W * sc), static_cast<LONG>(MIN_H * sc)};
                AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(hwnd_));
                mm->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_F11 && game_child_.load()) {
                toggle_fullscreen();
                return 0;
            }
            if (wp == VK_ESCAPE && drawer_ != Drawer::none) {
                drawer_ = Drawer::none;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            float x = GET_X_LPARAM(lp) / sc, y = GET_Y_LPARAM(lp) / sc;
            if (dragging_bar_ && content_h_ > view_h_) {
                float thumb = std::max(24.f, view_h_ * view_h_ / content_h_);
                float t = (y - drag_off_) / std::max(1.f, view_h_ - thumb);
                scroll_ = std::clamp(t * (content_h_ - view_h_), 0.f, content_h_ - view_h_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            int over = NONE;
            for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
                if (x >= it->x0 && x < it->x1 && y >= it->y0 && y < it->y1) {
                    over = it->id;
                    break;
                }
            if (over != hover_) {
                hover_ = over;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&t);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (hover_ != NONE) {
                hover_ = NONE;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, hover_ != NONE ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: {
            float x = GET_X_LPARAM(lp) / sc, y = GET_Y_LPARAM(lp) / sc;
            RECT cr;
            GetClientRect(hwnd_, &cr);
            if (drawer_ == Drawer::none && scrollbar_ && x >= cr.right / sc - 10) {
                float thumb = std::max(24.f, view_h_ * view_h_ / content_h_);
                float top = scroll_ / std::max(1.f, content_h_ - view_h_) * (view_h_ - thumb);
                drag_off_ = (y >= top && y <= top + thumb) ? y - top : thumb / 2;
                dragging_bar_ = true;
                SetCapture(hwnd_);
                handle_message(WM_MOUSEMOVE, wp, lp);
                return 0;
            }
            pressed_ = hover_;
            return 0;
        }
        case WM_LBUTTONUP:
            if (dragging_bar_) {
                dragging_bar_ = false;
                ReleaseCapture();
                return 0;
            }
            if (pressed_ != NONE && pressed_ == hover_) click(pressed_);
            pressed_ = NONE;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &p);
            float y = p.y / sc;
            int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            const float x = p.x / sc;
            if (drawer_ != Drawer::none) {
                if (x >= drawer_x_)
                    drawer_scroll_ = std::clamp(drawer_scroll_ - notches * 60.f, 0.f,
                                                std::max(0.f, drawer_h_ - drawer_view_h_));
            } else if (log_open_ && y >= log_box_y0_ && y < log_box_y1_) {
                log_scroll_ = std::clamp(log_scroll_ + notches * 3, 0, std::max(0, static_cast<int>(log_.size()) - 9));
            } else if (scrollbar_) {
                scroll_ = std::clamp(scroll_ - notches * 60.f, 0.f, std::max(0.f, content_h_ - view_h_));
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_CLOSE:
            // playing: the close button ends the game (its window ends its process) and the page comes back
            if (HWND child = game_child_.load()) {
                PostMessageW(child, WM_CLOSE, 0, 0);
                return 0;
            }
            if (splash_ && game_proc_) {  // still starting: there's no window of its own to close yet
                TerminateProcess(game_proc_, 0);
                return 0;
            }
            if (prep_ && MessageBoxW(hwnd_,
                                     L"A preparação ainda está rodando. Pausar e sair?\n\nO que já foi feito fica guardado.",
                                     TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            if (prep_) {
                stopping_ = "pause";
                prep_->stop();
            }
            remember();
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// The app's icon (app/flamingo.rc) at this monitor's sizes, for the title bar and the taskbar.
void App::set_icons() {
    // LoadImage picks the .ico's nearest size (comctl32's LoadIconWithScaleDown would load that DLL at start)
    const UINT dpi = GetDpiForWindow(hwnd_);
    auto load = [&](int metric_x, int metric_y) {
        return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_FLAMINGO), IMAGE_ICON,
                                             GetSystemMetricsForDpi(metric_x, dpi), GetSystemMetricsForDpi(metric_y, dpi),
                                             LR_DEFAULTCOLOR));
    };
    if (HICON big = load(SM_CXICON, SM_CYICON))
        if (auto old = reinterpret_cast<HICON>(SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big))))
            DestroyIcon(old);
    if (HICON little = load(SM_CXSMICON, SM_CYSMICON))
        if (auto old = reinterpret_cast<HICON>(SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(little))))
            DestroyIcon(old);
}

// ---------------------------------------------------------------- start

int App::run(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_app = this;
    load_settings();

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_FLAMINGO));
    wc.lpszClassName = L"FlamingoPortLauncher";
    wc.hbrBackground = CreateSolidBrush(RGB(0x14, 0x0b, 0x20));
    RegisterClassExW(&wc);
    const DWORD style = WS_OVERLAPPEDWINDOW;
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr,
                            wc.hInstance, nullptr);
    if (!hwnd_ || !g_.init(hwnd_)) return 1;
    // dark title bar (DWMWA_USE_IMMERSIVE_DARK_MODE)
    using SetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (auto set = reinterpret_cast<SetAttr>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmSetWindowAttribute"))) {
        BOOL dark = TRUE;
        set(hwnd_, 20, &dark, sizeof dark);
    }
    set_icons();
    // the design's 1280x880 where the screen has room for it, else most of the work area; centred
    {
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(mon, &mi);
        const RECT work = mi.rcWork;
        const float sc = g_.scale();
        const float ww = (work.right - work.left) / sc, wh = (work.bottom - work.top) / sc;
        const float cw = std::max(std::min(START_W, ww * 0.9f), std::min(MIN_W, ww));
        const float ch = std::max(std::min(START_H, wh * 0.92f), std::min(MIN_H, wh));
        RECT r{0, 0, static_cast<LONG>(cw * sc), static_cast<LONG>(ch * sc)};
        AdjustWindowRectExForDpi(&r, style, FALSE, 0, GetDpiForWindow(hwnd_));
        const int w = std::min<int>(r.right - r.left, work.right - work.left);
        const int h = std::min<int>(r.bottom - r.top, work.bottom - work.top);
        SetWindowPos(hwnd_, nullptr, work.left + (work.right - work.left - w) / 2,
                     work.top + (work.bottom - work.top - h) / 2, w, h, SWP_NOZORDER);
    }
    ShowWindow(hwnd_, SW_SHOW);

    if (port_ready()) {
        for (const auto& st : kStages) {
            state_[st.key] = "done";
            frac_[st.key] = 1;
        }
        mode_ = Mode::ready;
    }
    show_out();
    if (!game_.empty() && !prep::match_version(game_).is_null())
        set_game(game_);
    else
        find_game();
    SetTimer(hwnd_, 1, 40, nullptr);
    SetTimer(hwnd_, 2, 1000, nullptr);
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--prepare")) post([this] { prepare(); });
        // --play: straight to the game (a desktop shortcut's way in), when the port is ready
        if (!wcscmp(argv[i], L"--play")) post([this] {
            if (mode_ == Mode::ready && !version_.is_null()) play();
        });
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (prep_thread_.joinable()) prep_thread_.join();
    CoUninitialize();
    return 0;
}

int main(int argc, wchar_t** argv) {
    static App app;
    return app.run(argc, argv);
}

}  // namespace fp::launcher
