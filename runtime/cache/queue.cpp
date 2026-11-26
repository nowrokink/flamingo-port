#include "queue.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace fp::cache {

void Queue::put(JobPtr job) {
    std::lock_guard<std::mutex> l(m_);
    job->id = next_id_++;
    jobs_.push_back(std::move(job));
    cv_.notify_one();
}

void Queue::put_local(JobPtr job) {
    std::lock_guard<std::mutex> l(m_);
    local_.push_back(std::move(job));
    cv_.notify_all();
}

size_t Queue::size() {
    std::lock_guard<std::mutex> l(m_);
    return jobs_.size() + leased_.size() + local_.size();
}

void Queue::expire() {
    const auto now = Clock::now();
    for (auto it = leased_.begin(); it != leased_.end();) {
        if (it->second.deadline < now) {
            jobs_.push_front(it->second.job);
            it = leased_.erase(it);
        } else {
            ++it;
        }
    }
}

JobPtr Queue::take(double wait_s, const std::string& helper, bool local_only) {
    std::unique_lock<std::mutex> l(m_);
    expire();
    auto from_local = [&]() -> JobPtr {
        JobPtr j = local_.front();
        local_.pop_front();
        return j;
    };
    if (helper.empty() && !local_.empty()) return from_local();
    if (jobs_.empty() && wait_s > 0) {
        cv_.wait_for(l, std::chrono::duration<double>(wait_s));
        expire();
    }
    if (helper.empty() && !local_.empty()) return from_local();
    if (jobs_.empty() || local_only) return nullptr;
    JobPtr job = jobs_.front();
    jobs_.pop_front();
    if (!helper.empty()) {
        leased_[job->id] = {job,
                            Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                               std::chrono::duration<double>(lease_)),
                            helper};
        helpers_[helper] = Clock::now();
    }
    return job;
}

JobPtr Queue::give_back(uint64_t id) {
    std::lock_guard<std::mutex> l(m_);
    auto it = leased_.find(id);
    if (it == leased_.end()) return nullptr;
    JobPtr job = it->second.job;
    job->machine = it->second.helper;
    leased_.erase(it);
    ++by_helpers_;
    return job;
}

std::vector<std::string> Queue::active_helpers() {
    std::lock_guard<std::mutex> l(m_);
    const auto now = Clock::now();
    std::set<std::string> busy;
    for (const auto& [_, lease] : leased_) busy.insert(lease.helper);
    std::vector<std::string> out;
    for (const auto& [h, t] : helpers_)
        if (now - t < std::chrono::seconds(120) || busy.count(h)) out.push_back(h);
    return out;  // std::map order = sorted()
}

std::pair<std::map<std::string, std::pair<int, int64_t>>, size_t> Queue::working() {
    std::lock_guard<std::mutex> l(m_);
    const auto now = Clock::now();
    std::map<std::string, std::pair<int, int64_t>> held;
    for (const auto& [_, lease] : leased_) {
        auto& [n, longest] = held[lease.helper];
        const double since_taken =
            std::chrono::duration<double>(now - lease.deadline).count() + lease_;  // now - (deadline - lease)
        n += 1;
        longest = std::max<int64_t>(longest, static_cast<int64_t>(std::llround(since_taken)));
    }
    return {held, jobs_.size()};
}

void Queue::count_machine(const std::string& machine) {
    std::lock_guard<std::mutex> l(m_);
    ++by_machine_[machine];
}

std::map<std::string, int> Queue::by_machine() {
    std::lock_guard<std::mutex> l(m_);
    return by_machine_;
}

int Queue::by_helpers() {
    std::lock_guard<std::mutex> l(m_);
    return by_helpers_;
}

void Queue::finish() {
    std::lock_guard<std::mutex> l(m_);
    over_ = true;
    cv_.notify_all();
}

}  // namespace fp::cache
