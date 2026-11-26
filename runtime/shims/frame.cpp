// The console's frame clock. A console title doesn't wait on a swap chain: it sets a frame interval
// (SetFrameIntervalX, e.g. 33 333 us), schedules frame events at an offset into each interval (ScheduleFrameEventX:
// type 0 is the frame's origin, 1 and 2 further points of the frame) and blocks on them (WaitFrameEventX, which hands
// out a frame token at each origin). PresentX names the token of the frame it shows; GetFrameStatisticsX reports
// when that frame's origin and flip happened.
//
// Device slots, the device first:
//   78 SetFrameIntervalX(output, length us, periodicity, flags) -> S_FALSE; E_INVALIDARG for a length the display
//      can't run (8 333 / 25 000 us only at 120 Hz); length 0 stops the clock
//   79 ScheduleFrameEventX(type, offset us, ancillary, flags 1 = unschedule) -> S_FALSE, or 0x10000000 when the call
//      starts the clock (origin only)
//   80 WaitFrameEventX(type, timeout ms, ancillary, flags 1 = wait for all / 2 = token on timeout, token in/out)
//      -> S_OK, S_FALSE on timeout
//   81 GetFrameStatisticsX(token, type, count in/out, stats { u32 type; ...; i64 origin @0x10; i64 flip @0x18 })
// The ancillary block is { u32 count; HANDLE* list @8; i32* signaled @0x10 }: handles the clock signals with the
// event (schedule) or that a wait also wakes on (wait); `signaled` gets what woke a wait (-2 the frame event, an
// index into the list, -1 a timeout). Semantics as XCL documents them from its titles (see docs/design.md).
//
// The clock runs on a thread of its own: a high-resolution waitable timer, the last 0.6 ms spun, not locked to the
// monitor's vblank. At each deadline every event type due fires: the origin releases a semaphore (so a wait takes
// the origin that woke it, frames in flight each keep their own), the others set an auto-reset event, and the
// title's ancillary handles are signalled (duplicates, so the title closing one can't make us signal another).
#include "frame.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

namespace rt::frame {
namespace {

constexpr HRESULT kStartedClock = 0x10000000;
constexpr HRESULT kFail = static_cast<HRESULT>(0x80004005);
constexpr HRESULT kInvalidArg = static_cast<HRESULT>(0x80070057);
constexpr int kTypes = 3;
constexpr LONG kOriginSemaphoreMax = 4;
constexpr uint32_t kWaitAll = 1, kTokenOnTimeout = 2, kUnschedule = 1;
constexpr uint64_t kTokenMask = 0xFFFFFFFFFFFFull;
constexpr size_t kKeep = 64;  // issued and flipped tokens remembered

int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
int64_t qpf() {
    static int64_t f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x.QuadPart;
    }();
    return f;
}
int64_t us_to_ticks(int64_t us) { return us * qpf() / 1000000; }

// Duplicated title handles; closed when the last holder (the schedule or the clock thread mid-signal) lets go.
struct Handles {
    std::vector<HANDLE> list;
    ~Handles() {
        for (HANDLE h : list) CloseHandle(h);
    }
};

void signal(HANDLE h) {
    if (!SetEvent(h)) ReleaseSemaphore(h, 1, nullptr);  // the title may pass events or semaphores
}

struct Kind {
    bool scheduled = false;
    uint32_t offset_us = 0;
    HANDLE internal = nullptr;  // origin: semaphore; others: auto-reset event
    std::shared_ptr<Handles> extra = std::make_shared<Handles>();
};

struct Clock {
    std::mutex m;
    uint32_t interval_us = 0;
    bool active = false;
    int64_t base = 0;  // qpc the deadlines count from
    uint64_t generation = 0;
    Kind kinds[kTypes];
    HANDLE wake = nullptr;
    std::deque<int64_t> origins;                        // released, not yet taken by a wait
    std::deque<std::pair<uint64_t, int64_t>> issued;    // token -> its frame's origin
    std::deque<std::tuple<uint64_t, int64_t, int64_t>> flipped;  // token, origin, flip
    uint64_t next_token = 1;
    int64_t last_origin = 0;
    bool thread_started = false;

