#include "builder.h"

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cache/compile.h"
#include "cache/queue.h"
#include "cache/server.h"
#include "cache/toolchain.h"
#include "core/fs.h"
#include "core/text.h"
#include "fp_tools_id.h"  // generated: a hash of the translator's sources (CMakeLists.txt)
#include "shaders/dxbc.h"
#include "xlat/translator.h"

namespace fp::cache {

const char* tools_id() { return FP_TOOLS_ID; }

namespace {

std::wstring stem(const std::wstring& name) {
    size_t dot = name.rfind(L'.');
    return dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
}

// <digest>.vsin: one line per input, "SEMANTIC index register" — a vertex shader's inputs without system values. The
// translated shader takes input register r as TEXCOORD<r> (the fetch call becomes vertex inputs at location r); the
// runtime pairs the title's input layout (by semantic) with these registers.
void write_vsin(const std::wstring& out, const std::string& digest, const std::wstring& dxbc_path) {
    const std::wstring path = join(out, widen(digest) + L".vsin");
    if (is_file(path)) return;
    std::string c;
    if (!read_file(dxbc_path, c)) return;
    std::string text;
    for (const auto& e : dxbc::input_signature(c, true))
        if (e.system_value == 0)
            text += e.semantic + " " + std::to_string(e.index) + " " + std::to_string(e.reg) + "\n";
    write_file(path, text.data(), text.size());
}

// The reason of a failure kept by this version of the tools, or nothing.
std::optional<std::string> kept_failure(const std::wstring& path) {
    std::string text;
    if (!read_file(path, text)) return std::nullopt;
    const size_t nl = text.find('\n');
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    if (trim(text.substr(0, nl)) != tools_id()) return std::nullopt;
    return trim(nl == std::string::npos ? std::string() : text.substr(nl + 1));
}

// Python's collections.Counter as the build uses it: counts by key, most_common() by count, ties in first-seen order.
struct Counter {
    std::vector<std::pair<std::string, int>> items;
    std::unordered_map<std::string, size_t> where;
    void add(const std::string& k) {
        auto it = where.find(k);
        if (it == where.end()) {
            where[k] = items.size();
            items.emplace_back(k, 1);
        } else {
            ++items[it->second].second;
        }
    }
    int operator[](const std::string& k) const {
        auto it = where.find(k);
        return it == where.end() ? 0 : items[it->second].second;
    }
    std::vector<std::pair<std::string, int>> most_common(size_t n) const {
        auto v = items;
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (v.size() > n) v.resize(n);
        return v;
    }
};

// The priority class a tool starts with: idle while the game runs, else below normal or this process's.
DWORD tool_priority(bool below_normal, const std::atomic<bool>* idle_while) {
    if (idle_while && idle_while->load()) return IDLE_PRIORITY_CLASS;
    return below_normal ? BELOW_NORMAL_PRIORITY_CLASS : 0;
}

// A few threads turning disassembly into HLSL; results are collected by the build's loop.
class Translators {
public:
    Translators(int n, xlat::Context& ctx, const Tools& tools, const std::wstring& keep, Job* job, bool low,
                const std::atomic<bool>* idle_while)
        : ctx_(ctx), tools_(tools), keep_(keep), job_(job), low_(low), idle_while_(idle_while) {
        for (int i = 0; i < n; ++i) threads_.emplace_back([this] { work(); });
    }
    ~Translators() {
        {
            std::lock_guard<std::mutex> l(m_);
            closing_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }
    void submit(JobPtr j) {
        {
            std::lock_guard<std::mutex> l(m_);
            in_.push_back(std::move(j));
            ++pending_;
        }
        cv_.notify_one();
    }
    size_t pending() {
        std::lock_guard<std::mutex> l(m_);
        return pending_;
    }
    // The finished ones, waiting up to `wait_ms` for one.
    std::vector<std::pair<JobPtr, Outcome>> collect(int wait_ms) {
        std::unique_lock<std::mutex> l(m_);
        if (out_.empty() && wait_ms) done_cv_.wait_for(l, std::chrono::milliseconds(wait_ms));
        std::vector<std::pair<JobPtr, Outcome>> r(std::make_move_iterator(out_.begin()),
                                                  std::make_move_iterator(out_.end()));
        out_.clear();
        pending_ -= r.size();
        return r;
    }

private:
    void work() {
        if (low_) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            JobPtr j;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [&] { return closing_ || !in_.empty(); });
                if (closing_) return;
                j = in_.front();
                in_.pop_front();
            }
            Outcome o = to_hlsl(j->path, ctx_, tools_, keep_, job_, tool_priority(low_, idle_while_));
            {
                std::lock_guard<std::mutex> l(m_);
                out_.emplace_back(j, std::move(o));
            }
            done_cv_.notify_one();
        }
    }

