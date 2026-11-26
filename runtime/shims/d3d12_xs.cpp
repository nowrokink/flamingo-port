// d3d12_xs.dll: the Xbox Series build of D3D12: device creation, the GPU virtual memory setup, root signature
// serialization.
//
// ASSUMPTION (verify against the exe's call sites): root-signature descriptions and blobs have the same layout as
// desktop D3D12, so those forward to the PC d3d12.dll.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <iterator>

#include "com_probe.h"
#include "d3d12x.h"
#include "frame.h"
#include "shims.h"

namespace {

FARPROC host(const char* name) {
    static HMODULE d3d12 = LoadLibraryExA("d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    FARPROC p = d3d12 ? GetProcAddress(d3d12, name) : nullptr;
    if (!p) std::fprintf(stderr, "[rt] d3d12_xs: host d3d12.dll has no %s\n", name);
    return p;
}

using SerializeFn = HRESULT(WINAPI*)(const void*, void**, void**);
using DeserializerFn = HRESULT(WINAPI*)(const void*, SIZE_T, const GUID*, void**);

HRESULT WINAPI SerializeVersionedRootSignature_(const void* desc, void** blob, void** error) {
    static auto fn = reinterpret_cast<SerializeFn>(host("D3D12SerializeVersionedRootSignature"));
    if (!fn) return E_NOTIMPL;
    // Console flag bits the PC doesn't have (1.0.2.1's raytracing signatures set 0x3000000) are left out of the PC
    // blob; the root signatures translated shaders run with are the runtime's own (docs/gpu-memory.md).
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC copy;
    if (desc) {
        copy = *static_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*>(desc);
        constexpr uint32_t kPcFlags = 0xFFF;
        D3D12_ROOT_SIGNATURE_FLAGS* flags = copy.Version == D3D_ROOT_SIGNATURE_VERSION_1_0 ? &copy.Desc_1_0.Flags
                                            : copy.Version == D3D_ROOT_SIGNATURE_VERSION_1_1 ? &copy.Desc_1_1.Flags
                                                                                            : &copy.Desc_1_2.Flags;
        if (*flags & ~kPcFlags) {
            static uint32_t logged = 0;
            if (!(logged & *flags & ~kPcFlags)) {
                logged |= *flags & ~kPcFlags;
                std::fprintf(stderr, "[rt] d3d12: console root signature flags %#x left out on the PC\n",
                             *flags & ~kPcFlags);
            }
            *flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(*flags & kPcFlags);
        }
        desc = &copy;
    }
    ID3DBlob* message = nullptr;
    HRESULT hr = fn(desc, blob, reinterpret_cast<void**>(&message));
    if (FAILED(hr)) {
        const auto* d = static_cast<const uint32_t*>(desc);
        std::fprintf(stderr, "[rt] d3d12: the PC refused a root signature (version %#x): %08lX %.*s\n", d ? d[0] : 0,
                     static_cast<unsigned long>(hr),
                     message ? static_cast<int>(message->GetBufferSize()) : 0,
                     message ? static_cast<const char*>(message->GetBufferPointer()) : "");
    }
    if (error) *error = message;
    else if (message) message->Release();
    return hr;
}

HRESULT WINAPI CreateRootSignatureDeserializer_(const void* data, SIZE_T size, const GUID* iid, void** out) {
    static auto fn = reinterpret_cast<DeserializerFn>(host("D3D12CreateRootSignatureDeserializer"));
    return fn ? fn(data, size, iid, out) : E_NOTIMPL;
}

// ---------------------------------------------------------------- the device (probe stage)
//
// The device object has the console's layout, because title code inlines parts of D3D12.X and reads device fields
// directly: the object header (method table, bits whose low 3 are the device index, reference count), the GPU
// address of a null constant buffer, then 16 dwords whose byte 0, when non-zero, sends calls the title would
// otherwise inline to the method table instead. The table starts like the PC device interface (IUnknown, the
// ID3D12Object methods, then ID3D12Device and its later levels in PC order); console additions follow.
//
// Methods not written yet are probes (com_probe.h): they log their slot and arguments, so a run shows what to
// write next. The objects behind it are made on the host's D3D12 (d3d12x.cpp).
struct Device {
    void** vtbl;
    uint32_t bits;
    uint32_t refs;
    uint64_t null_constant_buffer;
    uint32_t out_of_line[16];
};
static_assert(sizeof(Device) == 0x58);

// d3d12.h: ID3D12Device, then ID3D12Device1..8 and ID3D12Device11 (1.0.2.1 creates the device as Device11).
constexpr GUID kDeviceIids[] = {
    {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}},
    {0x77acce80, 0x638e, 0x4e65, {0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e}},
    {0x30baa41e, 0xb15b, 0x475c, {0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28}},
    {0x81dadc15, 0x2bad, 0x4392, {0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98}},
    {0xe865df17, 0xa9ee, 0x46f9, {0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5}},
    {0x8b4f173b, 0x2fea, 0x4b80, {0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d}},
    {0xc70b221b, 0x40e4, 0x4a17, {0x89, 0xaf, 0x02, 0x5a, 0x07, 0x27, 0xa6, 0xdc}},
    {0x5c014b53, 0x68a1, 0x4b9b, {0x8b, 0xd1, 0xdd, 0x60, 0x46, 0xb9, 0x35, 0x8b}},
    {0x728f1fa2, 0xea11, 0x4a9f, {0x9b, 0x52, 0x9b, 0x42, 0xa4, 0x84, 0xd5, 0xc9}},
    {0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}},
};
const char* const kDeviceNames[] = {"ID3D12Device", "ID3D12Device1", "ID3D12Device2", "ID3D12Device3",
                                    "ID3D12Device4", "ID3D12Device5", "ID3D12Device6", "ID3D12Device7",
                                    "ID3D12Device8", "ID3D12Device11"};

