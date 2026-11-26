// Tests the virtual drives: title paths on a mounted drive reach the host folder behind it, through the same
// shims the title's imports are bound to.
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>

#include "../shims/files.h"
#include "../shims/shims.h"

namespace {

int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

template <class F>
F shim(const char* dll, const char* name) {
    void* p = rt::shims::find(dll, name);
    if (!p) {
        std::fprintf(stderr, "FAIL: %s!%s is not registered\n", dll, name);
        std::exit(1);
    }
    return reinterpret_cast<F>(p);
}

constexpr const char* kFile = "api-ms-win-core-file-l1-1-0.dll";
constexpr const char* kStdio = "api-ms-win-crt-stdio-l1-1-0.dll";

}  // namespace

int main() {
    namespace fs = std::filesystem;
    // A package with a non-ASCII folder name, to check narrow paths go through the wide functions.
    fs::path root = fs::temp_directory_path() / L"flamingo_files_test_\u00e7\u00e3o";
    fs::remove_all(root);
    fs::create_directories(root / L"data" / L"sub");
    std::ofstream(root / L"appdata0.rpf", std::ios::binary) << "RPF9";
    std::ofstream(root / L"data" / L"sub" / L"x.txt") << "hello";
    rt::files::mount(L'G', root);

    // Path forms.
    CHECK(rt::files::to_host(L"G:\\appdata0.rpf") == (root / L"appdata0.rpf").wstring());
    CHECK(rt::files::to_host(L"g:/data/sub/x.txt") == (root / L"data\\sub\\x.txt").wstring());
    CHECK(rt::files::to_host(L"\\\\?\\G:\\appdata0.rpf") == (root / L"appdata0.rpf").wstring());
    CHECK(rt::files::to_host(L"G:") == root.wstring());
    CHECK(rt::files::to_host(L"C:\\Windows") == L"C:\\Windows");  // not mounted: unchanged
    CHECK(rt::files::to_host(L"relative\\path") == L"relative\\path");

    // kernel32-style calls.
    auto attrs = shim<DWORD(WINAPI*)(LPCWSTR)>(kFile, "GetFileAttributesW");
    CHECK(attrs(L"G:\\appdata0.rpf") != INVALID_FILE_ATTRIBUTES);
    CHECK(attrs(L"G:\\missing.rpf") == INVALID_FILE_ATTRIBUTES);
    CHECK(attrs(L"G:/data") & FILE_ATTRIBUTE_DIRECTORY);
    auto attrs_a = shim<DWORD(WINAPI*)(LPCSTR)>(kFile, "GetFileAttributesA");
    CHECK(attrs_a("G:/data/sub/x.txt") != INVALID_FILE_ATTRIBUTES);

    auto create = shim<HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE)>(
        kFile, "CreateFileW");
    HANDLE h = create(L"G:\\appdata0.rpf", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    char buf[8] = {};
    DWORD got = 0;
    CHECK(ReadFile(h, buf, 4, &got, nullptr) && got == 4 && std::string(buf, 4) == "RPF9");
    CloseHandle(h);

    auto mkdir = shim<BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES)>(kFile, "CreateDirectoryW");
    CHECK(mkdir(L"G:\\saves", nullptr) && fs::is_directory(root / L"saves"));

    WIN32_FIND_DATAW fd{};
    auto find = shim<HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW)>(kFile, "FindFirstFileW");
    HANDLE f = find(L"G:\\data\\sub\\*.txt", &fd);
    CHECK(f != INVALID_HANDLE_VALUE && std::wstring(fd.cFileName) == L"x.txt");
    if (f != INVALID_HANDLE_VALUE) FindClose(f);

    auto move = shim<BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD)>("api-ms-win-core-file-l2-1-0.dll", "MoveFileExW");
    CHECK(move(L"G:\\data\\sub\\x.txt", L"G:/data/y.txt", 0) && fs::exists(root / L"data" / L"y.txt"));

    // The C runtime.
    auto fopen_ = shim<FILE*(__cdecl*)(const char*, const char*)>(kStdio, "fopen");
    FILE* file = fopen_("G:/data/y.txt", "rb");
    CHECK(file != nullptr);
    if (file) {
        char text[8] = {};
        CHECK(std::fread(text, 1, 5, file) == 5 && std::string(text) == "hello");
        std::fclose(file);
    }
    auto stat = shim<int(__cdecl*)(const wchar_t*, struct _stat64*)>("api-ms-win-crt-filesystem-l1-1-0.dll", "_wstat64");
    struct _stat64 st{};
    CHECK(stat(L"G:\\appdata0.rpf", &st) == 0 && st.st_size == 4);
    auto remove = shim<int(__cdecl*)(const char*)>("api-ms-win-crt-filesystem-l1-1-0.dll", "remove");
    CHECK(remove("G:\\data\\y.txt") == 0 && !fs::exists(root / L"data" / L"y.txt"));

    fs::remove_all(root);
    if (g_failures) {
        std::fprintf(stderr, "files_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("files_test: ok\n");
    return 0;
}
