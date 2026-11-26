// Tests the task-queue provider object the way title code calls it: through QueryApiImpl's object and its slots.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../shims/xgameruntime.h"

namespace {

int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

constexpr GUID kProvider = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};
constexpr HRESULT kPending = static_cast<HRESULT>(0x8000000A);
constexpr HRESULT kAbort = static_cast<HRESULT>(0x80004004);

void* g_obj = nullptr;
template <class F>
F slot(int i) {
    return reinterpret_cast<F>((*static_cast<void***>(g_obj))[i]);
}

struct AsyncBlock {
    void* queue;
    void* context;
    void(CALLBACK* callback)(AsyncBlock*);
    uintptr_t internal[4];
};
struct ProviderData {
    AsyncBlock* async;
    size_t buffer_size;
    void* buffer;
    void* context;
};
using Callback = void(CALLBACK*)(void*, bool);
using Provider = HRESULT(CALLBACK*)(uint32_t, const ProviderData*);

using Create = HRESULT(CALLBACK*)(void*, uint32_t, uint32_t, void**);
using Submit = HRESULT(CALLBACK*)(void*, void*, uint32_t, void*, Callback);
using SubmitDelayed = HRESULT(CALLBACK*)(void*, void*, uint32_t, uint32_t, void*, Callback);
using Dispatch = bool(CALLBACK*)(void*, void*, uint32_t, uint32_t);
using Close = void(CALLBACK*)(void*, void*);
using Begin = HRESULT(CALLBACK*)(void*, AsyncBlock*, void*, const void*, const char*, Provider);
using Schedule = HRESULT(CALLBACK*)(void*, AsyncBlock*, uint32_t);
using Complete = void(CALLBACK*)(void*, AsyncBlock*, HRESULT, size_t);
using GetStatus = HRESULT(CALLBACK*)(void*, AsyncBlock*, bool);
using GetResultSize = HRESULT(CALLBACK*)(void*, AsyncBlock*, size_t*);
using GetResult = HRESULT(CALLBACK*)(void*, AsyncBlock*, const void*, size_t, void*, size_t*);
using Cancel = void(CALLBACK*)(void*, AsyncBlock*);
using Run = HRESULT(CALLBACK*)(void*, AsyncBlock*, HRESULT(CALLBACK*)(AsyncBlock*));
using Terminate = HRESULT(CALLBACK*)(void*, void*, bool, void*, void(CALLBACK*)(void*));

std::atomic<int> g_sum{0};
void CALLBACK add(void* ctx, bool canceled) { g_sum += canceled ? 1000 : static_cast<int>(reinterpret_cast<intptr_t>(ctx)); }

std::atomic<int> g_order{0}, g_bad_order{0};
void CALLBACK ordered(void* ctx, bool) {
    int want = static_cast<int>(reinterpret_cast<intptr_t>(ctx));
    if (g_order.fetch_add(1) != want) ++g_bad_order;
    Sleep(1);
}

// A provider that schedules its work and returns a 64-bit result.
const int kIdentity = 0;
HRESULT CALLBACK provider(uint32_t op, const ProviderData* d) {
    switch (op) {
        case 0: return slot<Schedule>(9)(g_obj, d->async, 0);
        case 1: slot<Complete>(10)(g_obj, d->async, S_OK, sizeof(uint64_t)); return S_OK;
        case 2: *static_cast<uint64_t*>(d->buffer) = 0x1234567890ull; return S_OK;
        default: return S_OK;
    }
}
// A provider whose work never finishes by itself.
HRESULT CALLBACK stuck(uint32_t op, const ProviderData* d) {
    if (op == 0) return slot<Schedule>(9)(g_obj, d->async, 0);
    if (op == 1) return kPending;
    return S_OK;
}

std::atomic<int> g_completed{0};
void CALLBACK on_complete(AsyncBlock*) { ++g_completed; }

HRESULT CALLBACK work(AsyncBlock* b) { return b->context == reinterpret_cast<void*>(7) ? S_OK : E_FAIL; }

std::atomic<int> g_terminated{0};
void CALLBACK on_terminated(void*) { ++g_terminated; }

bool wait_for(std::atomic<int>& v, int want) {
    for (int i = 0; i < 500 && v.load() != want; ++i) Sleep(2);
    return v.load() == want;
}

}  // namespace

