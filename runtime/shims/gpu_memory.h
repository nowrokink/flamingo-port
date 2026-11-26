// The title's memory as translated shaders see it (docs/gpu-memory.md): each 1 GiB pool chunk of uma is a D3D12 heap
// (the same pagefile section the CPU maps, no copies) with one buffer over it, and the page table (one uint per
// 64 KiB page of R: pool page + 1) is a GPU buffer kept in step with uma's.
//
// The translator's bindings (tools/rdna2spirv.py, --target d3d12): the page table at u8 space0, the chunks as an
// array at u9 space0 (index kNullChunk is a null view: unmapped pages read zero), R's base in user data words 32-33.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace rt::gpumem {

constexpr uint32_t kNullChunk = 64;                 // NULL_CHUNK in the translator
constexpr uint32_t kChunkViews = kNullChunk + 1;

// Fixed slots of the shader-visible view heap (counted from bindless's fixed area, see d3d12x_internal.h).
constexpr uint32_t kSlotPageTable = 0;
constexpr uint32_t kSlotChunks = 1;                 // kChunkViews of them
constexpr uint32_t kSlotHashTable = kSlotChunks + kChunkViews;
constexpr uint32_t kFixedSlots = kSlotHashTable + 1;

// Before a submission whose commands may read the title's memory: opens chunks uma added since, and records into
// `list` the copy of the page table's changed entries (with the barriers around it). False if the GPU side couldn't
// be set up (the commands still run; translated shaders would read zeros).
bool prepare(ID3D12GraphicsCommandList* list);

// R's base (user data words 32 and 33).
uint64_t r_base();

// The PC GPU address of title memory [va, va + bytes) (vertex and index buffers): false unless its pages are
// consecutive pool pages of one chunk the GPU has (the usual case for one allocation).
bool host_va(uint64_t va, uint64_t bytes, D3D12_GPU_VIRTUAL_ADDRESS* out);

// The same, as the chunk's buffer and the offset in it (for copies): false on the same terms.
bool host_buffer(uint64_t va, uint64_t bytes, ID3D12Resource** buffer, uint64_t* offset);

}  // namespace rt::gpumem
