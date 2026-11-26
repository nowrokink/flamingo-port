// Tests XGameSave through the QueryApiImpl object, as title code calls it: a container written, read back, partly
// updated, enumerated and deleted, with the synchronous calls and the asynchronous read.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../shims/shims.h"
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

constexpr GUID kProvider = {0x704C3F58, 0xE629, 0x4CC2, {0xB1, 0x97, 0x30, 0x51, 0x1B, 0x99, 0x6F, 0xE2}};
constexpr GUID kIid2026 = {0x1BFFF3AF, 0xF14A, 0x40A3, {0x8E, 0x35, 0x9A, 0xDA, 0x90, 0x65, 0x93, 0xF9}};

struct AsyncBlock {
    void* queue;
    void* context;
    void(CALLBACK* callback)(AsyncBlock*);
    uintptr_t internal[4];
};
struct ContainerInfo {
    const char* name;
    const char* display;
    uint32_t blobs;
    uint64_t total;
    int64_t modified;
    bool needs_sync;
};
struct Blob {
    const char* name;
    uint32_t size;
    uint8_t* data;
};

template <class F>
F slot(void* obj, int i) {
    return reinterpret_cast<F>((*static_cast<void***>(obj))[i]);
}

struct Seen {
    std::vector<std::string> names, displays;
    std::vector<uint32_t> blobs;
};
bool CALLBACK on_container(const ContainerInfo* c, void* ctx) {
    auto* s = static_cast<Seen*>(ctx);
    s->names.push_back(c->name);
    s->displays.push_back(c->display);
    s->blobs.push_back(c->blobs);
    return true;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    fs::path base = fs::temp_directory_path() / L"flamingo_gamesave_test";
    fs::remove_all(base);
    rt::shims::set_save_root(base.wstring());
    void* gs = rt::xgr::find_area(kProvider, kIid2026);
    CHECK(gs);
    if (!gs) return 1;

    using Init = HRESULT(WINAPI*)(void*, void*, const char*, bool, void**);
    using Create = HRESULT(WINAPI*)(void*, void*, const char*, void**);
    using CreateUpdate = HRESULT(WINAPI*)(void*, void*, const char*, void**);
    using Write = HRESULT(WINAPI*)(void*, void*, const char*, const uint8_t*, size_t);
    using Del = HRESULT(WINAPI*)(void*, void*, const char*);
    using Submit = HRESULT(WINAPI*)(void*, void*);
    using Close = void(WINAPI*)(void*, void*);
    using Read = HRESULT(WINAPI*)(void*, void*, const char**, uint32_t*, size_t, uint8_t*);
    using Enumerate = HRESULT(WINAPI*)(void*, void*, void*, bool(CALLBACK*)(const ContainerInfo*, void*));
    using DeleteContainer = HRESULT(WINAPI*)(void*, void*, const char*);

    void* provider = nullptr;
    CHECK(slot<Init>(gs, 3)(gs, nullptr, "config", false, &provider) == S_OK && provider);

    // write two blobs
    void *container = nullptr, *update = nullptr;
    CHECK(slot<Create>(gs, 16)(gs, provider, "Slot 1/profile", &container) == S_OK);
    CHECK(slot<CreateUpdate>(gs, 23)(gs, container, "Profile", &update) == S_OK);
    const uint8_t a[] = {1, 2, 3}, b[] = {9, 8, 7, 6, 5};
    CHECK(slot<Write>(gs, 25)(gs, update, "a", a, sizeof a) == S_OK);
    CHECK(slot<Write>(gs, 25)(gs, update, "b.dat", b, sizeof b) == S_OK);
    CHECK(slot<Submit>(gs, 27)(gs, update) == S_OK);
    slot<Close>(gs, 24)(gs, update);

    // read them back
    std::vector<uint8_t> buf(4096);
    uint32_t count = 0;
    CHECK(slot<Read>(gs, 20)(gs, container, nullptr, &count, buf.size(), buf.data()) == S_OK);
    CHECK(count == 2);
    auto* blobs = reinterpret_cast<Blob*>(buf.data());
    bool found_a = false, found_b = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!std::strcmp(blobs[i].name, "a")) found_a = blobs[i].size == 3 && !std::memcmp(blobs[i].data, a, 3);
        if (!std::strcmp(blobs[i].name, "b.dat")) found_b = blobs[i].size == 5 && !std::memcmp(blobs[i].data, b, 5);
    }
    CHECK(found_a && found_b);

    // an update replacing one blob and deleting the other: all or nothing, and the rest kept
    const uint8_t c[] = {42};
    CHECK(slot<CreateUpdate>(gs, 23)(gs, container, nullptr, &update) == S_OK);
    CHECK(slot<Write>(gs, 25)(gs, update, "c", c, 1) == S_OK);
    CHECK(slot<Del>(gs, 26)(gs, update, "b.dat") == S_OK);
    CHECK(slot<Submit>(gs, 27)(gs, update) == S_OK);
    slot<Close>(gs, 24)(gs, update);
    count = 0;
    CHECK(slot<Read>(gs, 20)(gs, container, nullptr, &count, buf.size(), buf.data()) == S_OK);
    CHECK(count == 2);
    found_a = false;
    bool found_c = false, still_b = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!std::strcmp(blobs[i].name, "a")) found_a = blobs[i].size == 3 && !std::memcmp(blobs[i].data, a, 3);
        if (!std::strcmp(blobs[i].name, "c")) found_c = blobs[i].size == 1 && blobs[i].data[0] == 42;
        if (!std::strcmp(blobs[i].name, "b.dat")) still_b = true;
    }
    CHECK(found_a && found_c && !still_b);

    // the asynchronous read: size, then the layout with pointers into the caller's buffer
    AsyncBlock block{};  // no queue: the process queue
    using ReadAsync = HRESULT(WINAPI*)(void*, void*, const char**, uint32_t, AsyncBlock*);
    using ReadResult = HRESULT(WINAPI*)(void*, AsyncBlock*, size_t, uint8_t*, uint32_t*);
    const char* one[] = {"c"};
    CHECK(slot<ReadAsync>(gs, 21)(gs, container, one, 1, &block) == S_OK);
    std::vector<uint8_t> abuf(4096);
    uint32_t acount = 0;
    CHECK(slot<ReadResult>(gs, 22)(gs, &block, abuf.size(), abuf.data(), &acount) == S_OK);
    auto* ablobs = reinterpret_cast<Blob*>(abuf.data());
    CHECK(acount == 1 && !std::strcmp(ablobs[0].name, "c") && ablobs[0].size == 1 && ablobs[0].data[0] == 42);

    // enumerate: the exact name (with characters a file name can't hold) and display name
    Seen seen;
    CHECK(slot<Enumerate>(gs, 14)(gs, provider, &seen, &on_container) == S_OK);
    CHECK(seen.names.size() == 1 && seen.names[0] == "Slot 1/profile" && seen.displays[0] == "Profile" &&
          seen.blobs[0] == 2);

    // delete
    CHECK(slot<DeleteContainer>(gs, 10)(gs, provider, "Slot 1/profile") == S_OK);
    seen = {};
    slot<Enumerate>(gs, 14)(gs, provider, &seen, &on_container);
    CHECK(seen.names.empty());

    slot<Close>(gs, 17)(gs, container);
    slot<Close>(gs, 6)(gs, provider);
    fs::remove_all(base);
    std::printf(g_failures ? "%d FAILED\n" : "all gamesave checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
