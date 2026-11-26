// HLSL waiting for DXC (dxil_cache.Queue): taken by this machine's compilers and by helper PCs; what a helper holds
// is leased, and goes back to the queue if the helper doesn't answer in time.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fp::cache {

struct ShaderJob {
    std::wstring path;   // the disassembly
    std::string name, stage, digest;
    int64_t size = 0;    // of the disassembly: small ones go first
    std::string hlsl;
    uint64_t id = 0;
    std::string machine; // the helper that compiled it ("" = this machine)
};
using JobPtr = std::shared_ptr<ShaderJob>;

class Queue {
public:
    explicit Queue(double lease_s) : lease_(lease_s) {}

    void put(JobPtr job);
    // A job a helper failed: only this machine compiles it again (the helper's DXC may be what's broken).
    void put_local(JobPtr job);
    size_t size();
    // Helpers take from the queue (leased); this machine first what helpers failed; `local_only`: only those.
    JobPtr take(double wait_s, const std::string& helper = "", bool local_only = false);
    // A helper's result: the job it had, or null if it came too late (the job went back to the queue).
    JobPtr give_back(uint64_t id);
    // Helpers seen in the last two minutes or holding a job (the largest shaders keep one busy for minutes).
    std::vector<std::string> active_helpers();
    // What each helper holds: helper -> (jobs, the longest it has held one, s); and the jobs waiting.
    std::pair<std::map<std::string, std::pair<int, int64_t>>, size_t> working();

    void count_machine(const std::string& machine);
    std::map<std::string, int> by_machine();
    int by_helpers();

    void finish();  // the build is over: wakes everyone waiting
    bool over() const { return over_; }

private:
    using Clock = std::chrono::steady_clock;
    struct Lease {
        JobPtr job;
        Clock::time_point deadline;
        std::string helper;
    };
    void expire();  // with the lock held

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<JobPtr> jobs_, local_;
    std::map<uint64_t, Lease> leased_;
    std::map<std::string, Clock::time_point> helpers_;
    std::map<std::string, int> by_machine_;
    double lease_;
    uint64_t next_id_ = 1;
    int by_helpers_ = 0;
    volatile bool over_ = false;
};

}  // namespace fp::cache
