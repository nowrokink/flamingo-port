// XGameSave: the user's saves, on disk under %LOCALAPPDATA%\Flamingo\saves (next to persistent local storage), offline:
// no cloud copy, so nothing ever needs a sync. Signatures from the public GDK documentation; slots from gdk_api.inc
// (1.0.2.1 asks with interface 1BFFF3AF, the public API's arguments in the same order).
//
// On disk: saves\<configuration id>\<container>\v<N>\ holds one version of a container: a `manifest` (its name, display
// name and blob names, so names never have to be file names) and blob `i` as the file `i`. An update writes a whole new
// version (`v<N>.new`), flushed, then one rename commits it and older versions go: all or nothing, as documented.
// A container's folder is its name made safe plus a hash of the exact name.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "shims.h"
#include "xgameruntime.h"

namespace fs = std::filesystem;
namespace xgr = rt::xgr;

namespace {
// Every call, the first few times each (what the title does with its saves: a load that waits on one shows here).
std::atomic<int> g_traced{0};
void trace(const char* what) {
    if (g_traced++ < 200) std::fprintf(stderr, "[rt] xgamesave: %s\n", what);
}
}  // namespace

namespace {

constexpr HRESULT kInvalidContainerName = static_cast<HRESULT>(0x80830001);
constexpr HRESULT kBufferTooSmall = static_cast<HRESULT>(0x80830007);
constexpr HRESULT kContainerNotFound = static_cast<HRESULT>(0x80830008);  // E_GS_BLOB_NOT_FOUND
constexpr int64_t kQuota = 256ll << 20;  // per user and title, as documented

std::mutex g_lock;  // every save operation, so none sees another's half-made version
fs::path* g_root = new fs::path;

// ---- names and versions

std::string safe(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '-' || c == '_') out += static_cast<char>(c);
        else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            out += b;
        }
        if (out.size() > 60) break;
    }
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : name) h = (h ^ c) * 1099511628211ull;
    char tail[24];
    std::snprintf(tail, sizeof tail, "~%016llx", static_cast<unsigned long long>(h));
    return out + tail;
}

struct Version {
    fs::path dir;
    uint32_t number = 0;
    std::string name, display;
    std::vector<std::string> blobs;  // blob i is the file `i`
};

bool read_manifest(Version& v) {
    std::ifstream f(v.dir / "manifest", std::ios::binary);
    auto text = [&](std::string& s) {
        uint32_t n = 0;
        if (!f.read(reinterpret_cast<char*>(&n), 4)) return false;
        s.resize(n);
        return static_cast<bool>(f.read(s.data(), n));
    };
    if (!text(v.name) || !text(v.display)) return false;
    std::string b;
    while (text(b)) v.blobs.push_back(b);
    return true;
}

// The container's newest committed version; what a crash left behind goes.
bool newest(const fs::path& container, Version* out) {
    std::error_code ec;
    if (!fs::is_directory(container, ec)) return false;
    Version best;
    for (auto& e : fs::directory_iterator(container, ec)) {
        std::string n = e.path().filename().string();
        if (n.size() > 4 && n.ends_with(".new")) {
            fs::remove_all(e.path(), ec);
            continue;
        }
        if (n.size() < 2 || n[0] != 'v') continue;
        uint32_t number = static_cast<uint32_t>(std::strtoul(n.c_str() + 1, nullptr, 10));
        if (number >= best.number) best = {e.path(), number};
    }
    if (best.dir.empty() || !read_manifest(best)) return false;
    *out = best;
    return true;
}

uint64_t blob_size(const Version& v, size_t i) {
    std::error_code ec;
    auto n = fs::file_size(v.dir / std::to_string(i), ec);
    return ec ? 0 : n;
}

// ---- handles

struct Provider {
    fs::path root;
};
struct Container {
    Provider* provider;
    std::string name;
    fs::path dir() const { return provider->root / safe(name); }
};
struct Update {
    Container* container;
    std::string display;
    std::map<std::string, std::vector<uint8_t>> writes;
    std::set<std::string> deletes;
};

// ---- the area

const char kInit = 0, kQuotaId = 0, kDelete = 0, kRead = 0, kSubmit = 0;