// The interface version the layouts were checked against (2.4); 1.0.2.1 passes it.
constexpr uint32_t kCheckedVersion = 0x20004;

// Methods returning a struct take the result buffer first and the object second: (out, this, ...).
struct HeapProperties {
    uint32_t type, cpu_page_property, memory_pool, creation_node_mask, visible_node_mask;
};

// D3D12_HEAP_PROPERTIES GetCustomHeapProperties(UINT nodeMask, D3D12_HEAP_TYPE) (slot 26). The console has one
// memory pool (L0): default heaps aren't CPU-visible, upload and readback ones are write-back.
HeapProperties* WINAPI get_custom_heap_properties(void* a, void* b, uint32_t nodes, uint32_t type) {
    // called the C way (buffer, device) or the C++ way (device, buffer)
    auto* out = static_cast<HeapProperties*>(rt::shims::probe_object_name(a) ? b : a);
    constexpr uint32_t kCustom = 4, kNotAvailable = 1, kWriteBack = 3, kL0 = 1;
    nodes = nodes ? nodes : 1;
    uint32_t cpu = type == 1 ? kNotAvailable : (type == 2 || type == 3) ? kWriteBack : 0;
    if (out) *out = {kCustom, cpu, cpu ? kL0 : 0, nodes, nodes};
    return out;
}

// UINT GetDescriptorHandleIncrementSize(type) (slot 15): the console's descriptor sizes, not the PC's, because
// titles write descriptors into heaps as memory (T# 8 dwords / V# 4 dwords in a 32-byte view slot, S# 4 dwords).
// RTV / DSV sizes aren't known; 32 until a title shows otherwise.
UINT WINAPI get_descriptor_increment(void*, uint32_t type) {
    switch (type) {
        case 0: return 32;  // CBV / SRV / UAV
        case 1: return 16;  // sampler
        default: {
            static bool logged[8] = {};
            if (type < 8 && !logged[type]) {
                logged[type] = true;
                std::fprintf(stderr, "[rt] d3d12: descriptor increment for heap type %u: 32 (a guess)\n", type);
            }
            return 32;
        }
    }
}

int device_level(const GUID& iid) {
    for (int i = 0; i < static_cast<int>(std::size(kDeviceIids)); ++i)
        if (IsEqualGUID(iid, kDeviceIids[i])) return i;
    return -1;
}

constexpr GUID kIUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIDXGIDevice = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};

// The device answers as itself for IUnknown and any device level, and as IDXGIDevice through d3d12x.
HRESULT WINAPI device_query_interface(void* self, const GUID* iid, void** out) {
    if (!iid || !out) return E_POINTER;
    if (IsEqualGUID(*iid, kIUnknown) || device_level(*iid) >= 0) {
        *out = self;
        return S_OK;
    }
    if (IsEqualGUID(*iid, kIDXGIDevice)) {
        *out = rt::d3d12x::dxgi_device();
        return S_OK;
    }
    std::fprintf(stderr, "[rt] d3d12: device asked for {%08lX-%04X-%04X-...}: no\n", iid->Data1, iid->Data2, iid->Data3);
    *out = nullptr;
    return E_NOINTERFACE;
}

