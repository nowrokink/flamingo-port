// xgameruntime.dll: the console game runtime. The title doesn't import it: a stub linked into the exe loads it by
// name (LoadLibraryExW, answered with a pseudo handle in loader.cpp), finds these functions with GetProcAddress
// and initialises. Each runtime API call (XPackage*, XUser*, XTaskQueue*...) then asks QueryApiImpl for a
// COM-style object of its area, by provider and interface GUID, calls one method and releases it. An area whose
// query fails makes the call return that error without touching an object.
//
// Areas are added one at a time as the title needs them. Until then a query gets a stub object whose every method
// logs the GDK function it stands for (by name, from gdk_api.inc) the first time and returns E_NOTIMPL, so a run
// shows which functions to write next.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "shims.h"
#include "xgameruntime.h"

namespace {

// HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED): what the title gets for an area we don't have.
constexpr HRESULT kNotSupported = static_cast<HRESULT>(0x80070032);

std::string guid_text(const GUID& g) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.Data1, g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

// The stub passes its own version and build; not checked.
HRESULT WINAPI InitializeApiImpl(uint64_t version, uint64_t build) {
    std::fprintf(stderr, "[rt] xgameruntime: InitializeApiImpl(%llx, %llx)\n", version, build);
    return S_OK;
}
HRESULT WINAPI InitializeApiImplEx(uint64_t version, uint64_t build, uint64_t flags) {
    std::fprintf(stderr, "[rt] xgameruntime: InitializeApiImplEx(%llx, %llx, %llx)\n", version, build, flags);
    return S_OK;
}
// Preferred by the stub when present; `options` is null unless the title initialises with options.
HRESULT WINAPI InitializeApiImplEx2(uint64_t version, uint64_t build, uint64_t flags, const void* options) {
    std::fprintf(stderr, "[rt] xgameruntime: InitializeApiImplEx2(%llx, %llx, %llx, %p)\n", version, build, flags,
                 options);
    return S_OK;
}
void WINAPI UninitializeApiImpl() { std::fprintf(stderr, "[rt] xgameruntime: UninitializeApiImpl\n"); }

// ---- stub areas

struct ApiFunction {
    const char* name;
    GUID provider, iid;
    int slot;
};
const ApiFunction kApi[] = {
#include "gdk_api.inc"
};

struct StubArea {
    void** vtbl;
    GUID provider, iid;
    uint64_t called[2];  // slots logged so far (a bit per slot)
};

// Slots an area has: XStore's go up to 81 (gdk_api.inc). A call past a table's end reads past the array: 64 slots
// crashed 1.0.2.1 on XStoreAcquireLicenseForDurablesAsync (slot 71).
constexpr int kSlots = 128;

const char* api_name(const StubArea* a, int slot) {
    for (const ApiFunction& f : kApi)
        if (f.slot == slot && IsEqualGUID(f.provider, a->provider) && IsEqualGUID(f.iid, a->iid)) return f.name;
    return nullptr;
}

std::mutex g_stub_lock;

template <int Slot>
HRESULT WINAPI stub_slot(StubArea* a) {
    if constexpr (Slot == 1 || Slot == 2) {
        return 1;  // AddRef / Release
    } else {
        std::lock_guard lock(g_stub_lock);
        if (!(a->called[Slot / 64] & (uint64_t{1} << (Slot % 64)))) {
            a->called[Slot / 64] |= uint64_t{1} << (Slot % 64);
            const char* name = api_name(a, Slot);
            std::fprintf(stderr, "[rt] xgameruntime: %s (slot %d of {%s}) isn't written: E_NOTIMPL\n",
                         name ? name : "an unknown function", Slot, guid_text(a->provider).c_str());
        }
        return E_NOTIMPL;
    }
}

template <int... I>
std::array<void*, sizeof...(I)> stub_table(std::integer_sequence<int, I...>) {
    return {reinterpret_cast<void*>(&stub_slot<I>)...};
}
std::array<void*, kSlots> g_stub_vtbl = stub_table(std::make_integer_sequence<int, kSlots>{});

}  // namespace

