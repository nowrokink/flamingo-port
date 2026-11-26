// The launcher's window object. Its members are spread over the ui/launcher_*.cpp files by what they do:
//   launcher_window.cpp    window plumbing, messages, the start (run), the window's size on each screen
//   launcher_settings.cpp  settings, the copy and the folders, the helpers' token and folder, the LAN address
//   launcher_prepare.cpp   preparing (the engine on a thread, its events), a translation started elsewhere, the log
//   launcher_play.cpp      starting the game, the splash
//   launcher_page.cpp      the page: what the state says, the header (cover, title, tools), the scrollbar
//   launcher_panels.cpp    the page's panels: the status card, the folders, the log
//   launcher_drawer.cpp    the side panel: settings and monitoring
//   widgets.cpp            the drawing helpers (cards, buttons, switches, icons, the stage bar) and clicks
//
// Layout is in DIPs and follows the window's width (the design's flex rules): the column is at most CONTENT_MAX wide,
// rows of tiles, chips, folders and buttons wrap when they don't fit, long paths end in an ellipsis, and the page
// scrolls when it is taller than the window.
#pragma once
#include <windows.h>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/json.h"
#include "prep/preparation.h"
#include "app/resources.h"
#include "ui/canvas.h"
#include "ui/theme.h"

namespace fp::launcher {

// This PC's address on the local network (what helpers connect to), 127.0.0.1 without one.
std::wstring lan_address();
std::string random_hex(int bytes);

class App {
public:
    int run(int argc, wchar_t** argv);

private:
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_message(UINT msg, WPARAM wp, LPARAM lp);
    void post(std::function<void()> fn);

    // settings and folders
    void load_settings();
    void remember();
    bool flag(const char* key, bool fallback) const { return s_[key].truthy(fallback); }
    void set_flag(const char* key, bool v) {
        s_.set(key, v);
        remember();
    }
    std::wstring work() const;
    bool port_ready() const;
    void find_game();
    void set_game(const std::wstring& path);
    void pick_game();
    void pick_out();
    void show_out();
    void open_folder(const std::wstring& p);
    std::string helper_token();
    void helper_folder();
    std::wstring pick_folder(const wchar_t* title, const std::wstring& start);

    // preparing
    // The stages (none: all of them, "check", "shaders", "pipelines", then the heavy shaders).
    void prepare(std::vector<std::string> stages = {});
    int64_t pipeline_lines() const;  // the lines of <cache>\pipelines.txt
    void stop_preparation(const char* how);
    void handle(const json::Value& e);
    void finished(bool ok);
    void take_machines(const json::Value& e);
    void poll_status();
    void explain_missing();
    // a translation running outside this window (translation.json), or null while this window prepares
    const json::Value& watching() const;

    // playing
    void play();
    void enter_splash();
    void game_shown(HWND child);
    void layout_child();
    void toggle_fullscreen();
    static bool has_window(DWORD pid);
    void leave_splash();

    // log
    void add_log(const std::wstring& stamp, const std::string& text);
    void log_line(const std::string& line);

public:
    // what the page shows for the state (summarize())
    struct Summary {  // what the state says (the Tk launcher's refresh())
        Mode m;
        bool background;  // ready to play, the heavy shaders translating behind
        int shown;        // the percentage
        std::wstring kicker, headline, subline, stages_n;
        uint32_t head_colour;
        uint32_t kicker_colour = LABEL;
        bool kicker_check = false;  // the green check before "Tudo pronto"
        bool ready_card = false;    // the "pronto" card (play first) rather than the progress one
    };

private:
    void paint();
    Summary summarize();
    void paint_header(float width);
    float paint_status(float x, float y, float w, const Summary& s);
    float paint_ready(float x, float y, float w, const Summary& s);
    float paint_progress(float x, float y, float w, const Summary& s);
    float paint_chips(float x, float y, float w);
    float paint_stage_labels(float x, float y, float cw);
    float paint_tiles(float x, float y, float cw, const Summary& s);
    float paint_actions(float x, float y, float cw, const Summary& s);
    float paint_folders(float x, float y, float w);
    float paint_log(float x, float y, float w);
    void paint_drawer(float width, float height);
    float paint_config(float x, float y, float w);
    float paint_monitor(float x, float y, float w);
    void paint_scrollbar(const RECT& cr);
    void paint_splash();
    void open_drawer(Drawer d);
    void set_icons();

