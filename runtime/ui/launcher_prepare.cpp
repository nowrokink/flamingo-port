// Preparing from the launcher: the preparation on a thread of its own, its events and log lines brought to the
// window, the end of it, and a translation started outside the launcher (its status file).
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "core/fs.h"
#include "core/text.h"
#include "ui/app.h"

namespace fp::launcher {

// ---------------------------------------------------------------- preparing

int64_t App::pipeline_lines() const {
    std::string text;
    if (!read_file(join(out_, L"shaders\\pipelines.txt"), text)) return 0;
    return static_cast<int64_t>(std::count(text.begin(), text.end(), '\n'));
}

void App::prepare(std::vector<std::string> stages) {
    if (prep_) return;
    if (game_proc_) {
        MessageBoxW(hwnd_, L"Feche o jogo antes de preparar.", TITLE, MB_ICONINFORMATION);
        return;
    }
    if (stages.empty()) stages = {"check", "shaders", "pipelines", HEAVY};
    if (version_.is_null()) {
        MessageBoxW(hwnd_, L"Escolha antes a pasta da sua cópia do jogo.", TITLE, MB_ICONINFORMATION);
        return;
    }
    prep::Options o;
    o.game_running = &game_running_;
    o.game = game_;
    o.out = out_;
    o.work = work();
    o.version = version_;
    o.full_check = flag("full_check", false);
    o.helpers = flag("helpers", true);
    o.local_compile = flag("local_compile", true);
    o.helper_port = static_cast<int>(s_["helper_port"].integer(8765));
    o.helper_token = helper_token();
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    o.jobs = std::max<int>(1, static_cast<int>(si.dwNumberOfProcessors) / 2);
    for (const auto& st : kStages)
        if (std::find(stages.begin(), stages.end(), st.key) != stages.end()) {
            state_[st.key] = "idle";
            frac_[st.key] = 0;
        }
    current_.clear();
    step_text_.clear();
    error_.clear();
    eta_ = -1;
    mode_ = Mode::running;
    helpers_.clear();
    by_helpers_ = 0;
    machines_.clear();
    heavy_ = json::Value();
    add_log(timestamp(), "$ preparar --game \"" + narrow(game_) + "\" --out \"" + narrow(out_) + "\"");
    prep_ = new prep::Preparation();
    prep::Preparation* p = prep_;
    if (prep_thread_.joinable()) prep_thread_.join();
    prep_thread_ = std::thread([this, p, o, stages] {
        bool ok = p->run(o, stages, [this](const json::Value& e) { post([this, e] { handle(e); }); },
                         [this](const std::string& line) { post([this, line] { log_line(line); }); });
        post([this, ok] { finished(ok); });
    });
}

void App::stop_preparation(const char* how) {
    if (!prep_) return;
    if (!strcmp(how, "cancel") &&
        MessageBoxW(hwnd_, L"Cancelar a preparação?\n\nO que já foi feito fica guardado: da próxima vez ela continua "
                           L"de onde parou.",
                    TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    stopping_ = how;
    prep_->stop();
}

void App::handle(const json::Value& e) {
    const std::string kind = e["event"].str();
    std::string st = e["stage"].str();
    if (st == "shaders" && current_ == HEAVY) st = HEAVY;  // the translator reports as "shaders" in either stage
    if (kind == "stage" && st == HEAVY) {
        current_ = HEAVY;
        heavy_ = json::Value::object();
        eta_ = -1;
        return;
    }
    if (kind == "progress" && st == HEAVY) {
        heavy_ = json::Value::object();
        for (const char* k : {"done", "total", "ok", "deferred"}) heavy_.set(k, e[k].integer());
        eta_ = e["eta"].num(-1);
        take_machines(e);
        return;
    }
    if (kind == "stage_done" && st == HEAVY) heavy_ = json::Value();
    if (kind == "stage" && state_.count(st)) {
        current_ = st;
        state_[st] = "running";
        frac_[st] = 0;
        eta_ = -1;
        step_text_.clear();
    } else if (kind == "stage_done" && state_.count(st)) {
        state_[st] = "done";
        frac_[st] = 1;
        if (st == "pipelines") pipelines_new_ = 0;
    } else if (kind == "step") {
        step_text_ = e["text"].str();
    } else if (kind == "progress" && frac_.count(st)) {
        frac_[st] = e["done"].num() / std::max(1.0, e["total"].num(1));
        if (st == "shaders" && e.has("ok")) {
            eta_ = e["eta"].num(-1);
            take_machines(e);
            stats_ = json::Value::object();
            for (const char* k : {"done", "total", "ok", "failed", "skipped", "cached", "deferred", "unmet"})
                stats_.set(k, e[k].integer());
        } else if (!e["text"].str().empty()) {
            step_text_ = e["text"].str() + " · " + format_thousands(e["done"].integer()) + " de " +
                         format_thousands(e["total"].integer());
        }
    } else if (kind == "error") {
        error_ = e["text"].str();
    } else if (kind == "finished") {
        mode_ = Mode::ready;
    }
}

void App::finished(bool ok) {
    if (prep_thread_.joinable()) prep_thread_.join();
    delete prep_;
    prep_ = nullptr;
    std::string how = stopping_;
    stopping_.clear();
    if (how == "pause") {
        mode_ = Mode::paused;
        if (!current_.empty() && state_.count(current_)) state_[current_] = "paused";
    } else if (how == "cancel") {
        mode_ = port_ready() ? Mode::ready : Mode::idle;
        for (auto& [k, v] : state_)
            if (v != "done") {
                v = "idle";
                frac_[k] = 0;
            }
    } else if (!ok) {
        mode_ = Mode::error;
        if (!current_.empty() && state_.count(current_)) state_[current_] = "error";
        if (error_.empty()) error_ = "A preparação parou. Veja o registro.";
        log_open_ = true;
    } else {
        mode_ = port_ready() ? Mode::ready : Mode::idle;
    }
    if (current_ == HEAVY) heavy_ = json::Value();
    remember();
    show_out();
}

void App::take_machines(const json::Value& e) {
    if (e.has("helpers")) {
        helpers_.clear();
        for (const auto& h : e["helpers"].items) helpers_.push_back(h.str());
    }
    by_helpers_ = e["by_helpers"].integer();
    for (const auto& [k, v] : e["machines"].members) machines_[k] = v.integer();
}

// Once a second: is a translation running that this window didn't start? Its status file says so while it is fresh
// (tools/dxil_cache.py rewrites it about every second, with the time).
void App::poll_status() {
    const bool was = !watch_.is_null();
    json::Value st;
    if (!prep_) {
        if (json::load_file(join(out_, L"shaders\\translation.json"), st)) {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            double now = ((static_cast<uint64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 1e7) - 11644473600.0;
            if (st["finished"].truthy() || now - st["time"].num() > 8) st = json::Value();
        } else {
            st = json::Value();
        }
    }
    watch_ = st;
    if (!st.is_null()) {
        helpers_.clear();
        for (const auto& h : st["helpers"].items) helpers_.push_back(h.str());
        by_helpers_ = st["by_helpers"].integer();
        machines_.clear();
        for (const auto& [k, v] : st["machines"].members) machines_[k] = v.integer();
        stats_ = json::Value::object();
        for (const char* k : {"done", "total", "ok", "failed", "skipped", "cached", "deferred", "unmet"})
            stats_.set(k, st[k].integer());
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (was) {  // it ended (or this window started its own)
        helpers_.clear();
        machines_.clear();
        remember();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::explain_missing() {
    if (stats_.members.empty()) return;
    std::wstring text = num(stats_["skipped"].integer()) +
                        L" são geometry shaders, um estágio que o tradutor não trata; os draws que dependem deles "
                        L"não aparecem por enquanto.\n\n" +
                        num(stats_["failed"].integer()) +
                        L" falharam na tradução (instruções que o tradutor ainda não conhece, ou o DXC recusou).\n\n";
    if (const int64_t unmet = stats_["unmet"].integer())
        text += num(unmet) + L" shaders de compute aguardam o jogo (não são falhas): os registradores deles vêm do "
                             L"pipeline que o jogo cria, e eles são traduzidos na preparação seguinte à passagem do "
                             L"jogo por eles.\n\n";
    text += L"A lista com o motivo de cada um sai no fim do registro, quando a etapa termina.";
    MessageBoxW(hwnd_, text.c_str(), TITLE, MB_ICONINFORMATION);
}

const json::Value& App::watching() const {
    static const json::Value none;
    return prep_ ? none : watch_;
}

// ---------------------------------------------------------------- log

void App::add_log(const std::wstring& stamp, const std::string& text) {
    log_.emplace_back(stamp, text);
    if (log_.size() > 4000) log_.erase(log_.begin(), log_.begin() + 1000);
    if (log_scroll_) ++log_scroll_;  // keep the lines being read in place
}

void App::log_line(const std::string& line) {
    // the engine's own lines start with HH:MM:SS and two spaces
    if (line.size() > 10 && line[2] == ':' && line[5] == ':' && line[8] == ' ' && line[9] == ' ')
        add_log(w(line.substr(0, 8)), line.substr(10));
    else if (line.find_first_not_of(" \t") != std::string::npos)
        add_log(timestamp(), line);
}

}  // namespace fp::launcher
