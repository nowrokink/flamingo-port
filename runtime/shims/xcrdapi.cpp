// XCRDAPI.dll: the console's DirectStorage. The exe imports only DStorageGetFactory; the rest is COM objects:
//
//   factory   4 OpenFile(path, iid, out), 5 CreateStatusArray(capacity, name, iid, out), 8 SetStagingBufferSize(bytes),
//             10 CreateQueue(desc, iid, out)
//   queue     3 EnqueueRequest(request*), 4 EnqueueStatus(array, index), 6 Submit(), 8 Close()
//   status    3 IsComplete(index), 4 GetHResult(index)
//
// (slots as the VIctory project reads them from 1.0.2.1's call sites; other slots are probes that log.)
//
// A request (0x48 bytes): options @0 (bit 0 zlib, bits 1-4 a BCPack format, bit 21 destination in 64 KiB physical
// pages, bit 22 source in memory), destination @8 (an address, or an array of page numbers), offset in the first
// page @16 (low 16 bits), destination size @24, source @32 (our file, or an address), file offset @40, source size
// @48. Each queue runs its requests in order on a thread of its own; a status entry completes once every request
// enqueued before it has, with the first failure since the previous entry. BCPack is decoded by bcpack.cpp
// (execute_bcpack).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "bcpack.h"
#include "com_probe.h"
#include "files.h"
#include "inflate.h"
#include "shims.h"
#include "uma.h"

