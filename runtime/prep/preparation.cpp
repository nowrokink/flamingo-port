#include "prep/preparation.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

#include "cache/builder.h"
#include "cache/pending.h"
#include "core/fs.h"
#include "core/system.h"
#include "core/text.h"
#include "prep/toolchain.h"

namespace fp::prep {

json::Value Preparation::event(const char* kind, const char* stage) {
    json::Value e = json::Value::object();
    e.set("event", kind);
    if (stage) e.set("stage", stage);
    return e;
}

void Preparation::stop() {
    stop_ = true;
    job_.kill();
}

void Preparation::note(const std::string& text) { log_(narrow(timestamp()) + "  " + text); }

bool Preparation::fail(const std::string& msg) {
    json::Value e = event("error");
    e.set("text", msg);
    event_(e);
    log_("");
    log_("ERROR: " + msg);
    return false;
}

bool Preparation::run(const Options& o, const std::vector<std::string>& stages, const Event& ev, const Log& log) {
    event_ = ev;
    log_ = log;
    path_ = tool_path();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // keep the PC usable
    auto has = [&](const char* s) { return std::find(stages.begin(), stages.end(), s) != stages.end(); };
    if (has("check")) {
        event_(event("stage", "check"));
        if (!check(o)) return false;
        event_(event("stage_done", "check"));
    }
    for (const std::wstring& p : {o.out, o.work})
        if (inside(p, o.game)) return fail("A pasta do port e a de trabalho precisam ficar fora da pasta do jogo.");
    if (!preflight(o, stages)) return false;
    note("building into " + narrow(o.out) + " (work files in " + narrow(o.work) + ")");
    const ULONGLONG t0 = GetTickCount64();
    for (const auto& s : stages) {
        if (s == "check") continue;
        event_(event("stage", s.c_str()));
        bool ok = s == "shaders"     ? shaders(o)
                  : s == "pipelines" ? pipelines(o)
                  : s == "heavy"     ? heavy(o)
                  : s == "assets"    ? assets(o)
                                     : fail("unknown stage " + s);
        if (!ok) return false;
        event_(event("stage_done", s.c_str()));
    }
    event_(event("finished"));
    note("finished in " + std::to_string((GetTickCount64() - t0) / 60000) + " min");
    return true;
}

bool Preparation::preflight(const Options& o, const std::vector<std::string>& stages) {
    const bool shader_work = std::find(stages.begin(), stages.end(), "shaders") != stages.end() ||
                             std::find(stages.begin(), stages.end(), "heavy") != stages.end();
    const std::wstring disasm = join(o.work, L"shaders\\dxbc\\disasm");
    std::vector<std::string> missing;
    if (shader_work)
        missing = missing_shader_tools(path_, is_dir(disasm) && count_dir(disasm, 1000) >= 1000);
    const bool asset_work = std::find(stages.begin(), stages.end(), "assets") != stages.end();
    if (asset_work)
        for (const auto& m : missing_asset_tools(path_, repo_root())) missing.push_back(m);
    if (!missing.empty()) {
        std::string text = "Instale antes estes programas e prepare de novo:";
        for (const auto& m : missing) text += "\n  - " + m;
        return fail(text);
    }
    // Rough sizes of what the stages write in the work folder: the carve and disassembly about 20 GB; the assets an
    // estimate of 220 GB, 27 of them the audio banks (copied out of audio.rpf as they are). The assets stage also
    // checks before every export, so a full disk stops it cleanly and the next run continues.
    int need = (shader_work && !is_dir(disasm)) ? 20 : 0;
    if (asset_work) need += o.no_audio ? 220 - 27 : 220;
    if (need) {
        make_dirs(o.work);
        double free = free_gb(o.work);
        if (free >= 0 && free < need) {
            char buf[200];
            std::snprintf(buf, sizeof buf, "%.0f GB livres no disco de ", free);
            return fail(buf + narrow(o.work) + "; são precisos uns " + std::to_string(need) +
                        " GB. Libere espaço ou escolha outra pasta de trabalho.");
        }
    }
    return true;
}

bool Preparation::translate(const Options& o, const std::wstring& disasm, const std::wstring& dxbc,
                            const std::wstring& cache, bool defer_big, bool pending) {
    if (stop_) return false;
    cache::BuildOptions b;
    b.disasm = disasm;
    b.dxbc = dxbc;
    b.out = cache;
    b.jobs = o.jobs;
    b.defer_big = defer_big;
    b.job = &job_;
    b.below_normal = true;
    b.idle_while = o.game_running;  // keep the PC usable, as the Python tools ran
    if (o.helpers) {
        b.serve = o.helper_port;
        b.token = o.helper_token;
        if (!o.local_compile) b.local_dxc = 0;
    }
    const int code = pending ? cache::translate_pending(cache, b, stop_, event_, log_)
                             : cache::build(b, stop_, event_, log_);
    if (stop_) return false;
    if (code != 0) return fail("A tradução dos shaders parou (código " + std::to_string(code) + "). Veja o registro.");
    return true;
}

}  // namespace fp::prep
