// Persistent local storage (XPersistentLocalStorage*): a host folder of the title's own, seen by the title as the
// root of a drive, as on the console (titles build paths on that), with the quota its MicrosoftGame.Config
// declares. Served through QueryApiImpl; slots in the public API's order, the object first:
//   3 GetPathSize(size_t*)  4 GetPath(size, char*, size_t* used)  5 GetSpaceInfo(SpaceInfo*)
//   6 PromptUserForSpaceAsync(u64 bytes, XAsyncBlock*)  7 PromptUserForSpaceResult(XAsyncBlock*)
// 1.0.2.1 asks with interface 41A4E10C-...; the older id D29411DF-... has the same slots.
#include "storage.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "com_probe.h"
#include "files.h"
#include "xgameruntime.h"

namespace rt::storage {
namespace {

constexpr GUID kProvider = {0xf4faf4d4, 0x2d04, 0x4fce, {0xb3, 0xe0, 0x47, 0x4a, 0x71, 0x3a, 0x3e, 0x84}};
constexpr GUID kIid = {0xd29411df, 0x0794, 0x4553, {0x8b, 0x27, 0x95, 0xfc, 0x02, 0xd0, 0xf7, 0x5d}};
constexpr GUID kIid2026 = {0x41a4e10c, 0x5a7e, 0x41d9, {0x8c, 0x37, 0x37, 0xbd, 0xe6, 0x2a, 0x07, 0xd6}};

constexpr HRESULT kFail = static_cast<HRESULT>(0x80004005);         // E_FAIL: the documented "buffer too small"
constexpr HRESULT kNotSupported = static_cast<HRESULT>(0x80070032);  // not set up
constexpr HRESULT kAbort = static_cast<HRESULT>(0x80004004);

std::filesystem::path g_folder;
uint64_t g_quota = 0;
std::string g_path;  // "X:\", what the title gets

struct SpaceInfo {
    uint64_t available_free;  // writable now
    uint64_t total_free;      // left in the quota
    uint64_t used;
    uint64_t total;           // the quota
};

uint64_t used_bytes() {
    uint64_t sum = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(g_folder, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) sum += it->file_size(ec);
    return sum;
}

SpaceInfo space() {
    uint64_t used = used_bytes();
    uint64_t left = g_quota > used ? g_quota - used : 0;
    ULARGE_INTEGER disk{};
    uint64_t now = GetDiskFreeSpaceExW(g_folder.c_str(), &disk, nullptr, nullptr) ? disk.QuadPart : left;
    return {left < now ? left : now, left, used, g_quota};
}

using This = void*;

HRESULT CALLBACK get_path_size(This, size_t* out) {
    if (g_path.empty()) return kNotSupported;
    if (!out) return E_POINTER;
    *out = g_path.size() + 1;
    return S_OK;
}

HRESULT CALLBACK get_path(This, size_t size, char* buffer, size_t* used) {
    if (g_path.empty()) return kNotSupported;
    if (!buffer || size <= g_path.size()) return kFail;
    std::memcpy(buffer, g_path.c_str(), g_path.size() + 1);
    if (used) *used = g_path.size() + 1;
    return S_OK;
}

HRESULT CALLBACK get_space_info(This, SpaceInfo* out) {
    if (g_path.empty()) return kNotSupported;
    if (!out) return E_POINTER;
    *out = space();
    return S_OK;
}

// There's nobody to ask: granted while the quota has room, else declined.
const char kPromptIdentity = 0;
HRESULT CALLBACK prompt_for_space(This, uint64_t bytes, void* block) {
    if (g_path.empty()) return kNotSupported;
    HRESULT hr = bytes <= space().total_free ? S_OK : kAbort;
    std::fprintf(stderr, "[rt] storage: %llu more bytes asked for -> %s\n", bytes, hr == S_OK ? "granted" : "declined");
    return xgr::finish_async(block, &kPromptIdentity, hr);
}

HRESULT CALLBACK prompt_result(This, void* block) { return xgr::async_result(block, &kPromptIdentity); }

shims::ProbeObject* make_object() {
    void** vt = shims::make_probe_vtable("PersistentLocalStorage");
    shims::set_probe_method(vt, 3, reinterpret_cast<void*>(&get_path_size));
    shims::set_probe_method(vt, 4, reinterpret_cast<void*>(&get_path));
    shims::set_probe_method(vt, 5, reinterpret_cast<void*>(&get_space_info));
    shims::set_probe_method(vt, 6, reinterpret_cast<void*>(&prompt_for_space));
    shims::set_probe_method(vt, 7, reinterpret_cast<void*>(&prompt_result));
    return shims::make_probe_object(vt, "PersistentLocalStorage");
}

shims::ProbeObject* const g_object = make_object();
xgr::AreaRegistration g_area(kProvider, kIid, g_object);
xgr::AreaRegistration g_area_2026(kProvider, kIid2026, g_object);

// <PersistentLocalStorage><SizeMB>n</SizeMB> in the title's MicrosoftGame.Config; 0 if absent.
uint64_t quota_from_config(const std::filesystem::path& game_dir) {
    std::ifstream f(game_dir / L"MicrosoftGame.Config");
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    size_t block = text.find("<PersistentLocalStorage");
    if (block == std::string::npos) return 0;
    size_t tag = text.find("<SizeMB>", block);
    size_t end = text.find("</PersistentLocalStorage", block);
    if (tag == std::string::npos || (end != std::string::npos && tag > end)) return 0;
    return std::strtoull(text.c_str() + tag + 8, nullptr, 10) << 20;
}

}  // namespace

void init(const std::filesystem::path& game_dir, const std::filesystem::path& folder) {
    g_quota = quota_from_config(game_dir);
    if (!g_quota) {
        std::fprintf(stderr, "[rt] storage: the title declares no persistent local storage\n");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    g_folder = folder;
    wchar_t letter = files::mount_free(folder);
    if (!letter) {
        std::fprintf(stderr, "[rt] storage: no drive letter left for %ls\n", folder.c_str());
        return;
    }
    g_path = std::string(1, static_cast<char>(letter)) + ":\\";
    std::fprintf(stderr, "[rt] storage: %s (%llu MiB)\n", g_path.c_str(), g_quota >> 20);
}

}  // namespace rt::storage