namespace {

constexpr uint64_t kZlib = 1, kBcpack = 0xFull << 1, kPages = 1ull << 21, kMemory = 1ull << 22;
constexpr uint64_t kPage = 64 * 1024;

struct Object {
    void** vtbl;
    std::atomic<uint32_t> refs{1};
};

uint32_t WINAPI add_ref(Object* o) { return ++o->refs; }
// Objects stay alive: the title releases queues after Close and files at shutdown; nothing here is large.
uint32_t WINAPI release(Object* o) { return o->refs ? --o->refs : 0; }
HRESULT WINAPI query_interface(Object* o, const GUID*, void** out) {
    if (!out) return E_POINTER;
    add_ref(o);
    *out = o;
    return S_OK;
}

void** table(const char* name) {
    void** t = rt::shims::make_probe_vtable(name);
    rt::shims::set_probe_method(t, 0, reinterpret_cast<void*>(&query_interface));
    rt::shims::set_probe_method(t, 1, reinterpret_cast<void*>(&add_ref));
    rt::shims::set_probe_method(t, 2, reinterpret_cast<void*>(&release));
    return t;
}

void note(const char* what) {
    static std::mutex m;
    static std::vector<const char*> seen;
    std::lock_guard lock(m);
    for (const char* s : seen)
        if (s == what) return;
    seen.push_back(what);
    std::fprintf(stderr, "[rt] dstorage: %s\n", what);
}

void** file_table();
void** status_table();
void** queue_table();

// ---- files

struct File : Object {
    HANDLE handle;
};

// ---- status arrays

struct StatusArray : Object {
    std::vector<std::atomic<HRESULT>> entries;
};

bool WINAPI status_is_complete(StatusArray* a, uint32_t i) {
    return i < a->entries.size() && a->entries[i].load(std::memory_order_acquire) != E_PENDING;
}
HRESULT WINAPI status_result(StatusArray* a, uint32_t i) {
    return i < a->entries.size() ? a->entries[i].load(std::memory_order_acquire) : E_INVALIDARG;
}

// ---- requests

struct Request {
    uint64_t options;
    void* destination;
    uint64_t page_offset;
    uint32_t destination_size;
    uint32_t pad;
    const void* source;
    uint64_t offset;
    uint32_t source_size;
    uint32_t intermediate_size;
    uint64_t tag;
    const char* name;
};
static_assert(sizeof(Request) == 0x48);

bool read_file(HANDLE h, uint64_t offset, uint8_t* out, uint32_t size) {
    uint32_t done = 0;
    while (done < size) {
        OVERLAPPED o{};
        uint64_t at = offset + done;
        o.Offset = static_cast<DWORD>(at);
        o.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD n = 0;
        if (!ReadFile(h, out + done, size - done, &n, &o) || !n) return false;
        done += n;
    }
    return true;
}

// Writes `data` to the request's destination.
HRESULT put(const Request& r, const uint8_t* data, size_t size) {
    if (!(r.options & kPages)) {
        std::memcpy(r.destination, data, size);
        return S_OK;
    }
    auto* pages = static_cast<const uint64_t*>(r.destination);
    size_t at = r.page_offset & 0xFFFF, done = 0;
    for (size_t k = 0; done < size; ++k, at = 0) {
        uint8_t* page = rt::uma::frame_pointer(pages[k]);
        if (!page) return E_INVALIDARG;
        size_t n = std::min<size_t>(kPage - at, size - done);
        std::memcpy(page + at, data + done, n);
        done += n;
    }
    return S_OK;
}

bool is_file(const void* p) {
    if (!p) return false;
    __try {
        return *static_cast<void* const*>(p) == file_table();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// What the queues did, logged every few seconds they were busy: a load that stops shows here whether the title
// still asks for data, and what kind (zlib, BCPack, plain) and outcome.
std::atomic<uint64_t> g_stat_requests{0}, g_stat_bytes{0}, g_stat_failed{0}, g_stat_bcpack{0}, g_stat_zlib{0},
    g_stat_since{0}, g_stat_bcpack_decoded{0};
void stats(const void* request_options, uint32_t bytes, HRESULT hr) {
    const uint64_t options = *static_cast<const uint64_t*>(request_options);
    ++g_stat_requests;
    g_stat_bytes += bytes;
    if (FAILED(hr)) ++g_stat_failed;
    if (options & kBcpack) ++g_stat_bcpack;
    if (options & kZlib) ++g_stat_zlib;
    uint64_t now = GetTickCount64(), since = g_stat_since.load();
    if (now - since >= 5000 && g_stat_since.compare_exchange_strong(since, now))
        std::fprintf(stderr,
                     "[rt] dstorage: so far %llu requests, %llu MiB out, %llu zlib, %llu BCPack (%llu decoded), "
                     "%llu failed\n",
                     (unsigned long long)g_stat_requests.load(), (unsigned long long)(g_stat_bytes.load() >> 20),
                     (unsigned long long)g_stat_zlib.load(), (unsigned long long)g_stat_bcpack.load(),
                     (unsigned long long)g_stat_bcpack_decoded.load(), (unsigned long long)g_stat_failed.load());
}

// The destinations of the BCPack requests left blank (the last 16,384): so a crash of the title's can be told from
// one over data that was never there.
struct Blank {
    std::atomic<uint64_t> at{0}, bytes{0};
};
constexpr uint64_t kBlanks = 1 << 14;
Blank g_blanks[kBlanks];
std::atomic<uint64_t> g_blank_next{0};

void note_blank(const void* destination, uint64_t bytes) {
    Blank& b = g_blanks[g_blank_next.fetch_add(1) % kBlanks];
    b.bytes = bytes;
    b.at = reinterpret_cast<uint64_t>(destination);
}

// A BCPack request (options bits 1-4 = the BC format, 1-7; bit 0 = the packed stream is itself zlib-compressed).
// Decoded by bcpack.cpp (BC1, BC3-BC7). A failed request makes 1.0.2.1 stop (an assert once its texture databases
// don't load), so for a stream that doesn't decode (or BC2, which no title data uses) the destination is filled with
// zeros instead: such a texture is blank (black or transparent), and the title goes on.
std::atomic<uint64_t> g_bcpack_decoded[8], g_bcpack_blank[8];

HRESULT execute_bcpack(const Request& r) {
    const int format = static_cast<int>((r.options & kBcpack) >> 1);
    std::vector<uint8_t> source, inflated, out(r.destination_size);
    const uint8_t* src = nullptr;
    size_t src_size = r.source_size;
    rt::bcpack::Result result = rt::bcpack::Result::unsupported;
    if (rt::bcpack::supported(format)) {
        if (r.options & kMemory) {
            src = static_cast<const uint8_t*>(r.source);
        } else {
            source.resize(r.source_size);
            if (!read_file(static_cast<const File*>(r.source)->handle, r.offset, source.data(), r.source_size))
                return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
            src = source.data();
        }
        if (r.options & kZlib) {
            // The packed size after zlib isn't known for sure (the request's field @0x34 may be it): a bound that grows.
            int64_t n = -1;
            size_t cap = std::max<size_t>(r.intermediate_size, r.destination_size + r.destination_size / 4 + 4096);
            for (int tries = 0; n < 0 && tries < 3; ++tries, cap *= 4) {
                inflated.resize(cap);
                n = rt::inflate::zlib(src, r.source_size, inflated.data(), inflated.size());
            }
            src = n < 0 ? nullptr : inflated.data();
            src_size = n < 0 ? 0 : static_cast<size_t>(n);
        }
        result = src ? rt::bcpack::decode(src, src_size, format, out.data(), out.size())
                     : rt::bcpack::Result::bad_stream;
    }
    if (result == rt::bcpack::Result::ok) {
        ++g_stat_bcpack_decoded;
        if (g_bcpack_decoded[format]++ == 0)
            std::fprintf(stderr, "[rt] dstorage: first %s BCPack request decoded (%u -> %u bytes)\n",
                         rt::bcpack::name(format), r.source_size, r.destination_size);
        return put(r, out.data(), out.size());
    }
    if (g_bcpack_blank[format]++ < 4)
        std::fprintf(stderr, "[rt] dstorage: a %s BCPack request left blank (%s; options %llx, %u -> %u bytes)\n",
                     rt::bcpack::name(format),
                     result == rt::bcpack::Result::unsupported ? "no decoder for it" : "the stream didn't decode",
                     static_cast<unsigned long long>(r.options), r.source_size, r.destination_size);
    if (!(r.options & kPages)) note_blank(r.destination, r.destination_size);
    std::memset(out.data(), 0, out.size());
    return put(r, out.data(), out.size());
}

HRESULT execute_one(const Request& r);
HRESULT execute(const Request& r) {
    HRESULT hr = execute_one(r);
    stats(&r.options, r.destination_size, hr);
    return hr;
}

HRESULT execute_one(const Request& r) {
    static std::atomic<int> logged{0};
    if (logged++ < 6)
        std::fprintf(stderr, "[rt] dstorage: request options %llx dest %p (%u bytes) source %p @%llx (%u bytes)%s\n",
                     static_cast<unsigned long long>(r.options), r.destination, r.destination_size, r.source,
                     static_cast<unsigned long long>(r.offset), r.source_size,
                     (r.options & kMemory) || is_file(r.source) ? "" : " - not one of our files");
    if (!(r.options & kMemory) && !is_file(r.source)) return E_INVALIDARG;
    if (r.options & ~(kZlib | kBcpack | kPages | kMemory)) {
        note("a request with options not modelled (failed)");
        return E_NOTIMPL;
    }
    if (r.options & kBcpack) return execute_bcpack(r);
    const auto* file = static_cast<const File*>(r.source);
    bool direct = !(r.options & (kZlib | kPages | kMemory));
    if (direct)  // a plain read lands in place
        return read_file(file->handle, r.offset, static_cast<uint8_t*>(r.destination), r.source_size)
                   ? S_OK
                   : HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_HANDLE_EOF);
    std::vector<uint8_t> source;
    const uint8_t* src;
    if (r.options & kMemory) {
        src = static_cast<const uint8_t*>(r.source);
    } else {
        source.resize(r.source_size);
        if (!read_file(file->handle, r.offset, source.data(), r.source_size))
            return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        src = source.data();
    }
    if (!(r.options & kZlib)) return put(r, src, r.source_size);
    if (!(r.options & kPages)) {
        int64_t n = rt::inflate::zlib(src, r.source_size, static_cast<uint8_t*>(r.destination), r.destination_size);
        return n < 0 ? HRESULT_FROM_WIN32(ERROR_INVALID_DATA) : S_OK;
    }
    std::vector<uint8_t> out(r.destination_size);
    int64_t n = rt::inflate::zlib(src, r.source_size, out.data(), out.size());
    if (n < 0) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    return put(r, out.data(), static_cast<size_t>(n));
}

// ---- queues

struct Command {
    bool status;
    Request request;
    StatusArray* array;
    uint32_t index;
};

struct Queue : Object {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Command> enqueued, submitted;
    bool closed = false;
    HRESULT failure = S_OK;  // the first failure since the last status entry
    std::string name;
};

DWORD WINAPI queue_thread(void* p) {
    auto* q = static_cast<Queue*>(p);
    for (;;) {
        Command c;
        {
            std::unique_lock lock(q->m);
            q->cv.wait(lock, [&] { return q->closed || !q->submitted.empty(); });
            if (q->submitted.empty()) return 0;
            c = q->submitted.front();
            q->submitted.pop_front();
        }
        if (c.status) {
            c.array->entries[c.index].store(q->failure, std::memory_order_release);
            q->failure = S_OK;
            continue;
        }
        HRESULT hr = execute(c.request);
        if (FAILED(hr)) {
            static std::atomic<int> logged{0};
            if (logged++ < 16)
                std::fprintf(stderr, "[rt] dstorage: request failed %08lX (options %llx, %u -> %u bytes, at %llx)\n",
                             static_cast<unsigned long>(hr), static_cast<unsigned long long>(c.request.options),
                             c.request.source_size, c.request.destination_size,
                             static_cast<unsigned long long>(c.request.offset));
            if (SUCCEEDED(q->failure)) q->failure = hr;
        }
    }
}

void WINAPI queue_enqueue_request(Queue* q, const Request* r) {
    if (!r) return;
    std::lock_guard lock(q->m);
    q->enqueued.push_back({false, *r, nullptr, 0});
}
void WINAPI queue_enqueue_status(Queue* q, StatusArray* a, uint32_t index) {
    if (!a || index >= a->entries.size()) return;
    a->entries[index].store(E_PENDING, std::memory_order_release);
    std::lock_guard lock(q->m);
    q->enqueued.push_back({true, {}, a, index});
}
void WINAPI queue_submit(Queue* q) {
    {
        std::lock_guard lock(q->m);
        for (auto& c : q->enqueued) q->submitted.push_back(c);
        q->enqueued.clear();
    }
    q->cv.notify_one();
}
void WINAPI queue_close(Queue* q) {
    {
        std::lock_guard lock(q->m);
        q->closed = true;
        q->enqueued.clear();
    }
    q->cv.notify_one();
}

// ---- the factory

struct QueueDesc {
    uint64_t source_type;
    uint16_t capacity;
    int8_t priority;
    const char* name;
    uint64_t unknown;
};


// HRESULT OpenFile(const wchar_t* path, iid, out) (factory slot 4). The path is UTF-16 ("G:\appdata0.rpf"), as on
// the PC's DirectStorage. Read as 8-bit text it was "G", no file opened, and the title's package device (which opens
// every file here) found nothing: the reason the G: device was once replaced from outside (game_patches.cpp), which
// in turn bypassed the device's own decompression of RDS5-wrapped files (.ttpf texture databases).
HRESULT WINAPI factory_open_file(Object*, const char* path, const GUID*, void** out) {
    if (!path || !out) return E_INVALIDARG;
    const bool wide = path[0] && !path[1];  // an ASCII character then a zero byte: UTF-16
    std::wstring host = wide ? rt::files::to_host(reinterpret_cast<const wchar_t*>(path)) : rt::files::to_host(path);
    HANDLE h = CreateFileW(host.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    const DWORD open_error = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    static std::atomic<int> logged{0};
    if (logged++ < 12 || h == INVALID_HANDLE_VALUE)
        std::fprintf(stderr, "[rt] dstorage: OpenFile(%ls): %s (%lu)\n", host.c_str(),
                     h == INVALID_HANDLE_VALUE ? "failed" : "ok", open_error);
    if (h == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(open_error);
    auto* f = new File{};
    f->vtbl = file_table();
    f->handle = h;
    *out = f;
    return S_OK;
}

HRESULT WINAPI factory_status_array(Object*, uint32_t capacity, const char*, const GUID*, void** out) {
    if (!out || !capacity) return E_INVALIDARG;
    auto* a = new StatusArray{};
    a->vtbl = status_table();
    a->entries = std::vector<std::atomic<HRESULT>>(capacity);
    for (auto& e : a->entries) e.store(S_OK);
    *out = a;
    return S_OK;
}

HRESULT WINAPI factory_staging(Object*, uint32_t bytes) {
    std::fprintf(stderr, "[rt] dstorage: staging buffer %u MiB\n", bytes >> 20);
    return S_OK;
}

HRESULT WINAPI factory_create_queue(Object*, const QueueDesc* desc, const GUID*, void** out) {
    if (!desc || !out) return E_INVALIDARG;
    auto* q = new Queue{};
    q->vtbl = queue_table();
    q->name = desc->name ? desc->name : "";
    HANDLE t = CreateThread(nullptr, 0, &queue_thread, q, 0, nullptr);
    if (!t) {
        delete q;
        return E_OUTOFMEMORY;
    }
    CloseHandle(t);
    std::fprintf(stderr, "[rt] dstorage: queue \"%s\" (source %llu, capacity %u, priority %d)\n", q->name.c_str(),
                 static_cast<unsigned long long>(desc->source_type), desc->capacity, desc->priority);
    *out = q;
    return S_OK;
}

void** factory_table() {
    static void** t = [] {
        void** v = table("DStorageFactory");
        rt::shims::set_probe_method(v, 4, reinterpret_cast<void*>(&factory_open_file));
        rt::shims::set_probe_method(v, 5, reinterpret_cast<void*>(&factory_status_array));
        rt::shims::set_probe_method(v, 8, reinterpret_cast<void*>(&factory_staging));
        rt::shims::set_probe_method(v, 10, reinterpret_cast<void*>(&factory_create_queue));
        return v;
    }();
    return t;
}
void** file_table() {
    static void** t = table("DStorageFile");
    return t;
}
void** status_table() {
    static void** t = [] {
        void** v = table("DStorageStatusArray");
        rt::shims::set_probe_method(v, 3, reinterpret_cast<void*>(&status_is_complete));
        rt::shims::set_probe_method(v, 4, reinterpret_cast<void*>(&status_result));
        return v;
    }();
    return t;
}
void** queue_table() {
    static void** t = [] {
        void** v = table("DStorageQueue");
        rt::shims::set_probe_method(v, 3, reinterpret_cast<void*>(&queue_enqueue_request));
        rt::shims::set_probe_method(v, 4, reinterpret_cast<void*>(&queue_enqueue_status));
        rt::shims::set_probe_method(v, 6, reinterpret_cast<void*>(&queue_submit));
        rt::shims::set_probe_method(v, 8, reinterpret_cast<void*>(&queue_close));
        return v;
    }();
    return t;
}

Object g_factory;
// Made at startup: the runtime has no thread-safe statics.
[[maybe_unused]] const bool g_made_early = (g_factory.vtbl = factory_table(), file_table(), status_table(),
                                            queue_table(), true);

HRESULT WINAPI DStorageGetFactory(const GUID* iid, void** out) {
    if (!out) return E_POINTER;
    std::fprintf(stderr, "[rt] DStorageGetFactory({%08lX-...})\n", iid ? iid->Data1 : 0ul);
    *out = &g_factory;
    return S_OK;
}

}  // namespace

void* rt::shims::make_dstorage_file(void* handle) {
    auto* f = new File{};
    f->vtbl = file_table();
    f->handle = handle;
    return f;
}

SHIM_TABLE(xcrdapi,
    {"xcrdapi.dll", "DStorageGetFactory", reinterpret_cast<void*>(&DStorageGetFactory)},
);

namespace rt::shims {
// Whether `address` is inside the destination of a BCPack request that was left blank; its start and size if so.
bool in_blank_bcpack(uint64_t address, uint64_t* start, uint64_t* bytes) {
    for (const Blank& b : g_blanks) {
        const uint64_t at = b.at.load(), n = b.bytes.load();
        if (at && address >= at && address < at + n) {
            *start = at;
            *bytes = n;
            return true;
        }
    }
    return false;
}
}  // namespace rt::shims
