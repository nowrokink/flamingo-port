// The shader build's progress (dxil_cache.Progress): percentage, counts, rate and time left, as events for the
// launcher (the "progress" events the Python sent as "@@ui" lines), a line every 1% for a console, and
// <cache>/translation.json about once a second, for a launcher that didn't start this build (it shows the progress and
// the helpers of a build started from a console or by another tool). The rate counts only shaders built in this run:
// those already in the cache are done at once and would make the time left look short.
#pragma once
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "core/json.h"

namespace fp::cache {

using Event = std::function<void(const json::Value&)>;
using Log = std::function<void(const std::string& line)>;

class Progress {
public:
    Progress(size_t total, Event event, Log log);

    std::wstring status_path;  // translation.json ("" = none)

    // Extra members sent with every event and status (helpers, machines, deferred, port, ...).
    void set_extra(const std::string& key, json::Value v);

    // One shader done (force: no shader, just send the current numbers).
    void step(int ok, int failed, int skipped, bool built = true, bool cached = false, bool force = false);
    // translation.json now, with the last counts, if a second has gone by since the last write.
    void refresh_status();
    void done();

private:
    void write_status(bool finished);   // with the lock held
    std::pair<double, std::optional<double>> left() const;
    json::Value base() const;

    std::mutex lock_;
    size_t total_, k_ = 0, built_ = 0, cached_ = 0;
    int ok_ = 0, failed_ = 0, skipped_ = 0;
    std::chrono::steady_clock::time_point t0_;
    double started_;  // seconds since 1970
    std::chrono::steady_clock::time_point sent_{}, status_sent_{};
    int64_t last_pct_ = -1;
    json::Value extra_ = json::Value::object();
    Event event_;
    Log log_;
};

double unix_time();

}  // namespace fp::cache
