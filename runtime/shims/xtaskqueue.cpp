// XTaskQueue and XAsync, served through xgameruntime's QueryApiImpl as the task-queue provider's object.
//
// The object's slots are the public API (XAsync.h, XTaskQueue.h) in header order, each taking the object first:
//   3 XAsyncGetStatus  4 XAsyncGetResultSize  5 XAsyncCancel  6 XAsyncRun  7 XAsyncBegin  8 XAsyncBeginAlloc
//   9 XAsyncSchedule  10 XAsyncComplete  11 XAsyncGetResult  12 XTaskQueueCreate  13 XTaskQueueCreateComposite
//   14 XTaskQueueGetPort  15 XTaskQueueDuplicateHandle  16 XTaskQueueDispatch  17 XTaskQueueCloseHandle
//   18 XTaskQueueSubmitCallback  19 XTaskQueueSubmitDelayedCallback  20 XTaskQueueRegisterWaiter
//   21 XTaskQueueUnregisterWaiter  22 XTaskQueueTerminate  23 XTaskQueueRegisterMonitor
//   24 XTaskQueueUnregisterMonitor  25 XTaskQueueGetCurrentProcessTaskQueue  26 XTaskQueueSetCurrentProcessTaskQueue
//   29 (no arguments; nothing to do on a PC)
// Slots 27, 28 and 30 aren't known yet and log through the probe vtable.
//
// Behaviour follows the public documentation: a queue has a work port and a completion port, each dispatched
// manually (the title calls XTaskQueueDispatch), on the thread pool, serialised on one thread, or immediately on
// the submitting thread. A null queue handle means the process queue. The async block's system area holds our
// state, the status and the result size, so status and size stay readable after the call is cleaned up.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "com_probe.h"
#include "xgameruntime.h"

namespace {

constexpr GUID kProvider = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};

constexpr HRESULT kPending = static_cast<HRESULT>(0x8000000A);
constexpr HRESULT kAbort = static_cast<HRESULT>(0x80004004);
constexpr HRESULT kInvalidArg = static_cast<HRESULT>(0x80070057);
constexpr HRESULT kNotSufficientBuffer = static_cast<HRESULT>(0x8007007A);
constexpr HRESULT kIllegalMethodCall = static_cast<HRESULT>(0x8000000E);

enum Mode : uint32_t { kManual = 0, kThreadPool = 1, kSerialized = 2, kImmediate = 3 };
enum PortId : uint32_t { kWork = 0, kCompletion = 1 };

using Callback = void(CALLBACK*)(void* context, bool canceled);
using MonitorCallback = void(CALLBACK*)(void* context, void* queue, uint32_t port);
using TerminatedCallback = void(CALLBACK*)(void* context);

// ---------------------------------------------------------------- ports

struct Item {
    void* context;
    Callback callback;
};

struct Port {
    Mode mode;
    std::mutex m;
    std::condition_variable cv;
    std::deque<Item> items;   // manual and serialised ports
    int in_flight = 0;        // submitted and not finished yet (all modes)
    bool stop = false;        // serialised worker: exit when drained
    std::thread worker;       // serialised ports (detached; the port is then kept for the process)

    explicit Port(Mode m_) : mode(m_) {}

    void finished() {
        std::lock_guard lock(m);
        --in_flight;
        cv.notify_all();
    }
};

void run_serialized(Port* p) {
    for (;;) {
        Item item;
        {
            std::unique_lock lock(p->m);
            p->cv.wait(lock, [&] { return p->stop || !p->items.empty(); });
            if (p->items.empty()) return;
            item = p->items.front();
            p->items.pop_front();
        }
        item.callback(item.context, false);
        p->finished();
    }
}

struct PoolJob {
    std::shared_ptr<Port> port;
    Item item;
};

void CALLBACK run_pooled(PTP_CALLBACK_INSTANCE, void* data) {
    std::unique_ptr<PoolJob> job(static_cast<PoolJob*>(data));
    job->item.callback(job->item.context, false);
    job->port->finished();
}

