#include "desc_table.h"

#include <algorithm>

namespace rt::gpu {

DescriptorHashTable::DescriptorHashTable(uint32_t slots) {
    uint32_t n = kProbes;
    while (n < slots) n <<= 1;
    reset(n);
}

DescKey DescriptorHashTable::tsharp_key(const uint32_t t[8]) {
    DescKey k;
    std::copy(t, t + 8, k.begin());
    return k;
}

DescKey DescriptorHashTable::ssharp_key(const uint32_t s[4]) {
    return {s[0], s[1], s[2], s[3], 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
}

uint32_t DescriptorHashTable::hash(const DescKey& key) {
    uint32_t h = 2166136261u;
    for (uint32_t w : key) h = (h ^ w) * 16777619u;
    return h;
}

void DescriptorHashTable::reset(uint32_t slots) {
    mask_ = slots - 1;
    count_ = 0;
    words_.assign(1 + size_t(slots) * kEntryWords, 0);
    words_[0] = mask_;
    used_.assign(slots, 0);
    mark(0, uint32_t(words_.size()));
}

void DescriptorHashTable::mark(uint32_t begin, uint32_t end) {
    if (dirty_begin_ == dirty_end_) {
        dirty_begin_ = begin;
        dirty_end_ = end;
    } else {
        dirty_begin_ = std::min(dirty_begin_, begin);
        dirty_end_ = std::max(dirty_end_, end);
    }
}

int DescriptorHashTable::find_slot(const DescKey& key) const {
    uint32_t h = hash(key);
    for (uint32_t k = 0; k < kProbes; k++) {
        uint32_t slot = (h + k) & mask_;
        if (!used_[slot]) continue;
        const uint32_t* e = &words_[1 + size_t(slot) * kEntryWords];
        if (std::equal(key.begin(), key.end(), e)) return int(slot);
    }
    return -1;
}

void DescriptorHashTable::write_slot(uint32_t slot, const DescKey& key, uint32_t index) {
    uint32_t base = 1 + slot * kEntryWords;
    std::copy(key.begin(), key.end(), words_.begin() + base);
    words_[base + 8] = index;
    words_[base + 9] = 0;
    mark(base, base + kEntryWords);
}

bool DescriptorHashTable::try_insert(const DescKey& key, uint32_t index) {
    if (int s = find_slot(key); s >= 0) {
        write_slot(uint32_t(s), key, index);
        return true;
    }
    uint32_t h = hash(key);
    for (uint32_t k = 0; k < kProbes; k++) {
        uint32_t slot = (h + k) & mask_;
        if (used_[slot]) continue;
        // The shader takes the FIRST matching slot in the window, used or not. An unused slot holds zeros, so a
        // zero key would match an earlier empty slot instead: harmless, since that also yields index 0.
        used_[slot] = 1;
        count_++;
        write_slot(slot, key, index);
        return true;
    }
    return false;
}

void DescriptorHashTable::insert(const DescKey& key, uint32_t index) {
    if (try_insert(key, index)) return;
    // Window full: grow and rehash everything (plus the new key) until it all fits.
    std::vector<std::pair<DescKey, uint32_t>> all;
    all.reserve(count_ + 1);
    for (uint32_t s = 0; s <= mask_; s++)
        if (used_[s]) {
            DescKey k;
            const uint32_t* e = &words_[1 + size_t(s) * kEntryWords];
            std::copy(e, e + 8, k.begin());
            all.emplace_back(k, e[8]);
        }
    all.emplace_back(key, index);
    uint32_t slots = (mask_ + 1) * 2;
    for (;;) {
        reset(slots);
        bool ok = true;
        for (auto& [k, i] : all)
            if (!try_insert(k, i)) { ok = false; break; }
        if (ok) break;
        slots *= 2;
    }
    generation_++;
}

bool DescriptorHashTable::remove(const DescKey& key) {
    int s = find_slot(key);
    if (s < 0) return false;
    used_[s] = 0;
    count_--;
    write_slot(uint32_t(s), DescKey{}, 0);
    return true;
}

uint32_t DescriptorHashTable::lookup(const DescKey& key) const {
    // Mirrors the generated SPIR-V: every probe compares the stored words, used or not.
    uint32_t mask = words_[0], h = hash(key);
    for (uint32_t k = 0; k < kProbes; k++) {
        const uint32_t* e = &words_[1 + size_t((h + k) & mask) * kEntryWords];
        if (std::equal(key.begin(), key.end(), e)) return e[8];
    }
    return 0;
}

}  // namespace rt::gpu
