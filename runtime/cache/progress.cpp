#include "progress.h"

#include <windows.h>

#include <cmath>
#include <cstdio>

namespace fp::cache {

double unix_time() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<double>(t - 116444736000000000ull) / 1e7;
}

Progress::Progress(size_t total, Event event, Log log)
    : total_(total), t0_(std::chrono::steady_clock::now()), started_(unix_time()), event_(std::move(event)),
      log_(std::move(log)) {}

void Progress::set_extra(const std::string& key, json::Value v) {
    std::lock_guard<std::mutex> l(lock_);
    extra_.set(key, std::move(v));
}

std::pair<double, std::optional<double>> Progress::left() const {
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    const double rate = dt > 0 && built_ ? static_cast<double>(built_) / dt : 0;
    if (!rate) return {0, std::nullopt};
    return {rate, static_cast<double>(total_ - k_) / rate};
}

namespace {
void merge(json::Value& into, const json::Value& extra) {
    for (const auto& [k, v] : extra.members) into.set(k, v);
}
}  // namespace

void Progress::write_status(bool finished) {
    if (status_path.empty()) return;
    auto [rate, eta] = left();
    json::Value s = json::Value::object();
    s.set("pid", static_cast<int64_t>(GetCurrentProcessId()));
    s.set("time", unix_time());
    s.set("started", started_);
    s.set("done", static_cast<int64_t>(k_));
    s.set("total", static_cast<int64_t>(total_));
    s.set("ok", ok_);
    s.set("failed", failed_);
    s.set("skipped", skipped_);
    s.set("cached", static_cast<int64_t>(cached_));
    s.set("rate", std::round(rate * 100) / 100);
    s.set("eta", eta ? json::Value(std::round(*eta)) : json::Value());
    s.set("finished", finished);
    merge(s, extra_);
    json::save_file(status_path, s, -1);  // written to a .tmp and moved over, so a reader never sees half of it
    status_sent_ = std::chrono::steady_clock::now();
}

void Progress::step(int ok, int failed, int skipped, bool built, bool cached, bool force) {
    std::lock_guard<std::mutex> l(lock_);
    if (!force) {
        ++k_;
        built_ += built;
        cached_ += cached;
    }
    ok_ = ok;
    failed_ = failed;
    skipped_ = skipped;
    const int64_t pct = static_cast<int64_t>(100 * k_ / std::max<size_t>(1, total_));
    const auto now = std::chrono::steady_clock::now();
    if (force || now - status_sent_ >= std::chrono::seconds(1) || k_ == total_) write_status(false);
    if (event_) {
        if (force || now - sent_ >= std::chrono::milliseconds(500) || k_ == total_) {
            sent_ = now;
            auto [rate, eta] = left();
            json::Value e = json::Value::object();
            e.set("event", "progress");
            e.set("stage", "shaders");
            e.set("done", static_cast<int64_t>(k_));
            e.set("total", static_cast<int64_t>(total_));
            e.set("ok", ok);
            e.set("failed", failed);
            e.set("skipped", skipped);
            e.set("cached", static_cast<int64_t>(cached_));
            e.set("eta", eta ? json::Value(std::round(*eta)) : json::Value());
            merge(e, extra_);
            event_(e);
        }
    } else if (log_ && pct != last_pct_) {
        auto [rate, eta] = left();
        const double secs = eta.value_or(0);
        char bar[26] = {};
        for (int i = 0; i < 25; ++i) bar[i] = i < pct / 4 ? '#' : '-';
        char line[256];
        std::snprintf(line, sizeof line, "[%s] %5.1f%%  %zu/%zu  DXIL %d  falhas %d  %.1f/s  restante %dh%02dm", bar,
                      100.0 * static_cast<double>(k_) / static_cast<double>(std::max<size_t>(1, total_)), k_, total_,
                      ok, failed, rate, static_cast<int>(secs / 3600), static_cast<int>(std::fmod(secs, 3600) / 60));
        log_(line);
    }
    last_pct_ = pct;
}

void Progress::refresh_status() {
    std::lock_guard<std::mutex> l(lock_);
    if (std::chrono::steady_clock::now() - status_sent_ >= std::chrono::seconds(1)) write_status(false);
}

void Progress::done() {
    std::lock_guard<std::mutex> l(lock_);
    write_status(true);
}

}  // namespace fp::cache
