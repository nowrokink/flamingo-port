// D3D12.X objects over the host's D3D12 (see d3d12x.cpp). Console objects keep the console's header (method table,
// bits, reference count) so the title's inlined D3D12.X code finds what it expects, then point at a host object.
#pragma once
#include <d3d12.h>

#include <cstdint>

#include "../gpu/gfx10_layout.h"

namespace rt::d3d12x {

// The host device every console object is made on; created on first use (null if the PC can't make one).
ID3D12Device* host_device();

// The upscale the title asks FidelityFX for (d3d12x_upscale.cpp): the colour's rendered part resized into the output
// with a linear filter, recorded on the title's list. The pointers are the title's (a list and two resources); the
// sizes may be zero (the textures' own). False when it wasn't recorded.
bool upscale(void* list, void* colour, void* output, uint32_t render_w, uint32_t render_h, uint32_t out_w,
             uint32_t out_h);
// Whether `p` is a resource of the runtime's (for reading a description whose layout isn't certain).
bool is_resource(void* p);

// The console object header.
struct Header {
    void** vtbl;
    uint32_t bits;  // low 3 bits: device index (0)
    uint32_t refs;
};

// Methods returning a struct take the result buffer first and the object second: (out, this, ...).

// Device methods implemented here (slots of the console device table).
HRESULT WINAPI create_command_queue(void* device, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID iid, void** out);
HRESULT WINAPI create_command_queue_x(void* device, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID iid, void** out);
HRESULT WINAPI create_fence(void* device, uint64_t initial, D3D12_FENCE_FLAGS flags, REFIID iid, void** out);
HRESULT WINAPI create_descriptor_heap(void* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID iid, void** out);
void WINAPI create_cbv(void* device, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, uint64_t dest);
void WINAPI create_srv(void* device, void* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, uint64_t dest);
void WINAPI create_uav(void* device, void* resource, void* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                       uint64_t dest);
void WINAPI create_rtv(void* device, void* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, uint64_t dest);
void WINAPI create_dsv(void* device, void* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, uint64_t dest);
void WINAPI create_sampler(void* device, const D3D12_SAMPLER_DESC* desc, uint64_t dest);
// Slots 70 / 71: views of memory the title placed itself (a console D3D12_RESOURCE_DESC, the view desc followed by
// the GPU address at +40, dest).
void WINAPI create_placed_srv(void* device, const gfx10::ResourceDesc* resource, const uint8_t* desc, uint64_t dest);
void WINAPI create_placed_uav(void* device, const gfx10::ResourceDesc* resource, const uint8_t* desc, uint64_t dest);
// CreateRootSignature(node mask, blob, size, iid, out) (slot 16).
HRESULT WINAPI create_root_signature(void* device, UINT node_mask, const void* blob, SIZE_T size, REFIID iid,
                                     void** out);
// Pipelines: CreateGraphicsPipelineState (10), CreateComputePipelineState (11), and the console's compute variant
// (62: desc, two more arguments, iid, out).
HRESULT WINAPI create_graphics_pipeline(void* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID iid,
                                        void** out);
HRESULT WINAPI create_compute_pipeline(void* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid,
                                       void** out);
HRESULT WINAPI create_compute_pipeline_x(void* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, void* a,
                                         void* b, REFIID iid, void** out);
// GetResourceAllocationInfo2 (slot 108): descs are D3D12_RESOURCE_DESC1 (0x48 bytes).
D3D12_RESOURCE_ALLOCATION_INFO* WINAPI get_resource_allocation_info2(D3D12_RESOURCE_ALLOCATION_INFO* out, void* device,
                                                                     UINT mask, UINT count,
                                                                     const struct ResourceDesc1* descs,
                                                                     D3D12_RESOURCE_ALLOCATION_INFO1* each);
// More objects (d3d12x_objects.cpp).
HRESULT WINAPI create_pipeline_57(void* device, const uint8_t* desc, REFIID iid, void** out);
HRESULT WINAPI create_pipeline_67(void* device, const uint8_t* desc, REFIID iid, void** out);
HRESULT WINAPI create_state_block(void* device, void* first, UINT count, const uint8_t* entries, REFIID iid, void** out);
HRESULT WINAPI create_heap(void* device, const D3D12_HEAP_DESC* desc, REFIID iid, void** out);
HRESULT WINAPI create_placed_resource(void* device, void* heap, uint64_t offset, const gfx10::ResourceDesc* desc,
                                      D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID iid,
                                      void** out);
HRESULT WINAPI create_resource_at(void* device, uint64_t address, const struct ResourceDesc1* desc, UINT layout,
                                  const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out);
HRESULT WINAPI register_page_pool(void* device, uint64_t address, uint64_t pages, uint64_t* handle);
HRESULT WINAPI unregister_page_pool(void* device, uint64_t handle);
HRESULT WINAPI create_query_heap(void* device, const D3D12_QUERY_HEAP_DESC* desc, REFIID iid, void** out);
HRESULT WINAPI create_command_signature(void* device, const D3D12_COMMAND_SIGNATURE_DESC* desc, void* root,
                                        REFIID iid, void** out);
HRESULT WINAPI create_pipeline_stream(void* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID iid,
                                      void** out);
void WINAPI create_sampler_x(void* device, const uint32_t* desc, uint64_t dest);
void WINAPI copy_descriptors_simple(void* device, UINT count, uint64_t dest, uint64_t src,
                                    D3D12_DESCRIPTOR_HEAP_TYPE type);
void WINAPI copy_descriptors(void* device, UINT dest_ranges, const uint64_t* dest_starts, const UINT* dest_sizes,
                             UINT src_ranges, const uint64_t* src_starts, const UINT* src_sizes,
                             D3D12_DESCRIPTOR_HEAP_TYPE type);
// GetResourceAllocationInfo(out, this, visible mask, count, descs) (slot 25): the console's sizes (gfx10_layout.h).
D3D12_RESOURCE_ALLOCATION_INFO* WINAPI get_resource_allocation_info(D3D12_RESOURCE_ALLOCATION_INFO* out, void* device,
                                                                    UINT mask, UINT count, const void* descs);
// CreateCommittedResource (slot 27, the PC's) and CreateCommittedResource3 (slot 127: heap properties, flags,
// D3D12_RESOURCE_DESC1, initial barrier layout, clear value, protected session, castable formats, iid, out).
HRESULT WINAPI create_committed_resource(void* device, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags,
                                         const void* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                                         REFIID iid, void** out);
HRESULT WINAPI create_committed_resource3(void* device, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags,
                                          const void* desc1, UINT layout, const D3D12_CLEAR_VALUE* clear,
                                          void* session, UINT castable_count, const DXGI_FORMAT* castable, REFIID iid,
                                          void** out);
HRESULT WINAPI create_command_allocator(void* device, D3D12_COMMAND_LIST_TYPE type, REFIID iid, void** out);
HRESULT WINAPI create_command_list(void* device, UINT node_mask, D3D12_COMMAND_LIST_TYPE type, void* allocator,
                                   void* pso, REFIID iid, void** out);
// CreateCommandListX(params { u32 type @0; u32 node mask @8 }, allocator, initial pipeline, iid, out): slot 73.
HRESULT WINAPI create_command_list_x(void* device, const uint32_t* params, void* allocator, void* pso, REFIID iid,
                                     void** out);

// The device's IDXGIDevice face (QueryInterface on the device): GetAdapter gives the host's adapter.
void* dxgi_device();

// The host object behind a console object of ours; null for anything else.
ID3D12Fence* host_fence(void* fence);
ID3D12CommandQueue* host_queue(void* queue);

// FlamingoPort.exe --pso-time ...: the driver's time to make one PC pipeline from translated shaders
// (d3d12x_pipelines.cpp).
int pso_time(int argc, wchar_t** argv);
// FlamingoPort.exe --warm-pipelines [--jobs N]: makes every pipeline the game has asked for so far (the recipes in
// <cache>\pipelines.txt), so the driver has them compiled before the next run.
int warm_pipelines(int argc, wchar_t** argv);

}  // namespace rt::d3d12x
