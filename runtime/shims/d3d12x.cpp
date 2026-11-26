// D3D12.X objects over the host's D3D12: command queues and fences so far.
//
// Each console object is the console header (method table, bits, reference count) followed by our fields. The
// tables are probe tables (com_probe.h) with the methods we know filled in, so a run logs every other method the
// title calls, with its slot. Slots follow the console layout of the runtime 1.0.2.1 was built with (interface
// version 0x20004, device created as ID3D12Device11), which differs from the PC's where noted.
//
// Queue (newer console layout: two more methods before Signal than the PC and the older console layout):
//   16 Signal(fence, value)  17 Wait(fence, value)  18 GetTimestampFrequency(u64*)
//   32 PresentX, 34 Signal with pipe sync settings, 35 ExecuteCommandLists with a frame token (as VIctory reads them)
// Fence (the PC's ID3D12Fence order):
//   8 GetCompletedValue()  9 SetEventOnCompletion(value, event)  10 Signal(value)
#include "d3d12x.h"

#include <deque>
#include <windows.h>
#include <intrin.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

#include "../gpu/gfx10_layout.h"
#include "d3d12x_internal.h"
#include "com_probe.h"
#include "gpu_memory.h"
#include "overlay.h"
#include "shims.h"
#include "uma.h"
#include "../src/cpu_emul.h"
#include "window.h"

namespace rt::d3d12x {
namespace {

// ---------------------------------------------------------------- host device

IDXGIAdapter1* g_adapter = nullptr;  // the host adapter the device is on (kept for IDXGIDevice::GetAdapter)

ID3D12Device* create_host_device() {
    char debug[4] = {};
    if (GetEnvironmentVariableA("RT_D3D12_DEBUG", debug, sizeof(debug)) && debug[0] == '1') {
        ID3D12Debug* d = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d)))) {
            d->EnableDebugLayer();
            d->Release();
            std::fprintf(stderr, "[rt] d3d12: debug layer on\n");
        }
        // DRED needs no SDK layers: breadcrumbs of what each list was doing when the device is removed
        ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->Release();
            std::fprintf(stderr, "[rt] d3d12: DRED breadcrumbs on\n");
        }
    }
    // The high-performance adapter, so a laptop's discrete GPU is used.
    IDXGIFactory6* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        factory->Release();
    }
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device));
    DXGI_ADAPTER_DESC1 desc{};
    if (adapter) {
        adapter->GetDesc1(&desc);
        g_adapter = adapter;  // kept: IDXGIDevice::GetAdapter hands it out
    }
    // With the debug layer, its messages (warnings and up) go to our log, the first 200 of them.
    if (debug[0] == '1' && device) {
        ID3D12InfoQueue1* q = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&q)))) {
            DWORD cookie = 0;
            q->RegisterMessageCallback(
                [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR text, void*) {
                    // errors (and corruption) up to 600, warnings up to 60: a removed device is explained by the
                    // error just before it, which a flood of warnings must not push out
                    static std::atomic<int> errors{0}, warnings{0};
                    if (severity <= D3D12_MESSAGE_SEVERITY_ERROR ? errors++ < 600
                                                                  : severity == D3D12_MESSAGE_SEVERITY_WARNING &&
                                                                        warnings++ < 60)
                        std::fprintf(stderr, "[rt] d3d12 debug %s (%d): %s\n",
                                     severity <= D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" : "warning",
                                     static_cast<int>(id), text);
                },
                D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
        }
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "[rt] d3d12: no host device (%08lX)\n", static_cast<unsigned long>(hr));
        return nullptr;
    }
    std::fprintf(stderr, "[rt] d3d12: host device on %ls\n", desc.Description);
    return device;
}

// ---------------------------------------------------------------- fences

// The console's fences are memory the title can read: slot 12 gives the address of the completed value. Ours keep a
// copy, updated on every CPU signal and by a thread that follows the host fences (fence_watch).
struct Fence {
    Header header;
    ID3D12Fence* host;
    alignas(8) volatile uint64_t completed;
    Fence* next;  // all fences, for the watcher
};

std::atomic<Fence*> g_fences{nullptr};

// Submissions don't wait for the GPU (RT_SYNC_SUBMIT=1: they do, as before). The value in a fence's memory is then
// the one truth the title sees: a queue's signal reaches it only once the lists before it have run and their
// bottom-of-pipe writes are done (complete_queue), and reads of the fence and its events go by it, not by the PC
// fence (which gets there a little earlier, before the writes).
const bool g_async = [] {
    char v[4] = {};
    return !(GetEnvironmentVariableA("RT_SYNC_SUBMIT", v, sizeof v) && v[0] == '1');
}();

// SetEventOnCompletion with submissions not waited for: the events of values the memory hasn't reached.
struct FenceWaiter {
    Fence* f;
    uint64_t value;
    HANDLE event;
};
std::mutex g_waiters_lock;
std::vector<FenceWaiter> g_waiters;
std::atomic<size_t> g_waiter_count{0};

void check_waiters() {
    if (!g_waiter_count.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_waiters_lock);
    size_t keep = 0;
    for (const FenceWaiter& w : g_waiters)
        if (w.f->completed >= w.value) SetEvent(w.event);
        else g_waiters[keep++] = w;
    g_waiters.resize(keep);
    g_waiter_count = keep;
}

// The memory moves up to `v` (never back), and the events waiting for it are set.
void raise_completed(Fence* f, uint64_t v) {
    uint64_t mem = f->completed;
    while (v > mem && mem != UINT64_MAX) {
        const uint64_t was = static_cast<uint64_t>(InterlockedCompareExchange64(
            reinterpret_cast<volatile LONG64*>(&f->completed), static_cast<LONG64>(v), static_cast<LONG64>(mem)));
        if (was == mem) break;
        mem = was;
    }
    check_waiters();
}

struct Queue;
// Submissions held for a value another queue's list writes (see hold_submission): runs the ones that can go.
void run_held(Queue* all_of);
Queue* const kEveryQueue = reinterpret_cast<Queue*>(~uintptr_t{0});  // for run_held: nothing stays held
std::atomic<size_t> g_held_count{0};

void release_value_waits();
DWORD WINAPI fence_watch(void*) {
    for (;;) {
        if (g_held_count.load(std::memory_order_relaxed)) run_held(nullptr);
        for (Fence* f = g_fences.load(std::memory_order_acquire); f; f = f->next) {
            const uint64_t v = f->host->GetCompletedValue();
            if (v == UINT64_MAX) continue;  // the device is gone
            uint64_t mem = f->completed;
            // never back: signals set it ahead (not waiting on submissions, the queues move it themselves, after
            // the writes before the signal)
            while (!g_async && v > mem) {
                const uint64_t was = static_cast<uint64_t>(InterlockedCompareExchange64(
                    reinterpret_cast<volatile LONG64*>(&f->completed), static_cast<LONG64>(v), static_cast<LONG64>(mem)));
                if (was == mem) break;
                mem = was;
            }
            // The other way: the value is the title's memory, and it is written there too (a bottom-of-pipe write
            // to the address slot 12 gave, or the title itself). The PC fence follows, so that its waits (the
            // queues' Wait, SetEventOnCompletion) see what the title's readers see.
            if (mem > v && mem != UINT64_MAX) {
                f->host->Signal(mem);
                static std::atomic<uint64_t> raised{0};
                const uint64_t n = ++raised;
                if (n <= 4 || (n & (n - 1)) == 0)
                    std::fprintf(stderr, "[rt] fences: fence %p raised to %llu from its memory (the PC's was at %llu); "
                                         "%llu such so far\n",
                                 static_cast<void*>(f), static_cast<unsigned long long>(mem),
                                 static_cast<unsigned long long>(v), static_cast<unsigned long long>(n));
            }
        }
        check_waiters();  // the title, or a bottom-of-pipe write, may have written a fence's memory
        if (g_async) release_value_waits();
        Sleep(1);
    }
}

const volatile uint64_t* WINAPI fence_value_address(Fence* f) { return &f->completed; }

uint64_t WINAPI fence_completed_value(Fence* f) {
    return g_async ? f->completed : f->host->GetCompletedValue();
}
HRESULT WINAPI fence_set_event(Fence* f, uint64_t value, HANDLE event) {
    if (!g_async) return f->host->SetEventOnCompletion(value, event);
    if (f->completed >= value) return event ? (SetEvent(event), S_OK) : S_OK;
    if (!event) {  // no event: the call itself waits
        while (f->completed < value) Sleep(1);
        return S_OK;
    }
    {
        std::lock_guard lock(g_waiters_lock);
        g_waiters.push_back({f, value, event});
        g_waiter_count = g_waiters.size();
    }
    check_waiters();  // it may have got there meanwhile
    return S_OK;
}
HRESULT WINAPI fence_signal(Fence* f, uint64_t value) {
    HRESULT hr = f->host->Signal(value);
    if (SUCCEEDED(hr)) {
        f->completed = value;
        check_waiters();
    }
    return hr;
}

void** fence_table() {
    static void** vt = [] {
        void** t = shims::make_probe_vtable("D3D12Fence");
        shims::set_probe_method(t, 8, reinterpret_cast<void*>(&fence_completed_value));
        shims::set_probe_method(t, 9, reinterpret_cast<void*>(&fence_set_event));
        shims::set_probe_method(t, 10, reinterpret_cast<void*>(&fence_signal));
        shims::set_probe_method(t, 12, reinterpret_cast<void*>(&fence_value_address));
        return t;
    }();
    return vt;
}

// ---------------------------------------------------------------- queues

struct Pending;

struct Queue {
    Header header;
    ID3D12CommandQueue* host;
    D3D12_COMMAND_LIST_TYPE type;
    // Submissions finish before ExecuteCommandLists returns (the title's fences are signalled at once, and it reuses
    // allocators as soon as they are): a fence of ours to wait on.
    ID3D12Fence* done = nullptr;
    uint64_t done_value = 0;
    HANDLE done_event = nullptr;
    std::mutex m;
    std::vector<ID3D12GraphicsCommandList*> running;  // the title's lists of the submission being waited for
    // presentation (the queue PresentX is called on)
    IDXGISwapChain3* swapchain = nullptr;
    ID3D12CommandAllocator* present_allocator = nullptr;
    ID3D12GraphicsCommandList* present_list = nullptr;
    // before each submission: what the GPU must have of the title's memory (d3d12x_draw.cpp, prepare_submission)
    ID3D12CommandAllocator* pre_allocator = nullptr;
    ID3D12GraphicsCommandList* pre_list = nullptr;
    uint32_t width = 0, height = 0;
    // Submissions not waited for (g_async): what is still to be done once the GPU has run each, oldest first,
    // finished by complete_queue; pre-lists (with their allocators) free for another submission.
    std::mutex pending_lock;
    std::deque<Pending*> pending;
    std::vector<std::pair<ID3D12CommandAllocator*, ID3D12GraphicsCommandList*>> free_pre;
    uint64_t present_done = 0;  // the `done` value after the last present's list
    Queue* next_queue = nullptr;  // every queue, for the completion thread
};
std::atomic<Queue*> g_queues{nullptr};
HANDLE g_completion_event = nullptr;  // set when the GPU reaches a value the completion thread asked about

// Runs what the queue was given and waits for it.
// ---- where a frame's time goes (logged every 32 presents): microseconds in each part of the runtime
struct Perf {
    std::atomic<uint64_t> draw_us{0}, draws{0}, upload_us{0}, execute_us{0}, executes{0}, present_us{0};
    std::atomic<uint64_t> wait_us{0};  // inside the two above: blocked until the GPU was done
};
Perf g_perf;
uint64_t perf_now_us() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<uint64_t>(t.QuadPart * 1000000.0 / f.QuadPart);
}

// The queue's own fence moves to a new value once the GPU gets past what was submitted so far: the value. With
// `pending_lock` held (the values must reach the queue in order, and pending entries be kept in that order).
uint64_t mark_locked(Queue* q) {
    if (!q->done) {
        host_device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&q->done));
        q->done_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    const uint64_t v = ++q->done_value;
    q->host->Signal(q->done, v);
    return v;
}

// Blocks until the GPU has got to the queue's value `v`.
void wait_value(Queue* q, uint64_t v) {
    if (!q->done || !v) return;
    if (q->done->GetCompletedValue() < v) {
        struct Count {
            uint64_t t0 = perf_now_us();
            ~Count() { g_perf.wait_us += perf_now_us() - t0; }
        } count;
        q->done->SetEventOnCompletion(v, q->done_event);
        // A submission the GPU hasn't finished in 8 s is one command that doesn't end (the title's watchdog stops
        // the game at 30 s): the dispatches and mesh draws remembered on its lists are logged, once.
        if (WaitForSingleObject(q->done_event, 8000) == WAIT_TIMEOUT) {
            static std::atomic<int> told{0};
            if (told++ < 2) {
                std::fprintf(stderr, "[rt] d3d12: a submission of %zu list(s) on a queue of type %d has run for 8 s:\n",
                             q->running.size(), static_cast<int>(q->type));
                for (ID3D12GraphicsCommandList* l : q->running) {
                    std::fprintf(stderr, "[rt] d3d12:  list %p:\n", static_cast<void*>(l));
                    report_dispatches(l, 0);
                    report_mesh_draws(l, 0);
                }
            }
            WaitForSingleObject(q->done_event, INFINITE);
        }
    }
}

void wait_idle(Queue* q) {
    uint64_t v;
    {
        std::lock_guard lock(q->pending_lock);
        v = mark_locked(q);
    }
    wait_value(q, v);
}

// A title fence's signal, reached in its memory once the GPU has run what came before (complete_queue).
void pend_signal(Queue* q, Fence* f, uint64_t value);

// Submissions waited for (RT_SYNC_SUBMIT=1): the queue has run everything before the signal when it's made, so the
// value in memory moves at once, and the host fence follows. Not waited for: the PC fence is signalled on the queue,
// the memory moves after what came before it has run.
HRESULT signal_now(Queue* q, void* fence, uint64_t value) {
    ID3D12Fence* f = host_fence(fence);
    if (!f) return E_INVALIDARG;
    if (g_async) {
        std::lock_guard lock(q->m);  // in order with the queue's submissions
        HRESULT hr = q->host->Signal(f, value);
        if (SUCCEEDED(hr)) pend_signal(q, static_cast<Fence*>(fence), value);
        return hr;
    }
    HRESULT hr = q->host->Signal(f, value);
    if (SUCCEEDED(hr)) static_cast<Fence*>(fence)->completed = value;
    return hr;
}
bool hold_signal(Queue* q, void* fence, uint64_t value);
HRESULT WINAPI queue_signal(Queue* q, void* fence, uint64_t value) {
    if (!host_fence(fence)) return E_INVALIDARG;
    if (hold_signal(q, fence, value)) return S_OK;  // behind a held submission of this queue: reached after it
    return signal_now(q, fence, value);
}
// Slot 34: Signal with sync settings for the top and bottom of the pipe, (fence, value, bottom, top).
HRESULT WINAPI queue_signal_x(Queue* q, void* fence, uint64_t value, const void*, const void*) {
    return queue_signal(q, fence, value);
}
HRESULT WINAPI queue_wait(Queue* q, void* fence, uint64_t value) {
    ID3D12Fence* f = host_fence(fence);
    return f ? q->host->Wait(f, value) : E_INVALIDARG;
}
HRESULT WINAPI queue_timestamp_frequency(Queue* q, uint64_t* out) { return q->host->GetTimestampFrequency(out); }

struct Plane;
void WINAPI queue_execute(Queue* q, UINT count, void* const* lists);
HRESULT WINAPI queue_map_pages(Queue* q, uint64_t count, const uint8_t* ranges, uint64_t pool, uint64_t pool_pages);
void WINAPI queue_execute_x(Queue* q, UINT count, void* const* lists, uint64_t token, uint32_t flags);
HRESULT WINAPI queue_present(Queue* q, uint32_t planes, const Plane* plane, const void* parameters);

void** queue_table() {
    static void** vt = [] {
        void** t = shims::make_probe_vtable("D3D12CommandQueue");
        shims::set_probe_method(t, 16, reinterpret_cast<void*>(&queue_signal));
        shims::set_probe_method(t, 17, reinterpret_cast<void*>(&queue_wait));
        shims::set_probe_method(t, 18, reinterpret_cast<void*>(&queue_timestamp_frequency));
        shims::set_probe_method(t, 34, reinterpret_cast<void*>(&queue_signal_x));
        shims::set_probe_method(t, 10, reinterpret_cast<void*>(&queue_execute));
        shims::set_probe_method(t, 35, reinterpret_cast<void*>(&queue_execute_x));
        shims::set_probe_method(t, 32, reinterpret_cast<void*>(&queue_present));
        shims::set_probe_method(t, 31, reinterpret_cast<void*>(&queue_map_pages));
        return t;
    }();
    return vt;
}

void start_completion_thread();

// Console queue types: the PC's, plus 0x10 for the DMA engine, which is the PC's copy queue.
D3D12_COMMAND_LIST_TYPE host_list_type(uint32_t type) {
    return type == 0x10 ? D3D12_COMMAND_LIST_TYPE_COPY : static_cast<D3D12_COMMAND_LIST_TYPE>(type);
}