Device* device() {
    static Device d = [] {
        Device x{};
        x.vtbl = rt::shims::make_probe_vtable("D3D12Device");
        rt::shims::set_probe_method(x.vtbl, 0, reinterpret_cast<void*>(&device_query_interface));
        rt::shims::set_probe_method(x.vtbl, 27, reinterpret_cast<void*>(&rt::d3d12x::create_committed_resource));
        // 127: ID3D12Device10::CreateCommittedResource3 (by its arguments; the console table is longer than the PC's)
        rt::shims::set_probe_method(x.vtbl, 127, reinterpret_cast<void*>(&rt::d3d12x::create_committed_resource3));
        rt::shims::set_probe_method(x.vtbl, 25,reinterpret_cast<void*>(&rt::d3d12x::get_resource_allocation_info));
        rt::shims::set_probe_method(x.vtbl, 26,reinterpret_cast<void*>(&get_custom_heap_properties));
        rt::shims::set_probe_method(x.vtbl, 15, reinterpret_cast<void*>(&get_descriptor_increment));
        rt::shims::set_probe_method(x.vtbl, 14, reinterpret_cast<void*>(&rt::d3d12x::create_descriptor_heap));
        rt::shims::set_probe_method(x.vtbl, 17, reinterpret_cast<void*>(&rt::d3d12x::create_cbv));
        rt::shims::set_probe_method(x.vtbl, 18, reinterpret_cast<void*>(&rt::d3d12x::create_srv));
        rt::shims::set_probe_method(x.vtbl, 19, reinterpret_cast<void*>(&rt::d3d12x::create_uav));
        rt::shims::set_probe_method(x.vtbl, 10, reinterpret_cast<void*>(&rt::d3d12x::create_graphics_pipeline));
        rt::shims::set_probe_method(x.vtbl, 11, reinterpret_cast<void*>(&rt::d3d12x::create_compute_pipeline));
        // 62: the console's CreateComputePipelineState (two more arguments before the iid)
        rt::shims::set_probe_method(x.vtbl, 62, reinterpret_cast<void*>(&rt::d3d12x::create_compute_pipeline_x));
        // 23-41: the PC's order; 61, 74, 75, 86, 133: console slots (d3d12x_objects.cpp)
        rt::shims::set_probe_method(x.vtbl, 23, reinterpret_cast<void*>(&rt::d3d12x::copy_descriptors));
        rt::shims::set_probe_method(x.vtbl, 24, reinterpret_cast<void*>(&rt::d3d12x::copy_descriptors_simple));
        rt::shims::set_probe_method(x.vtbl, 28, reinterpret_cast<void*>(&rt::d3d12x::create_heap));
        rt::shims::set_probe_method(x.vtbl, 29, reinterpret_cast<void*>(&rt::d3d12x::create_placed_resource));
        rt::shims::set_probe_method(x.vtbl, 39, reinterpret_cast<void*>(&rt::d3d12x::create_query_heap));
        rt::shims::set_probe_method(x.vtbl, 41, reinterpret_cast<void*>(&rt::d3d12x::create_command_signature));
        rt::shims::set_probe_method(x.vtbl, 61, reinterpret_cast<void*>(&rt::d3d12x::create_sampler_x));
        rt::shims::set_probe_method(x.vtbl, 74, reinterpret_cast<void*>(&rt::d3d12x::register_page_pool));
        rt::shims::set_probe_method(x.vtbl, 75, reinterpret_cast<void*>(&rt::d3d12x::unregister_page_pool));
        rt::shims::set_probe_method(x.vtbl, 86, reinterpret_cast<void*>(&rt::d3d12x::create_pipeline_stream));
        rt::shims::set_probe_method(x.vtbl, 133, reinterpret_cast<void*>(&rt::d3d12x::create_resource_at));
        rt::shims::set_probe_method(x.vtbl, 108, reinterpret_cast<void*>(&rt::d3d12x::get_resource_allocation_info2));
        rt::shims::set_probe_method(x.vtbl, 57, reinterpret_cast<void*>(&rt::d3d12x::create_pipeline_57));
        rt::shims::set_probe_method(x.vtbl, 67, reinterpret_cast<void*>(&rt::d3d12x::create_pipeline_67));
        rt::shims::set_probe_method(x.vtbl, 55, reinterpret_cast<void*>(&rt::d3d12x::create_state_block));
        rt::shims::set_probe_method(x.vtbl, 16, reinterpret_cast<void*>(&rt::d3d12x::create_root_signature));
        rt::shims::set_probe_method(x.vtbl, 20, reinterpret_cast<void*>(&rt::d3d12x::create_rtv));
        rt::shims::set_probe_method(x.vtbl, 21, reinterpret_cast<void*>(&rt::d3d12x::create_dsv));
        rt::shims::set_probe_method(x.vtbl, 22, reinterpret_cast<void*>(&rt::d3d12x::create_sampler));
        // 70 / 71: console additions, views of memory the title placed itself
        rt::shims::set_probe_method(x.vtbl, 70, reinterpret_cast<void*>(&rt::d3d12x::create_placed_srv));
        rt::shims::set_probe_method(x.vtbl, 71, reinterpret_cast<void*>(&rt::d3d12x::create_placed_uav));
        // 78-81: the console's frame clock (frame.cpp)
        rt::shims::set_probe_method(x.vtbl, 78, reinterpret_cast<void*>(&rt::frame::set_frame_interval));
        rt::shims::set_probe_method(x.vtbl, 79, reinterpret_cast<void*>(&rt::frame::schedule_frame_event));
        rt::shims::set_probe_method(x.vtbl, 80, reinterpret_cast<void*>(&rt::frame::wait_frame_event));
        rt::shims::set_probe_method(x.vtbl, 81, reinterpret_cast<void*>(&rt::frame::frame_statistics));
        rt::shims::set_probe_method(x.vtbl, 8, reinterpret_cast<void*>(&rt::d3d12x::create_command_queue));
        rt::shims::set_probe_method(x.vtbl, 36, reinterpret_cast<void*>(&rt::d3d12x::create_fence));
        rt::shims::set_probe_method(x.vtbl, 9, reinterpret_cast<void*>(&rt::d3d12x::create_command_allocator));
        rt::shims::set_probe_method(x.vtbl, 12, reinterpret_cast<void*>(&rt::d3d12x::create_command_list));
        // 73: a console addition (PC slot 73 is ID3D12Device9-era; the console's CreateCommandListX)
        rt::shims::set_probe_method(x.vtbl, 73, reinterpret_cast<void*>(&rt::d3d12x::create_command_list_x));
        // 48: a console addition (on the PC, ID3D12Device3::OpenExistingHeapFromAddress)
        rt::shims::set_probe_method(x.vtbl, 48, reinterpret_cast<void*>(&rt::d3d12x::create_command_queue_x));
        x.out_of_line[0] = 1;
        return x;
    }();
    return &d;
}
[[maybe_unused]] Device* const g_made_early = device();  // the runtime has no thread-safe statics