// Queues an item on a port, by its mode. False if the item couldn't be queued.
bool post(const std::shared_ptr<Port>& p, Item item) {
    {
        std::lock_guard lock(p->m);
        ++p->in_flight;
        if (p->mode == kManual || p->mode == kSerialized) {
            p->items.push_back(item);
            if (p->mode == kSerialized && !p->worker.joinable()) {
                // The worker uses the port until the process ends, so the port lives that long too (titles make
                // few serialised queues).
                static auto* keep = new std::vector<std::shared_ptr<Port>>;
                keep->push_back(p);
                p->worker = std::thread(run_serialized, p.get());
                p->worker.detach();
            }
        }
    }
    switch (p->mode) {
        case kManual: p->cv.notify_all(); return true;
        case kSerialized: p->cv.notify_all(); return true;
        case kImmediate:
            item.callback(item.context, false);
            p->finished();
            return true;
        default: {
            auto* job = new PoolJob{p, item};
            if (TrySubmitThreadpoolCallback(&run_pooled, job, nullptr)) return true;
            delete job;
            p->finished();
            return false;
        }
    }
}

// ---------------------------------------------------------------- queues

struct Monitor {
    uint64_t token;
    void* context;
    MonitorCallback callback;
};

struct Queue {
    static constexpr uint32_t kMagic = 0x51554555;  // "QUEU"
    uint32_t magic = kMagic;
    std::atomic<int> refs{1};
    std::shared_ptr<Port> ports[2];
    std::atomic<bool> terminated{false};
    std::mutex monitors_m;
    std::vector<Monitor> monitors;
};

std::mutex g_process_m;
Queue* g_process = nullptr;      // the process queue; created on first use
bool g_process_set_null = false;  // the title set it to null on purpose

void add_ref(Queue* q) { q->refs.fetch_add(1); }
void release(Queue* q) {
    if (q->refs.fetch_sub(1) == 1) {
        q->magic = 0;
        delete q;
    }
}

Queue* create(Mode work, Mode completion) {
    auto* q = new Queue;
    q->ports[kWork] = std::make_shared<Port>(work);
    q->ports[kCompletion] = std::make_shared<Port>(completion);
    return q;
}

// The queue a handle names: null is the process queue. Not referenced.
Queue* resolve(void* handle) {
    if (handle) {
        auto* q = static_cast<Queue*>(handle);
        return q->magic == Queue::kMagic ? q : nullptr;
    }
    std::lock_guard lock(g_process_m);
    if (!g_process && !g_process_set_null) g_process = create(kThreadPool, kThreadPool);
    return g_process;
}

HRESULT submit(Queue* q, uint32_t port, Item item) {
    if (!q || port > kCompletion || !item.callback) return kInvalidArg;
    if (q->terminated) return kAbort;
    std::vector<Monitor> monitors;
    {
        std::lock_guard lock(q->monitors_m);
        monitors = q->monitors;
    }
    if (!post(q->ports[port], item)) return E_OUTOFMEMORY;
    for (auto& m : monitors) m.callback(m.context, q, port);
    return S_OK;
}

// Delayed submission: a thread-pool timer that submits when due.
struct Delayed {
    Queue* queue;  // referenced
    uint32_t port;
    Item item;
};

void CALLBACK delayed_due(PTP_CALLBACK_INSTANCE, void* data, PTP_TIMER timer) {
    std::unique_ptr<Delayed> d(static_cast<Delayed*>(data));
    if (submit(d->queue, d->port, d->item) != S_OK) d->item.callback(d->item.context, true);
    release(d->queue);
    CloseThreadpoolTimer(timer);
}

HRESULT submit_delayed(Queue* q, uint32_t port, uint32_t delay_ms, Item item) {
    if (!delay_ms) return submit(q, port, item);
    if (!q || port > kCompletion || !item.callback) return kInvalidArg;
    if (q->terminated) return kAbort;
    add_ref(q);
    auto* d = new Delayed{q, port, item};
    PTP_TIMER timer = CreateThreadpoolTimer(&delayed_due, d, nullptr);
    if (!timer) {
        release(q);
        delete d;
        return E_OUTOFMEMORY;
    }
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(delay_ms) * 10000;  // relative, 100 ns units
    FILETIME ft{static_cast<DWORD>(due.LowPart), static_cast<DWORD>(due.HighPart)};
    SetThreadpoolTimer(timer, &ft, 0, 0);
    return S_OK;
}

// ---------------------------------------------------------------- async blocks