HRESULT make_queue(const D3D12_COMMAND_QUEUE_DESC* desc, void** out, const char* how) {
    ID3D12Device* dev = host_device();
    if (!dev) return DXGI_ERROR_DEVICE_REMOVED;
    if (!desc || !out) return E_INVALIDARG;
    D3D12_COMMAND_QUEUE_DESC d = *desc;
    d.Type = host_list_type(static_cast<uint32_t>(desc->Type));
    // Console priorities beyond the PC's (normal 0, high 100, global realtime 10000) become high.
    if (d.Priority != D3D12_COMMAND_QUEUE_PRIORITY_NORMAL && d.Priority != D3D12_COMMAND_QUEUE_PRIORITY_HIGH)
        d.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    d.Flags &= D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT;
    d.NodeMask = 0;
    ID3D12CommandQueue* host = nullptr;
    HRESULT hr = dev->CreateCommandQueue(&d, IID_PPV_ARGS(&host));
    std::fprintf(stderr, "[rt] d3d12: %s(type %x prio %d flags %x) -> %08lX\n", how, desc->Type, desc->Priority,
                 desc->Flags, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return hr;
    auto* q = new Queue{};
    q->header = {queue_table(), 0, 1};
    q->host = host;
    q->type = d.Type;
    {
        std::lock_guard lock(q->pending_lock);
        mark_locked(q);  // the queue's own fence, made now: the completion thread reads it without a lock
    }
    start_completion_thread();
    for (Queue* head = g_queues.load(); ;) {
        q->next_queue = head;
        if (g_queues.compare_exchange_weak(head, q)) break;
    }
    *out = q;
    return S_OK;
}

// ---------------------------------------------------------------- command allocators
//
// The PC's ID3D12CommandAllocator order: 8 Reset.

struct Allocator {
    Header header;
    ID3D12CommandAllocator* host;
    D3D12_COMMAND_LIST_TYPE type;
};

HRESULT WINAPI allocator_reset(Allocator* a) { return a->host->Reset(); }

void** allocator_table() {
    static void** vt = [] {
        void** t = shims::make_probe_vtable("D3D12CommandAllocator");
        shims::set_probe_method(t, 8, reinterpret_cast<void*>(&allocator_reset));
        return t;
    }();
    return vt;
}

Allocator* as_allocator(void* p) {
    auto* a = static_cast<Allocator*>(p);
    return a && a->header.vtbl == allocator_table() ? a : nullptr;
}

// ---------------------------------------------------------------- command lists
//
// The console list table: 0-27 as the PC's ID3D12GraphicsCommandList (so 8 GetType, 9 Close, 10 Reset), other PC
// methods moved, console methods in between; the newer layout 1.0.2.1 uses also has Reset at 139. For now only
// these are written; every other method is a probe.

// The console list object starts with the state the title's inlined D3D12.X writers use: a write pointer (+0x18),
// a limit (+0x28) and an end (+0x30) of the command buffer, and flag bytes around +0x140. A record goes into the
// buffer when there's room, else through the method it stands for. Ours never has room (all three equal), so every
// command arrives as a method call.
// A bottom-of-pipe write (list slots 71 and 72): `value` (or a timestamp) into title memory once the GPU has run
// everything recorded before it. The title polls that memory to learn the GPU is done (its streaming waits on it).
struct PipeWrite {
    uint64_t address, value;
    uint32_t bytes, flags;  // flags: 1 the GPU timestamp instead of the value, 2 the system timestamp (64-bit form)
};

// A value the GPU is to wait for before going on with a list (WaitOnValue): on the console a list of one queue is
// held until a list of another has written the memory (VIctory: the compute queue's lighting passes wait for a value
// the graphics queue's depth pass writes at the bottom of the pipe; run at once they read a cleared depth).
struct PipeWait {
    uint64_t address, reference;
    uint32_t comparison, bytes;
};

struct List {
    Header header;           // +0x00
    uint64_t console0;       // +0x10
    uint8_t* write;          // +0x18
    uint64_t console1;       // +0x20
    uint8_t* limit;          // +0x28
    uint8_t* end;            // +0x30
    uint8_t console2[0x200 - 0x38];
    ID3D12GraphicsCommandList* host;
    D3D12_COMMAND_LIST_TYPE type;
    bool closed;  // only closed lists may be submitted (an open one would remove the device)
    GfxState gfx;  // what the title set, for draws (d3d12x_draw.cpp)
    GfxState compute;  // the same for dispatches: pso, root signature and root arguments
    std::vector<PipeWrite> writes;  // bottom-of-pipe value writes, done once the list has run (queue_execute)
    std::vector<PipeWait> waits;    // the values the list waits for (looked at when it is submitted)
    // since the last Reset: SetPipelineState calls and the last object passed, and what Reset got as the initial
    // pipeline (for the report of draws that find no pipeline on the list)
    uint32_t pso_sets = 0;
    void* pso_last = nullptr;
    void* pso_initial = nullptr;
    int reset_slot = 0;
};
static_assert(offsetof(List, write) == 0x18 && offsetof(List, limit) == 0x28 && offsetof(List, end) == 0x30);

void** list_table();
alignas(64) uint8_t g_no_room[64];  // where every list's write pointer, limit and end point

List* new_list(ID3D12GraphicsCommandList* host, D3D12_COMMAND_LIST_TYPE type) {
    auto* l = new List{};
    l->header = {list_table(), 0, 1};
    l->write = l->limit = l->end = g_no_room;
    l->host = host;
    l->type = type;
    l->closed = false;  // made open
    return l;
}

D3D12_COMMAND_LIST_TYPE WINAPI list_type(List* l) { return l->type; }
HRESULT WINAPI list_close(List* l) {
    if (!l->closed) release_targets(l->gfx, l->host);  // lists end with every target in the common state
    HRESULT hr = l->host->Close();
    l->closed = SUCCEEDED(hr);
    // A list the PC refuses to close recorded a call it found invalid; running it would remove the device.
    static std::atomic<int> told{0};
    if (FAILED(hr) && told++ < 8)
        std::fprintf(stderr, "[rt] d3d12: a list of type %d failed to close: %08lX\n", static_cast<int>(l->type),
                     static_cast<unsigned long>(hr));
    return hr;
}
void WINAPI list_pipeline(List* l, void* p);
bool read_title_memory(void* out, uint64_t va, size_t bytes);
// HRESULT Reset(allocator, initial pipeline) (slot 10): as on the PC, the initial pipeline is the list's pipeline
// until SetPipelineState. Slot 139 (the newer layout's Reset) is taken to have the same arguments; what it gets
// third is kept for the report of draws with no pipeline, and set only if it is a pipeline of ours.
HRESULT reset_list(List* l, void* allocator, void* initial, int slot) {
    Allocator* a = as_allocator(allocator);
    if (!a) return E_INVALIDARG;
    if (!l->closed) {  // a list made open, reset by the title before any Close
        release_targets(l->gfx, l->host);
        l->host->Close();
    }
    HRESULT hr = l->host->Reset(a->host, nullptr);
    l->closed = false;
    l->gfx = {};
    l->compute = {};
    l->writes.clear();
    l->waits.clear();
    uint64_t first = 0;
    if (initial && read_title_memory(&first, reinterpret_cast<uint64_t>(initial), sizeof first) && as_pipeline(initial))
        list_pipeline(l, initial);
    l->pso_sets = 0;
    l->pso_last = nullptr;
    l->pso_initial = initial;
    l->reset_slot = slot;
    return hr;
}
HRESULT WINAPI list_reset(List* l, void* allocator, void* initial) { return reset_list(l, allocator, initial, 10); }
HRESULT WINAPI list_reset_x(List* l, void* allocator, void* initial) { return reset_list(l, allocator, initial, 139); }

// void WriteValueBottomOfPipe(address, value, flush, flags), 32 bits (slot 71) and 64 bits (slot 72), as the ztrx
// project reads 1.0.2.1's call sites: kept on the list and done when a submission of it has run.
std::atomic<uint64_t> g_pipe_writes{0};
void WINAPI list_write32(List* l, uint64_t address, uint32_t value, uint32_t, uint32_t flags) {
    l->writes.push_back({address, value, 4, flags});
}
void WINAPI list_write64(List* l, uint64_t address, uint64_t value, uint32_t, uint32_t flags) {
    l->writes.push_back({address, value, 8, flags});
}
// void WaitOnValue(address, comparison, reference), 32 bits (73) and 64 bits (74): the GPU holds the list until the
// memory compares true. Submissions here run to completion one at a time, in the order the title submits them, and
// nothing is held back: a list submitted before the one that writes its value runs early. Whether that happens is
// counted (note_waits, at each submission) before anything is built to hold lists.
void WINAPI list_wait_value32(List* l, uint64_t address, uint32_t comparison, uint64_t reference) {
    if (l->waits.size() < 64) l->waits.push_back({address, reference, comparison, 4});
}
void WINAPI list_wait_value64(List* l, uint64_t address, uint32_t comparison, uint64_t reference) {
    if (l->waits.size() < 64) l->waits.push_back({address, reference, comparison, 8});
}

std::atomic<uint64_t> g_waits{0}, g_waits_equal{0}, g_wait_lists{0};
// A list about to run: what its waits ask for and what the memory holds now.
void note_waits(List* l, int queue_type) {
    if (l->waits.empty()) return;
    ++g_wait_lists;
    for (const PipeWait& w : l->waits) {
        uint64_t now = 0;
        const bool readable = read_title_memory(&now, w.address, w.bytes);
        const uint64_t ref = w.bytes == 4 ? static_cast<uint32_t>(w.reference) : w.reference;
        const uint64_t n = ++g_waits;
        if (readable && now == ref) ++g_waits_equal;
        if (n <= 12 || (n & (n - 1)) == 0)
            std::fprintf(stderr,
                         "[rt] waits: #%llu on a list of queue type %d: %u bytes at %llx, comparison %u, reference %llx; "
                         "the memory now: %s%llx. So far %llu waits on %llu lists, %llu found the reference there\n",
                         static_cast<unsigned long long>(n), queue_type, w.bytes,
                         static_cast<unsigned long long>(w.address), w.comparison, static_cast<unsigned long long>(ref),
                         readable ? "" : "(unreadable) ", static_cast<unsigned long long>(now),
                         static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_wait_lists.load()),
                         static_cast<unsigned long long>(g_waits_equal.load()));
    }
}

void apply_pipe_writes(const std::vector<PipeWrite>& writes) {
    for (const PipeWrite& w : writes) {
        uint64_t v = w.value;
        if (w.bytes == 8 && (w.flags & 3)) {  // a timestamp: the performance counter stands for both clocks
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            v = static_cast<uint64_t>(t.QuadPart);
        }
        if (!w.address || IsBadWritePtr(reinterpret_cast<void*>(w.address), w.bytes)) continue;
        std::memcpy(reinterpret_cast<void*>(w.address), &v, w.bytes);
        uint64_t n = ++g_pipe_writes;
        if ((n & (n - 1)) == 0)
            std::fprintf(stderr, "[rt] pipe writes: %llu done (last: %u bytes %llx at %llx)\n", (unsigned long long)n,
                         w.bytes, (unsigned long long)v, (unsigned long long)w.address);
    }
}

// ---- graphics state and draws (console slots: the PC's from 28 on are 4 further; see d3d12x_draw.cpp)
bool recording(List* l) { return l->type == D3D12_COMMAND_LIST_TYPE_DIRECT && !l->closed; }

// 12 DrawInstanced, 13 DrawIndexedInstanced
// Copies title memory that may not be mapped; false if it faults.
bool read_title_memory(void* out, uint64_t va, size_t bytes) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(va), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Draws on a list with no pipeline set: the first few per call site, with the list's calls before them.
void note_unpiped(List* l, void* from, const char* what, UINT count, UINT instances) {
    static std::mutex m;
    static void* sites[12];
    static int seen[12], n = 0;
    std::lock_guard lock(m);
    int i = 0;
    while (i < n && sites[i] != from) ++i;
    if (i == n) {
        if (n == 12) return;
        sites[n++] = from;
    }
    if (seen[i]++ >= 2) return;
    std::fprintf(stderr, "[rt] draws: no pipeline on list %p for %s(%u x %u) from %p; root %p, %u target(s), state "
                         "block %p; since its Reset (slot %d, initial pipeline %p): %u SetPipelineState, the last "
                         "with %p\n",
                 static_cast<void*>(l), what, count, instances, from, static_cast<void*>(l->gfx.root), l->gfx.rtv_count,
                 l->gfx.state_block, l->reset_slot, l->pso_initial, l->pso_sets, l->pso_last);
    if (void* first = state_block_first(l->gfx.state_block);
        reinterpret_cast<uintptr_t>(first) > 0x10000 && !IsBadReadPtr(first, 16))
        std::fprintf(stderr, "[rt] draws:   its state block was made from %p: %s\n", first,
                     as_pipeline(first) ? "a pipeline of ours" : shims::probe_object_name(first)
                                                                     ? shims::probe_object_name(first) : "unknown");
    shims::print_recent_probes(l, "[rt] draws:   before it:");
}

// HRESULT/void ExecuteIndirect(signature, max count, argument buffer, offset, count buffer, offset) (slot 61; the
// PC's 59). Every signature of 1.0.2.1 has one argument and every call one command. Run on the PC from the title's
// own argument memory, read when the list runs (the culling and tile passes write it on the GPU): mesh draws
// (DispatchMesh, type 10) and dispatches (type 2, and the console's 105, a "single-pass dispatch" with the same
// three dwords). Draws (0) and indexed draws (1) aren't run yet. Each kind is counted; the first calls of each
// signature are logged.
std::atomic<uint64_t> g_indirects{0}, g_indirect_kind[5], g_indirect_run[5], g_indirect_bad{0};
void WINAPI list_dispatch(List* l, UINT x, UINT y, UINT z);
void WINAPI list_execute_indirect(List* l, void* signature, UINT max_count, void* arguments, uint64_t offset,
                                  void* counts, uint64_t count_offset) {
    const uint64_t n = ++g_indirects;
    IndirectSignature s{};
    const bool known = indirect_signature(signature, &s);
    {
        Resource* a = as_resource(arguments);
        Resource* c = as_resource(counts);
        const int type = known && s.count == 1 ? static_cast<int>(s.arguments[0].Type) : -1;
        // 0 draw, 1 indexed draw, 2 dispatch (and 105), 3 mesh draw, 4 anything else
        const int kind = type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW ? 0
                         : type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ? 1
                         : type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH || type == 105 ? 2
                         : type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH ? 3 : 4;
        ++g_indirect_kind[kind];
        // (dispatches come on the compute queue's lists too; draws only on the direct ones)
        const bool open = kind == 2 ? !l->closed && (l->type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
                                                     l->type == D3D12_COMMAND_LIST_TYPE_COMPUTE)
                                    : recording(l);
        if (kind <= 3 && max_count == 1 && a && a->va && !c && open) {
            // What the title's own code left there (its GPU passes write over it before the list runs): a count
            // no pass could mean (the title leaves all ones in records it doesn't fill, as VIctory found) would
            // hold the GPU for minutes, so such a record isn't run. A draw's are vertices (or indices) and
            // instances; a dispatch's or a mesh draw's the three group counts.
            uint32_t w[3] = {};
            const bool sane = !read_title_memory(w, a->va + offset, sizeof w) ||
                              (kind <= 1 ? w[0] < (1u << 23) && w[1] <= 0xFFFF
                                         : w[0] <= 0x100000 && w[1] <= 0xFFFF && w[2] <= 0xFFFF);
            if (!sane) {
                ++g_indirect_bad;
            } else if (kind <= 1) {
                draw(l->gfx, l->host, kind == 1, 0, 1, 0, 0, 0, a->va + offset);
                ++g_indirect_run[kind];
            } else if (kind == 3) {
                draw_mesh(l->gfx, l->host, 0, a->va + offset);
                ++g_indirect_run[kind];
            } else {
                release_targets(l->gfx, l->host);
                dispatch(l->compute, l->host, 1, 1, 1, a->va + offset);
                ++g_indirect_run[kind];
            }
        }
        if ((n & (n - 1)) == 0 && n >= 1024)
            std::fprintf(stderr,
                         "[rt] indirect: %llu calls: %llu draws (%llu recorded), %llu indexed draws (%llu recorded), %llu "
                         "dispatches (%llu recorded), %llu mesh draws (%llu recorded), %llu other; %llu left out for "
                         "their counts\n",
                         static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_indirect_kind[0].load()),
                         static_cast<unsigned long long>(g_indirect_run[0].load()),
                         static_cast<unsigned long long>(g_indirect_kind[1].load()),
                         static_cast<unsigned long long>(g_indirect_run[1].load()),
                         static_cast<unsigned long long>(g_indirect_kind[2].load()),
                         static_cast<unsigned long long>(g_indirect_run[2].load()),
                         static_cast<unsigned long long>(g_indirect_kind[3].load()),
                         static_cast<unsigned long long>(g_indirect_run[3].load()),
                         static_cast<unsigned long long>(g_indirect_kind[4].load()),
                         static_cast<unsigned long long>(g_indirect_bad.load()));
    }
    static std::mutex m;
    static void* sigs[32];
    static int sig_count = 0, shown[32];
    int i = 0;
    {
        std::lock_guard lock(m);
        while (i < sig_count && sigs[i] != signature) ++i;
        if (i == sig_count) {
            if (sig_count == 32) return;
            sigs[sig_count++] = signature;
        }
        if (shown[i]++ >= 3 && (n & (n - 1))) return;
    }
    Resource* a = as_resource(arguments);
    Resource* c = as_resource(counts);
    const bool is_dispatch = known && s.count == 1 && (s.arguments[0].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH ||
                                                       static_cast<int>(s.arguments[0].Type) == 105);
    const PipelineState* p = is_dispatch ? l->compute.pso : l->gfx.pso;
    std::fprintf(stderr, "[rt] indirect: #%llu on list %p from %p: signature %p (%s, stride %u, %zu argument(s):",
                 static_cast<unsigned long long>(n), static_cast<void*>(l), _ReturnAddress(), signature,
                 known ? "ours" : "unknown", s.stride, s.count);
    for (size_t k = 0; k < s.count; ++k) {
        const D3D12_INDIRECT_ARGUMENT_DESC& d = s.arguments[k];
        std::fprintf(stderr, " type %d [%u %u %u]", static_cast<int>(d.Type), d.Constant.RootParameterIndex,
                     d.Constant.DestOffsetIn32BitValues, d.Constant.Num32BitValuesToSet);
    }
    std::fprintf(stderr, "), max %u, arguments %p + %#llx, counts %p + %#llx; pipeline %p (%s), %u target(s), topology %d\n",
                 max_count, a ? reinterpret_cast<void*>(a->va) : nullptr, static_cast<unsigned long long>(offset),
                 c ? reinterpret_cast<void*>(c->va) : nullptr, static_cast<unsigned long long>(count_offset),
                 static_cast<const void*>(p),
                 !p ? "none" : p->code[0].empty() ? "no shader" : p->compute ? "compute" : "drawable", l->gfx.rtv_count,
                 static_cast<int>(l->gfx.topology));
    if (a && a->va) {
        uint32_t w[16] = {};
        const uint32_t bytes = s.stride && s.stride < sizeof w ? s.stride : sizeof w;
        if (!read_title_memory(w, a->va + offset, bytes)) {
            std::fprintf(stderr, "[rt] indirect:   (arguments unreadable)\n");
            return;
        }
        std::fprintf(stderr, "[rt] indirect:   arguments now:");
        for (uint32_t k = 0; k < bytes / 4; ++k) std::fprintf(stderr, " %x", w[k]);
        std::fprintf(stderr, "\n");
    }
}

// DispatchMesh(x, y, z) (slot 129; 100 is the console's DispatchMeshX with more arguments after them): the title's
// mesh programs are one-dimensional.
void WINAPI list_dispatch_mesh(List* l, UINT x, UINT, UINT) {
    if (recording(l) && x) draw_mesh(l->gfx, l->host, x, 0);
}