    // drawing helpers
    void hit(int id, float x0, float y0, float x1, float y1);
    float card(float x, float y, float w, uint32_t fill, uint32_t edge, float radius, float padx, float pady,
               const std::function<float(float, float, float)>& body);
    // A button; returns its width. `px`: the text's size in CSS pixels.
    float button(int id, float x, float y, const std::wstring& text, Kind kind, float h = 44, float px = 13,
                 Icon icon = Icon::none, bool enabled = true);
    float button_width(const std::wstring& text, Kind kind, float px = 13, Icon icon = Icon::none);
    void icon(Icon i, float x, float y, float size, uint32_t rgb);
    // The settings' switch row (label, hint and the switch at the right); returns its height.
    float switch_row(int id, float x, float y, float w, bool on, const std::wstring& label, const std::wstring& hint);
    // A choice card (quality); returns its height.
    float choice(int id, float x, float y, float w, bool on, const std::wstring& title, const std::wstring& sub);
    float section(float x, float y, const std::wstring& caps, uint32_t rgb = LABEL);
    void segbar(float x, float y, float w, float h);
    void click(int id);
    double overall() const;
    // A stage's state ("idle", "running", "paused", "error", "done") and how far it is, as the bar and its labels show
    // it: this window's preparation, or a translation running outside it (translation.json), whose numbers win.
    std::pair<std::string, double> stage_view(const std::string& key) const;
    int64_t missing_count() const;  // shaders without a translation: failures and stages it doesn't take

    HWND hwnd_ = nullptr;
    ui::Canvas g_;
    std::mutex queue_m_;
    std::deque<std::function<void()>> queue_;

    json::Value s_ = json::Value::object();
    std::wstring game_, out_;
    json::Value version_;
    bool searching_ = false;
    Mode mode_ = Mode::idle;
    std::map<std::string, std::string> state_;
    std::map<std::string, double> frac_;
    std::string current_, step_text_, error_;
    double eta_ = -1;
    json::Value stats_ = json::Value::object();
    json::Value heavy_;  // the heavy stage's progress while it runs
    std::map<std::string, int64_t> machines_;
    std::vector<std::string> helpers_;
    int64_t by_helpers_ = 0;
    json::Value watch_;  // a translation running outside this window (translation.json)

    prep::Preparation* prep_ = nullptr;
    std::thread prep_thread_;
    std::string stopping_;

    HANDLE game_proc_ = nullptr;
    bool game_closing_ = false;
    int64_t pipelines_before_ = 0, pipelines_new_ = 0;  // the game's list before a session, what it added
    std::atomic<HWND> game_child_{nullptr};  // the game's window inside ours, once its first frame is up
    bool fullscreen_ = false;
    WINDOWPLACEMENT saved_place_{};  // its window is gone, the process still handing memory back
    std::atomic<bool> game_running_{false};  // read by the preparation: its tools go idle while the game runs
    bool splash_ = false;
    ULONGLONG splash_t0_ = 0;
    LONG_PTR saved_style_ = 0;

    std::deque<std::pair<std::wstring, std::string>> log_;
    bool log_open_ = false;
    int log_scroll_ = 0;  // lines up from the end

    double out_free_ = -1, out_used_ = 0;
    bool out_measuring_ = false;

    struct Hit {
        int id;
        float x0, y0, x1, y1;
    };
    std::vector<Hit> hits_;
    int hover_ = NONE, pressed_ = NONE;
    float scroll_ = 0, content_h_ = 0, view_h_ = 0;
    float hit_dy_ = 0;  // what hit() adds to y: the page's scroll, or the side panel's
    float hit_top_ = -1e9f;  // hits above this (window y) are cut: the side panel's body under its header
    bool scrollbar_ = false;
    Drawer drawer_ = Drawer::none;
    float drawer_scroll_ = 0, drawer_h_ = 0, drawer_view_h_ = 0, drawer_x_ = 0;
    float log_box_y0_ = 0, log_box_y1_ = 0;  // the log's lines, in window coordinates
    bool dragging_bar_ = false;
    float drag_off_ = 0;
    unsigned phase_ = 0;
    std::map<int, std::function<void()>> actions_;
};

extern App* g_app;

}  // namespace fp::launcher
