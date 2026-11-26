// Objects and helpers shared by the D3D12.X files (d3d12x.cpp, d3d12x_views.cpp, d3d12x_bindless.cpp).
#pragma once
#include <d3d12.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <array>
#include <vector>

#include "../gpu/gfx10_layout.h"
#include "../gpu/stage_meta.h"
#include "shader_cache.h"
#include "d3d12x.h"

namespace rt::d3d12x {

struct ResourceDesc1 {
    gfx10::ResourceDesc desc;
    uint32_t mip_region[3];
    uint32_t pad;
};
static_assert(sizeof(ResourceDesc1) == 0x48);

// A console resource: unified memory at `va` (its GPU virtual address too); textures also have a PC texture.
struct Resource {
    Header header;
    ResourceDesc1 desc;
    uintptr_t va;
    uint64_t bytes;
    D3D12_HEAP_PROPERTIES heap;
    D3D12_HEAP_FLAGS heap_flags;
    ID3D12Resource* host;  // the PC texture (null for buffers, and textures the PC didn't make)
    // placed textures: the first console level the PC texture holds (its level 0); the levels before it weren't in
    // the title's memory when it was made (d3d12x_views.cpp)
    uint32_t first_mip = 0;
    uint64_t host_bytes = 0;  // placed textures: the PC texture's size, counted against the texture budget
    uint64_t last_used = 0;   // placed textures: the present count when a draw last used it
    // a texture made at an address the title maps itself (slot 133): streamed like the placed ones, its PC texture
    // follows what is mapped (first_mip) and the texture budget
    bool streamed = false;
};

// Whether `p` is an object whose method table is `table` (false for bad pointers too).
bool is_object(void* p, void** table);

// Methods returning a struct through a buffer are called both ways: the C way (buffer, this) and the C++ way
// (this, buffer), by different call sites in the same title. Whichever argument is our object is `this`.
template <typename Object, typename Out>
std::pair<Object*, Out*> this_and_out(void* a, void* b, void** table) {
    if (is_object(a, table)) return {static_cast<Object*>(a), static_cast<Out*>(b)};
    return {static_cast<Object*>(b), static_cast<Out*>(a)};
}

void** resource_table();
Resource* as_resource(void* p);  // null when `p` isn't one of ours
// PC textures of placed textures whose memory the title has reused since: released once no list can still use them
// (d3d12x_views.cpp; called at each present, the queue idle).
void release_retired_textures(uint64_t present);
// What is making PC textures on this thread, for the tally of texture memory (returns the tag; `set` replaces it).
const char* texture_source(const char* set = nullptr);
// Streamed resources (slot 133 textures that aren't targets): adopt gives one its PC texture, as much of it as is
// mapped; refresh looks again at what is mapped (when the title makes a view of it); drop is for its last release.
void adopt_streamed(Resource* r);
void refresh_streamed(Resource* r);
void drop_streamed(Resource* r);
// Logs the dispatches remembered on a PC list, the one after `ran` marked (d3d12x_draw.cpp; for a removed device).
void report_dispatches(ID3D12GraphicsCommandList* list, unsigned ran);
void report_mesh_draws(ID3D12GraphicsCommandList* list, unsigned ran);
// A command signature as the title made it (CreateCommandSignature, device slot 41); false if `p` isn't one.
struct RootSignature;
struct IndirectSignature {
    uint32_t stride;
    const D3D12_INDIRECT_ARGUMENT_DESC* arguments;
    size_t count;
    RootSignature* root;
};
bool indirect_signature(void* p, IndirectSignature* out);
// A PC texture nothing shows any more: out of the PC's descriptors now, released a few presents later.
void retire_texture(ID3D12Resource* texture);
// The resource a draw's T# shows, marked as used now; a placed texture whose PC texture was evicted (or put off, over
// the budget) gets it made and bound again. Null when the T# is nothing the runtime knows (d3d12x_views.cpp).
Resource* used_view(const uint32_t* tsharp);
// A PC texture about to be released: off the tally of live texture memory (d3d12x.cpp).
void note_texture_released(ID3D12Resource* texture);

// A root signature as the title described it (version 1.1 view of the description).
struct RootSignature {
    Header header;
    ID3D12VersionedRootSignatureDeserializer* deserializer;  // owns `desc`
    const D3D12_ROOT_SIGNATURE_DESC1* desc;
};
void** root_signature_table();
RootSignature* as_root_signature(void* p);

// A stage's code. The title's own binaries are kept; a translation found in the shader cache is not, only its digest
// and size, and bytes() reads it from the cache when a PC pipeline is made. Kept, the translations of the ~8,400
// pipelines the title makes held 10 GiB of RAM (measured at the title screen; they hardly repeat between pipelines).
class Code {
public:
    Code() = default;
    Code(std::vector<uint8_t>&& v) { *this = std::move(v); }
    Code& operator=(std::vector<uint8_t>&& v) {
        size_ = v.size();
        cached_ = false;
        bytes_ = size_ ? std::make_shared<const std::vector<uint8_t>>(std::move(v)) : nullptr;
        return *this;
    }
    // A translation of the cache, by digest (`size`: its bytes, read once to learn them).
    void set_cached(const uint8_t digest[16], size_t size) {
        std::memcpy(digest_, digest, 16);
        size_ = size;
        cached_ = true;
        bytes_.reset();
    }
    bool empty() const { return !size_; }
    size_t size() const { return size_; }
    void clear() { *this = Code(); }
    // The bytes, for as long as the caller holds them: the kept ones, or read from the cache now (null if the file
    // went away; empty code: null).
    std::shared_ptr<const std::vector<uint8_t>> bytes() const;

private:
    std::shared_ptr<const std::vector<uint8_t>> bytes_;
    uint8_t digest_[16]{};
    bool cached_ = false;
    size_t size_ = 0;
};

// The thread-group size a translated compute shader was compiled with (its DXIL's PSV0 part); false if not there.
bool dxil_group_size(const std::vector<uint8_t>& dxil, UINT size[3]);

// A pipeline as the title made it: console shader binaries (compute: code[0]; graphics: VS, PS, DS, HS, GS).
struct PipelineState {
    Header header;
    bool compute;
    RootSignature* root;
    Code code[5];
    UINT cs_group[3] = {};  // a translated compute shader's thread-group size (from its DXIL), 0 if not known
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};  // its shader pointers are the title's, not to be used
    std::vector<uint8_t> stream;                    // a pipeline-state stream, kept whole
    ID3D12PipelineState* host = nullptr;            // made from translated shaders on first use
    // slot 57: what the stage metadata says (per code[] slot), the input layout (+24), the vertex shader's inputs
    gpu::StageMeta meta[5];
    std::vector<gpu::InputElement> input;
    std::vector<shader_cache::VsInput> vs_in;
    std::vector<uint8_t> state;  // slot 57: the block at +8 (0x184 bytes, GFX10 register values)
    std::vector<std::array<uint8_t, 16>> missing;  // slot 57: digests of stages the cache doesn't have
    bool from_stream = false;    // slot 86: no stage metadata; root parameters go to user SGPRs in order from s8
    bool mesh = false;  // code[0] is a mesh shader (the title's NGG mesh stage), drawn with DispatchMesh
};