void WINAPI list_draw(List* l, UINT vertices, UINT instances, UINT start_vertex, UINT start_instance) {
    if (!l->gfx.pso && recording(l)) note_unpiped(l, _ReturnAddress(), "draw", vertices, instances);
    if (diag::listing()) std::fprintf(stderr, "[rt] diag:   (draw called from %p)\n", _ReturnAddress());
    uint64_t t0 = perf_now_us();
    if (recording(l)) draw(l->gfx, l->host, false, vertices, instances, start_vertex, 0, start_instance);
    g_perf.draw_us += perf_now_us() - t0;
    ++g_perf.draws;
}
void WINAPI list_draw_indexed(List* l, UINT indices, UINT instances, UINT start_index, INT base_vertex,
                              UINT start_instance) {
    if (!l->gfx.pso && recording(l)) note_unpiped(l, _ReturnAddress(), "indexed draw", indices, instances);
    if (diag::listing()) std::fprintf(stderr, "[rt] diag:   (draw called from %p)\n", _ReturnAddress());
    uint64_t t0 = perf_now_us();
    if (recording(l)) draw(l->gfx, l->host, true, indices, instances, start_index, base_vertex, start_instance);
    g_perf.draw_us += perf_now_us() - t0;
    ++g_perf.draws;
}
// 20 IASetPrimitiveTopology, 21 RSSetViewports, 22 RSSetScissorRects
void WINAPI list_topology(List* l, D3D12_PRIMITIVE_TOPOLOGY t) { l->gfx.topology = t; }
void WINAPI list_viewports(List* l, UINT n, const D3D12_VIEWPORT* v) {
    l->gfx.viewport_count = v ? (n < 16 ? n : 16) : 0;
    if (l->gfx.viewport_count) std::memcpy(l->gfx.viewports, v, l->gfx.viewport_count * sizeof *v);
}
void WINAPI list_scissors(List* l, UINT n, const D3D12_RECT* r) {
    l->gfx.scissor_count = r ? (n < 16 ? n : 16) : 0;
    if (l->gfx.scissor_count) std::memcpy(l->gfx.scissors, r, l->gfx.scissor_count * sizeof *r);
}
// 25 SetPipelineState, 34 SetGraphicsRootSignature
// 25 SetPipelineState: a pipeline, or one of the console's state blocks, which sets fixed-function state and leaves
// the pipeline as it was.
void WINAPI list_pipeline(List* l, void* p) {
    ++l->pso_sets;
    l->pso_last = p;
    if (p && is_object(p, state_block_table())) {
        // A state block is made from a pipeline (slot 55's first argument) and is set in its place: the draws after
        // it use that pipeline. (Seen in 1.0.2.1: lists whose only SetPipelineState since Reset is a state block,
        // then draws; over 120,000 draws in five minutes of the open world found no pipeline before this.)
        l->gfx.state_block = p;
        if (PipelineState* base = as_pipeline(state_block_first(p))) {
            if (base->compute)
                l->compute.pso = base;
            else
                l->gfx.pso = base;
        }
        return;
    }
    if (PipelineState* q = as_pipeline(p); q && q->compute) {  // a compute pipeline: the dispatches' own
        l->compute.pso = q;
        return;
    }
    l->gfx.pso = as_pipeline(p);
    l->gfx.state_block = nullptr;  // a pipeline of its own: the last block's state goes with the block
    if (!l->gfx.pso) {  // null, or an object that isn't a pipeline of ours: the draws after it have nothing to draw with
        static std::atomic<int> told{0};
        if (told++ < 12)
            std::fprintf(stderr, "[rt] d3d12: SetPipelineState(%p) on list %p from %p: %s\n", p, static_cast<void*>(l),
                         _ReturnAddress(),
                         !p ? "null" : shims::probe_object_name(p) ? shims::probe_object_name(p) : "not an object of ours");
    }
    if (p && l->gfx.pso && diag::listing())
        std::fprintf(stderr, "[rt] diag:   SetPipelineState(%p) on list %p: %s\n", p, static_cast<void*>(l),
                     l->gfx.pso->compute ? "compute" : "graphics");
    if (p && !l->gfx.pso && diag::listing())
        std::fprintf(stderr, "[rt] diag:   SetPipelineState(%p): not a pipeline of ours (%s, vtable %p), from %p\n", p,
                     shims::probe_object_name(p) ? shims::probe_object_name(p) : "unknown", *static_cast<void**>(p),
                     _ReturnAddress());
}
void WINAPI list_graphics_root(List* l, void* r) {
    RootSignature* rs = as_root_signature(r);
    if (rs != l->gfx.root) {  // a new root signature: its arguments start over, as on the PC
        GfxState kept = l->gfx;
        l->gfx = {};
        l->gfx.pso = kept.pso;
        l->gfx.topology = kept.topology;
        std::memcpy(l->gfx.viewports, kept.viewports, sizeof kept.viewports);
        l->gfx.viewport_count = kept.viewport_count;
        std::memcpy(l->gfx.scissors, kept.scissors, sizeof kept.scissors);
        l->gfx.scissor_count = kept.scissor_count;
        l->gfx.ib = kept.ib;
        std::memcpy(l->gfx.vb, kept.vb, sizeof kept.vb);
        l->gfx.vb_count = kept.vb_count;
        std::memcpy(l->gfx.rtv, kept.rtv, sizeof kept.rtv);
        l->gfx.rtv_count = kept.rtv_count;
        l->gfx.dsv = kept.dsv;
        l->gfx.stencil_ref = kept.stencil_ref;
        l->gfx.stencil_ref_set = kept.stencil_ref_set;
        l->gfx.used = std::move(kept.used);
        l->gfx.state_block = kept.state_block;
    }
    l->gfx.root = rs;
}
// 36 SetGraphicsRootDescriptorTable, 38 SetGraphicsRoot32BitConstant, 40 SetGraphicsRoot32BitConstants,
// 42 / 44 / 46 SetGraphicsRoot{ConstantBuffer,ShaderResource,UnorderedAccess}View
void WINAPI list_graphics_table(List* l, UINT param, uint64_t handle) {
    if (param < GfxState::kParams) l->gfx.tables[param] = handle;
}
void WINAPI list_graphics_constant(List* l, UINT param, UINT value, UINT dest) {
    if (param < GfxState::kParams && dest < 64) l->gfx.constants[param][dest] = value;
}
void WINAPI list_graphics_constants(List* l, UINT param, UINT n, const void* data, UINT dest) {
    if (param >= GfxState::kParams || !data || dest >= 64) return;
    std::memcpy(&l->gfx.constants[param][dest], data, (n < 64 - dest ? n : 64 - dest) * 4);
}
void WINAPI list_graphics_root_view(List* l, UINT param, uint64_t va) {
    if (param < GfxState::kParams) l->gfx.root_va[param] = va;
}
// The compute ones (the PC's 29-41, 4 further): 33 SetComputeRootSignature, 35 SetComputeRootDescriptorTable,
// 37 SetComputeRoot32BitConstant, 39 SetComputeRoot32BitConstants, 41 / 43 / 45 SetComputeRoot{CBV,SRV,UAV}
void WINAPI list_compute_root(List* l, void* r) {
    RootSignature* rs = as_root_signature(r);
    if (rs != l->compute.root) {  // a new root signature: its arguments start over
        GfxState kept = std::move(l->compute);
        l->compute = {};
        l->compute.pso = kept.pso;
        l->compute.used = std::move(kept.used);
    }
    l->compute.root = rs;
}
void WINAPI list_compute_table(List* l, UINT param, uint64_t handle) {
    if (param < GfxState::kParams) l->compute.tables[param] = handle;
}
void WINAPI list_compute_constant(List* l, UINT param, UINT value, UINT dest) {
    if (param < GfxState::kParams && dest < 64) l->compute.constants[param][dest] = value;
}
void WINAPI list_compute_constants(List* l, UINT param, UINT n, const void* data, UINT dest) {
    if (param >= GfxState::kParams || !data || dest >= 64) return;
    std::memcpy(&l->compute.constants[param][dest], data, (n < 64 - dest ? n : 64 - dest) * 4);
}
void WINAPI list_compute_root_view(List* l, UINT param, uint64_t va) {
    if (param < GfxState::kParams) l->compute.root_va[param] = va;
}
// 47 IASetIndexBuffer, 48 IASetVertexBuffers, 50 OMSetRenderTargets
void WINAPI list_index_buffer(List* l, const D3D12_INDEX_BUFFER_VIEW* v) { l->gfx.ib = v ? *v : D3D12_INDEX_BUFFER_VIEW{}; }
void WINAPI list_vertex_buffers(List* l, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) {
    for (UINT i = 0; i < n && start + i < 16; ++i) l->gfx.vb[start + i] = v ? v[i] : D3D12_VERTEX_BUFFER_VIEW{};
    UINT end = start + n < 16 ? start + n : 16;
    if (end > l->gfx.vb_count) l->gfx.vb_count = end;
}
void WINAPI list_targets(List* l, UINT n, const uint64_t* rtvs, BOOL single, const uint64_t* dsv) {
    l->gfx.rtv_count = rtvs ? (n < 8 ? n : 8) : 0;
    // (with the single-range flag the handles follow the first one in its heap, 128 bytes apart: the console's
    // increment for target views, as VIctory reads it; every target was taken to be the first before)
    for (UINT i = 0; i < l->gfx.rtv_count; ++i) l->gfx.rtv[i] = single ? rtvs[0] + uint64_t{128} * i : rtvs[i];
    l->gfx.dsv = dsv ? *dsv : 0;
}

// void OMSetStencilRef(reference) (slot 24).
std::atomic<uint64_t> g_stencil_refs{0};
void WINAPI list_stencil_ref(List* l, UINT ref) {
    l->gfx.stencil_ref = ref;
    l->gfx.stencil_ref_set = true;
    const uint64_t n = ++g_stencil_refs;
    if (n <= 8 || (n & (n - 1)) == 0)
        std::fprintf(stderr, "[rt] stencil: reference %u set on list %p (%llu so far)\n", ref, static_cast<void*>(l),
                     static_cast<unsigned long long>(n));
}

std::atomic<uint64_t> g_clears{0}, g_depth_clears{0};

// void ClearDepthStencilView(handle, flags, depth, stencil, rect count, rects) (slot 51): on the PC depth texture,
// in the common state between our commands. Flags: 1 depth, 2 stencil.
void WINAPI list_clear_dsv(List* l, uint64_t handle, UINT flags, float depth, UINT8 stencil, UINT count,
                           const D3D12_RECT* rects) {
    if (l->type != D3D12_COMMAND_LIST_TYPE_DIRECT || l->closed) return;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    ID3D12Resource* texture;
    DXGI_FORMAT format;
    bool read_only;
    if (!host_dsv(handle, &dsv, &texture, &format, &read_only)) return;
    const bool has_stencil = format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    UINT what = flags & (D3D12_CLEAR_FLAG_DEPTH | (has_stencil ? D3D12_CLEAR_FLAG_STENCIL : 0));
    if (!what || read_only) return;  // (a clear through a read-only view isn't one the PC takes)
    release_targets(l->gfx, l->host);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE};
    l->host->ResourceBarrier(1, &b);
    l->host->ClearDepthStencilView(dsv, static_cast<D3D12_CLEAR_FLAGS>(what), depth, stencil, rects ? count : 0, rects);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    l->host->ResourceBarrier(1, &b);
    const uint64_t n = ++g_depth_clears;
    if (n <= 3 || (n & (n - 1)) == 0)
        std::fprintf(stderr, "[rt] clear: depth texture %p (format %d) to %.3f / %u, flags %u, %u rects; %llu so far\n",
                     static_cast<void*>(texture), static_cast<int>(format), depth, stencil, flags, count,
                     static_cast<unsigned long long>(n));
}

// void ClearRenderTargetView(handle, const float color[4], rect count, rects) (slot 52): on the PC texture, which
// stays in the common state between our commands.
void WINAPI list_clear_rtv(List* l, uint64_t handle, const float* color, UINT count, const D3D12_RECT* rects) {
    if (l->type != D3D12_COMMAND_LIST_TYPE_DIRECT || l->closed || !color) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    ID3D12Resource* texture;
    if (!host_rtv(handle, &rtv, &texture)) return;
    release_targets(l->gfx, l->host);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_RENDER_TARGET};
    l->host->ResourceBarrier(1, &b);
    l->host->ClearRenderTargetView(rtv, color, count, rects);
    if (g_clears < 3 || diag::listing())
        std::fprintf(stderr, "[rt] clear: texture %p to (%.2f %.2f %.2f %.2f), %u rects\n", static_cast<void*>(texture),
                     color[0], color[1], color[2], color[3], count);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    l->host->ResourceBarrier(1, &b);
    ++g_clears;
    diag::cleared(texture);
}

// void CopyBufferRegion(dst, dst offset, src, src offset, bytes) (slot 15): between the title's buffers, which live in
// uma's pool chunks: the same copy between the chunks' PC buffers, a run of consecutive pool pages at a time (a
// buffer's pages are usually consecutive; a split range copies piecewise). The chunks stay in the common state;
// buffers are promoted to copy source / destination implicitly and decay at the end of the submission.
std::atomic<uint64_t> g_copies{0}, g_copies_failed{0}, g_copies_within{0};

// The buffer copies within one chunk pass through: kept in the copy-destination state between uses, a ring (lists run
// one submission at a time, so a region is free again long before the ring comes back to it).
constexpr uint64_t kCopyScratch = 16ull << 20;
std::atomic<uint64_t> g_copy_scratch_at{0};
ID3D12Resource* copy_scratch() {
    static std::mutex m;
    static ID3D12Resource* scratch = nullptr;
    std::lock_guard lock(m);
    if (!scratch) {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = kCopyScratch;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (ID3D12Device* dev = host_device())
            dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&scratch));
    }
    return scratch;
}

const bool g_skip_copies = [] {
    char v[4] = {};
    return GetEnvironmentVariableA("RT_NO_COPY", v, sizeof(v)) && v[0] == '1';
}();

void WINAPI list_copy_buffer(List* l, void* dst, uint64_t dst_offset, void* src, uint64_t src_offset, uint64_t bytes) {
    Resource* d = as_resource(dst);
    Resource* s = as_resource(src);
    if (!l->host || l->closed || !d || !s || !bytes) return;
    uint64_t dva = d->va + dst_offset, sva = s->va + src_offset, left = bytes;
    bool ok = true;
    while (left && ok) {
        // the part up to the next 64 KiB page boundary of either side
        uint64_t piece = left;
        piece = std::min<uint64_t>(piece, 0x10000 - (dva & 0xFFFF));
        piece = std::min<uint64_t>(piece, 0x10000 - (sva & 0xFFFF));
        ID3D12Resource *db = nullptr, *sb = nullptr;
        uint64_t doff = 0, soff = 0;
        // the whole rest in one go when both sides are contiguous
        if (gpumem::host_buffer(dva, left, &db, &doff) && gpumem::host_buffer(sva, left, &sb, &soff)) piece = left;
        else ok = gpumem::host_buffer(dva, piece, &db, &doff) && gpumem::host_buffer(sva, piece, &sb, &soff);
        if (!ok) break;
        if (g_skip_copies) {
            // RT_NO_COPY=1 (a diagnostic): the copy isn't recorded
        } else if (db != sb) {
            l->host->CopyBufferRegion(db, doff, sb, soff, piece);
        } else {
            // Both sides in one pool chunk, which is one PC buffer: the PC refuses a copy within a resource (the
            // list fails to close and the device is removed with it), so it goes through a buffer of ours.
            piece = std::min<uint64_t>(piece, kCopyScratch);
            ID3D12Resource* scratch = copy_scratch();
            if (!scratch) {
                ok = false;
                break;
            }
            uint64_t at = g_copy_scratch_at.fetch_add(piece) % kCopyScratch;
            if (at + piece > kCopyScratch) at = 0;
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = scratch;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            l->host->CopyBufferRegion(scratch, at, sb, soff, piece);
            l->host->ResourceBarrier(1, &b);
            l->host->CopyBufferRegion(db, doff, scratch, at, piece);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            l->host->ResourceBarrier(1, &b);
            ++g_copies_within;
        }
        dva += piece, sva += piece, left -= piece;
    }
    uint64_t n = ++g_copies;
    if (!ok) ++g_copies_failed;
    if ((n & (n - 1)) == 0)
        std::fprintf(stderr,
                     "[rt] copies: %llu buffer copies, %llu within a chunk, %llu not on the GPU (last: %llu bytes, %s)\n",
                     (unsigned long long)n, (unsigned long long)g_copies_within.load(),
                     (unsigned long long)g_copies_failed.load(), (unsigned long long)bytes,
                     ok ? "copied" : "memory the GPU doesn't have");
}

void WINAPI list_dispatch(List* l, UINT x, UINT y, UINT z);

// void DispatchX(x, y, z, flags) (slot 80): the console's Dispatch with flags. With flag 0x20 the three counts are
// threads, not thread groups (as the VIctory project reads the title's call sites): the groups are the counts over
// the pipeline's group size, rounded up. The size is in the compute pipeline's register block (dword 3 =
// NUM_THREAD_X | NUM_THREAD_Y << 16, dword 4 = NUM_THREAD_Z). Run as groups, such a dispatch did a group's worth of
// threads times too much work: 64 times for an 8 x 8 group. The threads of the last, partial group past the count
// still run here (the console masks them): not handled yet.
std::atomic<uint64_t> g_dispatch_x{0}, g_dispatch_threads{0};

void WINAPI list_dispatch_x(List* l, UINT x, UINT y, UINT z, UINT flags) {
    const uint64_t n = ++g_dispatch_x;
    UINT gx = x, gy = y, gz = z;
    bool sized = false;
    if (flags & 0x20) {
        ++g_dispatch_threads;
        // the size: the translated shader's own (what the PC will run), else the pipeline's register block
        const PipelineState* p = l->compute.pso;
        UINT size[3] = {};
        bool known = p && p->cs_group[0];
        if (known) std::memcpy(size, p->cs_group, sizeof size);
        if (!known && p && p->state.size() >= 0x1c) {
            uint32_t w[2];
            std::memcpy(w, p->state.data() + 12, 8);
            size[0] = w[0] & 0xFFFF, size[1] = w[0] >> 16, size[2] = w[1] & 0xFFFF;
            known = size[0] && size[1] && size[2];
        }
        if (known) {
            gx = (x + size[0] - 1) / size[0], gy = (y + size[1] - 1) / size[1], gz = (z + size[2] - 1) / size[2];
            sized = true;
        }
    }
    static std::atomic<int> told{0};
    if (((flags & 0x20) && told++ < 6) || (n & (n - 1)) == 0)
        std::fprintf(stderr, "[rt] dispatches: DispatchX #%llu (%u, %u, %u, flags %#x)%s%s; %llu so far counted in threads\n",
                     static_cast<unsigned long long>(n), x, y, z, flags,
                     flags & 0x20 ? (sized ? " -> groups " : " in threads, group size unknown: run as given") : "",
                     flags & 0x20 && sized
                         ? (std::to_string(gx) + " x " + std::to_string(gy) + " x " + std::to_string(gz)).c_str() : "",
                     static_cast<unsigned long long>(g_dispatch_threads.load()));
    list_dispatch(l, gx, gy, gz);
}