HRESULT init_provider(void*, void*, const char* config, bool, Provider** out) {
    if (!out) return E_POINTER;
    fs::path root = *g_root / safe(config ? config : "default");
    std::error_code ec;
    fs::create_directories(root, ec);
    *out = new Provider{root};
    std::fprintf(stderr, "[rt] xgamesave: provider for \"%s\"\n", config ? config : "");
    return S_OK;
}
HRESULT WINAPI initialize(void* t, void* user, const char* config, bool sync, Provider** out) { trace("initialize");
    return init_provider(t, user, config, sync, out);
}
HRESULT WINAPI initialize_async(void* t, void* user, const char* config, bool sync, void* block) { trace("initialize_async");
    Provider* p = nullptr;
    HRESULT hr = init_provider(t, user, config, sync, &p);
    std::vector<uint8_t> bytes(sizeof p);
    std::memcpy(bytes.data(), &p, sizeof p);
    return xgr::finish_async_with(block, &kInit, hr, bytes);
}
HRESULT WINAPI initialize_result(void*, void* block, Provider** out) { trace("initialize_result");
    return xgr::async_result_into(block, &kInit, sizeof *out, out, nullptr);
}
void WINAPI close_provider(void*, Provider* p) { trace("close_provider"); delete p; }

int64_t used_bytes(const fs::path& root) {
    int64_t n = 0;
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(root, ec))
        if (e.is_regular_file(ec)) n += static_cast<int64_t>(e.file_size(ec));
    return n;
}
HRESULT WINAPI quota(void*, Provider* p, int64_t* out) { trace("quota");
    if (!p || !out) return E_INVALIDARG;
    std::lock_guard lock(g_lock);
    *out = std::max<int64_t>(0, kQuota - used_bytes(p->root));
    return S_OK;
}
HRESULT WINAPI quota_async(void* t, Provider* p, void* block) { trace("quota_async");
    int64_t q = 0;
    HRESULT hr = quota(t, p, &q);
    std::vector<uint8_t> bytes(8);
    std::memcpy(bytes.data(), &q, 8);
    return xgr::finish_async_with(block, &kQuotaId, hr, bytes);
}
HRESULT WINAPI quota_result(void*, void* block, int64_t* out) { trace("quota_result");
    return xgr::async_result_into(block, &kQuotaId, 8, out, nullptr);
}

HRESULT remove_container(Provider* p, const char* name) {
    if (!p || !name) return E_INVALIDARG;
    std::lock_guard lock(g_lock);
    fs::path dir = p->root / safe(name), gone = dir;
    gone += ".deleting";
    std::error_code ec;
    if (!fs::exists(dir, ec)) return S_OK;
    fs::rename(dir, gone, ec);  // one step, then the rest
    fs::remove_all(ec ? dir : gone, ec);
    return S_OK;
}
HRESULT WINAPI delete_container(void*, Provider* p, const char* name) { trace("delete_container"); return remove_container(p, name); }
HRESULT WINAPI delete_async(void*, Provider* p, const char* name, void* block) { trace("delete_async");
    return xgr::finish_async(block, &kDelete, remove_container(p, name));
}
HRESULT WINAPI delete_result(void*, void* block) { trace("delete_result"); return xgr::async_result(block, &kDelete); }

// XGameSaveContainerInfo: {name, display name, blob count, total size, last modified (time_t), needs sync}
struct ContainerInfo {
    const char* name;
    const char* display;
    uint32_t blobs;
    uint64_t total;
    int64_t modified;
    bool needs_sync;
};
using ContainerCallback = bool(CALLBACK*)(const ContainerInfo*, void*);
struct BlobInfo {
    const char* name;
    uint32_t size;
};
using BlobCallback = bool(CALLBACK*)(const BlobInfo*, void*);