int main() {
    g_obj = rt::xgr::find_area(kProvider, kProvider);
    if (!g_obj) {
        std::fprintf(stderr, "FAIL: no task-queue area\n");
        return 1;
    }

    // Manual ports run only when dispatched.
    void* q = nullptr;
    CHECK(slot<Create>(12)(g_obj, 0, 0, &q) == S_OK && q);
    CHECK(slot<Submit>(18)(g_obj, q, 1, reinterpret_cast<void*>(5), &add) == S_OK);
    CHECK(g_sum == 0);
    CHECK(!slot<Dispatch>(16)(g_obj, q, 0, 0));
    CHECK(slot<Dispatch>(16)(g_obj, q, 1, 0));
    CHECK(g_sum == 5);
    CHECK(!slot<Dispatch>(16)(g_obj, q, 1, 0));

    // Thread pool ports run by themselves; delayed callbacks wait.
    void* pool = nullptr;
    CHECK(slot<Create>(12)(g_obj, 1, 1, &pool) == S_OK);
    g_sum = 0;
    CHECK(slot<Submit>(18)(g_obj, pool, 0, reinterpret_cast<void*>(3), &add) == S_OK);
    CHECK(wait_for(g_sum, 3));
    DWORD t0 = GetTickCount();
    CHECK(slot<SubmitDelayed>(19)(g_obj, pool, 0, 60, reinterpret_cast<void*>(4), &add) == S_OK);
    CHECK(wait_for(g_sum, 7));
    CHECK(GetTickCount() - t0 >= 50);

    // Serialised ports keep order.
    void* serial = nullptr;
    CHECK(slot<Create>(12)(g_obj, 2, 2, &serial) == S_OK);
    for (intptr_t i = 0; i < 20; ++i) slot<Submit>(18)(g_obj, serial, 0, reinterpret_cast<void*>(i), &ordered);
    CHECK(wait_for(g_order, 20));
    CHECK(g_bad_order == 0);

    // An async call: work on the pool, completion routine on the manual queue's completion port.
    AsyncBlock b{};
    b.queue = nullptr;  // process queue for the work...
    void* mixed = nullptr;
    CHECK(slot<Create>(12)(g_obj, 1, 0, &mixed) == S_OK);
    b.queue = mixed;  // ...or a pool work port and a manual completion port
    b.callback = &on_complete;
    CHECK(slot<Begin>(7)(g_obj, &b, nullptr, &kIdentity, "test", &provider) == S_OK);
    CHECK(slot<GetStatus>(3)(g_obj, &b, true) == S_OK);
    size_t size = 0;
    CHECK(slot<GetResultSize>(4)(g_obj, &b, &size) == S_OK && size == 8);
    CHECK(g_completed == 0);
    CHECK(slot<Dispatch>(16)(g_obj, mixed, 1, 1000));
    CHECK(g_completed == 1);
    uint64_t result = 0;
    size_t used = 0;
    CHECK(slot<GetResult>(11)(g_obj, &b, &kIdentity, sizeof(result), &result, &used) == S_OK);
    CHECK(result == 0x1234567890ull && used == 8);
    CHECK(slot<GetStatus>(3)(g_obj, &b, false) == S_OK);  // still readable after cleanup

    // XAsyncRun on the process queue (null handle).
    AsyncBlock r{};
    r.context = reinterpret_cast<void*>(7);
    CHECK(slot<Run>(6)(g_obj, &r, &work) == S_OK);
    CHECK(slot<GetStatus>(3)(g_obj, &r, true) == S_OK);

    // Cancelling a call that would never finish.
    AsyncBlock c{};
    c.queue = pool;
    CHECK(slot<Begin>(7)(g_obj, &c, nullptr, &kIdentity, "stuck", &stuck) == S_OK);
    Sleep(20);
    CHECK(slot<GetStatus>(3)(g_obj, &c, false) == kPending);
    slot<Cancel>(5)(g_obj, &c);
    CHECK(slot<GetStatus>(3)(g_obj, &c, true) == kAbort);

    // Terminate cancels what's queued and then reports.
    g_sum = 0;
    CHECK(slot<Submit>(18)(g_obj, q, 0, reinterpret_cast<void*>(1), &add) == S_OK);
    CHECK(slot<Terminate>(22)(g_obj, q, true, nullptr, &on_terminated) == S_OK);
    CHECK(g_sum == 1000);
    CHECK(g_terminated == 1);
    CHECK(slot<Submit>(18)(g_obj, q, 0, nullptr, &add) == kAbort);

    for (void* h : {q, pool, serial, mixed}) slot<Close>(17)(g_obj, h);

    if (g_failures) {
        std::fprintf(stderr, "xtaskqueue_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("xtaskqueue_test: ok\n");
    return 0;
}