// void Dispatch(x, y, z) (slot 14): run on the PC with the list's compute arguments (d3d12x_draw.cpp).
void WINAPI list_dispatch(List* l, UINT x, UINT y, UINT z) {
    if (l->type != D3D12_COMMAND_LIST_TYPE_DIRECT && l->type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return;
    diag::dispatched();
    if (!l->closed) {
        release_targets(l->gfx, l->host);  // what it writes may be a texture a draw was rendering to
        dispatch(l->compute, l->host, x, y, z);
    }
    if (diag::listing()) {
        char cs[40] = "-";
        if (l->compute.pso && l->compute.pso->compute)
            for (int i = 0; i < 6; ++i) std::snprintf(cs + i * 2, 3, "%02x", l->compute.pso->meta[0].digest[i]);
        else if (l->compute.pso && !l->compute.pso->missing.empty()) {  // a stage the cache doesn't have
            std::snprintf(cs, sizeof cs, "missing ");
            for (int i = 0; i < 8; ++i) std::snprintf(cs + 8 + i * 2, 3, "%02x", l->compute.pso->missing[0][i]);
        }
        std::fprintf(stderr, "[rt] diag:   dispatch %u x %u x %u (list type %d), cs %s, pso %p, from %p\n", x, y, z,
                     l->type, cs, static_cast<void*>(l->compute.pso), _ReturnAddress());
        if (const PipelineState* q = l->compute.pso) {  // the bound pipeline: its stages and its desc as the title gave it
            std::fprintf(stderr, "[rt] diag:     pso: code %zu / %zu bytes, digests %d / %d, %zu missing; desc:",
                         q->code[0].size(), q->code[1].size(), q->meta[0].has_digest, q->meta[1].has_digest,
                         q->missing.size());
            for (size_t i = 0; i < q->stream.size() && i < 160; i += 8) {
                uint64_t w;
                std::memcpy(&w, q->stream.data() + i, 8);
                std::fprintf(stderr, "%s%llx", i % 64 ? " " : "\n[rt] diag:       ", static_cast<unsigned long long>(w));
            }
            std::fprintf(stderr, "\n");
        }
        shims::print_recent_probes(l, "[rt] diag:     before it:");
    }
}

// void* (size, alignment) (list slot 148): upload memory for what the list is about to read, GPU-visible like all
// of the title's. XboxBvhBuilder.dll takes 40 bytes a record from it (edx = n * 40, r8d = 16) and gives up with
// "Failed to allocate upload buffer" on null. A ring of 32 MiB of title memory: what a list reads from it has run
// long before the ring comes round.
constexpr uint64_t kListUpload = 32ull << 20;
std::mutex g_list_upload_lock;
uint8_t* g_list_upload = nullptr;
uint64_t g_list_upload_at = 0;

void* WINAPI list_upload_memory(List*, uint32_t size, uint32_t alignment) {
    static std::atomic<int> told{0};
    if (!size || size > kListUpload / 4) {
        if (told++ < 4) std::fprintf(stderr, "[rt] d3d12: list upload memory of %u bytes refused\n", size);
        return nullptr;
    }
    std::lock_guard lock(g_list_upload_lock);
    if (!g_list_upload) {
        g_list_upload = static_cast<uint8_t*>(shims::xmem_alloc(kListUpload, (128ull << 16) | (2ull << 14)));
        if (!g_list_upload) return nullptr;
        std::fprintf(stderr, "[rt] d3d12: list upload memory (slot 148): a ring of %llu MiB at %p\n",
                     static_cast<unsigned long long>(kListUpload >> 20), static_cast<void*>(g_list_upload));
    }
    const uint64_t align = alignment && !(alignment & (alignment - 1)) ? alignment : 16;
    uint64_t at = (g_list_upload_at + align - 1) & ~(align - 1);
    if (at + size > kListUpload) at = 0;
    g_list_upload_at = at + size;
    return g_list_upload + at;
}

void** list_table() {
    static void** vt = [] {
        void** t = shims::make_probe_vtable("D3D12CommandList");
        shims::set_probe_method(t, 148, reinterpret_cast<void*>(&list_upload_memory));
        shims::set_probe_method(t, 8, reinterpret_cast<void*>(&list_type));
        shims::set_probe_method(t, 9, reinterpret_cast<void*>(&list_close));
        shims::set_probe_method(t, 10, reinterpret_cast<void*>(&list_reset));
        shims::set_probe_method(t, 139, reinterpret_cast<void*>(&list_reset_x));
        shims::set_probe_method(t, 51, reinterpret_cast<void*>(&list_clear_dsv));
        shims::set_probe_method(t, 52, reinterpret_cast<void*>(&list_clear_rtv));
        shims::set_probe_method(t, 12, reinterpret_cast<void*>(&list_draw));
        shims::set_probe_method(t, 61, reinterpret_cast<void*>(&list_execute_indirect));
        shims::set_probe_method(t, 129, reinterpret_cast<void*>(&list_dispatch_mesh));
        shims::set_probe_method(t, 100, reinterpret_cast<void*>(&list_dispatch_mesh));
        // 80: Dispatch with the console's dispatch-initiator bits as a fourth argument (XboxBvhBuilder.dll passes
        // 0x8020; inline it is a DISPATCH_DIRECT packet): the same dispatch here
        shims::set_probe_method(t, 80, reinterpret_cast<void*>(&list_dispatch_x));
        shims::set_probe_method(t, 13, reinterpret_cast<void*>(&list_draw_indexed));
        shims::set_probe_method(t, 14, reinterpret_cast<void*>(&list_dispatch));
        shims::set_probe_method(t, 15, reinterpret_cast<void*>(&list_copy_buffer));
        shims::set_probe_method(t, 71, reinterpret_cast<void*>(&list_write32));
        shims::set_probe_method(t, 72, reinterpret_cast<void*>(&list_write64));
        shims::set_probe_method(t, 73, reinterpret_cast<void*>(&list_wait_value32));
        shims::set_probe_method(t, 74, reinterpret_cast<void*>(&list_wait_value64));
        shims::set_probe_method(t, 33, reinterpret_cast<void*>(&list_compute_root));
        shims::set_probe_method(t, 35, reinterpret_cast<void*>(&list_compute_table));
        shims::set_probe_method(t, 37, reinterpret_cast<void*>(&list_compute_constant));
        shims::set_probe_method(t, 39, reinterpret_cast<void*>(&list_compute_constants));
        shims::set_probe_method(t, 41, reinterpret_cast<void*>(&list_compute_root_view));
        shims::set_probe_method(t, 43, reinterpret_cast<void*>(&list_compute_root_view));
        shims::set_probe_method(t, 45, reinterpret_cast<void*>(&list_compute_root_view));
        shims::set_probe_method(t, 20, reinterpret_cast<void*>(&list_topology));
        shims::set_probe_method(t, 21, reinterpret_cast<void*>(&list_viewports));
        shims::set_probe_method(t, 22, reinterpret_cast<void*>(&list_scissors));
        shims::set_probe_method(t, 24, reinterpret_cast<void*>(&list_stencil_ref));
        shims::set_probe_method(t, 25, reinterpret_cast<void*>(&list_pipeline));
        shims::set_probe_method(t, 34, reinterpret_cast<void*>(&list_graphics_root));
        shims::set_probe_method(t, 36, reinterpret_cast<void*>(&list_graphics_table));
        shims::set_probe_method(t, 38, reinterpret_cast<void*>(&list_graphics_constant));
        shims::set_probe_method(t, 40, reinterpret_cast<void*>(&list_graphics_constants));
        shims::set_probe_method(t, 42, reinterpret_cast<void*>(&list_graphics_root_view));
        shims::set_probe_method(t, 44, reinterpret_cast<void*>(&list_graphics_root_view));
        shims::set_probe_method(t, 46, reinterpret_cast<void*>(&list_graphics_root_view));
        shims::set_probe_method(t, 47, reinterpret_cast<void*>(&list_index_buffer));
        shims::set_probe_method(t, 48, reinterpret_cast<void*>(&list_vertex_buffers));
        shims::set_probe_method(t, 50, reinterpret_cast<void*>(&list_targets));
        return t;
    }();
    return vt;
}

List* as_list(void* p) { return is_object(p, list_table()) ? static_cast<List*>(p) : nullptr; }

// ---------------------------------------------------------------- submission and presentation

// Once, when a submission finds the PC device removed: the reason and, with RT_D3D12_DEBUG=1 (which turns DRED's
// automatic breadcrumbs on), the operations of each list that was running, the last one finished marked.
void report_removal(const char* where) {
    ID3D12Device* dev = host_device();
    HRESULT reason = dev ? dev->GetDeviceRemovedReason() : S_OK;
    static std::atomic<bool> told{false};
    if (SUCCEEDED(reason) || told.exchange(true)) return;
    std::fprintf(stderr, "[rt] d3d12: the PC device was removed (%08lX), first seen %s (thread %lu)\n",
                 static_cast<unsigned long>(reason), where, GetCurrentThreadId());
    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dred)))) return;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT out{};
    HRESULT got = dred->GetAutoBreadcrumbsOutput(&out);
    std::fprintf(stderr, "[rt] dred: breadcrumbs %08lX, head %p\n", static_cast<unsigned long>(got),
                 static_cast<const void*>(out.pHeadAutoBreadcrumbNode));
    if (SUCCEEDED(got)) {
        int n = 0;
        int nodes = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* node = out.pHeadAutoBreadcrumbNode; node && n < 40;
             node = node->pNext) {
            UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            ++nodes;
            if (done >= node->BreadcrumbCount && nodes < 100000) continue;  // a list that ran whole
            ++n;
            std::fprintf(stderr, "[rt] dred: list %p (queue %p): %u of %u operations done; around the stop:",
                         static_cast<void*>(node->pCommandList), static_cast<void*>(node->pCommandQueue), done,
                         node->BreadcrumbCount);
            for (UINT i = done > 12 ? done - 12 : 0; i < node->BreadcrumbCount && i < done + 6; ++i)
                std::fprintf(stderr, "%s%d", i == done ? " | >" : " ", static_cast<int>(node->pCommandHistory[i]));
            std::fprintf(stderr, "\n");
            unsigned ran = 0;  // Dispatch operations (6) that finished
            for (UINT i = 0; i < done && i < node->BreadcrumbCount; ++i)
                if (node->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_DISPATCH) ++ran;
            report_dispatches(node->pCommandList, ran);
            unsigned meshes = 0;  // mesh draws that finished: ExecuteIndirect and DispatchMesh operations
            for (UINT i = 0; i < done && i < node->BreadcrumbCount; ++i)
                if (node->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT ||
                    node->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH)
                    ++meshes;
            report_mesh_draws(node->pCommandList, meshes);
        }
        std::fprintf(stderr, "[rt] dred: %d list(s) in all, %d stopped part-way\n", nodes, n);
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA)
        std::fprintf(stderr, "[rt] dred: page fault at GPU address %llx\n",
                     static_cast<unsigned long long>(fault.PageFaultVA));
    dred->Release();
}

// void ExecuteCommandLists(count, lists) (queue slot 10) and the console's with a frame token (35: count, lists,
// token, flags): the closed lists run on the PC queue, and the call returns once they have.
// What a submission needs of one of its lists. The title may Reset a list as soon as ExecuteCommandLists returns
// (only the allocator has to wait for the GPU), so a submission that is held takes the list's PC list for itself
// and leaves the title's object another one to record into: run from the title's object later, a held list was run
// open, or with the next frame's commands, and the PC removed the device (887A0001).
struct Frozen {
    ID3D12GraphicsCommandList* host = nullptr;  // null: not run here (not closed, or of another queue type)
    std::vector<Resource*> used;                // the textures its draws and dispatches read
    std::vector<PipeWrite> writes;
    std::vector<PipeWait> waits;
    bool taken = false;  // `host` was the title's list's: a spare for another list once it has run
};

std::mutex g_spare_lock;
std::vector<ID3D12GraphicsCommandList*> g_spare_lists[2];  // direct, compute: closed, ready for a Reset

ID3D12GraphicsCommandList* spare_list(D3D12_COMMAND_LIST_TYPE type) {
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return nullptr;
    const int k = type == D3D12_COMMAND_LIST_TYPE_COMPUTE;
    {
        std::lock_guard lock(g_spare_lock);
        if (!g_spare_lists[k].empty()) {
            ID3D12GraphicsCommandList* l = g_spare_lists[k].back();
            g_spare_lists[k].pop_back();
            return l;
        }
    }
    ID3D12Device4* dev = nullptr;  // CreateCommandList1: a list made closed, with no allocator
    ID3D12GraphicsCommandList* made = nullptr;
    if (FAILED(host_device()->QueryInterface(IID_PPV_ARGS(&dev)))) return nullptr;
    if (FAILED(dev->CreateCommandList1(0, type, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&made)))) made = nullptr;
    dev->Release();
    return made;
}

// `take`: the list's PC list goes with the result and the title's object gets a spare (false if there is none).
bool freeze(Queue* q, List* l, bool take, Frozen& f) {
    f.waits = l->waits;
    if (l->closed && l->type == q->type) {
        for (const auto* used : {&l->gfx.used, &l->compute.used})
            for (Resource* r : *used)
                if (std::find(f.used.begin(), f.used.end(), r) == f.used.end()) f.used.push_back(r);
        if (take) {
            ID3D12GraphicsCommandList* spare = spare_list(l->type);
            f.host = l->host;
            if (spare) {
                f.taken = true;
                l->host = spare;  // closed, as the title's list is now: its next Reset records into it
            }
        } else {
            f.host = l->host;
        }
    }
    if (take) f.writes = std::move(l->writes), l->writes.clear();
    else f.writes = l->writes;
    return true;
}

// ---- submissions not waited for (g_async)
//
// Each submission, and each title fence signal, leaves an entry with the queue's value after it; once the GPU has
// got there the completion thread does, in order, what used to follow the wait: the lists' bottom-of-pipe writes,
// the fence's memory, and handing back the upload buffer, the pre-list and the spare lists.
struct Pending {
    uint64_t done = 0;
    std::vector<PipeWrite> writes;
    std::vector<ID3D12GraphicsCommandList*> spares;
    ID3D12Resource* upload = nullptr;
    ID3D12CommandAllocator* pre_allocator = nullptr;
    ID3D12GraphicsCommandList* pre_list = nullptr;
    Fence* fence = nullptr;  // a signal: the fence and the value its memory gets
    uint64_t value = 0;
};

void push_pending(Queue* q, Pending* p) {
    {
        std::lock_guard lock(q->pending_lock);
        p->done = mark_locked(q);
        q->pending.push_back(p);
    }
    if (g_completion_event) SetEvent(g_completion_event);  // a new value to ask the GPU about
}

void pend_signal(Queue* q, Fence* f, uint64_t value) {
    auto* p = new Pending{};
    p->fence = f;
    p->value = value;
    push_pending(q, p);
}

// What the GPU has got past on `q`, done. Only the completion thread calls it (the writes go in order).
std::atomic<uint64_t> g_completed_entries{0};
void complete_queue(Queue* q) {
    if (!q->done) return;
    const uint64_t reached = q->done->GetCompletedValue();
    if (reached == UINT64_MAX) return;  // the device is gone
    for (;;) {
        Pending* p = nullptr;
        {
            std::lock_guard lock(q->pending_lock);
            if (q->pending.empty() || q->pending.front()->done > reached) return;
            p = q->pending.front();
            q->pending.pop_front();
        }
        apply_pipe_writes(p->writes);
        if (p->fence) raise_completed(p->fence, p->value);
        if (p->upload) p->upload->Release();
        if (!p->spares.empty()) {
            std::lock_guard spares(g_spare_lock);
            for (ID3D12GraphicsCommandList* l : p->spares)
                g_spare_lists[q->type == D3D12_COMMAND_LIST_TYPE_COMPUTE].push_back(l);
        }
        if (p->pre_list) {
            std::lock_guard lock(q->pending_lock);
            q->free_pre.push_back({p->pre_allocator, p->pre_list});
        }
        delete p;
        ++g_completed_entries;
    }
}

DWORD WINAPI completion_thread(void*) {
    std::vector<std::pair<Queue*, uint64_t>> asked;  // the value each queue's fence was last asked to set the event at
    for (;;) {
        for (Queue* q = g_queues.load(); q; q = q->next_queue) {
            complete_queue(q);
            uint64_t next = 0;
            {
                std::lock_guard lock(q->pending_lock);
                if (!q->pending.empty()) next = q->pending.front()->done;
            }
            auto it = std::find_if(asked.begin(), asked.end(), [&](auto& a) { return a.first == q; });
            if (it == asked.end()) it = asked.emplace(asked.end(), q, uint64_t{0});
            if (next && it->second != next) {
                q->done->SetEventOnCompletion(next, g_completion_event);
                it->second = next;
            }
        }
        WaitForSingleObject(g_completion_event, 2);
    }
}

void start_completion_thread() {
    static std::atomic<bool> started{false};
    if (!g_async || started.exchange(true)) return;
    g_completion_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 0, &completion_thread, nullptr, 0, nullptr)) {
        SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
        SetThreadDescription(t, L"rt GPU completions");
        CloseHandle(t);
    }
    std::fprintf(stderr, "[rt] d3d12: submissions aren't waited for (RT_SYNC_SUBMIT=1 to wait for each)\n");
}

// ---- waits for a value between queues, on the GPU (g_async)
//
// A list can wait for a value in memory (WaitOnValue) that another queue's list writes at the bottom of its pipe:
// the compute queue's lighting passes wait for the frame's number the graphics queue writes after its depth pass
// (VIctory found them reading a cleared depth when run at once). Submissions not waited for, the PC queues run side
// by side: each address the title writes that way gets a fence of ours, signalled on the writing queue with the
// value after the list, and a list that waits for `memory >= reference` (comparison 7, the one 1.0.2.1 passes) has
// its queue wait on that fence for the reference first. A wait no write meets within kValueWaitMs is let go from
// the CPU (a comparison read wrongly must not stop a queue for good).
struct ValueFence {
    ID3D12Fence* fence = nullptr;
    uint64_t signalled = 0;  // the highest value a queue was told to signal
};
struct PendingValueWait {
    ID3D12Fence* fence;
    uint64_t reference;
    ULONGLONG since;
};
std::mutex g_value_lock;
std::map<uint64_t, ValueFence> g_value_fences;  // by address
std::vector<PendingValueWait> g_value_waits;
std::atomic<uint64_t> g_value_waited{0}, g_value_released{0}, g_value_met{0};
constexpr ULONGLONG kValueWaitMs = 1000;