struct AsyncBlock {
    void* queue;
    void* context;
    void(CALLBACK* callback)(AsyncBlock*);
    uintptr_t internal[4];  // ours: [state, status, result size, unused]
};

struct ProviderData {
    AsyncBlock* async;
    size_t buffer_size;
    void* buffer;
    void* context;
};

using Provider = HRESULT(CALLBACK*)(uint32_t op, const ProviderData* data);
using Work = HRESULT(CALLBACK*)(AsyncBlock* async);
enum Op : uint32_t { kBegin = 0, kDoWork = 1, kGetResult = 2, kCancel = 3, kCleanup = 4 };

struct AsyncState {
    Provider provider = nullptr;
    Work work = nullptr;  // XAsyncRun
    void* context = nullptr;
    const void* identity = nullptr;
    void* alloc = nullptr;  // XAsyncBeginAlloc's context
    Queue* queue = nullptr;  // referenced
    std::atomic<bool> completed{false};
    std::atomic<bool> canceled{false};
};

std::mutex g_status_m;  // waits for any status change
std::condition_variable g_status_cv;

std::atomic<uintptr_t>& word(AsyncBlock* b, int i) {
    static_assert(sizeof(std::atomic<uintptr_t>) == sizeof(uintptr_t));
    return *reinterpret_cast<std::atomic<uintptr_t>*>(&b->internal[i]);
}
AsyncState* state(AsyncBlock* b) { return reinterpret_cast<AsyncState*>(word(b, 0).load()); }
HRESULT status(AsyncBlock* b) { return static_cast<HRESULT>(static_cast<uint32_t>(word(b, 1).load())); }
void set_status(AsyncBlock* b, HRESULT hr) {
    {
        std::lock_guard lock(g_status_m);
        word(b, 1).store(static_cast<uint32_t>(hr));
    }
    g_status_cv.notify_all();
}

ProviderData data_for(AsyncBlock* b, AsyncState* s, size_t size = 0, void* buffer = nullptr) {
    return {b, size, buffer, s->context};
}

void cleanup(AsyncBlock* b) {
    AsyncState* s = state(b);
    if (!s) return;
    word(b, 0).store(0);
    if (s->provider) {
        ProviderData d = data_for(b, s);
        s->provider(kCleanup, &d);
    }
    if (s->alloc) std::free(s->alloc);
    if (s->queue) release(s->queue);
    delete s;
}

void CALLBACK run_completion(void* context, bool) {
    auto* b = static_cast<AsyncBlock*>(context);
    b->callback(b);
}

void XAsyncComplete(AsyncBlock* b, HRESULT result, size_t required);

void CALLBACK do_work(void* context, bool canceled) {
    auto* b = static_cast<AsyncBlock*>(context);
    AsyncState* s = state(b);
    if (!s || s->completed) return;
    if (canceled) return XAsyncComplete(b, kAbort, 0);
    HRESULT hr;
    if (s->work) {
        hr = s->work(b);
    } else {
        ProviderData d = data_for(b, s);
        hr = s->provider(kDoWork, &d);
    }
    if (hr != kPending) XAsyncComplete(b, hr, 0);
}

HRESULT start(AsyncBlock* b, AsyncState* s) {
    Queue* q = resolve(b->queue);
    if (!q) {
        delete s;
        return kInvalidArg;
    }
    add_ref(q);
    s->queue = q;
    word(b, 2).store(0);
    set_status(b, kPending);
    word(b, 0).store(reinterpret_cast<uintptr_t>(s));
    return S_OK;
}

HRESULT XAsyncBegin(AsyncBlock* b, void* context, const void* identity, const char*, Provider provider) {
    if (!b || !provider) return kInvalidArg;
    auto* s = new AsyncState;
    s->provider = provider;
    s->context = context;
    s->identity = identity;
    if (HRESULT hr = start(b, s); FAILED(hr)) return hr;
    ProviderData d = data_for(b, s);
    HRESULT hr = provider(kBegin, &d);
    if (FAILED(hr)) {  // the call fails synchronously; the completion routine isn't called
        set_status(b, hr);
        cleanup(b);
    }
    return hr;
}

