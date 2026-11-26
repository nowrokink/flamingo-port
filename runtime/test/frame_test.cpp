// Tests the frame clock with real time: a 60 Hz interval, origins about one interval apart with fresh tokens, an
// ancillary handle signalled with the event, a second event type at an offset, statistics once a frame is flipped.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "../shims/frame.h"

namespace {

int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

struct Ancillary {
    uint32_t count;
    uint32_t pad;
    HANDLE* list;
    int32_t* signaled;
};

double ms_since(LARGE_INTEGER t0) {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return 1000.0 * static_cast<double>(t.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
}

}  // namespace

int main() {
    using namespace rt::frame;
    reset_for_test();

    // Lengths: 20 ms isn't a console interval; scheduling needs an interval and the origin first.
    CHECK(set_frame_interval(nullptr, nullptr, 20000, 1, 0) == static_cast<HRESULT>(0x80070057));
    CHECK(schedule_frame_event(nullptr, 0, 0, nullptr, 0) == static_cast<HRESULT>(0x80070057));  // no interval yet
    CHECK(set_frame_interval(nullptr, nullptr, 16667, 1, 0) == S_FALSE);
    CHECK(schedule_frame_event(nullptr, 1, 8000, nullptr, 0) == static_cast<HRESULT>(0x80070057));  // origin first

    // The origin, with an ancillary event the clock also sets.
    HANDLE extra = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Ancillary sched{1, 0, &extra, nullptr};
    CHECK(schedule_frame_event(nullptr, 0, 0, &sched, 0) == 0x10000000);  // starts the clock
    CHECK(schedule_frame_event(nullptr, 1, 8000, nullptr, 0) == S_FALSE);

    // Five origins: fresh tokens, about 16.7 ms apart.
    uint64_t tokens[5] = {};
    LARGE_INTEGER t0;
    int32_t signaled = 99;
    Ancillary wait_block{0, 0, nullptr, &signaled};
    CHECK(wait_frame_event(nullptr, 0, 1000, &wait_block, 0, &tokens[0]) == S_OK);
    CHECK(signaled == -2 && tokens[0] != 0);
    QueryPerformanceCounter(&t0);
    for (int i = 1; i < 5; ++i) {
        CHECK(wait_frame_event(nullptr, 0, 1000, nullptr, 0, &tokens[i]) == S_OK);
        CHECK(tokens[i] != 0 && tokens[i] != tokens[i - 1]);
    }
    double elapsed = ms_since(t0);  // four intervals
    std::printf("frame_test: 4 origins in %.2f ms (66.67 expected)\n", elapsed);
    CHECK(elapsed > 60.0 && elapsed < 80.0);
    CHECK(WaitForSingleObject(extra, 100) == WAIT_OBJECT_0);  // the ancillary handle fired with the origin

    // The second event type comes between origins.
    CHECK(wait_frame_event(nullptr, 1, 1000, nullptr, 0, nullptr) == S_OK);
    // A wait that times out.
    CHECK(set_frame_interval(nullptr, nullptr, 66667, 1, 0) == S_FALSE);  // 15 Hz; the origin must be rescheduled
    CHECK(schedule_frame_event(nullptr, 0, 0, nullptr, 0) == S_FALSE);
    signaled = 99;
    uint64_t none = 0;
    CHECK(wait_frame_event(nullptr, 0, 1, &wait_block, 0, &none) == S_FALSE || signaled == -2);

    // Statistics exist once the frame is flipped.
    uint8_t stats[0x20] = {};
    uint32_t count = 1;
    CHECK(frame_statistics(nullptr, tokens[4], 0, &count, stats) == static_cast<HRESULT>(0x80004005) && count == 0);
    mark_flipped(tokens[4]);
    count = 1;
    CHECK(frame_statistics(nullptr, tokens[4], 0, &count, stats) == S_OK && count == 1);
    int64_t origin = *reinterpret_cast<int64_t*>(stats + 0x10), flip = *reinterpret_cast<int64_t*>(stats + 0x18);
    CHECK(origin > 0 && origin <= flip);

    // Interval 0 stops the clock.
    CHECK(set_frame_interval(nullptr, nullptr, 0, 0, 0) == S_FALSE);
    CHECK(wait_frame_event(nullptr, 0, 0, nullptr, 0, nullptr) == static_cast<HRESULT>(0x80070057));
    CloseHandle(extra);

    if (g_failures) {
        std::fprintf(stderr, "frame_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("frame_test: ok\n");
    std::fflush(stdout);
    // The clock thread is still parked on its wake event; end without waiting for it.
    ExitProcess(0);
}