ValueFence& value_fence_locked(uint64_t address, uint32_t bytes) {
    ValueFence& v = g_value_fences[address];
    if (!v.fence) {
        uint64_t now = 0;
        if (!read_title_memory(&now, address, bytes)) now = 0;
        if (bytes == 4) now = static_cast<uint32_t>(now);
        host_device()->CreateFence(now, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&v.fence));
        v.signalled = now;
    }
    return v;
}

// Before a submission's lists run on `q`: its queue waits for the values they wait for that memory doesn't hold.
void queue_value_waits(Queue* q, const std::vector<Frozen>& lists) {
    for (const Frozen& f : lists)
        for (const PipeWait& w : f.waits) {
            if (w.comparison != 7) continue;
            const uint64_t ref = w.bytes == 4 ? static_cast<uint32_t>(w.reference) : w.reference;
            uint64_t now = 0;
            if (read_title_memory(&now, w.address, w.bytes) && (w.bytes == 4 ? static_cast<uint32_t>(now) : now) >= ref) {
                ++g_value_met;
                continue;
            }
            std::lock_guard lock(g_value_lock);
            ValueFence& v = value_fence_locked(w.address, w.bytes);
            if (!v.fence || v.fence->GetCompletedValue() >= ref) continue;
            q->host->Wait(v.fence, ref);
            g_value_waits.push_back({v.fence, ref, GetTickCount64()});
            const uint64_t n = ++g_value_waited;
            if (n <= 4 || (n & (n - 1)) == 0)
                std::fprintf(stderr, "[rt] waits: %llu waits for a value made on the GPU (queue type %d now, %llx >= %llx); "
                                     "%llu were met at once, %llu let go after %llu ms\n",
                             static_cast<unsigned long long>(n), static_cast<int>(q->type),
                             static_cast<unsigned long long>(w.address), static_cast<unsigned long long>(ref),
                             static_cast<unsigned long long>(g_value_met.load()),
                             static_cast<unsigned long long>(g_value_released.load()),
                             static_cast<unsigned long long>(kValueWaitMs));
        }
}

// After a submission's lists on `q`: the fence of each address they write at the bottom of the pipe gets the value.
void queue_value_signals(Queue* q, const std::vector<Frozen>& lists) {
    for (const Frozen& f : lists)
        for (const PipeWrite& w : f.writes) {
            if (w.flags & 3) continue;  // a timestamp
            const uint64_t value = w.bytes == 4 ? static_cast<uint32_t>(w.value) : w.value;
            std::lock_guard lock(g_value_lock);
            ValueFence& v = value_fence_locked(w.address, w.bytes);
            if (!v.fence || value <= v.signalled) continue;  // fences only go up
            q->host->Signal(v.fence, value);
            v.signalled = value;
        }
}

// The fence watcher, every millisecond: waits met are dropped, the ones over kValueWaitMs let go.
void release_value_waits() {
    std::lock_guard lock(g_value_lock);
    if (g_value_waits.empty()) return;
    const ULONGLONG now = GetTickCount64();
    size_t keep = 0;
    for (const PendingValueWait& w : g_value_waits) {
        if (w.fence->GetCompletedValue() >= w.reference) continue;
        if (now - w.since >= kValueWaitMs) {
            w.fence->Signal(w.reference);
            const uint64_t n = ++g_value_released;
            if (n <= 4 || (n & (n - 1)) == 0)
                std::fprintf(stderr, "[rt] waits: a wait for %llx on the GPU let go after %llu ms (%llu so far)\n",
                             static_cast<unsigned long long>(w.reference), static_cast<unsigned long long>(kValueWaitMs),
                             static_cast<unsigned long long>(n));
            continue;
        }
        g_value_waits[keep++] = w;
    }
    g_value_waits.resize(keep);
}

void execute_frozen(Queue* q, std::vector<Frozen>& lists) {
    std::vector<ID3D12CommandList*> hosts;
    std::vector<Resource*> textures;  // what the lists' draws read, uploaded first
    for (const Frozen& f : lists)
        if (f.host) {
            hosts.push_back(f.host);
            for (Resource* r : f.used)
                if (std::find(textures.begin(), textures.end(), r) == textures.end()) textures.push_back(r);
        }
    std::lock_guard lock(q->m);
    ID3D12Resource* upload = nullptr;
    // the pre-list: the queue's one when submissions are waited for, else one free again (its last run is done)
    ID3D12CommandAllocator* pre_allocator = nullptr;
    ID3D12GraphicsCommandList* pre_list = nullptr;
    if (!hosts.empty() && q->type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ID3D12Device* dev = host_device();
        if (g_async) {
            {
                std::lock_guard pl(q->pending_lock);
                if (!q->free_pre.empty()) {
                    std::tie(pre_allocator, pre_list) = q->free_pre.back();
                    q->free_pre.pop_back();
                }
            }
            if (pre_list) {
                if (FAILED(pre_allocator->Reset()) || FAILED(pre_list->Reset(pre_allocator, nullptr))) {
                    pre_list->Release();
                    pre_allocator->Release();
                    pre_list = nullptr, pre_allocator = nullptr;
                }
            } else if (SUCCEEDED(dev->CreateCommandAllocator(q->type, IID_PPV_ARGS(&pre_allocator))) &&
                       FAILED(dev->CreateCommandList(0, q->type, pre_allocator, nullptr, IID_PPV_ARGS(&pre_list)))) {
                pre_allocator->Release();
                pre_list = nullptr, pre_allocator = nullptr;
            }
        } else {
            if (!q->pre_list && SUCCEEDED(dev->CreateCommandAllocator(q->type, IID_PPV_ARGS(&q->pre_allocator))))
                dev->CreateCommandList(0, q->type, q->pre_allocator, nullptr, IID_PPV_ARGS(&q->pre_list));
            else if (q->pre_list && SUCCEEDED(q->pre_allocator->Reset()))
                q->pre_list->Reset(q->pre_allocator, nullptr);
            pre_list = q->pre_list;
        }
        if (pre_list) {
            report_removal("before preparing a submission");
            prepare_submission(pre_list);
            report_removal("after preparing a submission (page table, chunks)");
            uint64_t t0 = perf_now_us();
            upload = upload_textures(pre_list, textures);
            report_removal("after the texture uploads of a submission");
            g_perf.upload_us += perf_now_us() - t0;
            if (SUCCEEDED(pre_list->Close())) hosts.insert(hosts.begin(), pre_list);
        }
    }
    uint64_t t0 = perf_now_us();
    report_removal("before a submission");
    if (g_async) queue_value_waits(q, lists);
    if (!hosts.empty()) q->host->ExecuteCommandLists(static_cast<UINT>(hosts.size()), hosts.data());
    if (g_async) queue_value_signals(q, lists);
    report_removal("right after ExecuteCommandLists");
    if (g_async) {
        // what is left for when the GPU has run them (complete_queue)
        auto* p = new Pending{};
        for (const Frozen& f : lists) {
            p->writes.insert(p->writes.end(), f.writes.begin(), f.writes.end());
            if (f.taken && f.host) p->spares.push_back(f.host);
        }
        p->upload = upload;
        p->pre_allocator = g_async ? pre_allocator : nullptr;
        p->pre_list = g_async ? pre_list : nullptr;
        push_pending(q, p);
        g_perf.execute_us += perf_now_us() - t0;
        ++g_perf.executes;
        return;
    }
    q->running.clear();
    for (const Frozen& f : lists)
        if (f.host) q->running.push_back(f.host);
    wait_idle(q);
    q->running.clear();
    g_perf.execute_us += perf_now_us() - t0;
    ++g_perf.executes;
    report_removal("after a submission ran");
    if (upload) upload->Release();
    // the GPU has run them: their bottom-of-pipe writes, in submission order (a list of another queue type isn't
    // run here, but the title still waits on its writes)
    for (const Frozen& f : lists) apply_pipe_writes(f.writes);
    {
        std::lock_guard spares(g_spare_lock);
        for (const Frozen& f : lists)
            if (f.taken && f.host) g_spare_lists[q->type == D3D12_COMMAND_LIST_TYPE_COMPUTE].push_back(f.host);
    }
}

void execute_now(Queue* q, UINT count, void* const* lists) {
    std::vector<Frozen> frozen;
    for (UINT i = 0; i < count && lists; ++i)
        if (List* l = as_list(lists[i])) {
            frozen.emplace_back();
            freeze(q, l, false, frozen.back());
        }
    execute_frozen(q, frozen);
}

// ---- submissions held for a value
//
// A list can wait for a value in memory (WaitOnValue, slots 73 / 74), and the title uses that between queues: the
// compute queue's lists of a frame wait for what a graphics list writes at the bottom of the pipe (the frame's
// number at a fixed address), and are submitted before it is. On the console the queue just sits at the wait. Here a
// submission runs to its end inside ExecuteCommandLists, so such a list ran a step early, on what the frame before
// left (measured in the open world: 560 of 2,048 waits found the memory one short of the reference). So a
// submission with a wait that isn't met is held: it, and every later submission and signal of its queue, in order,
// until the value is there (looked at after each submission's writes, and by the fence watcher every millisecond)
// or kHoldMs have passed (the comparison's kinds aren't all known: one read wrongly must not stop the queue).
struct Held {
    Queue* q;
    std::vector<Frozen> lists;  // a submission's lists (their PC lists taken from the title's objects), or
    void* fence = nullptr;      // a signal
    uint64_t value = 0;
    ULONGLONG since = 0;
};
std::mutex g_held_lock;
std::deque<Held> g_held;
std::vector<Queue*> g_held_running;  // queues with a held entry being run now: what comes for them waits its turn
std::atomic<uint64_t> g_held_total{0}, g_held_by_time{0}, g_held_signals{0};
constexpr ULONGLONG kHoldMs = 250;

// Comparison 7 is the only one 1.0.2.1 passes; its references are counters that only grow, so "the memory has
// reached the reference" covers both equal and greater-or-equal.
bool waits_met(const std::vector<PipeWait>& waits) {
    for (const PipeWait& w : waits) {
        uint64_t now = 0;
        if (!read_title_memory(&now, w.address, w.bytes)) continue;  // unreadable: nothing to wait for
        const uint64_t ref = w.bytes == 4 ? static_cast<uint32_t>(w.reference) : w.reference;
        if (now < ref) return false;
    }
    return true;
}
bool waits_met(const std::vector<Frozen>& lists) {
    for (const Frozen& f : lists)
        if (!waits_met(f.waits)) return false;
    return true;
}
bool waits_met(void* const* lists, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (List* l = as_list(lists[i]); l && !waits_met(l->waits)) return false;
    return true;
}

bool queue_held_locked(Queue* q) {
    for (const Held& h : g_held)
        if (h.q == q) return true;
    return std::find(g_held_running.begin(), g_held_running.end(), q) != g_held_running.end();
}

bool hold_signal(Queue* q, void* fence, uint64_t value) {
    if (!g_held_count.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_held_lock);
    if (!queue_held_locked(q)) return false;
    g_held.push_back(Held{q, {}, fence, value, GetTickCount64()});
    g_held_count = g_held.size() + g_held_running.size();
    ++g_held_signals;
    return true;
}

void run_held(Queue* all_of) {
    for (;;) {
        Held h;
        bool timed = false;
        {
            std::lock_guard lock(g_held_lock);
            const ULONGLONG now = GetTickCount64();
            auto it = g_held.begin();
            std::vector<Queue*> blocked = g_held_running;  // queues with an earlier entry that can't go yet
            for (; it != g_held.end(); ++it) {
                if (std::find(blocked.begin(), blocked.end(), it->q) != blocked.end()) continue;
                const bool old = now - it->since >= kHoldMs;
                const bool now_all = all_of == kEveryQueue || it->q == all_of;
                if (it->fence || now_all || old || waits_met(it->lists)) {
                    timed = !it->fence && !now_all && old && !waits_met(it->lists);
                    break;
                }
                blocked.push_back(it->q);
            }
            if (it == g_held.end()) return;
            h = std::move(*it);
            g_held.erase(it);
            g_held_running.push_back(h.q);
            g_held_count = g_held.size() + g_held_running.size();
        }
        if (h.fence) {
            signal_now(h.q, h.fence, h.value);
        } else {
            if (timed) ++g_held_by_time;
            execute_frozen(h.q, h.lists);
        }
        {
            std::lock_guard lock(g_held_lock);
            g_held_running.erase(std::find(g_held_running.begin(), g_held_running.end(), h.q));
            g_held_count = g_held.size() + g_held_running.size();
        }
    }
}

void WINAPI queue_execute(Queue* q, UINT count, void* const* lists) {
    for (UINT i = 0; i < count && lists; ++i)
        if (List* l = as_list(lists[i])) note_waits(l, static_cast<int>(q->type));
    // RT_HOLD_WAITS=1: on. Off by default for now: with lists held, the PC removed the device (887A0001, seen at an
    // ExecuteCommandLists) a minute into the open world in two runs of two, and not in the run before without it;
    // what the held lists break isn't found yet.
    static const bool holding = [] {
        char v[4];
        return GetEnvironmentVariableA("RT_HOLD_WAITS", v, sizeof v) && v[0] == '1';
    }();
    bool hold = false;
    if (holding && count && lists) {
        std::lock_guard lock(g_held_lock);
        if (queue_held_locked(q) || !waits_met(lists, count)) {
            // each list's PC list goes with the held submission (the title may reset its object at once)
            Held h{q, {}, nullptr, 0, GetTickCount64()};
            for (UINT i = 0; i < count; ++i)
                if (List* l = as_list(lists[i])) {
                    h.lists.emplace_back();
                    freeze(q, l, true, h.lists.back());
                }
            g_held.push_back(std::move(h));
            g_held_count = g_held.size() + g_held_running.size();
            hold = true;
            const uint64_t n = ++g_held_total;
            if (n <= 4 || (n & (n - 1)) == 0)
                std::fprintf(stderr,
                             "[rt] waits: %llu submissions held for a value so far (queue type %d now, %zu waiting); %llu "
                             "let go after %llu ms without it; %llu signals kept behind them\n",
                             static_cast<unsigned long long>(n), static_cast<int>(q->type), g_held.size(),
                             static_cast<unsigned long long>(g_held_by_time.load()),
                             static_cast<unsigned long long>(kHoldMs),
                             static_cast<unsigned long long>(g_held_signals.load()));
        }
    }
    if (!hold) execute_now(q, count, lists);
    run_held(nullptr);  // what this submission wrote may be what a held one waits for
}
void WINAPI queue_execute_x(Queue* q, UINT count, void* const* lists, uint64_t, uint32_t) {
    queue_execute(q, count, lists);
}

// Slot 31: (count, ranges, page pool address, the pool's page count, ...): maps pages of a page pool (slot 74) at GPU
// virtual addresses, which is how the title backs its virtual resources (video frames among them). A range is 24
// bytes: {address, page count, unused, records}; a record, one per 64 KiB page from the address on, is 12 bytes:
// {1 map / 2 unmap, the page's index in the pool, 1}. A page is mapped by mapping the physical page behind that pool
// page there too (uma::map_frame), so the CPU and the GPU see the same memory at both addresses.
HRESULT WINAPI queue_map_pages(Queue*, uint64_t count, const uint8_t* ranges, uint64_t pool, uint64_t pool_pages) {
    if (!ranges) return E_INVALIDARG;
    const uint32_t* table = uma::page_table();
    const uintptr_t base = uma::base();
    uint64_t mapped = 0, unmapped = 0, failed = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t va, records;
        uint32_t pages;
        std::memcpy(&va, ranges + i * 24, 8);
        std::memcpy(&pages, ranges + i * 24 + 8, 4);
        std::memcpy(&records, ranges + i * 24 + 16, 8);
        for (uint32_t k = 0; k < pages; ++k) {
            uint32_t rec[3];
            std::memcpy(rec, reinterpret_cast<const uint8_t*>(records) + size_t{k} * 12, sizeof rec);
            uintptr_t at = va + uint64_t{k} * uma::kPage;
            if (rec[0] == 2) {
                uma::map_frame(at, 0, PAGE_READWRITE) ? ++unmapped : ++failed;
                continue;
            }
            uintptr_t src = pool + uint64_t{rec[1]} * uma::kPage;
            uint32_t entry = rec[0] == 1 && rec[1] < pool_pages && uma::contains(src) ? table[(src - base) / uma::kPage] : 0;
            if (entry && uma::map_frame(at, uint64_t{entry} * 16, PAGE_READWRITE))  // entry: pool index + 1
                ++mapped;
            else
                ++failed;
        }
    }
    static std::atomic<uint64_t> calls{0};
    uint64_t n = ++calls;
    if (failed || (n & (n - 1)) == 0)
        std::fprintf(stderr, "[rt] map pages (call %llu): %llu mapped, %llu unmapped, %llu failed\n",
                     static_cast<unsigned long long>(n), static_cast<unsigned long long>(mapped),
                     static_cast<unsigned long long>(unmapped), static_cast<unsigned long long>(failed));
    return S_OK;
}

// HRESULT PresentX(planes, plane parameters, parameters) (queue slot 32). A plane starts with a frame token and its
// resources (as VIctory reads them); the first resource of the first plane is shown, copied into the swap chain's back
// buffer (same size; its format when the swap chain can take it, else a dark grey frame says presents happen).
struct Plane {
    uint64_t token;
    uint32_t count;
    void* const* resources;
};

DXGI_FORMAT swapchain_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

std::atomic<uint64_t> g_presents{0};