HRESULT XAsyncBeginAlloc(AsyncBlock* b, const void* identity, const char* name, Provider provider,
                         size_t context_size, size_t params_size, const void* params) {
    if (!b || !provider || params_size > context_size) return kInvalidArg;
    void* alloc = std::calloc(1, context_size ? context_size : 1);
    if (!alloc) return E_OUTOFMEMORY;
    if (params_size && params) std::memcpy(alloc, params, params_size);
    auto* s = new AsyncState;
    s->provider = provider;
    s->context = alloc;
    s->alloc = alloc;
    s->identity = identity;
    if (HRESULT hr = start(b, s); FAILED(hr)) return hr;
    (void)name;
    ProviderData d = data_for(b, s);
    HRESULT hr = provider(kBegin, &d);
    if (FAILED(hr)) {
        set_status(b, hr);
        cleanup(b);
    }
    return hr;
}

HRESULT XAsyncSchedule(AsyncBlock* b, uint32_t delay_ms) {
    AsyncState* s = b ? state(b) : nullptr;
    if (!s || s->completed) return kIllegalMethodCall;
    return submit_delayed(s->queue, kWork, delay_ms, {b, &do_work});
}

void XAsyncComplete(AsyncBlock* b, HRESULT result, size_t required) {
    AsyncState* s = b ? state(b) : nullptr;
    if (!s || s->completed.exchange(true)) return;
    if (FAILED(result)) required = 0;
    word(b, 2).store(required);
    Queue* q = s->queue;
    add_ref(q);
    bool has_callback = b->callback != nullptr;
    if (!required) cleanup(b);  // nothing for GetResult to hand over
    set_status(b, result);
    if (has_callback && submit(q, kCompletion, {b, &run_completion}) != S_OK) b->callback(b);
    release(q);
}

HRESULT XAsyncRun(AsyncBlock* b, Work work) {
    if (!b || !work) return kInvalidArg;
    auto* s = new AsyncState;
    s->work = work;
    if (HRESULT hr = start(b, s); FAILED(hr)) return hr;
    HRESULT hr = submit(s->queue, kWork, {b, &do_work});
    if (FAILED(hr)) {
        set_status(b, hr);
        cleanup(b);
    }
    return hr;
}

HRESULT XAsyncGetStatus(AsyncBlock* b, bool wait) {
    if (!b) return kInvalidArg;
    if (!wait) return status(b);
    std::unique_lock lock(g_status_m);
    g_status_cv.wait(lock, [&] { return static_cast<HRESULT>(static_cast<uint32_t>(word(b, 1).load())) != kPending; });
    return status(b);
}

HRESULT XAsyncGetResultSize(AsyncBlock* b, size_t* size) {
    if (!b || !size) return kInvalidArg;
    HRESULT hr = status(b);
    if (FAILED(hr)) return hr;
    *size = word(b, 2).load();
    return S_OK;
}

HRESULT XAsyncGetResult(AsyncBlock* b, const void* identity, size_t size, void* buffer, size_t* used) {
    if (!b) return kInvalidArg;
    HRESULT hr = status(b);
    if (hr == kPending || FAILED(hr)) return hr;
    size_t required = word(b, 2).load();
    AsyncState* s = state(b);
    if (!required || !s) {
        if (used) *used = 0;
        return hr;
    }
    if (identity != s->identity) return kInvalidArg;
    if (size < required || !buffer) return kNotSufficientBuffer;
    ProviderData d = data_for(b, s, size, buffer);
    HRESULT got = s->provider ? s->provider(kGetResult, &d) : S_OK;
    if (used) *used = required;
    cleanup(b);
    return FAILED(got) ? got : hr;
}

void XAsyncCancel(AsyncBlock* b) {
    AsyncState* s = b ? state(b) : nullptr;
    if (!s || s->completed || s->canceled.exchange(true)) return;
    if (s->provider) {
        ProviderData d = data_for(b, s);
        s->provider(kCancel, &d);
    }
    XAsyncComplete(b, kAbort, 0);  // no-op if the provider completed it
}

// ---------------------------------------------------------------- the provider object's methods

using This = void*;

