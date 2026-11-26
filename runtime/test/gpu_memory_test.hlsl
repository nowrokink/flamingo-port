// gpu_memory_test: reads the title's memory the way translated shaders do (tools/rdna2spirv.py mem_access): the page
// table gives the pool page of a 64 KiB page of R, the pool is 1 GiB chunk buffers, unmapped pages read the null chunk.
cbuffer Args : register(b0) {
    uint base_lo, base_hi;      // R's base (user data words 32-33 in translated shaders)
    uint addr_lo, addr_hi;      // where to read: count dwords, `stride` bytes apart
    uint count, stride;
};
RWByteAddressBuffer pagetable : register(u8, space0);
RWByteAddressBuffer chunks[] : register(u9, space0);
RWByteAddressBuffer result : register(u0, space9);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) return;
    uint64_t base = (uint64_t(base_hi) << 32) | base_lo;
    uint64_t a = ((uint64_t(addr_hi) << 32) | addr_lo) + uint64_t(id.x) * stride;
    uint page = uint((a - base) >> 16);
    uint e = pagetable.Load(page * 4);
    uint64_t off = (uint64_t(e - 1) << 16) | (a & 0xffff);
    uint chunk = e != 0 ? uint(off >> 30) : 64;
    result.Store(id.x * 4, chunks[NonUniformResourceIndex(chunk)].Load(uint(off) & 0x3fffffff));
}