// Every 32 presents: the frame time and the runtime's parts of it (the rest is the title's own work, and waits).
void perf_report(uint64_t frame) {
    static uint64_t last_us = 0, last_emulated = 0;  // the presenting thread only
    if (frame % 32) return;
    uint64_t now = perf_now_us();
    if (last_us) {
        double per = (now - last_us) / 32.0 / 1000.0;
        auto ms = [](std::atomic<uint64_t>& v) { return v.exchange(0) / 32.0 / 1000.0; };
        uint64_t draws = g_perf.draws.exchange(0), executes = g_perf.executes.exchange(0);
        double draw = ms(g_perf.draw_us), upload = ms(g_perf.upload_us), execute = ms(g_perf.execute_us),
               present = ms(g_perf.present_us), wait = ms(g_perf.wait_us);
        std::fprintf(stderr,
                     "[rt] perf: %.1f ms a frame (%.1f fps): draws %.1f ms (%llu), texture uploads %.1f ms, "
                     "submissions + GPU waits %.1f ms (%llu), present %.1f ms, the rest %.1f ms (GPU waits in "
                     "all: %.1f ms); %llu emulated instructions a frame\n",
                     per, 1000.0 / per, draw, static_cast<unsigned long long>(draws / 32), upload, execute,
                     static_cast<unsigned long long>(executes / 32), present,
                     per - draw - upload - execute - present, wait, (emulated_instructions() - last_emulated) / 32);
        overlay::RuntimeNumbers on;
        on.gpu_wait_ms = wait;
        on.draw_ms = draw;
        on.pipelines_making = pipelines_being_made();
        overlay::set_runtime(on);
    }
    last_us = now;
    last_emulated = emulated_instructions();
}

// The planes of a present, bottom first. 1.0.2.1 presents two: the scene (1920x1080 on a Series S) and, over it, the
// interface (3840x2160, alpha where the scene shows through); the console's display pipe scales and blends them. The
// plane parameters' size isn't known from a header here: the second plane is looked for at each plausible stride
// (a small resource count and a resource of ours), once.
struct Blit {
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* opaque[2] = {};   // per swap-chain format seen: [0] the first plane, [1] the planes over it
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ID3D12DescriptorHeap* srv = nullptr;   // 8 views, shader visible
    ID3D12DescriptorHeap* rtv = nullptr;
    bool failed = false;
};
Blit g_blit;

bool plain_readable(const void* p, size_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    return p && VirtualQuery(p, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           reinterpret_cast<uintptr_t>(p) + size <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

void present_planes(uint32_t planes, const Plane* plane, std::vector<Resource*>& out) {
    static std::atomic<uint32_t> stride{0};
    static std::atomic<bool> logged{false};
    if (Resource* r = as_resource(plane->resources[0]); r && r->host) out.push_back(r);
    if (planes < 2 || out.empty()) return;
    auto at = [&](uint32_t s, uint32_t i) -> Resource* {
        const Plane* p = reinterpret_cast<const Plane*>(reinterpret_cast<const uint8_t*>(plane) + size_t{s} * i);
        if (!plain_readable(p, sizeof *p) || !p->count || p->count > 8 || !plain_readable(p->resources, 8)) return nullptr;
        Resource* r = as_resource(p->resources[0]);
        return r && r->host ? r : nullptr;
    };
    uint32_t s = stride.load();
    if (!s) {
        for (uint32_t c = 24; c <= 160 && !s; c += 8)
            if (at(c, 1)) s = c;
        if (!logged.exchange(true)) {
            std::fprintf(stderr, "[rt] present: %u planes, %s (stride %u); the first plane's words:", planes,
                         s ? "the second found" : "the second NOT found", s);
            const uint64_t* w = reinterpret_cast<const uint64_t*>(plane);
            for (int i = 0; i < 24 && plain_readable(w + i, 8); ++i)
                std::fprintf(stderr, " %llx", static_cast<unsigned long long>(w[i]));
            std::fprintf(stderr, "\n");
        }
        if (!s) return;
        stride = s;
    }
    for (uint32_t i = 1; i < planes && i < 4; ++i)
        if (Resource* r = at(s, i)) out.push_back(r);
}

// The pipeline that draws a plane over the back buffer: one triangle over the whole target, the plane sampled with a
// linear filter. Compiled with the system's d3dcompiler_47.dll on first use (the runtime ships no shader of its own).
bool blit_ready(ID3D12Device* dev, DXGI_FORMAT target) {
    Blit& b = g_blit;
    if (b.failed) return false;
    if (b.root && b.format == target) return true;
    static const char kSource[] =
        "Texture2D t : register(t0); SamplerState s : register(s0);\n"
        "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
        "V vs(uint i : SV_VertexID) { V o; o.uv = float2((i << 1) & 2, i & 2);\n"
        "  o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "float4 ps(V i) : SV_Target { return t.Sample(s, i.uv); }\n";
    using Compile = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE lib = LoadLibraryW(L"d3dcompiler_47.dll");
    Compile compile = lib ? reinterpret_cast<Compile>(GetProcAddress(lib, "D3DCompile")) : nullptr;
    ID3DBlob *vs = nullptr, *ps = nullptr, *sig = nullptr;
    b.failed = true;
    if (!compile || FAILED(compile(kSource, sizeof kSource - 1, "blit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, nullptr)) ||
        FAILED(compile(kSource, sizeof kSource - 1, "blit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, nullptr))) {
        std::fprintf(stderr, "[rt] present: the plane shaders didn't compile (d3dcompiler_47.dll %s)\n",
                     lib ? "loaded" : "missing");
        return false;
    }
    if (!b.root) {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable = {1, &range};
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{1, &param, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_DESCRIPTOR_HEAP_DESC sh{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, nullptr)) ||
            FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&b.root))) ||
            FAILED(dev->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&b.srv))) ||
            FAILED(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&b.rtv)))) {
            std::fprintf(stderr, "[rt] present: no root signature for the planes\n");
            return false;
        }
    }
    for (int over = 0; over < 2; ++over) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = b.root;
        d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        d.SampleMask = ~0u;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        auto& rt = d.BlendState.RenderTarget[0];
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (over) {  // the interface plane over the scene: its colour by its alpha
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = target;
        d.SampleDesc.Count = 1;
        if (b.opaque[over]) b.opaque[over]->Release();
        b.opaque[over] = nullptr;
        if (FAILED(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&b.opaque[over])))) {
            std::fprintf(stderr, "[rt] present: no pipeline for the planes (target format %d)\n", target);
            return false;
        }
    }
    b.format = target;
    b.failed = false;
    return true;
}

// The resource overlay (overlay.h) over the top-left of the back buffer: its latest image copied into a texture of
// ours when it changes (the present before has run: the present waits for it), drawn with the planes' blending
// pipeline into a viewport of the image's size. The texture stays in the common state between presents.
struct OverlayGpu {
    ID3D12Resource* texture = nullptr;
    ID3D12Resource* upload = nullptr;
    uint8_t* upload_cpu = nullptr;
    uint64_t upload_bytes = 0;
    uint32_t w = 0, h = 0;
    uint64_t version = 0;
    std::vector<uint32_t> pixels;
};
OverlayGpu g_overlay;

void draw_overlay(ID3D12Device* dev, ID3D12GraphicsCommandList* pl, uint32_t bw, uint32_t bh) {
    OverlayGpu& o = g_overlay;
    Blit& bl = g_blit;
    uint32_t w = 0, h = 0;
    uint64_t version = 0;
    if (!overlay::copy_image(o.pixels, &w, &h, &version) || !w || !h) return;
    if (w != o.w || h != o.h) {  // a texture of the image's size
        if (o.texture) o.texture->Release();
        o.texture = nullptr;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&o.texture))))
            return;
        o.w = w, o.h = h;
        o.version = 0;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE at = bl.srv->GetCPUDescriptorHandleForHeapStart();
        at.ptr += 7 * dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        dev->CreateShaderResourceView(o.texture, &sd, at);  // the planes use the first views, at most 7 here
    }
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition = {o.texture, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    if (version != o.version) {
        const D3D12_RESOURCE_DESC td = o.texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
        if (total > o.upload_bytes) {
            if (o.upload) o.upload->Release();
            o.upload = nullptr, o.upload_cpu = nullptr, o.upload_bytes = 0;
            D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
            D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
            D3D12_RANGE none{0, 0};
            if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr, IID_PPV_ARGS(&o.upload))) ||
                FAILED(o.upload->Map(0, &none, reinterpret_cast<void**>(&o.upload_cpu))))
                return;
            o.upload_bytes = total;
        }
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(o.upload_cpu + fp.Offset + size_t{y} * fp.Footprint.RowPitch, o.pixels.data() + size_t{y} * w,
                        size_t{w} * 4);
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        pl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{o.texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{o.upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = fp;
        pl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        o.version = version;
    }
    pl->ResourceBarrier(1, &b);
    const float x = 0, y = 0;  // the image has the top bar and the panel at their places
    D3D12_VIEWPORT vp{x, y, static_cast<float>(w), static_cast<float>(h), 0, 1};
    D3D12_RECT sc{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(std::min<float>(x + w, static_cast<float>(bw))),
                  static_cast<LONG>(std::min<float>(y + h, static_cast<float>(bh)))};
    pl->RSSetViewports(1, &vp);
    pl->RSSetScissorRects(1, &sc);
    pl->SetGraphicsRootSignature(bl.root);
    pl->SetDescriptorHeaps(1, &bl.srv);
    D3D12_GPU_DESCRIPTOR_HANDLE gh = bl.srv->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += 7 * dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    pl->SetPipelineState(bl.opaque[1]);
    pl->SetGraphicsRootDescriptorTable(0, gh);
    pl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pl->DrawInstanced(3, 1, 0, 0);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    pl->ResourceBarrier(1, &b);
}

HRESULT WINAPI queue_present(Queue* q, uint32_t planes, const Plane* plane, const void*) {
    uint64_t frame = ++g_presents;
    overlay::poll_toggle();
    overlay::frame_presented(host_device(), q->width);
    uint64_t present_t0 = perf_now_us();
    struct Done {
        uint64_t t0, frame;
        ~Done() {
            g_perf.present_us += perf_now_us() - t0;
            perf_report(frame);
        }
    } done{present_t0, frame};
    if ((frame & (frame - 1)) == 0)
        std::fprintf(stderr, "[rt] present: frame %llu (%llu clears so far)\n", static_cast<unsigned long long>(frame),
                     static_cast<unsigned long long>(g_clears.load()));
    if (!planes || !plane || !plane->count || !plane->resources || q->type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return S_OK;
    // a frame ends here: everything submitted for it runs first, on whatever queue (the title resets the frame's
    // allocators after this, and a held list's commands are in them)
    if (g_held_count.load(std::memory_order_relaxed)) run_held(kEveryQueue);
    std::vector<Resource*> shown;
    present_planes(planes, plane, shown);
    if (shown.empty()) return S_OK;
    Resource* r = shown[0];
    if (frame <= 2) std::fprintf(stderr, "[rt] present: texture %p\n", static_cast<void*>(r->host));
    D3D12_RESOURCE_DESC rd = r->host->GetDesc();
    DXGI_FORMAT format = swapchain_format(rd.Format);
    std::lock_guard lock(q->m);
    ID3D12Device* dev = host_device();
    if (!q->swapchain) {
        q->width = static_cast<uint32_t>(rd.Width);
        q->height = rd.Height;
        HWND w = window::get(1280, 720);
        IDXGIFactory2* factory = nullptr;
        IDXGISwapChain1* sc = nullptr;
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = q->width;
        d.Height = q->height;
        d.Format = format ? format : DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateSwapChainForHwnd(q->host, w, &d, nullptr, nullptr, &sc)) ||
            FAILED(sc->QueryInterface(IID_PPV_ARGS(&q->swapchain))) ||
            FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&q->present_allocator))) ||
            FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, q->present_allocator, nullptr,
                                          IID_PPV_ARGS(&q->present_list)))) {
            std::fprintf(stderr, "[rt] present: no swap chain for a %ux%u frame\n", q->width, q->height);
            return S_OK;
        }
        q->present_list->Close();
        // Inside the launcher's window the launcher owns full screen (Alt+Enter goes to it): DXGI keeps out
        if (window::embedded()) factory->MakeWindowAssociation(w, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        factory->Release();
        std::fprintf(stderr, "[rt] present: swap chain %ux%u, format %d (the frame's %d)\n", q->width, q->height,
                     d.Format, rd.Format);
    }
    ID3D12Resource* back = nullptr;
    if (FAILED(q->swapchain->GetBuffer(q->swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back)))) return S_OK;
    // Not waiting for submissions, the GPU may still be on the frame before: its present list (allocator and the
    // planes' views) is used again now. One frame in flight.
    if (g_async) wait_value(q, q->present_done);
    q->present_allocator->Reset();
    q->present_list->Reset(q->present_allocator, nullptr);
    D3D12_RESOURCE_BARRIER b[2]{};
    for (auto& x : b) x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {back, 0, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST};
    b[1].Transition = {r->host, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_SOURCE};
    bool copy = format && rd.Width == q->width && rd.Height == q->height && rd.MipLevels == 1 &&
                rd.DepthOrArraySize == 1 && rd.SampleDesc.Count == 1;
    // More than one plane, or one of another size: each drawn over the back buffer, scaled to it, bottom first.
    bool drawn = false;
    // RT_SHOW_MESH=<k> (a way to look, not what the title shows): the k-th target of the last mesh draw, in the
    // scene plane's place. The lit image needs passes that aren't there yet; the geometry buffers show what the
    // mesh shaders draw.
    static int show_mesh = -2;
    if (show_mesh == -2) {
        char v[8] = {};
        show_mesh = GetEnvironmentVariableA("RT_SHOW_MESH", v, sizeof v) && v[0] >= '0' && v[0] <= '7' ? v[0] - '0' : -1;
    }
    ID3D12Resource* mesh_target = show_mesh >= 0 ? last_mesh_target(show_mesh) : nullptr;
    if ((shown.size() > 1 || !copy || mesh_target || overlay::enabled()) && blit_ready(dev, back->GetDesc().Format)) {
        drawn = true;
        Blit& bl = g_blit;
        const UINT step = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        std::vector<D3D12_RESOURCE_BARRIER> bs;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bs.push_back(b[0]);
        std::vector<ID3D12Resource*> hosts, planes_ok;
        for (Resource* x : shown) hosts.push_back(x->host);
        if (mesh_target) hosts[0] = mesh_target;
        for (ID3D12Resource* p : hosts) {
            D3D12_RESOURCE_DESC pd = p->GetDesc();
            DXGI_FORMAT pf = swapchain_format(pd.Format);
            if (!pf || pd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || pd.SampleDesc.Count != 1 ||
                planes_ok.size() >= 8 || std::find(planes_ok.begin(), planes_ok.end(), p) != planes_ok.end())
                continue;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = pf;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (pd.DepthOrArraySize > 1) {
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                sd.Texture2DArray.MipLevels = 1;
                sd.Texture2DArray.ArraySize = 1;
            } else {
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE h = bl.srv->GetCPUDescriptorHandleForHeapStart();
            h.ptr += planes_ok.size() * step;
            dev->CreateShaderResourceView(p, &sd, h);
            D3D12_RESOURCE_BARRIER x{};
            x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            x.Transition = {p, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
            bs.push_back(x);
            planes_ok.push_back(p);
        }
        ID3D12GraphicsCommandList* pl = q->present_list;
        pl->ResourceBarrier(static_cast<UINT>(bs.size()), bs.data());
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = bl.rtv->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(back, nullptr, rtv);
        const float black[4] = {0, 0, 0, 1};
        pl->ClearRenderTargetView(rtv, black, 0, nullptr);
        pl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{0, 0, static_cast<float>(q->width), static_cast<float>(q->height), 0, 1};
        D3D12_RECT sc{0, 0, static_cast<LONG>(q->width), static_cast<LONG>(q->height)};
        pl->RSSetViewports(1, &vp);
        pl->RSSetScissorRects(1, &sc);
        pl->SetGraphicsRootSignature(bl.root);
        pl->SetDescriptorHeaps(1, &bl.srv);
        pl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (size_t i = 0; i < planes_ok.size(); ++i) {
            D3D12_GPU_DESCRIPTOR_HANDLE h = bl.srv->GetGPUDescriptorHandleForHeapStart();
            h.ptr += i * step;
            pl->SetPipelineState(bl.opaque[i ? 1 : 0]);
            pl->SetGraphicsRootDescriptorTable(0, h);
            pl->DrawInstanced(3, 1, 0, 0);
        }
        if (overlay::enabled()) draw_overlay(dev, pl, q->width, q->height);
        for (auto& x : bs) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        pl->ResourceBarrier(static_cast<UINT>(bs.size()), bs.data());
        static bool said = false;
        if (!said) {
            said = true;
            std::fprintf(stderr, "[rt] present: %zu plane(s) drawn over a %ux%u back buffer (the top one %llux%u)\n",
                         planes_ok.size(), q->width, q->height,
                         static_cast<unsigned long long>(planes_ok.empty() ? 0 : planes_ok.back()->GetDesc().Width),
                         planes_ok.empty() ? 0 : planes_ok.back()->GetDesc().Height);
        }
    }
    if (drawn) {
    } else if (copy) {
        q->present_list->ResourceBarrier(2, b);
        q->present_list->CopyResource(back, r->host);
        for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        q->present_list->ResourceBarrier(2, b);
    } else {
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        q->present_list->ResourceBarrier(1, b);
        static ID3D12DescriptorHeap* heap = nullptr;
        if (!heap) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
        }
        dev->CreateRenderTargetView(back, nullptr, heap->GetCPUDescriptorHandleForHeapStart());
        const float grey[4] = {0.08f, 0.08f, 0.1f, 1};
        q->present_list->ClearRenderTargetView(heap->GetCPUDescriptorHandleForHeapStart(), grey, 0, nullptr);
        std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
        q->present_list->ResourceBarrier(1, b);
    }
    q->present_list->Close();
    ID3D12CommandList* list = q->present_list;
    report_removal("before the present list");
    q->host->ExecuteCommandLists(1, &list);
    report_removal("after the present list");
    q->swapchain->Present(0, 0);
    window::frame_shown();
    report_removal("after Present");
    back->Release();
    if (g_async) {
        std::lock_guard pl(q->pending_lock);
        q->present_done = mark_locked(q);
    } else {
        wait_idle(q);
    }
    release_retired_textures(frame);
    diag::frame_end(frame, q->host, r->host);
    return S_OK;
}

