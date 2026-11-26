#include "shader_cache.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace rt::shader_cache {
namespace {

// Set once, under the lock (no function-local statics with dynamic init: see docs/design.md).
SRWLOCK g_lock = SRWLOCK_INIT;
bool g_ready = false;
wchar_t g_folder[MAX_PATH];

void init() {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_ready) {
        DWORD n = GetEnvironmentVariableW(L"RT_SHADER_CACHE", g_folder, MAX_PATH);
        if (!n || n >= MAX_PATH) {
            n = GetModuleFileNameW(nullptr, g_folder, MAX_PATH);
            while (n && g_folder[n - 1] != L'\\' && g_folder[n - 1] != L'/') --n;
            g_folder[n] = 0;
            wcsncat_s(g_folder, L"shaders", _TRUNCATE);
        }
        DWORD attr = GetFileAttributesW(g_folder);
        std::fprintf(stderr, "[rt] shaders: cache %ls%s\n", g_folder,
                     attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) ? "" : " (missing)");
        g_ready = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

uint32_t u32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// The program kind of a DXIL container: the DXIL chunk's program header starts with (kind << 16 | version).
int kind_of(const std::vector<uint8_t>& c) {
    if (c.size() < 32 || std::memcmp(c.data(), "DXBC", 4) != 0) return -1;
    uint32_t count = u32(&c[28]);
    for (uint32_t i = 0; i < count && 32 + 4 * i + 4 <= c.size(); ++i) {
        uint32_t at = u32(&c[32 + 4 * i]);
        if (at + 12 <= c.size() && std::memcmp(&c[at], "DXIL", 4) == 0) return static_cast<int>(u32(&c[at + 8]) >> 16);
    }
    return -1;
}

}  // namespace

const wchar_t* folder() {
    if (!g_ready) init();
    return g_folder;
}

bool digest_of(const uint8_t* block, size_t size, uint8_t digest[16]) {
    if (!block || size < 28) return false;
    const uint8_t* tail = block + size - 12;
    for (int i = 0; i < 12; ++i)
        if (tail[i]) return false;
    std::memcpy(digest, block + size - 28, 16);
    for (int i = 0; i < 16; ++i)
        if (digest[i]) return true;
    return false;
}

bool vs_inputs(const uint8_t digest[16], std::vector<VsInput>& out) {
    out.clear();
    wchar_t path[MAX_PATH + 48];
    int n = swprintf_s(path, L"%ls\\", folder());
    for (int i = 0; i < 16; ++i) n += swprintf_s(path + n, MAX_PATH + 48 - n, L"%02x", digest[i]);
    wcscat_s(path, L".vsin");
    FILE* f = _wfopen(path, L"r");
    if (!f) return false;
    VsInput v{};
    while (std::fscanf(f, "%31s %u %u", v.semantic, &v.index, &v.reg) == 3) out.push_back(v);
    std::fclose(f);
    return true;
}

namespace {
// <cache>\skip.txt: digests (32 hex digits a line; the rest of a line is a note) of translated shaders not to use, as
// if they weren't in the cache: for a shader whose translation is known to be wrong in a way that costs more than
// its absence (one that never ends takes the PC device with it). Read once.
SRWLOCK g_skip_lock = SRWLOCK_INIT;
std::set<std::string>* g_skip = nullptr;

bool skipped(const uint8_t digest[16]) {
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    AcquireSRWLockExclusive(&g_skip_lock);
    if (!g_skip) {
        g_skip = new std::set<std::string>;
        wchar_t path[MAX_PATH + 32];
        swprintf_s(path, MAX_PATH + 32, L"%ls\\skip.txt", folder());
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"r") == 0 && f) {
            char line[256];
            while (std::fgets(line, sizeof line, f)) {
                std::string d(line, strnlen(line, 32));
                if (d.size() == 32 && d.find_first_not_of("0123456789abcdef") == std::string::npos) g_skip->insert(d);
            }
            std::fclose(f);
            std::fprintf(stderr, "[rt] shaders: %zu translated shader(s) set aside by skip.txt\n", g_skip->size());
        }
    }
    const bool skip = g_skip->count(hex) != 0;
    ReleaseSRWLockExclusive(&g_skip_lock);
    return skip;
}
}  // namespace

