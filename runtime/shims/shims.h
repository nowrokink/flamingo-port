// Shim registry: reimplementations of console-only imports.
// Add a DLL by writing a shims/<dll>.cpp with a SHIM_TABLE and listing it in CMakeLists.txt.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace rt::shims {

struct Entry {
    const char* dll;   // lower-case, with .dll
    const char* name;  // export name, or "#ordinal"
    void* fn;
};

// Registers a static table at startup.
struct Table {
    Table(const Entry* entries, size_t count);
};

void* find(const std::string& dll_lower, const std::string& name);

// Makes a small thunk that logs "dll!name" once and returns 0 (in rax and xmm0).
void* make_unimplemented_thunk(const std::string& dll, const std::string& name);

// Makes a thunk that logs "dll!name" on its first call, then jumps to `real` with every argument intact. Used by
// FlamingoPort.exe --trace-host for host and shim imports. The thunk has no unwind data, but it only has a frame of its own
// while the logger runs.
void* make_trace_thunk(const std::string& dll, const std::string& name, void* real);

// The path the title sees as its own executable (GetModuleFileName of the process), e.g. L"G:\\game.exe".
void set_title_path(const std::wstring& path);

// Where XGameSave keeps the user's saves (gamesave.cpp).
void set_save_root(const std::wstring& root);

// The title's identity (name, version, title id) from its MicrosoftGame.Config, for the runtime areas.
void set_game_identity(const std::wstring& config_path);

// The title's XMem allocation hooks (its __XMemAllocationHooks__ export: {alloc, free}); null: none.
void set_xmem_hooks(const void* hooks);
// XMemAlloc / XMemFree as the system calls them: through the title's hooks when it has them.
void* xmem_alloc(size_t size, uint64_t attributes);
void xmem_free(void* p, uint64_t attributes);
// The title's CPU memory (XMemAlloc without a page size) now allocated, in bytes.
uint64_t xmem_cpu_bytes();

// An IDStorageFile of the console's DirectStorage (xcrdapi.cpp) reading `file` (which stays the caller's).
void* make_dstorage_file(void* file);

// Logs the title's waits of two seconds and more: who waits on what, made where, signalled last from where
// (waits.cpp).
void report_waits();

// Whether `address` is inside the destination of a BCPack request left blank (xcrdapi.cpp).
bool in_blank_bcpack(uint64_t address, uint64_t* start, uint64_t* bytes);

// Prints every unimplemented import that has been called.
void report_called_unimplemented();

// The console the title is told it runs on: RT_CONSOLE=seriesx (the default) or seriess. The title picks its
// settings by it: Series S renders at 1080p instead of 4K with Series S memory budgets, but 1.0.2.1 then draws its
// interface through pipelines the runtime doesn't make yet (a black screen), so it stays opt-in. Read once at
// startup.
bool series_s();
// The title memory that console gives a game (its budget): Series S 7,776 MiB (one range), Series X 13.5 GiB
// (10 GiB GPU-optimal + 3.5 GiB standard). (Figures as the ztrx project uses them.)
uint64_t title_memory_bytes();

}  // namespace rt::shims

#define SHIM_TABLE(id, ...)                                                       \
    static const ::rt::shims::Entry id##_entries[] = {__VA_ARGS__};               \
    static ::rt::shims::Table id##_table(id##_entries, sizeof(id##_entries) / sizeof(id##_entries[0]))
