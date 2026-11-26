// Patches for one exact build of the game: places where the runtime stands in for a console subsystem from inside
// the game's own code, because the console side isn't emulated (yet). Applied only when the exe matches the build
// (PE timestamp, checksum and image size); every patch says why it exists.
#include "game_patches.h"

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <utility>
#include <string>

#include "../shims/files.h"
#include "../shims/shims.h"
#include "../shims/shims.h"

namespace rt {
namespace {

std::filesystem::path g_game_dir;

// ---------------------------------------------------------------- 1.0.2.1: the G: package device
//
// Paths starting with "g:" go to a device object (exe+0x78f5e80, vtable exe+0x62fc550) backed by DirectStorage.
// Its methods only answer from an in-memory index that, on the console, is filled before the first check
// (exe+0x2236e6e asks whether G:/appdata0.rpf exists); here nothing fills it, so every file is missing. The
// replacements answer from the game folder instead: "G:/x" -> <game dir>\x. Slots, from the method bodies:
//   4 Open(path, readOnly)  5 OpenBulk(path, &bias)  6 OpenBulk + size  10 Read  12 ReadBulk  15/16 Seek/Seek64
//   17/18 Close/CloseBulk (no-ops)  19/20 Size  31 GetFileSize(path)  33 GetFileTime(path)  45 GetAttributes(path)
constexpr uint32_t kG_DeviceVtbl = 0x62fc550;
constexpr int kG_GetAttributes = 45;

// "G:/a/b" or "g:\a\b" -> the host path behind the G: mount (files.cpp). Empty if the path isn't on G:.
std::wstring package_path(const char* path) {
    if (!path || (path[0] != 'G' && path[0] != 'g') || path[1] != ':') return {};
    return files::to_host(path);
}

// DWORD GetAttributes(this, const char* path): FILE_ATTRIBUTE_* or 0xFFFFFFFF when missing (the original returns
// FILE_ATTRIBUTE_NORMAL for indexed files).
DWORD package_get_attributes(void* /*self*/, const char* path) {
    std::wstring host = package_path(path);
    DWORD attrs = host.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(host.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) std::fprintf(stderr, "[rt] G: no \"%s\"\n", path ? path : "");
    return attrs;
}

// The device's file handles are its index entries: size at +8, time at +16, read position at +24, shared by
// every open of a path and never freed (Close is a no-op). HostFile keeps that layout, so the methods left alone
// (Seek, Seek64, Size) work on it unchanged, and adds the Windows handle the data comes from.
struct HostFile {
    uint64_t dstorage;  // the console device keeps the file's IDStorageFile* here (fiDeviceDirectStorage reads it)
    uint64_t size;
    uint64_t time;  // FILETIME of the last write
    uint64_t pos;
    // The console's package files have a map of their extents in the package here (a sorted array of entries, the
    // file page in bits 30-51, and its count), which physical reads search; none (null) is a file read as is.
    const uint64_t* extents;
    uint64_t extent_count;
    HANDLE handle;
};
static_assert(offsetof(HostFile, size) == 8 && offsetof(HostFile, time) == 16 && offsetof(HostFile, pos) == 24 &&
              offsetof(HostFile, extents) == 32 && offsetof(HostFile, extent_count) == 40);

HostFile* const kNoFile = reinterpret_cast<HostFile*>(~uintptr_t{0});  // the device's "no handle" (-1)

SRWLOCK g_files_lock = SRWLOCK_INIT;
std::map<std::wstring, HostFile*>* g_files = new std::map<std::wstring, HostFile*>;  // by host path, lower case

HostFile* host_file(const char* path) {
    std::wstring host = package_path(path);
    if (host.empty()) return nullptr;
    std::wstring key = host;
    for (auto& c : key) c = static_cast<wchar_t>(towlower(c));
    AcquireSRWLockExclusive(&g_files_lock);
    auto it = g_files->find(key);
    HostFile* f = it == g_files->end() ? nullptr : it->second;
    if (!f) {
        HANDLE h = CreateFileW(host.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        LARGE_INTEGER size{};
        FILETIME time{};
        if (h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &size) && GetFileTime(h, nullptr, nullptr, &time)) {
            f = new HostFile{reinterpret_cast<uint64_t>(rt::shims::make_dstorage_file(h)), static_cast<uint64_t>(size.QuadPart),
                             (uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime, 0, nullptr, 0, h};
            (*g_files)[key] = f;
            std::fprintf(stderr, "[rt] G: open \"%s\" (%llu bytes)\n", path, f->size);
        } else {
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            std::fprintf(stderr, "[rt] G: can't open \"%s\" (error %lu)\n", path, GetLastError());
        }
    }
    ReleaseSRWLockExclusive(&g_files_lock);
    return f;
}

// Reads n bytes at offset with no shared file pointer, so threads reading the same file don't interfere.
uint32_t read_at(HostFile* f, uint64_t offset, void* buf, uint32_t n) {
    if (offset >= f->size) return 0;
    if (offset + n > f->size) n = static_cast<uint32_t>(f->size - offset);
    uint32_t done = 0;
    while (done < n) {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset + done);
        ov.OffsetHigh = static_cast<DWORD>((offset + done) >> 32);
        DWORD got = 0;
        if (!ReadFile(f->handle, static_cast<uint8_t*>(buf) + done, n - done, &got, &ov) || !got) {
            std::fprintf(stderr, "[rt] G: read of %u bytes at %llu failed (error %lu)\n", n - done, offset + done,
                         GetLastError());
            break;
        }
        done += got;
    }
    return done;
}

// handle Open(this, path, bool readOnly) (slot 4) and OpenBulk(this, path, u64* bias) (slot 5): -1 when missing.
HostFile* package_open(void*, const char* path, uint64_t /*read_only*/) {
    HostFile* f = host_file(path);
    if (!f) return kNoFile;
    f->pos = 0;
    return f;
}
HostFile* package_open_bulk(void* self, const char* path, uint64_t* bias) {
    if (bias) *bias = 0;
    return package_open(self, path, 1);
}
// handle (this, path, u64* bias, u64* size) (slot 6): OpenBulk plus the size.
HostFile* package_open_bulk_size(void* self, const char* path, uint64_t* bias, uint64_t* size) {
    HostFile* f = package_open_bulk(self, path, bias);
    if (f != kNoFile && size) *size = f->size;
    return f;
}

// int Read(this, handle, void* buf, int n) (slot 10): at the read position, which moves on.
int package_read(void*, HostFile* f, void* buf, int n) {
    if (f == kNoFile || !f || n <= 0) return 0;
    uint32_t got = read_at(f, f->pos, buf, static_cast<uint32_t>(n));
    f->pos += got;
    return static_cast<int>(got);
}
// int ReadBulk(this, handle, u64 offset, void* buf, int n) (slot 12): leaves the position after the data read.
int package_read_bulk(void*, HostFile* f, uint64_t offset, void* buf, int n) {
    if (f == kNoFile || !f || n <= 0 || offset >= f->size) return 0;
    uint32_t got = read_at(f, offset, buf, static_cast<uint32_t>(n));
    f->pos = offset + got;
    return static_cast<int>(got);
}

// u64 GetFileSize(this, path) (slot 31) and u64 GetFileTime(this, path) (slot 33): 0 when missing.
uint64_t package_file_size(void*, const char* path) {
    HostFile* f = host_file(path);
    return f ? f->size : 0;
}
uint64_t package_file_time(void*, const char* path) {
    HostFile* f = host_file(path);
    return f ? f->time : 0;
}

void set_vtable_slot(Module& exe, uint32_t vtbl_rva, int slot, void* fn) {
    auto** entry = reinterpret_cast<void**>(exe.base + vtbl_rva) + slot;
    DWORD old;
    VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old);
    *entry = fn;
    VirtualProtect(entry, sizeof(void*), old, &old);
}

// With tracing on, every other slot of a patched vtable logs its first call ("G:device::slotN").
void trace_vtable(Module& exe, uint32_t vtbl_rva, int slots, const char* name) {
    auto** vt = reinterpret_cast<void**>(exe.base + vtbl_rva);
    for (int i = 0; i < slots; ++i) {
        auto* fn = static_cast<uint8_t*>(vt[i]);
        if (fn < exe.base || fn >= exe.base + exe.size) continue;  // already ours
        set_vtable_slot(exe, vtbl_rva, i, shims::make_trace_thunk(name, "slot" + std::to_string(i), fn));
    }
}

bool g_trace = false;

void patch_1_0_2_1(Module& exe) {
    const std::pair<int, void*> slots[] = {
        {4, reinterpret_cast<void*>(&package_open)},
        {5, reinterpret_cast<void*>(&package_open_bulk)},
        {6, reinterpret_cast<void*>(&package_open_bulk_size)},
        {10, reinterpret_cast<void*>(&package_read)},
        {12, reinterpret_cast<void*>(&package_read_bulk)},
        {31, reinterpret_cast<void*>(&package_file_size)},
        {33, reinterpret_cast<void*>(&package_file_time)},
        {kG_GetAttributes, reinterpret_cast<void*>(&package_get_attributes)},
    };
    for (auto& [slot, fn] : slots) set_vtable_slot(exe, kG_DeviceVtbl, slot, fn);
    if (g_trace) trace_vtable(exe, kG_DeviceVtbl, 60, "G:device");
}

struct Build {
    const char* name;
    uint32_t timestamp, checksum, image_size;
    void (*apply)(Module&);
};
const Build kBuilds[] = {
    {"Xbox Series 1.0.2.1", 0x6af783d4, 0x72aa97f, 184995840, &patch_1_0_2_1},
};

}  // namespace