bool find(const uint8_t digest[16], std::vector<uint8_t>& dxil, int& kind) {
    if (skipped(digest)) return false;
    wchar_t path[MAX_PATH + 48];
    int n = swprintf_s(path, L"%ls\\", folder());
    for (int i = 0; i < 16; ++i) n += swprintf_s(path + n, MAX_PATH + 48 - n, L"%02x", digest[i]);
    wcscat_s(path, L".dxil");
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64 << 20);
    if (ok) {
        dxil.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, dxil.data(), static_cast<DWORD>(dxil.size()), &got, nullptr) && got == dxil.size();
    }
    CloseHandle(f);
    kind = ok ? kind_of(dxil) : -1;
    return ok && kind >= 0;
}

bool container_digest(const uint8_t* d, size_t n, uint8_t digest[16]) {
    if (!d || n < 32 || std::memcmp(d, "DXBC", 4) != 0) return false;
    uint32_t parts;
    std::memcpy(&parts, d + 28, 4);
    for (uint32_t k = 0; k < parts && 32 + 4 * k + 4 <= n; ++k) {
        uint32_t at;
        std::memcpy(&at, d + 32 + 4 * k, 4);
        if (at + 28 <= n && std::memcmp(d + at, "HASH", 4) == 0) {  // tag, size, flags, then the 16-byte digest
            std::memcpy(digest, d + at + 12, 16);
            return true;
        }
    }
    return false;
}

bool save_pending(const uint8_t digest[16], const uint8_t* dxbc, size_t size) {
    wchar_t path[MAX_PATH + 64];
    int n = swprintf_s(path, MAX_PATH + 64, L"%ls\\pending", folder());
    CreateDirectoryW(path, nullptr);
    n += swprintf_s(path + n, MAX_PATH + 64 - n, L"\\");
    for (int i = 0; i < 16; ++i) n += swprintf_s(path + n, MAX_PATH + 64 - n, L"%02x", digest[i]);
    wcscat_s(path, L".dxbc");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_EXISTS;
    DWORD wrote = 0;
    WriteFile(f, dxbc, static_cast<DWORD>(size), &wrote, nullptr);
    CloseHandle(f);
    std::fprintf(stderr, "[rt] shaders: %ls saved for translation\n", path);
    return wrote == size;
}

bool save_compute_regs(const uint8_t digest[16], const uint8_t* regs, size_t size) {
    wchar_t path[MAX_PATH + 64];
    int n = swprintf_s(path, MAX_PATH + 64, L"%ls\\cs", folder());
    CreateDirectoryW(path, nullptr);
    n += swprintf_s(path + n, MAX_PATH + 64 - n, L"\\");
    for (int i = 0; i < 16; ++i) n += swprintf_s(path + n, MAX_PATH + 64 - n, L"%02x", digest[i]);
    wcscat_s(path, L".regs");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_EXISTS;
    DWORD wrote = 0;
    WriteFile(f, regs, static_cast<DWORD>(size), &wrote, nullptr);
    CloseHandle(f);
    return wrote == size;
}

namespace {
SRWLOCK g_missing_lock = SRWLOCK_INIT;
std::set<std::string>* g_missing = new std::set<std::string>;  // built at startup (no thread-safe statics)
}  // namespace

void write_owed(const std::vector<std::pair<std::array<uint8_t, 16>, uint64_t>>& owed) {
    wchar_t path[MAX_PATH + 32], part[MAX_PATH + 40];
    swprintf_s(path, MAX_PATH + 32, L"%ls\\owed.txt", folder());
    swprintf_s(part, MAX_PATH + 40, L"%ls.part", path);
    FILE* f = nullptr;
    if (_wfopen_s(&f, part, L"w") != 0 || !f) return;
    for (const auto& [digest, count] : owed) {
        for (uint8_t b : digest) std::fprintf(f, "%02x", b);
        std::fprintf(f, " %llu\n", static_cast<unsigned long long>(count));
    }
    std::fclose(f);
    MoveFileExW(part, path, MOVEFILE_REPLACE_EXISTING);
}

void note_missing(const uint8_t digest[16]) {
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    AcquireSRWLockExclusive(&g_missing_lock);
    const bool first = g_missing->insert(hex).second;
    if (first) {
        wchar_t path[MAX_PATH + 32];
        swprintf_s(path, MAX_PATH + 32, L"%ls\\missing.txt", folder());
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"a") == 0 && f) {
            std::fprintf(f, "%s\n", hex);
            std::fclose(f);
        }
    }
    ReleaseSRWLockExclusive(&g_missing_lock);
}

}  // namespace rt::shader_cache