HRESULT CALLBACK m_get_status(This, AsyncBlock* b, bool wait) { return XAsyncGetStatus(b, wait); }
HRESULT CALLBACK m_get_result_size(This, AsyncBlock* b, size_t* size) { return XAsyncGetResultSize(b, size); }
void CALLBACK m_cancel(This, AsyncBlock* b) { XAsyncCancel(b); }
HRESULT CALLBACK m_run(This, AsyncBlock* b, Work work) { return XAsyncRun(b, work); }
HRESULT CALLBACK m_begin(This, AsyncBlock* b, void* context, const void* identity, const char* name, Provider p) {
    return XAsyncBegin(b, context, identity, name, p);
}
HRESULT CALLBACK m_begin_alloc(This, AsyncBlock* b, const void* identity, const char* name, Provider p,
                               size_t context_size, size_t params_size, const void* params) {
    return XAsyncBeginAlloc(b, identity, name, p, context_size, params_size, params);
}
HRESULT CALLBACK m_schedule(This, AsyncBlock* b, uint32_t delay) { return XAsyncSchedule(b, delay); }
void CALLBACK m_complete(This, AsyncBlock* b, HRESULT result, size_t required) { XAsyncComplete(b, result, required); }
HRESULT CALLBACK m_get_result(This, AsyncBlock* b, const void* identity, size_t size, void* buffer, size_t* used) {
    return XAsyncGetResult(b, identity, size, buffer, used);
}

HRESULT CALLBACK m_create(This, uint32_t work, uint32_t completion, void** out) {
    if (!out || work > kImmediate || completion > kImmediate) return kInvalidArg;
    *out = create(static_cast<Mode>(work), static_cast<Mode>(completion));
    return S_OK;
}

// A port handle is the Port itself; a composite queue shares the ports of the queues they came from.
HRESULT CALLBACK m_create_composite(This, void* work_port, void* completion_port, void** out) {
    if (!out || !work_port || !completion_port) return kInvalidArg;
    auto* q = new Queue;  // port handles are heap copies of the ports' shared_ptr (m_get_port)
    q->ports[kWork] = *static_cast<std::shared_ptr<Port>*>(work_port);
    q->ports[kCompletion] = *static_cast<std::shared_ptr<Port>*>(completion_port);
    *out = q;
    return S_OK;
}

// Port handles: a heap copy of the port's shared_ptr, kept for the process (titles keep few).
HRESULT CALLBACK m_get_port(This, void* queue, uint32_t port, void** out) {
    Queue* q = resolve(queue);
    if (!q || !out || port > kCompletion) return kInvalidArg;
    *out = new std::shared_ptr<Port>(q->ports[port]);
    return S_OK;
}

HRESULT CALLBACK m_duplicate(This, void* queue, void** out) {
    Queue* q = resolve(queue);
    if (!q || !out) return kInvalidArg;
    add_ref(q);
    *out = q;
    return S_OK;
}

bool CALLBACK m_dispatch(This, void* queue, uint32_t port, uint32_t timeout_ms) {
    Queue* q = resolve(queue);
    if (!q || port > kCompletion) return false;
    Port* p = q->ports[port].get();
    if (p->mode != kManual) return false;
    Item item;
    {
        std::unique_lock lock(p->m);
        auto ready = [&] { return !p->items.empty(); };
        if (timeout_ms == INFINITE) p->cv.wait(lock, ready);
        else if (!p->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) return false;
        item = p->items.front();
        p->items.pop_front();
    }
    item.callback(item.context, false);
    p->finished();
    return true;
}

void CALLBACK m_close(This, void* queue) {
    if (!queue) return;
    auto* q = static_cast<Queue*>(queue);
    if (q->magic == Queue::kMagic) release(q);
}

HRESULT CALLBACK m_submit(This, void* queue, uint32_t port, void* context, Callback cb) {
    return submit(resolve(queue), port, {context, cb});
}

HRESULT CALLBACK m_submit_delayed(This, void* queue, uint32_t port, uint32_t delay_ms, void* context, Callback cb) {
    return submit_delayed(resolve(queue), port, delay_ms, {context, cb});
}

// Waiters: the callback is submitted to the port when the handle is signalled.
struct Waiter {
    Queue* queue;  // referenced
    uint32_t port;
    Item item;
    HANDLE wait = nullptr;
};

void CALLBACK waiter_signalled(void* data, BOOLEAN) {
    auto* w = static_cast<Waiter*>(data);
    submit(w->queue, w->port, w->item);
}