void apply_game_patches(Module& exe, const std::filesystem::path& game_dir, bool trace) {
    g_game_dir = std::filesystem::absolute(game_dir);
    g_trace = trace;
    // Off unless RT_GAME_PATCHES=1. The G: device patch dates from when the DirectStorage factory's OpenFile read
    // its UTF-16 path as bytes and failed (xcrdapi.cpp): the title's own device works now, and it must be the one
    // in use, because the replacement reads files raw, so the compressed packages' texture pages never pass through
    // DirectStorage's decompression and the title waits for them forever (the endless first loading screen).
    char on[8] = {};
    if (!GetEnvironmentVariableA("RT_GAME_PATCHES", on, sizeof(on)) || on[0] != '1') return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(exe.base + reinterpret_cast<IMAGE_DOS_HEADER*>(exe.base)->e_lfanew);
    for (const auto& b : kBuilds)
        if (nt->FileHeader.TimeDateStamp == b.timestamp && nt->OptionalHeader.CheckSum == b.checksum &&
            nt->OptionalHeader.SizeOfImage == b.image_size) {
            b.apply(exe);
            std::fprintf(stderr, "[rt] patches for %s applied\n", b.name);
            return;
        }
    std::fprintf(stderr, "[rt] no patches for this build of %s\n", exe.name.c_str());
}

}  // namespace rt