// ---- PC pipelines from translated shaders (d3d12x_pipelines.cpp)
// The root signatures every translated shader is compiled against: per stage a root CBV at b0 (its 36 user-data
// dwords; graphics: params 0 vertex, 1 pixel; compute: param 0), then a view table (2) and a sampler table (3).
ID3D12RootSignature* abi_root(bool compute);
// The PC graphics pipeline for a slot-57 pipeline with these targets (made once per combination); null if it can't be.
// `state_block`: the state block the pipeline was set through (its blend state replaces the pipeline's), or null.
ID3D12PipelineState* host_pso(PipelineState& p, UINT rt_count, const DXGI_FORMAT* rt_formats, DXGI_FORMAT dsv_format,
                              D3D12_PRIMITIVE_TOPOLOGY_TYPE topology, void* state_block);
// A topology type with this bit is a rectangle list the runtime can't expand itself: the pipeline gets the geometry
// shader that completes each rectangle (d3d12x_rects.cpp), and is drawn as a triangle list.
constexpr int kRectTopology = 0x100;
// That geometry shader for a translated vertex shader (made once); null when it can't be made.
const std::vector<uint8_t>* rect_geometry_shader(const std::vector<uint8_t>& vs);
// The translations of a pipeline made from shader containers, by their digests (d3d12x_objects.cpp).
void attach_container_shaders(PipelineState& p);
// RT_PSO_TRIAL=<n>: the first n slot-57 pipelines also make a PC pipeline with default state, and the counts are
// logged (whether the translated shaders, the ABI root signature and the input layouts agree).
void pso_trial(PipelineState& p);
// The PC compute pipeline for a compute pipeline of the title (made once); null if it can't be.
ID3D12PipelineState* host_compute_pso(PipelineState& p);
// The PC pipeline for a pipeline whose first stage is a mesh shader, with these targets; null if it isn't ready
// or can't be. Its root signature is abi_mesh_root(): as the graphics one, param 0 seen by the mesh stage.
ID3D12PipelineState* host_mesh_pso(PipelineState& p, UINT rt_count, const DXGI_FORMAT* rt_formats, DXGI_FORMAT dsv_format,
                                   void* state_block);