HRESULT make_list(D3D12_COMMAND_LIST_TYPE type, void* allocator, void* pso, void** out, const char* how) {
    ID3D12Device* dev = host_device();
    if (!dev) return DXGI_ERROR_DEVICE_REMOVED;
    Allocator* a = as_allocator(allocator);
    if (!a || !out) return E_INVALIDARG;
    if (pso) std::fprintf(stderr, "[rt] d3d12: %s with an initial pipeline (not passed on yet)\n", how);
    ID3D12GraphicsCommandList* host = nullptr;
    HRESULT hr = dev->CreateCommandList(0, type, a->host, nullptr, IID_PPV_ARGS(&host));
    std::fprintf(stderr, "[rt] d3d12: %s(type %d) -> %08lX\n", how, type, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return hr;
    *out = new_list(host, type);
    return S_OK;
}

// ---------------------------------------------------------------- descriptor heaps
//
// A console descriptor heap is GPU-visible memory (docs/gpu-memory.md): 32-byte view slots (T# / V#), 16-byte
// sampler slots (S#), written by the title as memory and by view-creation methods. Its slots live in unified
// memory below 4 GiB (reserve_low), and its CPU and GPU handles are both the slot's address. The PC's ID3D12
// DescriptorHeap order: 8 GetDesc, 9 GetCPUDescriptorHandleForHeapStart, 10 GetGPUDescriptorHandleForHeapStart
// (GetDesc returns its 16-byte struct through (out, this); the 8-byte handles come back in rax).

struct DescriptorHeap {
    Header header;
    D3D12_DESCRIPTOR_HEAP_DESC desc;
    uintptr_t base;      // CPU handle of slot 0
    uintptr_t gpu_base;  // GPU handle of slot 0 (below 4 GiB; the same pages)
    uint32_t increment;
};

// The console's heap description: the PC's, then memory the title allocated for the slots (0: the runtime
// allocates). 1.0.2.1 always passes it, and checks the heap's CPU start equals that address.
struct HeapDescX {
    D3D12_DESCRIPTOR_HEAP_DESC pc;
    uint64_t address;
    uint64_t bytes;
};

void** heap_table();
D3D12_DESCRIPTOR_HEAP_DESC* WINAPI heap_desc(void* a, void* b) {
    auto [h, out] = this_and_out<DescriptorHeap, D3D12_DESCRIPTOR_HEAP_DESC>(a, b, heap_table());
    if (out) *out = h->desc;
    return out;
}
// Handles are 8-byte structs: the console returns them in rax with the object in rcx (only larger structs come
// back through a buffer passed first). The title reads the result from rax.
uint64_t WINAPI heap_cpu_start(DescriptorHeap* h) { return h->base; }
uint64_t WINAPI heap_gpu_start(DescriptorHeap* h) { return h->gpu_base; }

void** heap_table() {
    static void** vt = [] {
        void** t = shims::make_probe_vtable("D3D12DescriptorHeap");
        shims::set_probe_method(t, 8, reinterpret_cast<void*>(&heap_desc));
        shims::set_probe_method(t, 9, reinterpret_cast<void*>(&heap_cpu_start));
        shims::set_probe_method(t, 10, reinterpret_cast<void*>(&heap_gpu_start));
        return t;
    }();
    return vt;
}

// ---------------------------------------------------------------- the device as IDXGIDevice
//
// Titles ask the device for IDXGIDevice to reach the adapter (and from it outputs and the factory). The adapter is
// the host's own DXGI adapter: DXGI's interfaces are the PC's. IDXGIDevice order: 3-6 IDXGIObject, 7 GetAdapter.

struct DxgiDevice {
    Header header;
};

HRESULT WINAPI dxgi_get_adapter(DxgiDevice*, IDXGIAdapter** out) {
    if (!out) return E_INVALIDARG;
    host_device();
    if (!g_adapter) return DXGI_ERROR_NOT_FOUND;
    g_adapter->AddRef();
    *out = g_adapter;
    return S_OK;
}

void** dxgi_device_table() {
    static void** vt = [] {
        void** t = shims::make_probe_vtable("IDXGIDevice");
        shims::set_probe_method(t, 7, reinterpret_cast<void*>(&dxgi_get_adapter));
        return t;
    }();
    return vt;
}

}  // namespace

void* dxgi_device() {
    static DxgiDevice d{{dxgi_device_table(), 0, 1}};
    return &d;
}

namespace {

uint32_t descriptor_size(uint32_t type) { return type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 16 : 32; }

}  // namespace




// Slot memory: the title's (its address as the CPU handle), else ours below 4 GiB. A shader-visible heap whose
// memory is above 4 GiB also gets a low alias of the same pages for its GPU handle, since shaders take descriptor
// table pointers as 32-bit addresses.
HRESULT WINAPI create_descriptor_heap(void*, const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID, void** out) {
    auto* desc = reinterpret_cast<const HeapDescX*>(d);
    if (!desc || !out || !desc->pc.NumDescriptors) return E_INVALIDARG;
    uint32_t inc = descriptor_size(desc->pc.Type);
    uint64_t bytes = uint64_t{desc->pc.NumDescriptors} * inc;
    // The extended desc is only there when the title allocated the slots itself: then the address is in GPU memory
    // and the size matches (a plain PC desc leaves whatever was on the stack after it).
    bool own = desc->address && desc->bytes == bytes && uma::contains(desc->address, bytes);
    uintptr_t base = own ? desc->address : 0, gpu = base;
    if (!base) {
        base = gpu = uma::reserve_low(bytes, uma::kPage);
        if (!base || !uma::commit(base, bytes, PAGE_READWRITE)) {
            if (base) uma::release(base);
            std::fprintf(stderr, "[rt] d3d12: no room below 4 GiB for a descriptor heap of %u x %u bytes\n",
                         desc->pc.NumDescriptors, inc);
            return E_OUTOFMEMORY;
        }
    } else if ((desc->pc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) && base + bytes > 0x100000000ull) {
        if (!uma::contains(base, bytes) || !(gpu = uma::alias_low(base, bytes))) {
            std::fprintf(stderr, "[rt] d3d12: descriptor heap memory at %p (%llu bytes) can't be given a GPU address\n",
                         reinterpret_cast<void*>(base), bytes);
            return E_OUTOFMEMORY;
        }
    }
    std::fprintf(stderr, "[rt] d3d12: CreateDescriptorHeap(type %u, %u descriptors, flags %x): CPU %p, GPU %p%s\n",
                 desc->pc.Type, desc->pc.NumDescriptors, desc->pc.Flags, reinterpret_cast<void*>(base),
                 reinterpret_cast<void*>(gpu), own ? " (title memory)" : "");
    *out = new DescriptorHeap{{heap_table(), 0, 1}, desc->pc, base, gpu, inc};
    return S_OK;
}

HRESULT WINAPI create_command_allocator(void*, D3D12_COMMAND_LIST_TYPE type, REFIID, void** out) {
    ID3D12Device* dev = host_device();
    if (!dev) return DXGI_ERROR_DEVICE_REMOVED;
    if (!out) return E_INVALIDARG;
    D3D12_COMMAND_LIST_TYPE t = host_list_type(static_cast<uint32_t>(type));
    ID3D12CommandAllocator* host = nullptr;
    HRESULT hr = dev->CreateCommandAllocator(t, IID_PPV_ARGS(&host));
    std::fprintf(stderr, "[rt] d3d12: CreateCommandAllocator(type %x) -> %08lX\n", type, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return hr;
    *out = new Allocator{{allocator_table(), 0, 1}, host, t};
    return S_OK;
}

HRESULT WINAPI create_command_list(void*, UINT, D3D12_COMMAND_LIST_TYPE type, void* allocator, void* pso, REFIID,
                                   void** out) {
    return make_list(host_list_type(static_cast<uint32_t>(type)), allocator, pso, out, "CreateCommandList");
}

HRESULT WINAPI create_command_list_x(void*, const uint32_t* params, void* allocator, void* pso, REFIID, void** out) {
    if (!params) return E_INVALIDARG;
    return make_list(host_list_type(params[0]), allocator, pso, out, "CreateCommandListX");
}

ID3D12Device* host_device() {
    static ID3D12Device* d = create_host_device();
    return d;
}

ID3D12Fence* host_fence(void* fence) {
    auto* f = static_cast<Fence*>(fence);
    return f && f->header.vtbl == fence_table() ? f->host : nullptr;
}

ID3D12CommandQueue* host_queue(void* queue) {
    auto* q = static_cast<Queue*>(queue);
    return q && q->header.vtbl == queue_table() ? q->host : nullptr;
}

HRESULT WINAPI create_command_queue(void*, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID, void** out) {
    return make_queue(desc, out, "CreateCommandQueue");
}

HRESULT WINAPI create_command_queue_x(void*, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID, void** out) {
    return make_queue(desc, out, "CreateCommandQueueX");
}

HRESULT WINAPI create_fence(void*, uint64_t initial, D3D12_FENCE_FLAGS flags, REFIID, void** out) {
    ID3D12Device* dev = host_device();
    if (!dev) return DXGI_ERROR_DEVICE_REMOVED;
    if (!out) return E_INVALIDARG;
    ID3D12Fence* host = nullptr;
    HRESULT hr = dev->CreateFence(initial, flags & D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&host));
    if (FAILED(hr)) return hr;
    auto* f = new Fence{{fence_table(), 0, 1}, host, initial, nullptr};
    static std::atomic<bool> watching{false};
    if (!watching.exchange(true)) CloseHandle(CreateThread(nullptr, 0, &fence_watch, nullptr, 0, nullptr));
    Fence* head = g_fences.load();
    do f->next = head;
    while (!g_fences.compare_exchange_weak(head, f));
    *out = f;
    return S_OK;
}

// ---------------------------------------------------------------- resource sizes

namespace {

// The PC's answer for one console desc, for the log (console formats and layouts the PC lacks: none).
D3D12_RESOURCE_ALLOCATION_INFO pc_allocation(const gfx10::ResourceDesc& c) {
    ID3D12Device* dev = host_device();
    if (!dev || c.format >= 116 || c.dimension > 4) return {UINT64_MAX, 0};
    D3D12_RESOURCE_DESC d{};
    std::memcpy(&d, &c, sizeof d);
    d.Alignment = 0;
    d.Layout = c.dimension == 1 ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags &= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    return dev->GetResourceAllocationInfo(0, 1, &d);
}

// {size, alignment} of one console resource. Buffers are plain pool memory (shaders reach them by address), so they
// get the 4 KiB alignment the title asks for, else 64 KiB. Textures get the console's layout; what it doesn't model
// (MSAA, 3D render targets) gets the PC's size, logged.
D3D12_RESOURCE_ALLOCATION_INFO console_allocation(const gfx10::ResourceDesc& d, const char** how) {
    if (d.dimension == 1) {
        uint64_t align = d.alignment == 4096 ? 4096 : 65536;
        *how = "buffer";
        return {(d.width + align - 1) / align * align, align};
    }
    gfx10::Texture t;
    switch (gfx10::layout_texture(d, &t)) {
        case gfx10::Result::ok: {
            *how = "console layout";
            // placement alignment: the surface's, at least the 4 KiB D3D12 places resources at
            uint64_t align = t.align < 4096 ? 4096 : t.align;
            return {t.size ? t.size : align, align};
        }
        case gfx10::Result::not_modelled: *how = "PC size (layout not modelled)"; break;
        case gfx10::Result::invalid: *how = "invalid"; return {UINT64_MAX, 0};
    }
    return pc_allocation(d);
}

std::mutex g_alloc_log_lock;
uint64_t g_alloc_logged[64][3];
int g_alloc_logged_count;

void log_allocation(const gfx10::ResourceDesc& d, const D3D12_RESOURCE_ALLOCATION_INFO& r, const char* how) {
    uint64_t key[3] = {d.dimension | uint64_t{d.format} << 8 | uint64_t{d.layout} << 16 | uint64_t{d.flags} << 32,
                       d.width | uint64_t{d.height} << 32, d.depth_or_array | uint64_t{d.mips} << 16 | d.alignment << 32};
    {
        std::lock_guard lock(g_alloc_log_lock);
        for (int i = 0; i < g_alloc_logged_count; ++i)
            if (!std::memcmp(g_alloc_logged[i], key, sizeof key)) return;
        if (g_alloc_logged_count == 64) return;
        std::memcpy(g_alloc_logged[g_alloc_logged_count++], key, sizeof key);
    }
    D3D12_RESOURCE_ALLOCATION_INFO pc = d.dimension == 1 ? D3D12_RESOURCE_ALLOCATION_INFO{0, 0} : pc_allocation(d);
    std::fprintf(stderr,
                 "[rt] d3d12: allocation info: dim %u %llux%ux%u mips %u format %u layout %#x flags %#x align %#llx "
                 "-> %#llx bytes, align %#llx (%s; PC %#llx / %#llx)\n",
                 d.dimension, static_cast<unsigned long long>(d.width), d.height, d.depth_or_array, d.mips, d.format,
                 d.layout, d.flags, static_cast<unsigned long long>(d.alignment),
                 static_cast<unsigned long long>(r.SizeInBytes), static_cast<unsigned long long>(r.Alignment), how,
                 static_cast<unsigned long long>(pc.SizeInBytes), static_cast<unsigned long long>(pc.Alignment));
}

}  // namespace

// ---------------------------------------------------------------- resources
//
// A console resource is unified memory: its bytes are in the pool, inside the GPU-visible range, and its GPU
// virtual address is their CPU address (shaders reach buffers by address; docs/gpu-memory.md). Textures also get a
// PC texture in video memory, which draws will use; the title's memory holds the console's tiled bytes.
// The PC's ID3D12Resource order: 8 Map, 9 Unmap, 10 GetDesc, 11 GetGPUVirtualAddress, 12 WriteToSubresource,
// 13 ReadFromSubresource, 14 GetHeapProperties, 15 GetProtectedResourceSession, 16 GetDesc1.

namespace {

HRESULT WINAPI resource_map(Resource* r, UINT, const D3D12_RANGE*, void** out) {
    if (out) *out = reinterpret_cast<void*>(r->va);
    return S_OK;
}
void WINAPI resource_unmap(Resource*, UINT, const D3D12_RANGE*) {}
gfx10::ResourceDesc* WINAPI resource_desc(void* a, void* b) {
    auto [r, out] = this_and_out<Resource, gfx10::ResourceDesc>(a, b, resource_table());
    if (out) *out = r->desc.desc;
    return out;
}
ResourceDesc1* WINAPI resource_desc1(void* a, void* b) {
    auto [r, out] = this_and_out<Resource, ResourceDesc1>(a, b, resource_table());
    if (out) *out = r->desc;
    return out;
}
uint64_t WINAPI resource_gpu_address(Resource* r) { return r->va; }
HRESULT WINAPI resource_heap_properties(Resource* r, D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS* flags) {
    if (props) *props = r->heap;
    if (flags) *flags = r->heap_flags;
    return S_OK;
}

}  // namespace

// Resources are counted (the header's count, as on the console): the title releases the textures it streams out, and
// the PC texture goes with the last reference (retired: unbound, released a few presents later). The object itself and
// its memory stay (a released pointer still met reads as a resource with no PC texture). Targets keep theirs: the
// records of their views hold the PC texture.
std::atomic<uint64_t> g_resources_released{0}, g_textures_released{0};

HRESULT WINAPI resource_query_interface(Resource* r, REFIID, void** out) {
    if (!out) return E_POINTER;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&r->header.refs));
    *out = r;
    return S_OK;
}
ULONG WINAPI resource_add_ref(Resource* r) {
    return static_cast<ULONG>(InterlockedIncrement(reinterpret_cast<volatile LONG*>(&r->header.refs)));
}
ULONG WINAPI resource_release(Resource* r) {
    const LONG n = InterlockedDecrement(reinterpret_cast<volatile LONG*>(&r->header.refs));
    if (n > 0) return static_cast<ULONG>(n);
    if (n < 0) {  // released more than held: stays at none
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&r->header.refs));
        return 0;
    }
    const uint64_t released = ++g_resources_released;
    if (r->streamed) {
        if (r->host) ++g_textures_released;
        drop_streamed(r);
    } else if (r->host && !(r->desc.desc.flags & 3)) {
        ID3D12Resource* host = r->host;
        r->host = nullptr;
        retire_texture(host);
        ++g_textures_released;
    }
    if ((released & (released - 1)) == 0 && released >= 64)
        std::fprintf(stderr, "[rt] d3d12: the title has released %llu resources; %llu PC textures went with them\n",
                     static_cast<unsigned long long>(released),
                     static_cast<unsigned long long>(g_textures_released.load()));
    return 0;
}

void** resource_table() {
    static void** vt = [] {
        void** t = shims::make_probe_vtable("D3D12Resource");
        shims::set_probe_method(t, 0, reinterpret_cast<void*>(&resource_query_interface));
        shims::set_probe_method(t, 1, reinterpret_cast<void*>(&resource_add_ref));
        shims::set_probe_method(t, 2, reinterpret_cast<void*>(&resource_release));
        shims::set_probe_method(t, 8, reinterpret_cast<void*>(&resource_map));
        shims::set_probe_method(t, 9, reinterpret_cast<void*>(&resource_unmap));
        shims::set_probe_method(t, 10, reinterpret_cast<void*>(&resource_desc));
        shims::set_probe_method(t, 11, reinterpret_cast<void*>(&resource_gpu_address));
        shims::set_probe_method(t, 14, reinterpret_cast<void*>(&resource_heap_properties));
        shims::set_probe_method(t, 16, reinterpret_cast<void*>(&resource_desc1));
        return t;
    }();
    return vt;
}

Resource* as_resource(void* p) { return is_object(p, resource_table()) ? static_cast<Resource*>(p) : nullptr; }