HRESULT CALLBACK m_register_waiter(This, void* queue, uint32_t port, HANDLE handle, void* context, Callback cb,
                                   uint64_t* token) {
    Queue* q = resolve(queue);
    if (!q || !handle || !cb || !token || port > kCompletion) return kInvalidArg;
    add_ref(q);
    auto* w = new Waiter{q, port, {context, cb}};
    if (!RegisterWaitForSingleObject(&w->wait, handle, &waiter_signalled, w, INFINITE, WT_EXECUTEONLYONCE)) {
        release(q);
        delete w;
        return HRESULT_FROM_WIN32(GetLastError());
    }
    *token = reinterpret_cast<uint64_t>(w);
    return S_OK;
}

void CALLBACK m_unregister_waiter(This, void*, uint64_t token) {
    auto* w = reinterpret_cast<Waiter*>(token);
    if (!w) return;
    UnregisterWaitEx(w->wait, INVALID_HANDLE_VALUE);  // waits for a running callback
    release(w->queue);
    delete w;
}

// Terminate: no new work; queued manual and serialised items are cancelled (called with canceled = true); the
// callback runs once everything in flight has finished.
struct Termination {
    Queue* queue;  // referenced
    void* context;
    TerminatedCallback callback;
};

DWORD WINAPI finish_termination(void* data) {
    std::unique_ptr<Termination> t(static_cast<Termination*>(data));
    for (auto& p : t->queue->ports) {
        std::unique_lock lock(p->m);
        p->cv.wait(lock, [&] { return p->in_flight == 0; });
    }
    if (t->callback) t->callback(t->context);
    release(t->queue);
    return 0;
}

HRESULT CALLBACK m_terminate(This, void* queue, bool wait, void* context, TerminatedCallback cb) {
    Queue* q = resolve(queue);
    if (!q) return kInvalidArg;
    q->terminated = true;
    for (auto& p : q->ports) {
        std::deque<Item> cancelled;
        {
            std::lock_guard lock(p->m);
            if (p->mode == kManual || p->mode == kSerialized) cancelled.swap(p->items);
        }
        for (auto& item : cancelled) {
            item.callback(item.context, true);
            p->finished();
        }
    }
    add_ref(q);
    auto* t = new Termination{q, context, cb};
    if (wait) {
        finish_termination(t);
        return S_OK;
    }
    HANDLE thread = CreateThread(nullptr, 0, &finish_termination, t, 0, nullptr);
    if (!thread) {
        finish_termination(t);
        return S_OK;
    }
    CloseHandle(thread);
    return S_OK;
}

std::atomic<uint64_t> g_next_token{1};

HRESULT CALLBACK m_register_monitor(This, void* queue, void* context, MonitorCallback cb, uint64_t* token) {
    Queue* q = resolve(queue);
    if (!q || !cb || !token) return kInvalidArg;
    std::lock_guard lock(q->monitors_m);
    *token = g_next_token++;
    q->monitors.push_back({*token, context, cb});
    return S_OK;
}

void CALLBACK m_unregister_monitor(This, void* queue, uint64_t token) {
    Queue* q = resolve(queue);
    if (!q) return;
    std::lock_guard lock(q->monitors_m);
    std::erase_if(q->monitors, [&](const Monitor& m) { return m.token == token; });
}

bool CALLBACK m_get_process_queue(This, void** out) {
    if (!out) return false;
    Queue* q = resolve(nullptr);
    if (!q) {
        *out = nullptr;
        return false;
    }
    add_ref(q);
    *out = q;
    return true;
}

void CALLBACK m_set_process_queue(This, void* queue) {
    Queue* q = queue ? resolve(queue) : nullptr;
    if (q) add_ref(q);
    std::lock_guard lock(g_process_m);
    if (g_process) release(g_process);
    g_process = q;
    g_process_set_null = !q;
}

void CALLBACK m_no_op(This) {}