// Whether a pipeline's register block tests or writes depth or stencil on a depth target of this format.
bool title_uses_depth(const PipelineState& p, DXGI_FORMAT dsv);
// The stencil reference in a pipeline's register block: a draw's, until the list sets one (OMSetStencilRef).
UINT title_stencil_ref(const PipelineState& p);
ID3D12RootSignature* abi_mesh_root();

// ---- a command list's graphics state, and draws on the PC (d3d12x_draw.cpp)
// What the title set on a console list, kept until a draw: root arguments by parameter, input assembly, viewports,
// targets (console handles). A draw turns it into PC commands on the list's host list.
struct GfxState {
    RootSignature* root = nullptr;
    PipelineState* pso = nullptr;
    void* state_block = nullptr;  // the last state block set (slot 55 objects; their state isn't applied yet)
    static constexpr UINT kParams = 32;
    uint64_t tables[kParams] = {};      // descriptor table addresses (title memory)
    uint64_t root_va[kParams] = {};     // root CBV / SRV / UAV addresses
    uint32_t constants[kParams][64] = {};
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    D3D12_VIEWPORT viewports[16] = {};
    UINT viewport_count = 0;
    D3D12_RECT scissors[16] = {};
    UINT scissor_count = 0;
    D3D12_INDEX_BUFFER_VIEW ib = {};
    D3D12_VERTEX_BUFFER_VIEW vb[16] = {};
    UINT vb_count = 0;
    uint64_t rtv[8] = {};
    UINT rtv_count = 0;
    uint64_t dsv = 0;
    UINT stencil_ref = 0;          // OMSetStencilRef (slot 24), once set
    bool stencil_ref_set = false;
    std::vector<Resource*> used;  // textures the recorded draws read (d3d12x_upload.cpp), uploaded at submission
    // The PC targets the last draw left in a writable state (render target / depth): kept so while the next draws
    // use the same ones, put back in the common state (release_targets) before anything else is recorded and at
    // Close. A transition pair around every draw made the GPU drain and decompress the targets thousands of times
    // a frame in the open world.
    static constexpr UINT kHeld = 9;
    ID3D12Resource* held[kHeld] = {};
    D3D12_RESOURCE_STATES held_state[kHeld] = {};
    UINT held_count = 0;
};
// Draws: the targets in `r` (in states `s`) made the writable ones; the ones held before go back to common unless
// they're the same. Anything else recorded on the list first calls release_targets.
void hold_targets(GfxState& g, ID3D12GraphicsCommandList* host, ID3D12Resource* const* r,
                  const D3D12_RESOURCE_STATES* s, UINT n);
