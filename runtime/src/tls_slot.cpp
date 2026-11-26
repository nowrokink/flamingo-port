// rt_tls_slot<N>.dll: lends a real Windows static-TLS slot to a manually mapped module.
//
// A module mapped by rt::Loader gets no TLS index from ntdll, but its code reads its __declspec(thread) data
// through gs:[0x58][_tls_index]. This DLL is built without the CRT and declares its own TLS directory, whose
// template is kCapacity zero bytes and nothing else, so its whole per-thread block is free for the module,
// starting at offset 0 (with the CRT linked in, the CRT's own TLS data came first). The template is real data, not
// SizeOfZeroFill: with zero fill, the end of the block wasn't usable. The runtime writes this DLL's index into the
// module's _tls_index and registers the module's template; every thread gets a copy of it here, before the
// module's TLS callbacks run. Loaded with LoadLibrary, so ntdll also sets the block up for existing threads.
//
// Link with /NODEFAULTLIB /ENTRY:DllMain /INCLUDE:_tls_used, compile with /GS- (see CMakeLists.txt).
#include <windows.h>
#include <intrin.h>

namespace {

constexpr size_t kCapacity = 16 * 1024;  // game_scarlett_master.exe 1.0.2.1 needs 5,248 bytes

using ThreadCallback = void (*)(void* ctx, DWORD reason);

const void* g_template = nullptr;
size_t g_template_size = 0;
ThreadCallback g_callback = nullptr;
void* g_ctx = nullptr;
// The TLS template: the whole block, all zero.
alignas(16) const unsigned char g_raw[kCapacity] = {};

}  // namespace

extern "C" {

ULONG _tls_index = 0;

// The linker points the TLS data directory at this symbol. Same layout as IMAGE_TLS_DIRECTORY64, but with pointer
// fields: casting an address to an integer isn't a constant expression in C++, so the integer version got a
// dynamic initializer, which never runs without the CRT (the directory stayed zero and ntdll gave no slot).
struct TlsDirectory {
    const void* start;
    const void* end;
    ULONG* index;
    const void* callbacks;
    DWORD zero_fill;
    DWORD characteristics;
};
static_assert(sizeof(TlsDirectory) == sizeof(IMAGE_TLS_DIRECTORY64));
const void* const g_no_callbacks[1] = {nullptr};
extern const TlsDirectory _tls_used = {g_raw, g_raw + sizeof(g_raw), &_tls_index, g_no_callbacks, 0,
                                       IMAGE_SCN_ALIGN_16BYTES};

__declspec(dllexport) unsigned long rt_tls_slot_index() { return _tls_index; }
__declspec(dllexport) size_t rt_tls_slot_capacity() { return kCapacity; }
__declspec(dllexport) void* rt_tls_slot_block() {
    return reinterpret_cast<void**>(__readgsqword(0x58))[_tls_index];
}

// Fills the calling thread's block from the template (the rest is zeroed).
__declspec(dllexport) void rt_tls_slot_fill() {
    if (!g_template) return;
    auto* block = static_cast<unsigned char*>(rt_tls_slot_block());
    __movsb(block, static_cast<const unsigned char*>(g_template), g_template_size);
    __stosb(block + g_template_size, 0, kCapacity - g_template_size);
}

// Registers the module's template and a callback run on every thread attach (after the fill) and detach.
__declspec(dllexport) bool rt_tls_slot_init(const void* tmpl, size_t size, ThreadCallback cb, void* ctx) {
    if (size > kCapacity) return false;
    g_template = tmpl;
    g_template_size = size;
    g_callback = cb;
    g_ctx = ctx;
    rt_tls_slot_fill();
    return true;
}

// The DLL must import something: ntdll sets up a dynamically loaded DLL's TLS after snapping its imports, and with
// no import directory it skipped that and gave no slot.
__declspec(dllexport) DWORD rt_tls_slot_owner = 0;

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) rt_tls_slot_owner = GetCurrentProcessId();
    if (reason == DLL_THREAD_ATTACH) rt_tls_slot_fill();
    if ((reason == DLL_THREAD_ATTACH || reason == DLL_THREAD_DETACH) && g_callback) g_callback(g_ctx, reason);
    return TRUE;
}

}  // extern "C"