rt::shims::ProbeObject* make_object() {
    void** vt = rt::shims::make_probe_vtable("XTaskQueue");
    const std::pair<int, void*> slots[] = {
        {3, reinterpret_cast<void*>(&m_get_status)},       {4, reinterpret_cast<void*>(&m_get_result_size)},
        {5, reinterpret_cast<void*>(&m_cancel)},           {6, reinterpret_cast<void*>(&m_run)},
        {7, reinterpret_cast<void*>(&m_begin)},            {8, reinterpret_cast<void*>(&m_begin_alloc)},
        {9, reinterpret_cast<void*>(&m_schedule)},         {10, reinterpret_cast<void*>(&m_complete)},
        {11, reinterpret_cast<void*>(&m_get_result)},      {12, reinterpret_cast<void*>(&m_create)},
        {13, reinterpret_cast<void*>(&m_create_composite)}, {14, reinterpret_cast<void*>(&m_get_port)},
        {15, reinterpret_cast<void*>(&m_duplicate)},       {16, reinterpret_cast<void*>(&m_dispatch)},
        {17, reinterpret_cast<void*>(&m_close)},           {18, reinterpret_cast<void*>(&m_submit)},
        {19, reinterpret_cast<void*>(&m_submit_delayed)},  {20, reinterpret_cast<void*>(&m_register_waiter)},
        {21, reinterpret_cast<void*>(&m_unregister_waiter)}, {22, reinterpret_cast<void*>(&m_terminate)},
        {23, reinterpret_cast<void*>(&m_register_monitor)}, {24, reinterpret_cast<void*>(&m_unregister_monitor)},
        {25, reinterpret_cast<void*>(&m_get_process_queue)}, {26, reinterpret_cast<void*>(&m_set_process_queue)},
        {29, reinterpret_cast<void*>(&m_no_op)},
    };
    for (auto& [slot, fn] : slots) rt::shims::set_probe_method(vt, slot, fn);
    return rt::shims::make_probe_object(vt, "XTaskQueue");
}

// The task-queue provider is also the interface id titles ask for.
rt::xgr::AreaRegistration g_area(kProvider, kProvider, make_object());

// finish_async: a provider that completes the call as it begins, with the result kept in its context.
HRESULT CALLBACK finished_provider(uint32_t op, const ProviderData* d) {
    if (op == kBegin)
        XAsyncComplete(d->async, static_cast<HRESULT>(reinterpret_cast<intptr_t>(d->context)), 0);
    return S_OK;
}

// finish_async_with: the same, with a result of bytes, copied out by XAsyncGetResult.
struct Payload {
    HRESULT hr;
    std::vector<uint8_t> bytes;
    std::vector<size_t> rebase;  // offsets of pointer fields holding offsets into `bytes`
};
HRESULT CALLBACK payload_provider(uint32_t op, const ProviderData* d) {
    auto* p = static_cast<Payload*>(d->context);
    if (op == kBegin) {
        XAsyncComplete(d->async, p->hr, SUCCEEDED(p->hr) ? p->bytes.size() : 0);
    } else if (op == kGetResult) {
        if (d->buffer && d->buffer_size >= p->bytes.size()) {
            std::memcpy(d->buffer, p->bytes.data(), p->bytes.size());
            for (size_t at : p->rebase) {
                uint64_t v;
                std::memcpy(&v, static_cast<uint8_t*>(d->buffer) + at, 8);
                v += reinterpret_cast<uintptr_t>(d->buffer);
                std::memcpy(static_cast<uint8_t*>(d->buffer) + at, &v, 8);
            }
        }
    } else if (op == kCleanup) {
        delete p;
    }
    return S_OK;
}

}  // namespace

namespace rt::xgr {

HRESULT finish_async_with(void* block, const void* identity, HRESULT hr, std::vector<uint8_t> result,
                          std::vector<size_t> rebase) {
    auto* p = new Payload{hr, std::move(result), std::move(rebase)};
    HRESULT begun = XAsyncBegin(static_cast<AsyncBlock*>(block), p, identity, "", &payload_provider);
    if (FAILED(begun)) delete p;
    return begun;
}

HRESULT finish_async(void* block, const void* identity, HRESULT hr) {
    return XAsyncBegin(static_cast<AsyncBlock*>(block), reinterpret_cast<void*>(static_cast<intptr_t>(hr)), identity,
                       "", &finished_provider);
}

HRESULT async_result(void* block, const void* identity) {
    return XAsyncGetResult(static_cast<AsyncBlock*>(block), identity, 0, nullptr, nullptr);
}

HRESULT async_result_into(void* block, const void* identity, size_t size, void* buffer, size_t* used) {
    return XAsyncGetResult(static_cast<AsyncBlock*>(block), identity, size, buffer, used);
}

}  // namespace rt::xgr