void release_targets(GfxState& g, ID3D12GraphicsCommandList* host);
// PC pipelines asked for and not ready yet (the driver is compiling them), for the overlay.
size_t pipelines_being_made();
// With `arguments` (an address in the title's memory holding a draw's four dwords, or an indexed draw's five), an
// indirect draw that reads them when it runs; the counts passed are then not used.
void draw(GfxState& g, ID3D12GraphicsCommandList* host, bool indexed, UINT count, UINT instances, UINT start,
          INT base_vertex, UINT start_instance, uint64_t arguments = 0);
// A dispatch with the compute arguments in `c` (its pso, root signature and root arguments); with `arguments` (an
// address in the title's memory holding the three counts), an indirect one that reads them when it runs.
void dispatch(GfxState& c, ID3D12GraphicsCommandList* host, UINT x, UINT y, UINT z, uint64_t arguments = 0);
// DispatchMesh(groups, 1, 1) with the list's graphics state, or, with `arguments` (an address in the title's
// memory holding three dwords), the same as an indirect draw that reads them when it runs.
void draw_mesh(GfxState& g, ID3D12GraphicsCommandList* host, UINT groups, uint64_t arguments);
// The PC list a console list records into; null when `list` isn't one of ours (d3d12x.cpp).
ID3D12GraphicsCommandList* host_list_of(void* list);
// The last upscale's textures (d3d12x_upscale.cpp): 0 the colour it read, 1 the output it wrote; null before one.
ID3D12Resource* last_upscale(int k);
// The k-th target of the mesh draw with the most targets seen lately (for RT_SHOW_MESH); null if none.
ID3D12Resource* last_mesh_target(int k);
// Once a frame: `asked` starts the measuring of the next mesh draws; a few frames later their numbers are logged.
void mesh_stats_frame(uint64_t frame, bool asked);
// Before a submission: the page table and the descriptor hash table to the GPU, recorded into `pre`.
void prepare_submission(ID3D12GraphicsCommandList* pre);
void** pipeline_table();
// The console's state blocks (slot 55): a pipeline (the one it was made from) with fixed-function state of its own,
// set through SetPipelineState in that pipeline's place.
void** state_block_table();
// A state block's entries (332 bytes each: a type, then the state), or null for anything else.
const std::vector<uint8_t>* state_block_entries(void* block);
// The first argument a state block was made with (what the title made it from), or null.
void* state_block_first(void* block);
PipelineState* as_pipeline(void* p);

// The PC texture for a console texture description (null: the PC refused it, or a console-only format).
ID3D12Resource* make_host_texture(const gfx10::ResourceDesc& desc, const D3D12_CLEAR_VALUE* clear);

// What an RTV / DSV handle names (d3d12x_views.cpp); false for anything else. No desc: the resource's defaults.
struct TargetView {
    Resource* resource;
    bool depth;
    bool has_rtv;
    D3D12_RENDER_TARGET_VIEW_DESC rtv;
    bool has_dsv;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv;
};
bool target_view(uint64_t handle, TargetView* out);

// ---- the PC's shader-visible descriptor heaps and the descriptor hash table (d3d12x_bindless.cpp)
//
// Translated shaders find the PC descriptor of a console T# / S# by hashing its words (desc_table.h). Index 0 of each
// heap is the fallback (a null view, a point/clamp sampler), and is what a miss resolves to.

// Maps a T# to index k of the view heap, with the texture's SRV at descriptor k and its UAV (a null one when the
// texture doesn't allow unordered access) at uav_view_base() + k: on the console one descriptor serves both. Null descs: the
// defaults. Returns k (0: the fallback, when the PC refused or the heap is full); the same T# again: the same k.
uint32_t bind_texture(const uint32_t tsharp[8], ID3D12Resource* texture, const D3D12_SHADER_RESOURCE_VIEW_DESC* srv,
                      const D3D12_UNORDERED_ACCESS_VIEW_DESC* uav);