bool is_object(void* p, void** table) {
    if (!p) return false;
    __try {
        return static_cast<Header*>(p)->vtbl == table;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace {

}  // namespace

namespace {
thread_local const char* t_texture_source = "other";
}
const char* texture_source(const char* set) {
    const char* was = t_texture_source;
    if (set) t_texture_source = set;
    return was;
}

namespace {

// What the live PC textures add up to, by shape (placed textures retired since are taken off), listed each time
// 512 MiB more have been made: where the video memory goes.
struct TextureKind {
    uint64_t width;
    uint32_t height, depth, mips, format, flags;
    bool operator<(const TextureKind& o) const {
        return std::tie(width, height, depth, mips, format, flags) <
               std::tie(o.width, o.height, o.depth, o.mips, o.format, o.flags);
    }
};
struct TextureTally {
    uint64_t count = 0, bytes = 0;
};
std::mutex g_tally_lock;
std::map<TextureKind, TextureTally> g_tally;
uint64_t g_tally_bytes = 0, g_tally_told = 0, g_tally_made = 0;
// by what made them (texture_source): bytes made, bytes live
std::map<std::string, std::pair<uint64_t, uint64_t>> g_tally_source;
std::map<ID3D12Resource*, std::pair<std::string, uint64_t>>* g_tally_of = new std::map<ID3D12Resource*, std::pair<std::string, uint64_t>>;

void note_texture_made(ID3D12Device* dev, const D3D12_RESOURCE_DESC& d, ID3D12Resource* host) {
    uint64_t bytes = dev->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
    std::lock_guard lock(g_tally_lock);
    {
        auto& src = g_tally_source[texture_source()];
        src.first += bytes;
        src.second += bytes;
        (*g_tally_of)[host] = {texture_source(), bytes};
    }
    TextureTally& t = g_tally[{d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, static_cast<uint32_t>(d.Format),
                               static_cast<uint32_t>(d.Flags)}];
    ++t.count;
    t.bytes += bytes;
    g_tally_bytes += bytes;
    g_tally_made += bytes;
    if (g_tally_made - g_tally_told < (512ull << 20)) return;
    g_tally_told = g_tally_made;
    std::vector<std::pair<TextureKind, TextureTally>> all(g_tally.begin(), g_tally.end());
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
    uint64_t targets = 0;
    for (const auto& [k, v] : all)
        if (k.flags & 3) targets += v.bytes;
    std::fprintf(stderr, "[rt] texture memory live: %llu MiB (%llu MiB made so far) in %zu shapes (%llu MiB of it "
                         "targets); largest:\n",
                 static_cast<unsigned long long>(g_tally_bytes >> 20),
                 static_cast<unsigned long long>(g_tally_made >> 20), all.size(),
                 static_cast<unsigned long long>(targets >> 20));
    for (const auto& [name, v] : g_tally_source)
        std::fprintf(stderr, "[rt]   made by %s: %llu MiB live of %llu MiB made\n", name.c_str(),
                     static_cast<unsigned long long>(v.second >> 20), static_cast<unsigned long long>(v.first >> 20));
    for (size_t i = 0; i < all.size() && i < 14; ++i)
        std::fprintf(stderr, "[rt]   %5llu MiB: %llu x %llux%ux%u mips %u format %u flags %#x\n",
                     static_cast<unsigned long long>(all[i].second.bytes >> 20),
                     static_cast<unsigned long long>(all[i].second.count),
                     static_cast<unsigned long long>(all[i].first.width), all[i].first.height, all[i].first.depth,
                     all[i].first.mips, all[i].first.format, all[i].first.flags);
}

}  // namespace

void note_texture_released(ID3D12Resource* texture) {
    ID3D12Device* dev = host_device();
    if (!dev || !texture) return;
    const D3D12_RESOURCE_DESC d = texture->GetDesc();
    const uint64_t bytes = dev->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
    std::lock_guard lock(g_tally_lock);
    TextureTally& t = g_tally[{d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, static_cast<uint32_t>(d.Format),
                               static_cast<uint32_t>(d.Flags)}];
    if (t.count) --t.count;
    t.bytes -= std::min(t.bytes, bytes);
    g_tally_bytes -= std::min(g_tally_bytes, bytes);
    if (auto it = g_tally_of->find(texture); it != g_tally_of->end()) {
        auto& src = g_tally_source[it->second.first];
        src.second -= std::min(src.second, it->second.second);
        g_tally_of->erase(it);
    }
}

// The PC texture for a console one: the PC's layout, the console's own flag bits off; console-only formats
// (116-120) aren't made yet.
ID3D12Resource* make_host_texture(const gfx10::ResourceDesc& c, const D3D12_CLEAR_VALUE* clear) {
    ID3D12Device* dev = host_device();
    if (!dev || c.dimension < 2 || c.dimension > 4 || c.format >= 116) return nullptr;
    D3D12_RESOURCE_DESC d{};
    std::memcpy(&d, &c, sizeof d);
    d.Alignment = 0;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags &= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE |
               D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    // The PC refuses depth targets that are also unordered-access (the console allows both: 1.0.2.1's shadow maps
    // ask for it); the PC copy loses UAV use. Depth formats become typeless so shaders can read them too, and sRGB
    // textures with unordered access typeless (the PC has no sRGB UAVs). As VIctory does.
    if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) d.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) {
        switch (d.Format) {
            case DXGI_FORMAT_D32_FLOAT: d.Format = DXGI_FORMAT_R32_TYPELESS; break;
            case DXGI_FORMAT_D16_UNORM: d.Format = DXGI_FORMAT_R16_TYPELESS; break;
            case DXGI_FORMAT_D24_UNORM_S8_UINT: d.Format = DXGI_FORMAT_R24G8_TYPELESS; break;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: d.Format = DXGI_FORMAT_R32G8X24_TYPELESS; break;
            default: break;
        }
    }
    if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
        switch (d.Format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; break;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: d.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS; break;
            case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: d.Format = DXGI_FORMAT_B8G8R8X8_TYPELESS; break;
            default: break;
        }
    }
    // Formats the console draws and computes into and the PC doesn't: a stand-in the PC can write, as VIctory
    // does. R9G9B9E5 (67) becomes RGBA16F: 1.0.2.1 makes the upscaler's output, the scene at the display size
    // (1920x1080), in it, as a target with unordered access, and the PC refused the texture (80070057), so nothing
    // after the upscale had a scene to read. B5G5R5A1 (86) and B4G4R4A4 (115) become RGBA8. Views of such a texture
    // take its format (d3d12x_bindless.cpp: fitted); targets and compute outputs are never filled from the
    // title's memory (d3d12x_upload.cpp), so the bytes' layout doesn't matter.
    D3D12_CLEAR_VALUE fitted_clear{};
    if (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        const DXGI_FORMAT asked = d.Format;
        switch (d.Format) {
            case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
            case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM: d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
            default: break;
        }
        if (d.Format != asked) {
            static std::atomic<int> told{0};
            if (told++ < 8)
                std::fprintf(stderr, "[rt] d3d12: a texture of format %d (%llux%u, flags %#x) made as format %d\n",
                             static_cast<int>(asked), static_cast<unsigned long long>(d.Width), d.Height,
                             static_cast<unsigned>(d.Flags), static_cast<int>(d.Format));
            if (clear) {
                fitted_clear = *clear;
                fitted_clear.Format = d.Format;
                clear = &fitted_clear;
            }
        }
    }
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    bool target = d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    ID3D12Resource* host = nullptr;
    report_removal("before a texture is created");
    HRESULT hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                              target ? clear : nullptr, IID_PPV_ARGS(&host));
    if (FAILED(hr) && target && clear)  // a clear value the PC refuses for this format
        hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(&host));
    if (hr == DXGI_ERROR_DEVICE_REMOVED) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true))
            std::fprintf(stderr, "[rt] d3d12: the PC device was removed: %08lX\n",
                         static_cast<unsigned long>(dev->GetDeviceRemovedReason()));
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "[rt] d3d12: the PC refused a texture (dim %u %llux%ux%u format %u flags %#x): %08lX\n",
                     c.dimension, static_cast<unsigned long long>(c.width), c.height, c.depth_or_array, c.format,
                     c.flags, static_cast<unsigned long>(hr));
        return nullptr;
    }
    note_texture_made(dev, d, host);
    return host;
}

namespace {

std::atomic<uint64_t> g_committed_bytes{0};

// XALLOC_ATTRIBUTES of the console runtime's resource memory: allocator ID 128 (bits 16-23), page size 2 (bits
// 14-15; what 1.0.2.1's render-target budget handler accepts).
constexpr uint64_t kResourceAttributes = (128ull << 16) | (2ull << 14);

HRESULT make_committed(const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags, const ResourceDesc1& desc,
                       const D3D12_CLEAR_VALUE* clear, void** out, const char* how) {
    if (!out) return S_FALSE;  // a validation call
    const char* sized = "";
    D3D12_RESOURCE_ALLOCATION_INFO info = console_allocation(desc.desc, &sized);
    if (info.SizeInBytes == UINT64_MAX) {
        std::fprintf(stderr, "[rt] d3d12: %s: a description the console refuses (dim %u format %u layout %#x)\n", how,
                     desc.desc.dimension, desc.desc.format, desc.desc.layout);
        return E_INVALIDARG;
    }
    // The memory comes from XMemAlloc as the console's runtime asks for it (allocator ID 128, 64 KiB pages), so it
    // goes through the title's allocation hooks, which budget it.
    uint64_t bytes = (info.SizeInBytes + uma::kPage - 1) & ~uint64_t{uma::kPage - 1};
    auto va = reinterpret_cast<uintptr_t>(shims::xmem_alloc(bytes, kResourceAttributes));
    if (!va) {
        std::fprintf(stderr, "[rt] d3d12: %s: no memory for %llu bytes\n", how, static_cast<unsigned long long>(bytes));
        return E_OUTOFMEMORY;
    }
    if (!uma::contains(va, bytes))
        std::fprintf(stderr, "[rt] d3d12: %s: the title's allocator gave memory outside the GPU-visible range (%p)\n",
                     how, reinterpret_cast<void*>(va));
    auto* r = new Resource{{resource_table(), 0, 1}, desc, va, bytes, props ? *props : D3D12_HEAP_PROPERTIES{}, flags,
                           nullptr};
    if (desc.desc.dimension != 1) r->host = make_host_texture(desc.desc, clear);
    uint64_t total = g_committed_bytes += bytes;
    std::fprintf(stderr,
                 "[rt] d3d12: %s(heap %u, dim %u %llux%ux%u format %u flags %#x): %llu KiB at %p%s (committed %llu MiB)\n",
                 how, props ? props->Type : 0, desc.desc.dimension, static_cast<unsigned long long>(desc.desc.width),
                 desc.desc.height, desc.desc.depth_or_array, desc.desc.format, desc.desc.flags,
                 static_cast<unsigned long long>(bytes >> 10), reinterpret_cast<void*>(va),
                 r->host ? ", PC texture" : "", static_cast<unsigned long long>(total >> 20));
    *out = r;
    return S_OK;
}

}  // namespace

HRESULT WINAPI create_committed_resource(void*, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags,
                                         const void* desc, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE* clear,
                                         REFIID, void** out) {
    if (!desc) return E_INVALIDARG;
    ResourceDesc1 d{};
    d.desc = *static_cast<const gfx10::ResourceDesc*>(desc);
    return make_committed(props, flags, d, clear, out, "CreateCommittedResource");
}

HRESULT WINAPI create_committed_resource3(void*, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags,
                                          const void* desc1, UINT /*initial layout*/, const D3D12_CLEAR_VALUE* clear,
                                          void* /*protected session*/, UINT, const DXGI_FORMAT*, REFIID, void** out) {
    if (!desc1) return E_INVALIDARG;
    return make_committed(props, flags, *static_cast<const ResourceDesc1*>(desc1), clear, out,
                          "CreateCommittedResource3");
}

// ---------------------------------------------------------------- root signatures
//
// Kept as the title described them (the PC's deserializer reads the blob, which our D3D12SerializeVersionedRootSignature
// made, or the shader compiler did): translated shaders run with the runtime's own root signature, and this one says
// how the title's root arguments become the shader's initial user SGPRs.

RootSignature* as_root_signature(void* p) { return is_object(p, root_signature_table()) ? static_cast<RootSignature*>(p) : nullptr; }

void** root_signature_table() {
    static void** vt = shims::make_probe_vtable("D3D12RootSignature");
    return vt;
}

HRESULT WINAPI create_root_signature(void*, UINT, const void* blob, SIZE_T size, REFIID, void** out) {
    if (!blob || !size || !out) return E_INVALIDARG;
    ID3D12VersionedRootSignatureDeserializer* des = nullptr;
    HRESULT hr = D3D12CreateVersionedRootSignatureDeserializer(blob, size, IID_PPV_ARGS(&des));
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
    if (SUCCEEDED(hr)) hr = des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[rt] d3d12: CreateRootSignature: a blob the PC can't read (%08lX, %zu bytes)\n",
                     static_cast<unsigned long>(hr), static_cast<size_t>(size));
        if (des) des->Release();
        return E_INVALIDARG;
    }
    static std::atomic<int> count{0};
    if (count++ < 4)
        std::fprintf(stderr, "[rt] d3d12: CreateRootSignature(%u parameters, %u static samplers, flags %#x)\n",
                     desc->Desc_1_1.NumParameters, desc->Desc_1_1.NumStaticSamplers, desc->Desc_1_1.Flags);
    *out = new RootSignature{{root_signature_table(), 0, 1}, des, &desc->Desc_1_1};
    return S_OK;
}

// ---------------------------------------------------------------- pipeline states
//
// The title's shaders are console binaries (RDNA2 code in the console's container). A pipeline keeps copies of them
// and its root signature; the PC pipeline is made from translated shaders when a command list first uses it.

namespace {

std::vector<uint8_t> copy_code(const D3D12_SHADER_BYTECODE& b) {
    if (!b.pShaderBytecode || !b.BytecodeLength) return {};
    auto* p = static_cast<const uint8_t*>(b.pShaderBytecode);
    return std::vector<uint8_t>(p, p + b.BytecodeLength);
}

// The first few shaders' containers: DXBC (magic, 16-byte hash, version, size, chunk count, chunk offsets; each chunk
// a fourcc and a size), logged as their chunks.
void note_container(const Code& stage_code, const char* stage) {
    const auto bytes = stage_code.bytes();
    static const std::vector<uint8_t> kNone;
    const std::vector<uint8_t>& code = bytes ? *bytes : kNone;
    static std::atomic<int> logged{0};
    if (code.size() < 32 || logged++ >= 6) return;
    uint32_t chunks;
    std::memcpy(&chunks, code.data() + 28, 4);
    std::fprintf(stderr, "[rt] d3d12: %s shader: %zu bytes, %.4s, %u chunks:", stage, code.size(),
                 reinterpret_cast<const char*>(code.data()), chunks);
    for (uint32_t i = 0; i < chunks && i < 16 && 32 + 4 * i + 4 <= code.size(); ++i) {
        uint32_t at, size;
        std::memcpy(&at, code.data() + 32 + 4 * i, 4);
        if (at + 8 > code.size()) break;
        std::memcpy(&size, code.data() + at + 4, 4);
        std::fprintf(stderr, " %.4s(%u)", reinterpret_cast<const char*>(code.data() + at), size);
    }
    std::fprintf(stderr, "\n");
}

}  // namespace

void** pipeline_table() {
    static void** vt = shims::make_probe_vtable("D3D12PipelineState");
    return vt;
}

PipelineState* as_pipeline(void* p) { return is_object(p, pipeline_table()) ? static_cast<PipelineState*>(p) : nullptr; }

ID3D12GraphicsCommandList* host_list_of(void* list) {
    List* l = as_list(list);
    if (l && !l->closed) release_targets(l->gfx, l->host);  // the caller records into it: targets as it expects
    return l ? l->host : nullptr;
}
bool is_resource(void* p) { return as_resource(p) != nullptr; }

// CreateComputePipelineState: the PC's at slot 11 (desc, iid, out); the console's at 62 (desc, 0, 0, iid, out).
HRESULT WINAPI create_compute_pipeline(void*, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID, void** out) {
    if (!desc || !out) return E_INVALIDARG;
    auto* p = new PipelineState{{pipeline_table(), 0, 1}, true, as_root_signature(desc->pRootSignature), {}};
    p->code[0] = copy_code(desc->CS);
    note_container(p->code[0], "compute");
    attach_container_shaders(*p);
    *out = p;
    return S_OK;
}
HRESULT WINAPI create_compute_pipeline_x(void* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, void*, void*,
                                         REFIID iid, void** out) {
    return create_compute_pipeline(device, desc, iid, out);
}

// CreateGraphicsPipelineState (slot 10): VS, PS, DS, HS, GS kept (code[0..4]); the fixed-function state is kept whole.
HRESULT WINAPI create_graphics_pipeline(void*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID, void** out) {
    if (!desc || !out) return E_INVALIDARG;
    auto* p = new PipelineState{{pipeline_table(), 0, 1}, false, as_root_signature(desc->pRootSignature), {}};
    p->code[0] = copy_code(desc->VS);
    p->code[1] = copy_code(desc->PS);
    p->code[2] = copy_code(desc->DS);
    p->code[3] = copy_code(desc->HS);
    p->code[4] = copy_code(desc->GS);
    p->graphics = *desc;
    note_container(p->code[0], "vertex");
    note_container(p->code[1], "pixel");
    attach_container_shaders(*p);
    *out = p;
    return S_OK;
}

D3D12_RESOURCE_ALLOCATION_INFO* WINAPI get_resource_allocation_info(D3D12_RESOURCE_ALLOCATION_INFO* out, void* device,
                                                                    UINT, UINT count, const void* descs) {
    if (shims::probe_object_name(out)) out = static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(device);  // (device, buffer)
    uint64_t size = 0, align = 0;
    auto* d = static_cast<const gfx10::ResourceDesc*>(descs);
    for (UINT i = 0; i < count && d; ++i) {
        const char* how = "";
        D3D12_RESOURCE_ALLOCATION_INFO r = console_allocation(d[i], &how);
        log_allocation(d[i], r, how);
        if (r.SizeInBytes == UINT64_MAX || !r.Alignment) {
            size = UINT64_MAX;
            break;
        }
        size = (size + r.Alignment - 1) / r.Alignment * r.Alignment + r.SizeInBytes;
        align = align > r.Alignment ? align : r.Alignment;
    }
    if (out) *out = {size, align ? align : 4096};
    return out;
}

// GetResourceAllocationInfo2(out, this, mask, count, D3D12_RESOURCE_DESC1s, D3D12_RESOURCE_ALLOCATION_INFO1* each)
// (slot 108): as slot 25, also telling where each resource goes.
D3D12_RESOURCE_ALLOCATION_INFO* WINAPI get_resource_allocation_info2(D3D12_RESOURCE_ALLOCATION_INFO* out, void* device,
                                                                     UINT, UINT count, const ResourceDesc1* descs,
                                                                     D3D12_RESOURCE_ALLOCATION_INFO1* each) {
    if (shims::probe_object_name(out)) out = static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(device);  // (device, buffer)
    uint64_t size = 0, align = 0;
    for (UINT i = 0; i < count && descs; ++i) {
        const char* how = "";
        D3D12_RESOURCE_ALLOCATION_INFO r = console_allocation(descs[i].desc, &how);
        log_allocation(descs[i].desc, r, how);
        if (r.SizeInBytes == UINT64_MAX || !r.Alignment) {
            size = UINT64_MAX;
            if (each) each[i] = {UINT64_MAX, 0, UINT64_MAX};
            break;
        }
        uint64_t at = (size + r.Alignment - 1) / r.Alignment * r.Alignment;
        if (each) each[i] = {at, r.Alignment, r.SizeInBytes};
        size = at + r.SizeInBytes;
        align = align > r.Alignment ? align : r.Alignment;
    }
    if (out) *out = {size, align ? align : 4096};
    return out;
}

// Made at startup: the runtime has no thread-safe statics.
[[maybe_unused]] const bool g_made_early =
    (fence_table(), queue_table(), allocator_table(), list_table(), heap_table(), resource_table(), root_signature_table(),
     pipeline_table(), dxgi_device(),
     true);

}  // namespace rt::d3d12x
