# GPU memory, descriptor heaps and shader access (D3D12 backend)

How title memory, descriptor heaps and translated shaders fit together on the PC's D3D12. Decided 2026-11-24 with the
move from Vulkan to D3D12 (see `design.md`). Parts marked **(to verify)** are assumptions the prototype or the
title still has to confirm.

## What the console does

- **Unified memory.** CPU and GPU share one pool. A GPU virtual address is the CPU address of the same byte: the
  title allocates graphics memory with `XMemAlloc` / `XMemVirtualAlloc` / physical pages, writes vertices, constants
  and descriptors through CPU pointers, and hands the same addresses to the GPU.
- **Physical pages are remapped.** 1.0.2.1 reserves placeholder regions and maps 64 KiB physical pages into them
  (`XMemMapPhysicalPages[Scatter]`), the same page sometimes at several addresses. The GPU sees the mapping too.
- **Descriptors are memory.** A descriptor heap is memory with 32-byte view slots (T#: 8 dwords, V#: 4) and 16-byte
  sampler slots (S#). The title writes slots itself (inlined `CopyDescriptorsSimple` is a memcpy) and through device
  methods. Shaders get a heap pointer in user SGPRs and `s_load` the descriptor bytes.
- **Low descriptor tables.** Some shaders force the high dword of a descriptor-table pointer to 0, so descriptor
  heaps live below 4 GiB.

## What the PC's D3D12 gives

- No pointers in HLSL: memory is reached through buffer views, each at most 4 GiB of byte offsets.
- SM 6.6 dynamic resources: `ResourceDescriptorHeap[i]` / `SamplerDescriptorHeap[i]`, so a shader can pick a view
  by computed index.
- `ID3D12Device3::OpenExistingHeapFromAddress` on a view of a pagefile-backed section the CPU also maps: a D3D12
  heap over it. The GPU then reads and writes that system memory directly, coherent with the CPU (over PCIe; slower
  than video memory). Not `OpenExistingHeapFromFileMapping` on the section itself: it works, but with the NVIDIA
  driver (32.0.16.1088) the section's commit is never given back — not on Release, not when the process ends — so
  every run lost its chunks' worth of commit until a reboot (measured: ~47 GB held, then error 1455 for the game).
  From an address it's given back, even when the process ends without releasing anything.
  Verified by `uma_test` (RTX 3070, 2026-11-24): a 1 GiB `SEC_COMMIT` section opens as a heap (`CUSTOM`,
  write-back, L0, flags `SHARED | SHARED_CROSS_ADAPTER`); resources on it need
  `D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER`; pages the CPU maps anywhere through placeholders, one page at two
  addresses too, are what the GPU copies, and its writes show at every alias after a fence.

Measured GPU copy bandwidth (256 MiB, best of 3):

| Copy | GB/s |
|---|---|
| pool (system RAM) -> video memory | 12.6 |
| video memory -> pool | 11.7 |
| pool -> pool | 8.1 |
| video memory -> video memory | 210.4 |

So the GPU reads title memory about 17 times slower than video memory. Correct but slow: data the GPU reads a lot
(vertex and index buffers, large constant and structured buffers) needs a video-memory copy kept in step with the
title's writes (write tracking on the pool pages), with the pool as the source of truth. That's an optimisation
after things draw, and the first thing to measure in benchmarks.

## The model

### 1. One pool of physical memory, in chunks

All memory the GPU may touch is backed by **pool chunks**: pagefile-backed sections of 1 GiB (`SEC_COMMIT`), made on
demand and each opened as a D3D12 heap with one buffer placed over all of it. A pool page is 64 KiB (the console
page and the Windows allocation granularity) and has a **frame number** `pfn = chunk * 16384 + page`. The physical
pages `xmem` hands out become pool pages (today each `XMemAllocatePhysicalPages` call makes its own section).

### 2. One GPU-visible address range, with a page table

The runtime reserves one range `R` of the process's address space at startup, starting below 4 GiB (so descriptor
heaps fit under 4 GiB) and as large as the title's GPU address needs (64 GiB to start). Every GPU-visible title
allocation is placed inside `R`:

- `XMemVirtualAlloc` and graphics `XMemAlloc` allocations, and the placeholder regions physical pages map into;
- descriptor heaps, at the start of `R`.

`R` is a placeholder reservation; committing memory in it maps pool pages there (the placeholder machinery `xmem`
already has), so the CPU writes pool memory directly. A **GPU page table** (one `uint` per 64 KiB page of `R`: the
pfn + 1, 0 = unmapped; 4 MiB for 64 GiB) mirrors the mapping. It changes when pages are mapped or unmapped and is
updated on the GPU before the next submission that could read it.

Memory outside `R` (the title's plain `VirtualAlloc`, its heap) isn't GPU-visible, as on the console where the title
must ask for GPU memory. **(to verify: which allocations 1.0.2.1 hands to the GPU, and with which XMem attributes)**

### 3. How a translated shader reaches memory

Console address `va` (from an SGPR pointer, a V# base, a table pointer):

```
page = PageTable.Load(((va - R_BASE) >> 16) * 4)      // pfn + 1; 0 = unmapped -> reads 0, writes dropped
phys = (uint64(page - 1) << 16) | (va & 0xFFFF)
win  = ResourceDescriptorHeap[POOL_VIEWS + uint(phys >> 30)]   // one raw view per 1 GiB chunk
value = win.Load(uint(phys & 0x3FFFFFFF))
```

One extra load per access (the page table is 4 MiB and stays in cache), no `switch` over windows: the chunk index
selects the view directly. Accesses that cross a 64 KiB page are split by the translator (rare: only wide loads at
page ends). This replaces the Vulkan design's `heap_base + va` buffer-device-address access.

### 4. Descriptors

- **V# (buffers): no PC descriptor at all.** The shader takes base, stride and format from the V# bytes and reads
  memory as above; typed formats are converted in the shader (the translator already does this).
- **T# (textures) and S# (samplers): the descriptor hash table**, as in the Vulkan design: the shader reads the raw
  T#/S# from heap memory, hashes it and finds the index of the PC SRV/UAV (in `ResourceDescriptorHeap`) or sampler
  (in `SamplerDescriptorHeap`). A miss gives index 0, a fallback. The table lives in pool memory too.
- The runtime fills the table when it learns a T#/S#: from the device's view-creation methods (it writes the console
  T# the title expects into the slot and registers it), from `CreateSampler` (S#), and from the literal-sampler list.
  Writing a genuine T# needs the console format codes and swizzle modes (from `xg_xs`, `XGCreateTextureComputer`).
- Textures themselves are PC textures in video memory (console tiling isn't the PC's): uploaded and detiled from
  title memory when the title writes them, and render targets / UAV textures live only on the GPU.

### 5. The ABI root signature

Every translated shader uses one root signature, with
`CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED`:

- a root constant buffer `UserData`: the 32 initial user SGPRs the console driver would load from the title's root
  signature, the code base (the shader's FSSM blob address, for `s_getpc_b64` constant tables), `R_BASE`, and the
  heap indices of the page table, the pool views and the hash table;
- nothing else: memory, textures and samplers are all reached through the heaps.

The runtime builds the user SGPRs from the title's `SetGraphicsRoot*` / `SetComputeRoot*` calls: descriptor tables
are heap addresses (handles are addresses), root CBVs are addresses, root constants are values. The order follows
the console driver's root-signature-to-SGPR mapping **(to verify from the exe)**.

## Order of work

1. **Prototype** (`uma_test`): done, see above.
2. **Pool and range `R`**: done (`shims/uma.cpp`, `xmem.cpp`, `vmem.cpp`). `R` is 64 GiB, found at the first 1 GiB
   boundary from 256 MiB up (0x80000000 here; address requirements with `MEM_RESERVE_PLACEHOLDER` are refused, so
   fixed addresses are tried). Physical pages are pool pages numbered as 4 KiB frames; a freed page stays valid
   while mapped and returns to the pool with its last mapping. Every `XMemVirtualAlloc` lands in `R` (executable
   memory excepted); the title's `VirtualAlloc` / `VirtualFree` / `VirtualQuery[Ex]` on `R` go to the bookkeeping
   (its placeholder splits are no-ops: `R` is split page by page as pages get mapped). The page table is kept on
   the CPU with a dirty range; uploading it is part of step 3. 1.0.2.1 uses 6 GiB of pool by device creation.
3. **Descriptor heaps**: done for what 1.0.2.1 asks before its first resource (`shims/d3d12x.cpp`). What it showed:
   - The console's `CreateDescriptorHeap` takes an extended desc `{D3D12_DESCRIPTOR_HEAP_DESC; u64 address @16;
     u64 bytes @24}`: the title allocates the heap's memory itself (e.g. 0xE1C810000, 1,245,184 view slots) and
     asserts that the CPU start handle equals that address. Without an address we reserve and commit one below 4 GiB.
   - A shader-visible heap above 4 GiB gets a **low alias** (`uma::alias_low`): the same pool pages mapped again
     below 4 GiB (0x80000000 here), and the GPU start handle is that alias, so a descriptor-table pointer with its
     high dword forced to 0 still works.
   - Handles (8-byte structs) come back in rax with `this` in rcx, unlike the larger structs (out, this, ...).
   - Increments: 32 bytes for views (CBV/SRV/UAV, RTV, DSV, a guess for the last two), 16 for samplers.
   - Views: CBV writes a V# (base, size, dword 3 = 0x31016fac); null SRV/UAV write a zero slot. Views of real
     resources wait for step 5.
   - The console CPU is Zen 2: `clwb` (and clzero, rdpru, monitorx/mwaitx, rdpid) fault on Intel CPUs and are
     emulated in the exception handler (`src/cpu_emul.cpp`); `clwb` shows up right after the first view.
   Still open: uploading the page table, D3D12 heaps over the pool chunks, T# encoding for real resources.
   The next call 1.0.2.1 makes is `GetResourceAllocationInfo` (device slot 25), which needs console resource sizes.
4. Translator: the D3D12 backend (HLSL/DXIL) with the memory access above, then the hash-table lookups.
5. Resources, pipelines, command lists replayed onto the PC's. **Resources started:**
   - `GetResourceAllocationInfo` (device slot 25) answers with the **console's sizes** (`gpu/gfx10_layout.cpp`, own
     model of addrlib's GFX10 rules: block dimensions, mip chain smallest level first with a shared mip-tail block,
     thick 3D blocks, linear rows of 256 bytes; the driver's default swizzle modes; HTILE before a depth buffer's
     texels, CMASK after a single-level render target's). The title places resources by these sizes and streams
     console-tiled data into them, so they must match; `gfx10_layout_test` checks them against retail texture sizes.
     1.0.2.1 first asks with a 4 KiB alignment for anything up to 512 KiB, and keeps it only if the answer is 4 KiB.
     Buffers get 4 KiB (when asked) or 64 KiB alignment: they're plain pool memory. MSAA isn't modelled (PC size).
   - **Resource memory goes through the title's XMem hooks.** The exe exports `__XMemAllocationHooks__`
     (`{alloc, free}`); the system calls them for its own allocations, and the title routes each request by allocator
     ID (XALLOC_ATTRIBUTES bits 16-23) to handlers it registers per scope. The console runtime's resource memory is
     allocator ID 128 with page size 2 (bits 14-15). 1.0.2.1 creates its two 3840x2160 render targets committed
     inside a 64 MiB scope whose handler bump-allocates them and asserts that less than 2 MiB is left: it only passes
     when the runtime allocates through the hooks with the console's sizes. `XMemAllocDefault` (the end of the chain)
     gives attributes with a page size whole 64 KiB pages in `R`.
   - `CreateCommittedResource` (27) and `CreateCommittedResource3` (127; the console table is longer than the PC's)
     make a resource object (PC `ID3D12Resource` order: Map, Unmap, GetDesc, GetGPUVirtualAddress, GetHeapProperties,
     GetDesc1) whose GPU address is its memory's address; textures also get a PC texture in video memory.
   - Root signatures: console flag bits 0x3000000 (raytracing ones) aren't the PC's and are left out of the PC blob.
   - Next: `CreateRootSignature` (16; `XboxBvhBuilder.dll` uses it right away), render target views (20), views of
     real resources (T# / hash table), placed resources and heaps, the address-only texture views of slot 70.
   - Noted for command lists: the console's inline writers keep a write pointer and end in the list object (+0x18,
     +0x30); our list objects must make them always fall back to the methods.

## Shaders as the title hands them over (1.0.2.1)

- **Slot 57 pipelines** (about ten thousand at start, the title's own shaders): the desc is the root signature, then
  (CPU pointer, size) pairs of metadata (e.g. 0x184, 0xbc bytes starting with a hash, 0xd0 bytes of register and user
  data layout) and (GPU address, size) pairs of **RDNA2 machine code already in GPU memory** (0x180-0x200 bytes for
  the first ones). The same ISA the translator handles from `shaders.rpf`; the runtime finds a shader by its code.
- **Pipelines made the PC's way** (slot 86 streams; slots 10, 11 and 62 graphics and compute descs): the stages
  come as DXBC containers (chunks SFI0, ISG1, OSG1, PSV0, RTS0, STAT, ILDN, HASH, DXIL) whose "DXIL" part is console
  code, not bitcode, as in `shaders.rpf`. Each stage takes its translation from the cache by the container's HASH
  digest (`attach_container_shaders`); there is no stage metadata, so the root parameters go to the user SGPRs in
  order. Until this was done for slots 11 and 62 the compute ones were handed to the PC as they came and refused
  (80070057): 24,922 of the 32,512 dispatches with no PC pipeline in one open-world run.
- **What the title does when slots 57 and 67 fail** (seen by running the VIctory project on the same copy of
  1.0.2.1: it returns E_NOTIMPL for both, 16,384 calls and more at start): it makes every pipeline the PC's way
  instead, with the fixed-function state as PC structs (rasterizer, depth-stencil, blend, formats, input layout).
  Slots 57 and 67 are the fast path (code already in GPU memory, state as GFX10 registers), which the runtime
  takes; the PC-style path would give the same state without reading registers, at the cost of the stage metadata.
- What the window shows today: 1.0.2.1 clears its two 3840x2160 RGBA8 targets to black and presents them in turn;
  ClearRenderTargetView and PresentX run on the PC, draws don't yet.
- **Finding a slot-57 shader in `shaders.rpf`**: each stage's metadata block (the (CPU pointer, size) pairs at +56,
  +88...) ends with the shader's 16-byte `HASH` chunk digest and 12 zero bytes. The GPU code itself (the pairs at +40,
  +72...) doesn't match the carved FSSM byte for byte: a ~36-byte header (the digest is in it), then the code as the
  driver finalised it for this pipeline. So the translated-shader cache is keyed by the `HASH` digest.
  (`RT_DUMP_SHADER_CODE=<dir>` writes the first pipelines' blocks for such checks.)
- **Stage metadata, decoded so far** (`RT_DUMP_SHADER_CODE` writes the blocks and each pipeline's root signature):
  the pairs at +56 and +88 are the **pixel** then the **vertex** shader's metadata. A block is a 16-byte header
  (`2`, size after the first 12 bytes, a per-shader value, `records << 15 | 0x440`) and 32-byte records. The pixel
  stage of a pipeline whose first stage is a mesh shader has a 24-byte header instead (`0x403`, size after the first
  20 bytes, a value, 0, 0, the same record count word), with the same records after it:
  - `0x41` binding: `kind` (`0x300` SRV, `0x302` CBV, `0x401` sampler, `0x2` root constants), offset in **dwords**
    into its descriptor table (each descriptor takes 8 dwords: ranges in order at 0, 8, 16...), the shader register
    (`t13`, `b27`, `s0..s21`, `b500`...), a resource-kind field, and a packed word whose byte 1 is the **user SGPR**
    that receives the table's address or the constants (`0x804` / `0x808`: s8; `0x10904`: s9). For static samplers
    the packed word's high half is the sampler's index in the root signature instead.
  - `0x42`: per-resource usage (type and dimension), not needed for the SGPR layout.
  - `0x43`: the shader's `HASH` digest (what the DXIL cache is keyed by).
  The table address goes in one SGPR, 32 bits: shaders clear the high half (`s_mov_b32 s9, 0`), so descriptor tables
  sit below 4 GiB, in the title's memory. Other user SGPRs are the driver's: the fetch shader's address (`s[4:5]`, a
  vertex shader's first instruction is `s_swappc_b64 s[4:5], s[4:5]`), the sampler heap (`s11` in a pixel shader
  that indexes it with a root constant), `m0` (`s12`). 1.0.2.1's root signatures are tiny (1 or 2 parameters,
  bindless heaps) and its UI (Scaleform `VTexTG` / `FTexTG`...) goes through the same pipelines.
- **The pipeline's register block** (the pair at +8 of a slot-57 desc: GFX10 context register values; 0x184 bytes
  for a pipeline that starts with a vertex shader, 0x1d8 for one that starts with a mesh shader, **whose registers
  below are all 0x54 further**). Found by listing the blocks of the pipelines a world load draws with next to what
  each draws into (`RT_DIAG_STATES=1`), graphics offsets:
  - `+0xA0` `PA_SU_SC_MODE_CNTL`: cull front [0], cull back [1], front face clockwise [2] (not applied yet).
  - `+0xB8` `DB_DEPTH_CONTROL`: stencil enable [0], depth enable [1], depth write [2], depth bounds [3], depth
    function [6:4], back-face stencil [7], stencil function [10:8], the back face's [22:20]. Functions: 0 never,
    1 less, 2 equal, 3 less or equal, 4 greater, 5 not equal, 6 greater or equal, 7 always. The world's depth is
    reversed (geometry and depth pre-pass pipelines: `0x007007e7`, greater or equal, on a D32 + S8 target); the
    shadow maps' isn't (`0x007007b6`, less or equal, on D16 targets).
  - `+0xBC` `DB_STENCIL_CONTROL`: fail [3:0], pass [7:4], depth fail [11:8], then the back face's three.
    Operations: 0 keep, 1 zero, 2 ones, 3 replace with the reference, 4 replace with the operation value, 5 / 6 add
    / subtract clamped, 7 invert, 8 / 9 add / subtract.
  - `+0xC0` / `+0xC4` `DB_STENCILREFMASK` and the back face's: reference [7:0], read mask [15:8], write mask
    [23:16], operation value [31:24].
  - `+0xCC` `CB_TARGET_MASK` (4 bits a target), `+0xD0..` `CB_BLEND0..7_CONTROL`.
  The runtime makes PC pipelines with the depth-stencil state and the blending read there, per depth-target format
  (`title_depth`, `title_blend` in `shims/d3d12x_pipelines.cpp`). Most of a world frame's mesh draws have no colour
  target at all (the depth pre-pass and the shadow maps), so without a depth target on the PC they drew nothing.
- **State blocks** (device slot 55: a pipeline plus fixed-function state, set in the pipeline's place): entries of
  332 bytes, a type and then the PC's struct. Type 1 is a `D3D12_RASTERIZER_DESC` (seen: solid, cull back, front
  counter-clockwise, depth clip on), type 2 a `D3D12_BLEND_DESC`. No other type seen in a world load.
- **Textures in formats the PC can't draw or compute into** (`make_host_texture`): R9G9B9E5 targets and compute
  outputs are made RGBA16F, B5G5R5A1 and B4G4R4A4 ones RGBA8, and their views take the texture's format. The
  upscaler's output (the scene at the display size, 1920x1080, R9G9B9E5 with target and unordered access) is one:
  refused by the PC, nothing after the upscale had a scene to read.
- **The upscale** (`ffxDispatch`, `shims/ffx.cpp` and `shims/d3d12x_upscale.cpp`): the SDK's description has the
  list at +16, the colour at +24, the output at +312, the render size at +376 and the display size at +384; one
  compute dispatch of the runtime's resizes the colour into the output with a linear filter (no anti-aliasing or
  history yet).
- **ExecuteIndirect** (list slot 61): one argument and one command a call. Run on the PC from the title's argument
  memory through the chunk's buffer, read when the list runs: mesh draws (type 10), dispatches (2, and the console's
  105), draws (0) and indexed draws (1). A record whose counts, as the title's CPU left them, are beyond what a
  pass could mean isn't run, and what the GPU passes write there later is limited on the GPU: each command reads a
  copy one compute thread of the runtime's makes into a scratch buffer, emptied when the counts are out of range
  (`limit_arguments` in `shims/d3d12x_draw.cpp`). Without that one command held the GPU and the title's 30 s
  watchdog ended the game.
- **Rectangle lists** (topology 17: three vertices a rectangle, the GPU finds the fourth opposite whichever vertex
  is at the right angle). Draws with a vertex buffer are expanded on the CPU (`expand_rects`); the others (the
  vertex shader makes the corners from the vertex number: full-screen passes; or the draw is indirect: the tiled
  lighting) go through a geometry shader the runtime writes for the vertex shader from its output signature
  (`shims/d3d12x_rects.cpp`), compiled with `d3dcompiler_47.dll`.
- **Stencil**: applied on the PC only together with a depth test. The pipelines that test stencil alone are the
  interface's masks (Scaleform: equal to the reference, add one on pass); with those applied the loading screen's
  pictures were all rejected. `RT_UI_STENCIL=1` applies them, to look into what the masks need.
- **Waits between queues** (`WaitOnValue`, list slots 73 / 74; comparison 7 is the only one seen): the compute
  queue's lists of a frame wait for a value (the frame's number, at a fixed address) that a graphics list writes at
  the bottom of the pipe, and are submitted before it. A submission with a wait that isn't met is held, with every
  later submission and signal of its queue, until the value is there, 250 ms have passed or the frame is
  presented (`run_held` in `shims/d3d12x.cpp`). A held list keeps its PC list and the title's object gets a spare:
  the title resets a list right after submitting it. **Off unless `RT_HOLD_WAITS=1`**: with lists held the PC
  removed the device (887A0001, at an ExecuteCommandLists) a minute into the open world, twice of two runs; what
  the held lists break isn't found yet. Measured without it: 560 of 2,048 waits found the memory one short.
- **The DXIL cache** (`tools/dxil_cache.py`; in the exe, `--build-cache`, the preparation's shaders step): `<port>/shaders/<digest>.dxil`, from
  `rdna2spirv.py --target d3d12` through SPIRV-Cross and DXC (SM 6.6). The runtime (`shims/shader_cache.cpp`) reads
  each slot-57 stage's digest and loads its DXIL into the pipeline (`RT_SHADER_CACHE` overrides the folder). The
  translator's register ABI: user data `b0` (uint4[9], the 36 user SGPRs), page table `u8`, memory chunks `u9[]`,
  descriptor hash table `t2`, bindless arrays each in a space of its own (`t0 space1` textures, `s0 space2`
  samplers, `s0 space11` the same samplers as comparison samplers, ...). Rewritten for HLSL on this target: add with
  carry, widening multiplies (16-bit halves), depth-compare samples.