namespace {
// The areas make_area made (their tables are ours to rewrite), and those whose slots already trace.
std::vector<StubArea*>& made_areas() {
    static std::vector<StubArea*> a;
    return a;
}
std::vector<StubArea*>& traced_areas() {
    static std::vector<StubArea*> a;
    return a;
}

// The first call of every named function of an area the title asks for is logged ("[rt] call: xgr!XUserAddAsync"):
// each slot goes through a trace thunk (registry.cpp). Written areas are otherwise silent, and a title that stops
// making progress shows here which runtime function it called last.
void trace_area(void* object) {
    std::lock_guard lock(g_stub_lock);
    auto& made = made_areas();
    auto* a = static_cast<StubArea*>(object);
    if (std::find(made.begin(), made.end(), a) == made.end()) return;
    auto& traced = traced_areas();
    if (std::find(traced.begin(), traced.end(), a) != traced.end()) return;
    traced.push_back(a);
    for (int slot = 3; slot < kSlots; ++slot)
        if (const char* name = api_name(a, slot))
            if (void* t = rt::shims::make_trace_thunk("xgr", name, a->vtbl[slot])) a->vtbl[slot] = t;
}
}  // namespace

void* rt::xgr::make_area(const GUID& provider, const GUID& iid, std::initializer_list<std::pair<int, void*>> methods) {
    auto* table = new std::array<void*, kSlots>(stub_table(std::make_integer_sequence<int, kSlots>{}));
    for (auto& [slot, fn] : methods)
        if (slot >= 0 && slot < kSlots) (*table)[slot] = fn;
    auto* area = new StubArea{table->data(), provider, iid, {}};
    made_areas().push_back(area);
    return area;
}

namespace {

// Built at startup: the runtime has no thread-safe statics.
auto* g_stubs = new std::map<std::pair<std::string, std::string>, StubArea*>;

HRESULT WINAPI QueryApiImpl(const GUID* provider, const GUID* iid, void** out) {
    if (!provider || !iid || !out) return kNotSupported;
    if (void* object = rt::xgr::find_area(*provider, *iid)) {
        trace_area(object);
        *out = object;
        return S_OK;
    }
    std::lock_guard lock(g_stub_lock);
    auto key = std::make_pair(guid_text(*provider), guid_text(*iid));
    StubArea*& stub = (*g_stubs)[key];
    if (!stub) {
        stub = new StubArea{g_stub_vtbl.data(), *provider, *iid, 0};
        std::fprintf(stderr, "[rt] xgameruntime: no area for provider {%s} interface {%s}: a stub\n",
                     key.first.c_str(), key.second.c_str());
    }
    *out = stub;
    return S_OK;
}

void WINAPI XErrorReport(HRESULT hr, const char* message) {
    std::fprintf(stderr, "[rt] xgameruntime: XErrorReport(%08lX, \"%s\")\n", static_cast<unsigned long>(hr),
                 message ? message : "");
}

}  // namespace

namespace rt::xgr {

namespace {
std::vector<Area>& areas() {
    static std::vector<Area> a;
    return a;
}
}  // namespace

AreaRegistration::AreaRegistration(const GUID& provider, const GUID& iid, void* object) {
    areas().push_back({provider, iid, object});
}

void* find_area(const GUID& provider, const GUID& iid) {
    for (auto& a : areas())
        if (IsEqualGUID(a.provider, provider) && IsEqualGUID(a.iid, iid)) return a.object;
    return nullptr;
}

}  // namespace rt::xgr

SHIM_TABLE(xgameruntime,
    {"xgameruntime.dll", "InitializeApiImpl", reinterpret_cast<void*>(&InitializeApiImpl)},
    {"xgameruntime.dll", "InitializeApiImplEx", reinterpret_cast<void*>(&InitializeApiImplEx)},
    {"xgameruntime.dll", "InitializeApiImplEx2", reinterpret_cast<void*>(&InitializeApiImplEx2)},
    {"xgameruntime.dll", "UninitializeApiImpl", reinterpret_cast<void*>(&UninitializeApiImpl)},
    {"xgameruntime.dll", "QueryApiImpl", reinterpret_cast<void*>(&QueryApiImpl)},
    {"xgameruntime.dll", "XErrorReport", reinterpret_cast<void*>(&XErrorReport)},
);