// Calls back for each container passing `keep`, never under our lock (the callback may call us).
template <typename Keep>
HRESULT each_container(Provider* p, void* context, ContainerCallback callback, Keep keep) {
    if (!p || !callback) return E_INVALIDARG;
    std::vector<Version> found;
    {
        std::lock_guard lock(g_lock);
        std::error_code ec;
        for (auto& e : fs::directory_iterator(p->root, ec)) {
            Version v;
            if (e.is_directory(ec) && newest(e.path(), &v) && keep(v.name)) found.push_back(v);
        }
    }
    for (const Version& v : found) {
        uint64_t total = 0;
        for (size_t i = 0; i < v.blobs.size(); ++i) total += blob_size(v, i);
        std::error_code ec;
        auto t = fs::last_write_time(v.dir, ec);
        int64_t modified = ec ? 0
                              : std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::clock_cast<std::chrono::system_clock>(t).time_since_epoch())
                                    .count();
        ContainerInfo info{v.name.c_str(), v.display.c_str(), static_cast<uint32_t>(v.blobs.size()), total, modified,
                           false};
        if (!callback(&info, context)) break;
    }
    return S_OK;
}
HRESULT WINAPI container_info(void*, Provider* p, const char* name, void* context, ContainerCallback callback) { trace("container_info");
    std::string n = name ? name : "";
    return each_container(p, context, callback, [&](const std::string& c) { return c == n; });
}
HRESULT WINAPI enumerate_containers(void*, Provider* p, void* context, ContainerCallback callback) { trace("enumerate_containers");
    return each_container(p, context, callback, [](const std::string&) { return true; });
}
HRESULT WINAPI enumerate_containers_by_name(void*, Provider* p, const char* prefix, void* context,
                                            ContainerCallback callback) { trace("enumerate_containers_by_name");
    std::string n = prefix ? prefix : "";
    return each_container(p, context, callback, [&](const std::string& c) { return c.starts_with(n); });
}

HRESULT WINAPI create_container(void*, Provider* p, const char* name, Container** out) { trace("create_container");
    if (!p || !name || !*name || std::strlen(name) > 256) return kInvalidContainerName;
    if (!out) return E_POINTER;
    *out = new Container{p, name};  // it exists once an update to it is submitted
    return S_OK;
}
void WINAPI close_container(void*, Container* c) { trace("close_container"); delete c; }

template <typename Keep>
HRESULT each_blob(Container* c, void* context, BlobCallback callback, Keep keep) {
    if (!c || !callback) return E_INVALIDARG;
    Version v;
    {
        std::lock_guard lock(g_lock);
        if (!newest(c->dir(), &v)) return S_OK;
    }
    for (size_t i = 0; i < v.blobs.size(); ++i) {
        if (!keep(v.blobs[i])) continue;
        BlobInfo info{v.blobs[i].c_str(), static_cast<uint32_t>(blob_size(v, i))};
        if (!callback(&info, context)) break;
    }
    return S_OK;
}
HRESULT WINAPI enumerate_blobs(void*, Container* c, void* context, BlobCallback callback) { trace("enumerate_blobs");
    return each_blob(c, context, callback, [](const std::string&) { return true; });
}
HRESULT WINAPI enumerate_blobs_by_name(void*, Container* c, const char* prefix, void* context, BlobCallback callback) { trace("enumerate_blobs_by_name");
    std::string n = prefix ? prefix : "";
    return each_blob(c, context, callback, [&](const std::string& b) { return b.starts_with(n); });
}

