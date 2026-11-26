// Tests persistent local storage through the QueryApiImpl object, as title code calls it.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "../shims/files.h"
#include "../shims/storage.h"
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

constexpr GUID kProvider = {0xf4faf4d4, 0x2d04, 0x4fce, {0xb3, 0xe0, 0x47, 0x4a, 0x71, 0x3a, 0x3e, 0x84}};
constexpr GUID kIid2026 = {0x41a4e10c, 0x5a7e, 0x41d9, {0x8c, 0x37, 0x37, 0xbd, 0xe6, 0x2a, 0x07, 0xd6}};
constexpr GUID kQueue = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};

struct SpaceInfo {
    uint64_t available_free, total_free, used, total;
};
struct AsyncBlock {
    void* queue;
    void* context;
    void(CALLBACK* callback)(AsyncBlock*);
    uintptr_t internal[4];
};

template <class F>
F slot(void* obj, int i) {
    return reinterpret_cast<F>((*static_cast<void***>(obj))[i]);
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    fs::path base = fs::temp_directory_path() / L"flamingo_storage_test";
    fs::remove_all(base);
    fs::create_directories(base / L"game");
    std::ofstream(base / L"game" / L"MicrosoftGame.Config")
        << "<Game><PersistentLocalStorage><SizeMB>1</SizeMB></PersistentLocalStorage></Game>";
    rt::storage::init(base / L"game", base / L"storage");

    void* obj = rt::xgr::find_area(kProvider, kIid2026);
    void* queue = rt::xgr::find_area(kQueue, kQueue);
    CHECK(obj && queue);
    if (!obj || !queue) return 1;

    size_t size = 0;
    CHECK(slot<HRESULT(CALLBACK*)(void*, size_t*)>(obj, 3)(obj, &size) == S_OK && size == 4);  // "X:\" + NUL
    char path[8] = {};
    size_t used = 0;
    auto get_path = slot<HRESULT(CALLBACK*)(void*, size_t, char*, size_t*)>(obj, 4);
    CHECK(get_path(obj, 3, path, &used) == static_cast<HRESULT>(0x80004005));  // too small: E_FAIL
    CHECK(get_path(obj, sizeof(path), path, &used) == S_OK && used == 4 && path[1] == ':' && path[2] == '\\');

    // The drive is the storage folder.
    std::string file = std::string(path) + "save.bin";
    std::wstring host = rt::files::to_host(file.c_str());
    CHECK(fs::path(host).parent_path() == base / L"storage");
    std::ofstream(host, std::ios::binary) << std::string(1000, 'x');

    SpaceInfo info{};
    CHECK(slot<HRESULT(CALLBACK*)(void*, SpaceInfo*)>(obj, 5)(obj, &info) == S_OK);
    CHECK(info.total == (1u << 20) && info.used == 1000 && info.total_free == (1u << 20) - 1000);
    CHECK(info.available_free <= info.total_free);

    // Asking for more space: granted within the quota, declined past it; the result comes back asynchronously.
    using Prompt = HRESULT(CALLBACK*)(void*, uint64_t, void*);
    using Result = HRESULT(CALLBACK*)(void*, void*);
    using Status = HRESULT(CALLBACK*)(void*, AsyncBlock*, bool);
    AsyncBlock a{}, b{};
    CHECK(slot<Prompt>(obj, 6)(obj, 1000, &a) == S_OK);
    CHECK(slot<Status>(queue, 3)(queue, &a, true) == S_OK);
    CHECK(slot<Result>(obj, 7)(obj, &a) == S_OK);
    CHECK(slot<Prompt>(obj, 6)(obj, 2u << 20, &b) == S_OK);
    CHECK(slot<Status>(queue, 3)(queue, &b, true) == static_cast<HRESULT>(0x80004004));
    CHECK(slot<Result>(obj, 7)(obj, &b) == static_cast<HRESULT>(0x80004004));

    fs::remove_all(base);
    if (g_failures) {
        std::fprintf(stderr, "storage_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("storage_test: ok\n");
    return 0;
}