// HRESULT D3D12XboxCreateDevice(adapter, const D3D12XBOX_CREATE_DEVICE_PARAMETERS*, REFIID, void** device).
// The parameters start with the interface version; the rest (ring and scratch sizes, features to leave out) doesn't
// matter yet.
HRESULT WINAPI D3D12XboxCreateDevice(void* adapter, const uint32_t* params, const GUID* iid, void** out) {
    if (!params || !iid) return E_INVALIDARG;
    int level = -1;
    for (int i = 0; i < static_cast<int>(std::size(kDeviceIids)); ++i)
        if (IsEqualGUID(*iid, kDeviceIids[i])) level = i;
    std::fprintf(stderr, "[rt] D3D12XboxCreateDevice(adapter %p, version %x, %s)\n", adapter, params[0],
                 level >= 0 ? kDeviceNames[level] : "unknown interface");
    if (params[0] != kCheckedVersion)
        std::fprintf(stderr, "[rt] d3d12_xs: device layouts were checked against version %x only\n", kCheckedVersion);
    if (level < 0) return E_NOINTERFACE;
    if (!rt::d3d12x::host_device()) return DXGI_ERROR_UNSUPPORTED;
    if (!out) return S_FALSE;
    *out = device();
    return S_OK;
}

// HRESULT D3D12ConfigureVirtualMemoryX(const desc*): the GPU virtual address layout the title wants (32 bytes; the
// dword at +4 is a count it computes). Accepted as is for now.
HRESULT WINAPI D3D12ConfigureVirtualMemoryX(const uint32_t* desc) {
    if (desc)
        std::fprintf(stderr, "[rt] D3D12ConfigureVirtualMemoryX(%x %x %x %x %x %x %x %x)\n", desc[0], desc[1], desc[2],
                     desc[3], desc[4], desc[5], desc[6], desc[7]);
    return S_OK;
}

}  // namespace

SHIM_TABLE(d3d12_xs,
    {"d3d12_xs.dll", "D3D12SerializeVersionedRootSignature", reinterpret_cast<void*>(&SerializeVersionedRootSignature_)},
    {"d3d12_xs.dll", "D3D12CreateRootSignatureDeserializer", reinterpret_cast<void*>(&CreateRootSignatureDeserializer_)},
    {"d3d12_xs.dll", "D3D12XboxCreateDevice", reinterpret_cast<void*>(&D3D12XboxCreateDevice)},
    {"d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX", reinterpret_cast<void*>(&D3D12ConfigureVirtualMemoryX)},
);
