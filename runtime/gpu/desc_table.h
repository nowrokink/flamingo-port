// Descriptor hash table read by translated shaders: raw T#/S# -> bindless index (design.md, "Descriptor hash table").
//
// Layout (storage buffer, set 0 binding 2), exactly as rdna2spirv.Gen.desc_lookup reads it:
//   uint mask;                       slots - 1, slots a power of two
//   entries[slots] of 10 uints:      key[0..7], index, pad
// Key = the 8 T# dwords, or the 4 S# dwords followed by four 0xFFFFFFFF. Hash = FNV-1a over the 8 key dwords
// (whole dwords). The shader probes slots (hash + k) & mask for k = 0..7, compares the key in every one of them
// (it doesn't stop at an empty slot) and takes the index of the first match; no match gives index 0, where the
// runtime keeps fallback descriptors. An empty slot is all zeros, so an all-zero key also resolves to index 0.
// Which slots are used is tracked here on the CPU, never in the table.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace rt::gpu {

using DescKey = std::array<uint32_t, 8>;

class DescriptorHashTable {
public:
    static constexpr uint32_t kProbes = 8;
    static constexpr uint32_t kEntryWords = 10;

    explicit DescriptorHashTable(uint32_t slots = 4096);   // rounded up to a power of two, at least kProbes

    static DescKey tsharp_key(const uint32_t t[8]);
    static DescKey ssharp_key(const uint32_t s[4]);
    static uint32_t hash(const DescKey& key);

    // Adds or updates key -> index. When the key's 8-slot window is full the table doubles until everything fits
    // (generation() changes, and the GPU copy must be reallocated).
    void insert(const DescKey& key, uint32_t index);
    bool remove(const DescKey& key);

    // What a translated shader would read for this key, from the current words (index 0 on a miss).
    uint32_t lookup(const DescKey& key) const;

    const std::vector<uint32_t>& words() const { return words_; }
    uint32_t slots() const { return mask_ + 1; }
    uint32_t size() const { return count_; }
    uint32_t generation() const { return generation_; }   // bumps on every resize
    // Word range touched since the last clear_dirty(): [begin, end). begin == end when nothing changed.
    uint32_t dirty_begin() const { return dirty_begin_; }
    uint32_t dirty_end() const { return dirty_end_; }
    void clear_dirty() { dirty_begin_ = dirty_end_ = 0; }

private:
    uint32_t mask_ = 0, count_ = 0, generation_ = 0;
    uint32_t dirty_begin_ = 0, dirty_end_ = 0;
    std::vector<uint32_t> words_;
    std::vector<uint8_t> used_;
    void reset(uint32_t slots);
    bool try_insert(const DescKey& key, uint32_t index);
    int find_slot(const DescKey& key) const;
    void write_slot(uint32_t slot, const DescKey& key, uint32_t index);
    void mark(uint32_t begin, uint32_t end);
};

}  // namespace rt::gpu
