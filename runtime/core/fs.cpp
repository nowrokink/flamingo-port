#include <windows.h>

#include "fs.h"

#include <shlobj.h>

#include <algorithm>

namespace fp {

std::wstring join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring parent(const std::wstring& path) {
    size_t k = path.find_last_of(L"\\/");
    if (k == std::wstring::npos) return L"";
    if (k == 2 && path.size() > 1 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, k);
}

std::wstring file_name(const std::wstring& path) {
    size_t k = path.find_last_of(L"\\/");
    return k == std::wstring::npos ? path : path.substr(k + 1);
}

bool is_file(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool is_dir(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool make_dirs(const std::wstring& path) {
    if (path.empty() || is_dir(path)) return true;
    int r = SHCreateDirectoryExW(nullptr, full_path(path).c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

int64_t file_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return -1;
    return (static_cast<int64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

bool read_file(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    bool ok = true;
    while (done < out.size()) {
        DWORD got = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 30));
        if (!ReadFile(h, out.data() + done, want, &got, nullptr) || !got) {
            ok = false;
            break;
        }
        done += got;
    }
    CloseHandle(h);
    return ok;
}

bool write_file(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const auto* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    bool ok = true;
    while (done < size) {
        DWORD put = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 30));
        if (!WriteFile(h, p + done, want, &put, nullptr) || !put) {
            ok = false;
            break;
        }
        done += put;
    }
    CloseHandle(h);
    return ok;
}

std::vector<std::wstring> list_dir(const std::wstring& dir, bool files, bool dirs) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileExW(join(dir, L"*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
        bool d = (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if ((d && dirs) || (!d && files)) out.emplace_back(f.cFileName);
    } while (FindNextFileW(h, &f));
    FindClose(h);
    // Python's sorted(): by code point, case-sensitive
    std::sort(out.begin(), out.end());
    return out;
}

size_t count_dir(const std::wstring& dir, size_t stop_at) {
    size_t n = 0;
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileExW(join(dir, L"*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(f.cFileName, L".") && wcscmp(f.cFileName, L"..")) ++n;
    } while (n < stop_at && FindNextFileW(h, &f));
    FindClose(h);
    return n;
}

std::wstring full_path(const std::wstring& path) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH * 4, buf, nullptr);
    if (!n || n >= MAX_PATH * 4) return path;
    std::wstring s(buf, n);
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

bool inside(const std::wstring& inner, const std::wstring& outer) {
    std::wstring a = full_path(inner), b = full_path(outer);
    if (a.size() < b.size()) return false;
    if (CompareStringOrdinal(a.c_str(), static_cast<int>(b.size()), b.c_str(), static_cast<int>(b.size()), TRUE) !=
        CSTR_EQUAL)
        return false;
    return a.size() == b.size() || a[b.size()] == L'\\' || b.back() == L'\\';
}

double free_gb(const std::wstring& path_in) {
    std::wstring path = path_in;
    while (!path.empty() && !is_dir(path)) {
        std::wstring p = parent(path);
        if (p == path) break;
        path = p;
    }
    ULARGE_INTEGER free{};
    if (path.empty() || !GetDiskFreeSpaceExW(join(path, L"").c_str(), &free, nullptr, nullptr)) return -1;
    return static_cast<double>(free.QuadPart) / (1ull << 30);
}

double folder_gb(const std::wstring& path, const std::atomic<bool>* stop) {
    uint64_t total = 0;
    std::vector<std::wstring> stack{path};
    while (!stack.empty() && !(stop && stop->load())) {
        std::wstring dir = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW f;
        HANDLE h = FindFirstFileExW(join(dir, L"*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back(join(dir, f.cFileName));
            } else {
                total += (static_cast<uint64_t>(f.nFileSizeHigh) << 32) | f.nFileSizeLow;
            }
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    return static_cast<double>(total) / (1ull << 30);
}

}  // namespace fp