    Clock() {
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        kinds[0].internal = CreateSemaphoreW(nullptr, 0, kOriginSemaphoreMax, nullptr);
        for (int i = 1; i < kTypes; ++i) kinds[i].internal = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
};

Clock& clock() {
    static Clock c;
    return c;
}

bool high_refresh() {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    return EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 119;
}

// Lengths the console accepts, with the rounding titles use (16 666 and 16 667 are both 60 Hz).
bool interval_valid(uint32_t us, bool high) {
    auto close_to = [&](uint32_t base, uint32_t span) { return (us > base ? us - base : base - us) <= span; };
    if (close_to(8333, 9) || close_to(25000, 25)) return high;
    return close_to(16667, 16) || close_to(33333, 34) || close_to(50000, 49) || close_to(66667, 67);
}

// The first deadline base + offset + k * interval after `after`.
int64_t next_deadline(int64_t base, int64_t interval, int64_t offset, int64_t after) {
    if (interval <= 0) return INT64_MAX;
    int64_t first = base + offset;
    if (first > after) return first;
    int64_t k = (after - first) / interval + 1;
    return first + k * interval;
}

// Reads an ancillary block; false if it isn't readable or holds too many handles.
bool read_block(const void* block, uint32_t* count, HANDLE** list, int32_t** signaled) {
    *count = 0;
    *list = nullptr;
    *signaled = nullptr;
    if (!block) return true;
    __try {
        auto* b = static_cast<const uint8_t*>(block);
        *count = *reinterpret_cast<const uint32_t*>(b);
        *list = *reinterpret_cast<HANDLE* const*>(b + 8);
        *signaled = *reinterpret_cast<int32_t* const*>(b + 0x10);
        if (*count > 63 || (*count && !*list)) return false;
        for (uint32_t i = 0; i < *count; ++i) (void)(*list)[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void note_signaled(int32_t* signaled, int32_t index) {
    if (!signaled) return;
    __try {
        *signaled = index;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Sleeps until `deadline`; false if the schedule changed first.
bool sleep_until(HANDLE timer, HANDLE wake, int64_t deadline) {
    const int64_t spin = us_to_ticks(600);
    for (;;) {
        int64_t now = qpc();
        if (now >= deadline) return true;
        int64_t left = deadline - now;
        if (left > spin) {
            LARGE_INTEGER due;
            due.QuadPart = -std::max<int64_t>(1, (left - spin) * 10000000 / qpf());  // 100 ns units, relative
            SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0);
            HANDLE hs[2] = {timer, wake};
            if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) return false;
        } else {
            YieldProcessor();
        }
    }
}

DWORD WINAPI clock_thread(void*) {
    Clock& c = clock();
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    int64_t last = 0;
    for (;;) {
        int64_t deadline = INT64_MAX;
        bool due[kTypes] = {};
        {
            std::lock_guard lock(c.m);
            if (c.active && c.interval_us) {
                int64_t interval = us_to_ticks(c.interval_us), after = std::max(qpc(), last);
                int64_t when[kTypes];
                for (int i = 0; i < kTypes; ++i) {
                    when[i] = c.kinds[i].scheduled
                                  ? next_deadline(c.base, interval, us_to_ticks(c.kinds[i].offset_us), after)
                                  : INT64_MAX;
                    deadline = std::min(deadline, when[i]);
                }
                for (int i = 0; i < kTypes; ++i) due[i] = when[i] == deadline && deadline != INT64_MAX;
            }
        }
        if (deadline == INT64_MAX) {
            WaitForSingleObject(c.wake, INFINITE);
            continue;
        }
        if (!sleep_until(timer, c.wake, deadline)) continue;  // the schedule changed: plan again
        last = deadline;
        std::vector<std::shared_ptr<Handles>> to_signal;
        {
            std::lock_guard lock(c.m);
            for (int i = 0; i < kTypes; ++i) {
                if (!due[i] || !c.kinds[i].scheduled) continue;
                if (i == 0) {
                    // Queued before the release, so the wait it wakes finds it.
                    int64_t now = qpc();
                    c.last_origin = now;
                    if (ReleaseSemaphore(c.kinds[0].internal, 1, nullptr)) c.origins.push_back(now);
                } else {
                    SetEvent(c.kinds[i].internal);
                }
                to_signal.push_back(c.kinds[i].extra);
            }
        }
        for (auto& h : to_signal)
            for (HANDLE x : h->list) signal(x);
    }
}

}  // namespace

HRESULT WINAPI set_frame_interval(void*, void*, uint32_t length_us, uint32_t periodicity, uint32_t flags) {
    if (length_us && !interval_valid(length_us, high_refresh())) {
        std::fprintf(stderr, "[rt] frame: SetFrameIntervalX(%u us): not an interval of this display\n", length_us);
        return kInvalidArg;
    }
    Clock& c = clock();
    {
        std::lock_guard lock(c.m);
        if (c.interval_us != length_us) {
            c.kinds[0].scheduled = false;  // the origin has to be scheduled again for the new interval
            if (c.active && length_us) c.base = qpc();
        }
        c.interval_us = length_us;
        if (!length_us) {
            c.active = false;
            for (auto& k : c.kinds) k.scheduled = false;
        }
        ++c.generation;
    }
    std::fprintf(stderr, "[rt] frame: interval %u us (periodicity %u, flags %x)\n", length_us, periodicity, flags);
    SetEvent(c.wake);
    return S_FALSE;
}

HRESULT WINAPI schedule_frame_event(void*, uint32_t type, uint32_t offset_us, const void* block, uint32_t flags) {
    Clock& c = clock();
    bool unschedule = flags & kUnschedule;
    {
        std::lock_guard lock(c.m);
        if (!c.interval_us || type >= kTypes || (!c.active && type != 0) ||
            (unschedule && (type == 0 || !c.kinds[0].scheduled)))
            return kInvalidArg;
    }
    uint32_t count;
    HANDLE* list;
    int32_t* signaled;
    if (!read_block(block, &count, &list, &signaled)) return kInvalidArg;
    auto dups = std::make_shared<Handles>();
    if (!unschedule)
        for (uint32_t i = 0; i < count; ++i) {
            HANDLE d = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), list[i], GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return kFail;
            dups->list.push_back(d);
        }
    HRESULT hr = S_FALSE;
    bool start_thread = false;
    {
        std::lock_guard lock(c.m);
        if (!c.active) {
            c.active = true;
            c.base = qpc();
            hr = kStartedClock;
        }
        Kind& k = c.kinds[type];
        k.extra = dups;  // the old handles close with their last holder
        if (unschedule) {
            k.scheduled = false;
        } else {
            k.offset_us = offset_us % std::max<uint32_t>(c.interval_us, 1);
            k.scheduled = true;
        }
        ++c.generation;
        start_thread = !c.thread_started;
        c.thread_started = true;
    }
    if (start_thread) {
        HANDLE t = CreateThread(nullptr, 0, &clock_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    std::fprintf(stderr, "[rt] frame: event %u at +%u us%s, %u handle(s)\n", type, offset_us,
                 unschedule ? " unscheduled" : "", count);
    SetEvent(c.wake);
    return hr;
}

HRESULT WINAPI wait_frame_event(void*, uint32_t type, uint32_t timeout_ms, const void* block, uint32_t flags,
                                uint64_t* token) {
    Clock& c = clock();
    HANDLE internal;
    uint64_t token_in = token ? *token : 0;
    {
        std::lock_guard lock(c.m);
        if (type >= kTypes || !c.kinds[type].scheduled) return kInvalidArg;
        internal = c.kinds[type].internal;
        if (type == 0 && token_in) {
            bool known = std::any_of(c.issued.begin(), c.issued.end(), [&](auto& e) { return e.first == token_in; });
            if (!known || (flags & kTokenOnTimeout)) return kInvalidArg;
        }
    }
    uint32_t count;
    HANDLE* list;
    int32_t* signaled;
    if (!read_block(block, &count, &list, &signaled)) return kInvalidArg;
    std::vector<HANDLE> hs{internal};
    for (uint32_t i = 0; i < count; ++i) hs.push_back(list[i]);
    DWORD r = WaitForMultipleObjects(static_cast<DWORD>(hs.size()), hs.data(), (flags & kWaitAll) != 0, timeout_ms);
    HRESULT hr;
    int32_t index;
    if (r == WAIT_TIMEOUT) {
        hr = S_FALSE;
        index = -1;
    } else if (r < WAIT_OBJECT_0 + hs.size()) {
        hr = S_OK;
        index = r == WAIT_OBJECT_0 ? -2 : static_cast<int32_t>(r - WAIT_OBJECT_0) - 1;
    } else {
        return kFail;
    }
    note_signaled(signaled, index);
    bool issue = index == -2 || (index == -1 && (flags & kTokenOnTimeout));
    if (type == 0 && issue) {
        std::lock_guard lock(c.m);
        int64_t origin = qpc();  // a timeout with "token on timeout": now
        if (index == -2 && !c.origins.empty()) {
            origin = c.origins.front();
            c.origins.pop_front();
        }
        if (token_in) {
            for (auto& e : c.issued)
                if (e.first == token_in) e.second = origin;
        } else {
            uint64_t t = c.next_token++ & kTokenMask;
            if (!t) t = c.next_token++ & kTokenMask;
            c.issued.emplace_back(t, origin);
            if (c.issued.size() > kKeep) c.issued.pop_front();
            if (token) *token = t;
        }
    }
    return hr;
}

void mark_flipped(uint64_t token) {
    if (!token) return;
    Clock& c = clock();
    std::lock_guard lock(c.m);
    int64_t origin = c.last_origin;
    for (auto it = c.issued.rbegin(); it != c.issued.rend(); ++it)
        if (it->first == token) {
            origin = it->second;
            break;
        }
    c.flipped.emplace_back(token, origin, qpc());
    if (c.flipped.size() > kKeep) c.flipped.pop_front();
}

HRESULT WINAPI frame_statistics(void*, uint64_t token, uint32_t type, uint32_t* count, void* stats) {
    if (!count || !stats) return kInvalidArg;
    Clock& c = clock();
    int64_t origin = 0, flip = 0;
    bool found = false;
    {
        std::lock_guard lock(c.m);
        for (auto it = c.flipped.rbegin(); it != c.flipped.rend(); ++it)
            if (std::get<0>(*it) == token) {
                origin = std::get<1>(*it);
                flip = std::get<2>(*it);
                found = true;
                break;
            }
    }
    if (!found) {
        *count = 0;
        return kFail;
    }
    if (*count < 1) return kInvalidArg;
    *count = 1;
    auto* s = static_cast<uint8_t*>(stats);
    *reinterpret_cast<uint32_t*>(s) = type;
    *reinterpret_cast<int64_t*>(s + 0x10) = (origin && origin <= flip) ? origin : flip;
    *reinterpret_cast<int64_t*>(s + 0x18) = flip;
    return S_OK;
}

void reset_for_test() {
    Clock& c = clock();
    {
        std::lock_guard lock(c.m);
        c.interval_us = 0;
        c.active = false;
        for (auto& k : c.kinds) {
            k.scheduled = false;
            k.extra = std::make_shared<Handles>();
        }
        while (WaitForSingleObject(c.kinds[0].internal, 0) == WAIT_OBJECT_0) {
        }
        c.origins.clear();
        c.issued.clear();
        c.flipped.clear();
        ++c.generation;
    }
    SetEvent(c.wake);
}

// Made at startup: the runtime has no thread-safe statics.
[[maybe_unused]] const bool g_made_early = (clock(), qpf(), true);

}  // namespace rt::frame