    xlat::Context& ctx_;
    const Tools& tools_;
    std::wstring keep_;
    Job* job_;
    bool low_;
    const std::atomic<bool>* idle_while_;
    std::mutex m_;
    std::condition_variable cv_, done_cv_;
    std::deque<JobPtr> in_;
    std::vector<std::pair<JobPtr, Outcome>> out_;
    size_t pending_ = 0;
    bool closing_ = false;
    std::vector<std::thread> threads_;
};

json::Value string_list(const std::vector<std::string>& v) {
    json::Value a = json::Value::array();
    for (const auto& s : v) a.push(s);
    return a;
}

}  // namespace

int build(const BuildOptions& a, const std::atomic<bool>& stop, const Event& event, const Log& log) {
    auto say = [&](const std::string& s) {
        if (log) log(s);
    };
    const std::string opt = "-O" + a.opt;
    make_dirs(a.out);
    if (!a.keep.empty()) make_dirs(a.keep);
    const Tools tools = find_tools();
    if (!a.sidecars_only && !tools.missing().empty()) {
        say("missing from the Vulkan SDK: " + tools.missing());
        return 1;
    }
    std::vector<std::wstring> files;
    for (const auto& f : list_dir(a.disasm))
        if (f.size() > 2 && f.compare(f.size() - 2, 2, L".s") == 0) files.push_back(join(a.disasm, f));
    if (a.limit > 0 && files.size() > static_cast<size_t>(a.limit)) files.resize(static_cast<size_t>(a.limit));

    const size_t n = files.size();
    Progress progress(n, event, log);
    if (!a.sidecars_only) {
        progress.status_path = join(a.out, L"translation.json");
        progress.set_extra("port", a.serve);
    }
    xlat::Context ctx({xlat::Target::d3d12, join(a.out, L"cs")});

    // the index: digest -> {name, stage, bytes}, in the order shaders finished
    std::mutex lock;
    json::Value index = json::Value::object();
    std::unordered_map<std::string, size_t> at_index;
    Counter why;
    Queue queue(a.timeout * 2.0 + 60);

    auto counts = [&](int& ok, int& failed, int& skipped) {  // with `lock` held
        skipped = why["stage not handled"];
        progress.set_extra("deferred", why["deferred"]);
        int sum = 0;
        for (const auto& [k, v] : why.items)
            if (k.compare(0, 2, "  ") != 0) sum += v;
        // Not failures: compute shaders the game hasn't made a pipeline of yet (their registers come from it, see
        // compute_regs in xlat/program.cpp). They are translated by the run after the game meets them.
        const int unmet = why["  translate: compute registers unknown"];
        progress.set_extra("unmet", unmet);
        ok = static_cast<int>(index.members.size());
        failed = sum - skipped - why["deferred"] - unmet;
    };
    auto record = [&](const std::string& name, const std::string& digest, const std::string& stage, int64_t bytes,
                      const std::string& error, bool built = true, bool cached = false) {
        int ok, failed, skipped;
        {
            std::lock_guard<std::mutex> l(lock);
            if (error.empty()) {
                json::Value e = json::Value::object();
                e.set("name", name);
                e.set("stage", stage);
                e.set("bytes", bytes);
                auto it = at_index.find(digest);
                if (it == at_index.end()) {
                    at_index[digest] = index.members.size();
                    index.members.emplace_back(digest, std::move(e));
                } else {
                    index.members[it->second].second = std::move(e);
                }
            } else {
                const size_t colon = error.find(':');
                why.add(colon == std::string::npos ? error : error.substr(0, colon));
                why.add("  " + error);
            }
            counts(ok, failed, skipped);
        }
        progress.step(ok, failed, skipped, built, cached);
    };
    // A compiled (or failed) job, from here or from a helper.
    auto finish = [&](const JobPtr& job, std::optional<std::string> dxil, const std::string& error) {
        const std::wstring d = widen(job->digest);
        if (dxil) {
            const std::wstring part = join(a.out, d + L".dxil." + std::to_wstring(GetCurrentProcessId()) + L"." +
                                                       std::to_wstring(GetCurrentThreadId()) + L".part");
            write_file(part, dxil->data(), dxil->size());
            MoveFileExW(part.c_str(), join(a.out, d + L".dxil").c_str(), MOVEFILE_REPLACE_EXISTING);
            queue.count_machine(job->machine);
            record(job->name, job->digest, job->stage, static_cast<int64_t>(dxil->size()), "");
        } else {
            const std::string text = std::string(tools_id()) + "\n" + error;
            write_file(join(a.out, d + L".fail"), text.data(), text.size());
            record(job->name, "", "", 0, error);
        }
    };

    // what's done already, and what's left: stage and digest are cheap, read here
    std::vector<JobPtr> todo;
    for (const auto& s_path : files) {
        if (stop) return 1;
        const std::wstring wname = stem(file_name(s_path));
        const std::string name = narrow(wname);
        const std::string stage = ctx.stage_of(name, s_path);
        if (!*profile(stage)) {
            record(name, "", "", 0, "stage not handled: " + stage, false);
            continue;
        }
        const std::wstring dxbc_path = join(a.dxbc, wname + L".dxbc");
        std::string container;
        std::optional<std::string> digest;
        if (read_file(dxbc_path, container)) digest = dxbc::hash_digest(container);
        if (!digest) {
            record(name, "", "", 0, "no HASH digest", false);
            continue;
        }
        if (stage == "vs") write_vsin(a.out, *digest, dxbc_path);
        if (a.sidecars_only) continue;
        const std::wstring dxil = join(a.out, widen(*digest) + L".dxil");
        if (is_file(dxil)) {
            record(name, *digest, stage, file_size(dxil), "", false, true);
            continue;
        }
        auto reason = kept_failure(join(a.out, widen(*digest) + L".fail"));
        if (reason && reason->find("compute registers unknown") != std::string::npos &&
            is_file(join(join(a.out, L"cs"), widen(*digest) + L".regs")))
            reason.reset();  // the runtime has seen its pipeline since: its registers are known now
        if (reason) {
            record(name, "", "", 0, *reason, false);
            continue;
        }
        const int64_t size = file_size(s_path);
        if (a.defer_big && size > static_cast<int64_t>(a.big_kb) * 1024) {
            record(name, "", "", 0, "deferred", false);
            continue;
        }
        auto job = std::make_shared<ShaderJob>();
        job->path = s_path;
        job->name = name;
        job->stage = stage;
        job->digest = *digest;
        job->size = size;
        todo.push_back(std::move(job));
    }
    // small first: the port is usable long before the largest are done
    std::stable_sort(todo.begin(), todo.end(), [](const JobPtr& x, const JobPtr& y) { return x->size < y->size; });
    if (a.sidecars_only) {
        progress.done();
        say("vertex-shader input files written");
        return 0;
    }

    Server server;
    if (a.serve && !todo.empty()) {
        if (a.token.empty()) {
            say("--serve needs a --token (FLAMINGO_TOKEN)");
            return 2;
        }
        if (server.start(queue, a.serve, a.token, opt, a.timeout,
                         [&](const JobPtr& job, std::string dxil) { finish(job, std::move(dxil), ""); }))
            say("helpers welcome on port " + std::to_string(a.serve) + " (local network)");
        else
            say("port " + std::to_string(a.serve) + " is taken: no helpers this time");
    }

    // local compilers: threads (DXC runs as its own process)
    const int local_dxc = a.local_dxc < 0 ? a.jobs : a.local_dxc;
    std::atomic<int64_t> helpers_seen{static_cast<int64_t>(GetTickCount64())};
    auto compiler = [&] {
        if (a.below_normal) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            bool only_retries = false;
            if (local_dxc == 0) {
                if (!queue.active_helpers().empty()) helpers_seen = static_cast<int64_t>(GetTickCount64());
                only_retries = static_cast<int64_t>(GetTickCount64()) - helpers_seen < 60000;
            }
            JobPtr job = queue.take(1.0, "", only_retries);
            if (!job) {
                if (queue.over()) return;
                continue;
            }
            Outcome r = run_dxc(job->hlsl, profile(job->stage), opt, a.timeout, tools, a.job,
                                tool_priority(a.below_normal, a.idle_while));
            if (stop) return;
            finish(job, std::move(r.data), r.why);
        }
    };
    std::vector<std::thread> compilers;
    const int ncomp = local_dxc ? std::max(1, local_dxc) : 2;
    if (local_dxc == 0) say("this machine translates; helpers compile");
    for (int i = 0; i < ncomp; ++i) compilers.emplace_back(compiler);

    // translation feeds the queue, kept a little ahead of the compilers (HLSL is held in memory)
    const size_t ahead = static_cast<size_t>(std::max(16, 4 * a.jobs));
    {
        Translators pool(a.jobs, ctx, tools, a.keep, a.job, a.below_normal, a.idle_while);
        size_t next = 0;
        std::vector<std::string> last_helpers;
        bool first_round = true;
        while (!stop) {
            const auto helpers = queue.active_helpers();
            while (next < todo.size() && pool.pending() + queue.size() < ahead + 2 * helpers.size() * 8)
                pool.submit(todo[next++]);
            auto done = pool.collect(50);
            for (auto& [job, o] : done) {
                if (!o.data) {
                    finish(job, std::nullopt, o.why);
                } else {
                    job->hlsl = std::move(*o.data);
                    queue.put(job);
                }
            }
            if (helpers != last_helpers || first_round) {
                if (helpers != last_helpers && !helpers.empty()) {
                    std::string list;
                    for (const auto& h : helpers) list += (list.empty() ? "" : ", ") + h;
                    say("helpers: " + list);
                }
                last_helpers = helpers;
                progress.set_extra("helpers", string_list(helpers));
                first_round = false;
            }
            progress.set_extra("by_helpers", queue.by_helpers());
            json::Value machines = json::Value::object();
            for (const auto& [m, c] : queue.by_machine()) machines.set(m, c);
            progress.set_extra("machines", machines);
            auto [held, waiting] = queue.working();
            json::Value working = json::Value::object();
            for (const auto& [h, v] : held) {
                json::Value pair = json::Value::array();
                pair.push(v.first);
                pair.push(v.second);
                working.set(h, pair);
            }
            progress.set_extra("working", working);
            progress.set_extra("waiting", static_cast<int64_t>(waiting));
            progress.refresh_status();  // the status file stays fresh while nothing finishes
            if (next >= todo.size() && pool.pending() == 0 && queue.size() == 0) break;
        }
    }
    queue.finish();
    for (auto& t : compilers) t.join();
    if (a.serve) {
        Sleep(1000);  // helpers waiting on a job hear "over"
        server.stop();
    }
    progress.done();
    json::save_file(join(a.out, L"index.json"), index, 0);
    if (stop) return 1;
    const int by_helpers = queue.by_helpers();
    say(std::to_string(n) + " shaders: " + std::to_string(index.members.size()) + " in DXIL" +
        (by_helpers ? " (" + std::to_string(by_helpers) + " by helpers)" : ""));
    for (const auto& [k, c] : why.most_common(30)) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "  %6d  ", c);
        say(buf + k);
    }
    return 0;
}

}  // namespace fp::cache