// Blobs as the title reads them: the XGameSaveBlob array ({name, size}, data), then each name (NUL-terminated) and
// its data; pointers as offsets into the layout (`rebase` lists them).
HRESULT layout_blobs(Container* c, const char** names, uint32_t count, std::vector<uint8_t>* out,
                     std::vector<size_t>* rebase, uint32_t* blobs) {
    if (!c) return E_INVALIDARG;
    Version v;
    {
        std::lock_guard lock(g_lock);
        if (!newest(c->dir(), &v)) return kContainerNotFound;
    }
    std::vector<size_t> pick;
    if (!names) {
        for (size_t i = 0; i < v.blobs.size(); ++i) pick.push_back(i);
    } else {
        for (uint32_t k = 0; k < count; ++k)
            for (size_t i = 0; i < v.blobs.size(); ++i)
                if (names[k] && v.blobs[i] == names[k]) pick.push_back(i);
    }
    size_t at = pick.size() * 24;
    std::vector<uint8_t> bytes(at);
    for (size_t k = 0; k < pick.size(); ++k) {
        const std::string& n = v.blobs[pick[k]];
        std::vector<char> data;
        {
            std::ifstream f(v.dir / std::to_string(pick[k]), std::ios::binary);
            data.assign(std::istreambuf_iterator<char>(f), {});
        }
        uint64_t name_at = bytes.size();
        bytes.insert(bytes.end(), n.begin(), n.end());
        bytes.push_back(0);
        uint64_t data_at = bytes.size();
        bytes.insert(bytes.end(), data.begin(), data.end());
        uint32_t size = static_cast<uint32_t>(data.size());
        std::memcpy(&bytes[k * 24], &name_at, 8);
        std::memcpy(&bytes[k * 24 + 8], &size, 4);
        std::memcpy(&bytes[k * 24 + 16], &data_at, 8);
        rebase->push_back(k * 24);
        rebase->push_back(k * 24 + 16);
    }
    *out = std::move(bytes);
    *blobs = static_cast<uint32_t>(pick.size());
    return S_OK;
}
HRESULT WINAPI read_blobs(void*, Container* c, const char** names, uint32_t* count, size_t size, uint8_t* buffer) { trace("read_blobs");
    if (!count) return E_POINTER;
    std::vector<uint8_t> bytes;
    std::vector<size_t> rebase;
    uint32_t n = 0;
    HRESULT hr = layout_blobs(c, names, *count, &bytes, &rebase, &n);
    if (FAILED(hr)) return hr;
    if (!buffer || size < bytes.size()) return kBufferTooSmall;
    std::memcpy(buffer, bytes.data(), bytes.size());
    for (size_t at : rebase) {
        uint64_t off;
        std::memcpy(&off, buffer + at, 8);
        off += reinterpret_cast<uintptr_t>(buffer);
        std::memcpy(buffer + at, &off, 8);
    }
    *count = n;
    return S_OK;
}
HRESULT WINAPI read_async(void*, Container* c, const char** names, uint32_t count, void* block) { trace("read_async");
    std::vector<uint8_t> bytes;
    std::vector<size_t> rebase;
    uint32_t n = 0;
    HRESULT hr = layout_blobs(c, names, count, &bytes, &rebase, &n);
    return xgr::finish_async_with(block, &kRead, hr, std::move(bytes), std::move(rebase));
}
HRESULT WINAPI read_result(void*, void* block, size_t size, uint8_t* buffer, uint32_t* count) { trace("read_result");
    size_t used = 0;
    HRESULT hr = xgr::async_result_into(block, &kRead, size, buffer, &used);
    if (FAILED(hr)) return hr;
    // the blob array comes first, and the first name right after it
    uint32_t n = 0;
    if (used >= 24) {
        uint64_t first_name;
        std::memcpy(&first_name, buffer, 8);
        n = static_cast<uint32_t>((first_name - reinterpret_cast<uintptr_t>(buffer)) / 24);
    }
    if (count) *count = n;
    return S_OK;
}

HRESULT WINAPI create_update(void*, Container* c, const char* display, Update** out) { trace("create_update");
    if (!c || !out) return E_INVALIDARG;
    *out = new Update{c, display ? display : "", {}, {}};
    return S_OK;
}
void WINAPI close_update(void*, Update* u) { trace("close_update"); delete u; }
HRESULT WINAPI write_blob(void*, Update* u, const char* name, const uint8_t* data, size_t size) { trace("write_blob");
    if (!u || !name || (!data && size)) return E_INVALIDARG;
    u->deletes.erase(name);
    u->writes[name].assign(data, data + size);
    return S_OK;
}
HRESULT WINAPI delete_blob(void*, Update* u, const char* name) { trace("delete_blob");
    if (!u || !name) return E_INVALIDARG;
    u->writes.erase(name);
    u->deletes.insert(name);
    return S_OK;
}

void flush(const fs::path& file) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }
}

