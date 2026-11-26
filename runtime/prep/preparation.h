// Preparing the port from the user's own copy of the game (what make_game.py did): check the copy, carve, unpack and
// disassemble the shaders, then the translator (xlat/, cache/) fills the DXIL cache the runtime reads. Progress goes
// out as events ({"event": "stage" | "step" | "progress" | ...}) the launcher shows.
//
// preparation.cpp: running the stages, the preflight and the cache builds; stages.cpp: the stages.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/process.h"

namespace fp::prep {

struct Options {
    std::wstring game, out, work;
    json::Value version;
    bool full_check = false;
    bool helpers = true, local_compile = true;
    int helper_port = 8765;
    std::string helper_token;
    int jobs = 1;
    bool no_audio = false;  // assets: leave the audio banks out (27 GB)
    const std::atomic<bool>* game_running = nullptr;  // the launcher's: the tools go idle while it's true
};

using Event = std::function<void(const json::Value&)>;   // {"event": "stage" | "stage_done" | "step" | ..., ...}
using Log = std::function<void(const std::string& line)>;  // a line for the log, "HH:MM:SS  text" or plain

// The stages, in order: "check", "shaders", "pipelines", "heavy" (and "assets", on its own). Runs on the calling thread; true when all finished. stop()
// from another thread ends it (the translator's processes too); what is done stays done.
class Preparation {
public:
    bool run(const Options& o, const std::vector<std::string>& stages, const Event& event, const Log& log);
    void stop();
    bool stopped() const { return stop_; }

private:
    static json::Value event(const char* kind, const char* stage = nullptr);
    bool fail(const std::string& msg);
    bool check(const Options& o);
    bool preflight(const Options& o, const std::vector<std::string>& stages);
    bool shaders(const Options& o);
    bool heavy(const Options& o);
    bool pipelines(const Options& o);  // stages.cpp
    bool assets(const Options& o);  // assets.cpp
    // The DXIL cache build (cache/builder.h) or, with `pending`, the shaders the runtime saved at run time.
    bool translate(const Options& o, const std::wstring& disasm, const std::wstring& dxbc, const std::wstring& cache,
                   bool defer_big, bool pending = false);
    void note(const std::string& text);

    std::atomic<bool> stop_{false};
    Job job_;
    Event event_;
    Log log_;
    std::wstring path_;
};

}  // namespace fp::prep