// Takes a PC texture out of every descriptor bind_texture gave it (they read as null, and their T#s bind anew): for a
// texture about to be released.
void unbind_texture(ID3D12Resource* texture);
// Forgets what was uploaded to a resource's PC texture (it has a new one).
void forget_upload(Resource* r);
// A PC render target view for a console RTV handle, and its PC texture; false for anything else (depth views, textures
// the PC has no copy of).
bool host_rtv(uint64_t handle, D3D12_CPU_DESCRIPTOR_HANDLE* out, ID3D12Resource** texture,
              DXGI_FORMAT* format = nullptr);

// The same for a console DSV handle: a PC depth-stencil view in the texture's depth format (`format`), and whether
// the view leaves every plane read only. False for anything else (colour views, textures the PC has no depth
// copy of).
bool host_dsv(uint64_t handle, D3D12_CPU_DESCRIPTOR_HANDLE* out, ID3D12Resource** texture, DXGI_FORMAT* format,
              bool* read_only);

uint32_t bind_sampler(const uint32_t ssharp[4], const D3D12_SAMPLER_DESC& desc);
// Where a T# / S# was bound (0: never, so translated shaders read the fallback). For diagnostics.
uint32_t view_index(const uint32_t tsharp[8]);
uint32_t sampler_index(const uint32_t ssharp[4]);

// The shader-visible heaps (null until the first view is made; ensure_heaps() makes them), and a fixed area at the end
// of the view heap for the runtime's own views (gpu_memory.h's slots): fixed_view(slot) is descriptor
// fixed_view_base() + slot.
bool ensure_heaps();
// The descriptor hash table to the GPU: its changed words copied into `list` (before commands that read it).
bool upload_descriptor_table(ID3D12GraphicsCommandList* list);
ID3D12DescriptorHeap* view_heap();
ID3D12DescriptorHeap* sampler_heap();
constexpr uint32_t kFixedViews = 128;
uint32_t fixed_view_base();
uint32_t uav_view_base();
D3D12_CPU_DESCRIPTOR_HANDLE fixed_view(uint32_t slot);

// ---- textures on the PC (d3d12x_upload.cpp)
// The resource behind view-heap index `index` (bind_texture's result), for finding a draw's textures from its T#s.
void remember_view(uint32_t index, Resource* r);
Resource* view_resource(uint32_t index);
// Records into `list` the uploads of the textures in `used` that are new or whose memory changed (detiled from the
// title's memory); returns the upload buffer to release once the GPU has run `list`, or null.
ID3D12Resource* upload_textures(ID3D12GraphicsCommandList* list, const std::vector<Resource*>& used);

// ---- frame diagnostics (d3d12x_diag.cpp), on when RT_DIAG_FRAMES lists frames ("300" or "100,300,600"; "0": none,
// only those asked for by creating %LOCALAPPDATA%\Flamingo\diag\now, which reports the next frame): at the end
// of each listed frame, a table of the PC textures the frame's commands wrote (draws, clears, size, format, whether
// presented), the draws not drawn by reason, the dispatches, and each texture read back from the GPU into a .bmp in
// %LOCALAPPDATA%\Flamingo\diag with its pixel statistics in the log.
namespace diag {
enum Skip { kNoShader, kNoTarget, kNoPipeline, kNoBuffer, kSkips };
bool enabled();
// True while recording a frame that will be reported: its draws and dispatches are logged one by one.
bool listing();
uint64_t frame();  // the frame being recorded
void drawn(ID3D12Resource* const* targets, UINT count);  // a draw recorded into these render targets
void cleared(ID3D12Resource* target);
void sampled(ID3D12Resource* texture);  // a texture a reported frame's shaders read (read back with the targets)
void skipped(Skip why);
void dispatched();
// `bytes` written to the diagnostics folder as `name` (once per name).
void save(const wchar_t* name, const void* bytes, size_t size);
// After frame `frame` was presented from `presented`; `queue` is idle.
void frame_end(uint64_t frame, ID3D12CommandQueue* queue, ID3D12Resource* presented);
}  // namespace diag

}  // namespace rt::d3d12x