HRESULT submit(Update* u) {
    if (!u) return E_INVALIDARG;
    std::lock_guard lock(g_lock);
    fs::path dir = u->container->dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    Version old;
    bool had = newest(dir, &old);
    // the new version's blobs: the old ones kept, replaced or deleted, then the new ones
    std::vector<std::pair<std::string, std::vector<uint8_t>>> blobs;
    std::vector<std::pair<std::string, size_t>> kept;  // name, old file index
    if (had)
        for (size_t i = 0; i < old.blobs.size(); ++i)
            if (!u->writes.count(old.blobs[i]) && !u->deletes.count(old.blobs[i])) kept.push_back({old.blobs[i], i});
    uint32_t number = had ? old.number + 1 : 1;
    fs::path next = dir / ("v" + std::to_string(number) + ".new"), done = dir / ("v" + std::to_string(number));
    fs::create_directories(next, ec);
    std::vector<std::string> names;
    size_t i = 0;
    for (auto& [name, idx] : kept) {
        fs::copy_file(old.dir / std::to_string(idx), next / std::to_string(i++), ec);
        names.push_back(name);
    }
    for (auto& [name, data] : u->writes) {
        fs::path f = next / std::to_string(i++);
        std::ofstream(f, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
        flush(f);
        names.push_back(name);
    }
    {
        std::ofstream m(next / "manifest", std::ios::binary);
        auto text = [&](const std::string& s) {
            uint32_t n = static_cast<uint32_t>(s.size());
            m.write(reinterpret_cast<const char*>(&n), 4).write(s.data(), n);
        };
        text(u->container->name);
        text(u->display.empty() && had ? old.display : u->display);
        for (auto& n : names) text(n);
    }
    flush(next / "manifest");
    fs::rename(next, done, ec);  // the commit
    if (ec) {
        fs::remove_all(next, ec);
        return E_FAIL;
    }
    if (had) fs::remove_all(old.dir, ec);
    return S_OK;
}
HRESULT WINAPI submit_update(void*, Update* u) { trace("submit_update"); return submit(u); }
HRESULT WINAPI submit_async(void*, Update* u, void* block) { trace("submit_async"); return xgr::finish_async(block, &kSubmit, submit(u)); }
HRESULT WINAPI submit_result(void*, void* block) { trace("submit_result"); return xgr::async_result(block, &kSubmit); }

constexpr GUID kProvider = {0x704C3F58, 0xE629, 0x4CC2, {0xB1, 0x97, 0x30, 0x51, 0x1B, 0x99, 0x6F, 0xE2}};
constexpr GUID kIid2026 = {0x1BFFF3AF, 0xF14A, 0x40A3, {0x8E, 0x35, 0x9A, 0xDA, 0x90, 0x65, 0x93, 0xF9}};

void* f(auto* p) { return reinterpret_cast<void*>(p); }
void* area(const GUID& iid) {
    return xgr::make_area(kProvider, iid,
                          {{3, f(&initialize)},          {4, f(&initialize_async)},  {5, f(&initialize_result)},
                           {6, f(&close_provider)},      {7, f(&quota)},             {8, f(&quota_async)},
                           {9, f(&quota_result)},        {10, f(&delete_container)}, {11, f(&delete_async)},
                           {12, f(&delete_result)},      {13, f(&container_info)},   {14, f(&enumerate_containers)},
                           {15, f(&enumerate_containers_by_name)}, {16, f(&create_container)},
                           {17, f(&close_container)},    {18, f(&enumerate_blobs)},  {19, f(&enumerate_blobs_by_name)},
                           {20, f(&read_blobs)},         {21, f(&read_async)},       {22, f(&read_result)},
                           {23, f(&create_update)},      {24, f(&close_update)},     {25, f(&write_blob)},
                           {26, f(&delete_blob)},        {27, f(&submit_update)},    {28, f(&submit_async)},
                           {29, f(&submit_result)}});
}
xgr::AreaRegistration g_area(kProvider, kProvider, area(kProvider));
xgr::AreaRegistration g_area_2026(kProvider, kIid2026, area(kIid2026));

}  // namespace

void rt::shims::set_save_root(const std::wstring& root) {
    *g_root = root;
    std::error_code ec;
    fs::create_directories(*g_root, ec);
}
