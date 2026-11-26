// BCPack decoder: BC1, BC3, BC4, BC5, BC6H and BC7. Tables in bcpack_tables.h (tools/gen_bcpack_tables.py).
//
// A packed stream has no header. It is 12 (BC1-BC5) or 16 (BC6H, BC7) interleaved rANS lanes of `size` / 8 symbols
// each. Each lane is a run of fragments: a header byte (bits 0-3 the model q, bit 4 a flush fragment, bit 5 a third
// header byte, bits 6-7 subtracted from the lane's symbol width), a payload length (one byte + 1, or two bytes + 257)
// and the payload. Which lane takes the next input byte is fixed by a cycle schedule (at most 8 bytes a cycle, lanes
// in order), so the lanes are run together, cycle by cycle, as the hardware does; the schedule here is the one
// rpf9-explorer's decoder models.
//
// The lanes' symbols make one record per block (BC1, BC4: one symbol per lane; the 16-byte formats two, so record
// symbol 2 j + n is lane j's n-th): two back references, one for the endpoints and one for the indices (to one of
// the last 256 blocks, or to a dictionary entry for "blocks" before the first), and residuals of the endpoints and
// the indices against the referenced blocks.
#include "bcpack.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

#include "bcpack_tables.h"

namespace rt::bcpack {
namespace {

namespace t = tables;

constexpr uint32_t kRansL = 1u << 24;
constexpr int kMaxWidth = 8;
constexpr int kPrefetchBudget = 8;
constexpr int kMaxVirtualZeros = 8;
constexpr int kMaxLanes = 16;
constexpr int kHistory = 256;
constexpr size_t kMaxOutput = 256u << 20;

struct Spec {
    uint32_t block_bytes;
    int lanes;
    uint8_t widths[kMaxLanes];  // symbol bits per lane
};

constexpr Spec kSpecs[8] = {
    {},
    {8, 12, {8, 8, 5, 6, 5, 5, 6, 5, 8, 8, 8, 8}},                    // BC1
    {},                                                               // BC2: no title data uses it
    {16, 12, {4, 4, 8, 6, 6, 6, 6, 5, 6, 5, 8, 8}},                   // BC3
    {8, 12, {8, 8, 8, 8, 6, 6, 6, 6, 6, 6, 6, 6}},                    // BC4
    {16, 12, {4, 4, 8, 8, 6, 6, 6, 6, 6, 6, 6, 6}},                   // BC5
    {16, 16, {4, 4, 8, 8, 8, 5, 5, 5, 6, 6, 6, 8, 8, 8, 8, 5}},       // BC6H
    {16, 16, {4, 4, 8, 8, 8, 8, 8, 7, 7, 5, 6, 8, 8, 8, 8, 5}},       // BC7
};

// ---- lanes

// One rANS model: a symbol of `width` bits falls in band p (0 for symbol 0, else its bit length), and each symbol
// of a band has the band's frequency.
struct Model {
    int width = 0;
    uint32_t freq[kMaxWidth + 1] = {};
    uint32_t base[kMaxWidth + 2] = {};
    std::vector<uint16_t> sym_freq, sym_base;

    bool build(int w, const uint16_t* bands) {
        width = w;
        uint32_t acc = 0;
        for (int p = 0; p <= w; ++p) {
            if (!bands[p]) return false;
            freq[p] = bands[p];
            base[p] = acc;
            acc += bands[p] * (p == 0 ? 1u : 1u << (p - 1));
        }
        base[w + 1] = acc;
        if (acc != 65536) return false;
        const int count = 1 << w;
        sym_freq.resize(count);
        sym_base.resize(count);
        for (int s = 0; s < count; ++s) {
            const int p = s == 0 ? 0 : static_cast<int>(std::bit_width(static_cast<unsigned>(s)));
            uint32_t f = freq[p];
            uint32_t c = base[p] + f * static_cast<uint32_t>(s - (p == 0 ? 0 : 1 << (p - 1)));
            if (c == 0) {  // symbol 0 gives up slot 0: a cumulative frequency of 0 never occurs
                f -= 1;
                c = 1;
            }
            sym_freq[s] = static_cast<uint16_t>(f);
            sym_base[s] = static_cast<uint16_t>(c);
        }
        return true;
    }

    int lookup(uint32_t cf) const {
        if (cf < base[1]) return 0;
        int p = 1;
        while (p < width && cf >= base[p + 1]) ++p;
        return (1 << (p - 1)) + static_cast<int>((cf - base[p]) / freq[p]);
    }

    uint64_t advance(int s, uint64_t x) const {
        return static_cast<uint64_t>(sym_freq[s]) * (x >> 16) + (x & 0xFFFF) - sym_base[s];
    }
};

// Built at startup: FlamingoPort.exe has no thread-safe statics, and the DirectStorage queues decode on threads of
// their own.
struct Models {
    Model m[kMaxWidth][16];  // [symbol bits - 1][q] = table model 16 (bits - 1) + q
    bool ok = true;
    Models() {
        for (int w = 1; w <= kMaxWidth; ++w)
            for (int q = 0; q < 16; ++q) ok &= m[w - 1][q].build(w, t::kModels[(w - 1) * 16 + q]);
    }
};
const Models g_models;

// Runs the lanes together, cycle by cycle, into `records` (`per` symbols per lane per block). False if the stream
// doesn't decode.
bool decode_lanes(const uint8_t* raw, size_t length, const Spec& spec, size_t blocks, int per, uint8_t* records) {
    enum : uint8_t { kHeader0, kHeader1, kHeader2, kPayload };
    const int lanes = spec.lanes;
    const size_t per_lane = blocks * per, stride = static_cast<size_t>(lanes) * per;
    uint8_t control[kMaxLanes] = {}, three_byte[kMaxLanes] = {}, flush[kMaxLanes] = {}, zeros[kMaxLanes] = {},
            input_byte[kMaxLanes] = {}, slot[kMaxLanes] = {};
    size_t remaining[kMaxLanes] = {}, out_count[kMaxLanes] = {};
    int width[kMaxLanes] = {}, q[kMaxLanes] = {}, symbol[kMaxLanes] = {}, payload[kMaxLanes] = {};
    bool done[kMaxLanes] = {}, input_full[kMaxLanes] = {}, held[kMaxLanes] = {}, slot_full[kMaxLanes] = {},
         stalled[kMaxLanes] = {};
    uint64_t before_merge[kMaxLanes] = {}, after_step[kMaxLanes] = {};
    const Model* model[kMaxLanes] = {};
    for (int i = 0; i < lanes; ++i) {
        remaining[i] = per_lane;
        done[i] = per_lane == 0;
    }
    // Lane i's n-th symbol goes to block n / per, record symbol per * i + n % per: `at` follows it.
    size_t at[kMaxLanes];
    for (int i = 0; i < lanes; ++i) at[i] = static_cast<size_t>(per) * i;
    const size_t skip = stride - per;
    auto store = [&](int i, uint8_t v) {
        records[at[i]++] = v;
        if (++out_count[i] % per == 0) at[i] += skip;
    };
    size_t pos = 0;

    for (;;) {
        bool all_done = true;
        for (int i = 0; i < lanes && all_done; ++i) all_done = done[i] && !slot_full[i];
        if (all_done) break;

        // Fetch: each lane that can take a byte takes the next one, at most 8 a cycle, in lane order.
        int budget = kPrefetchBudget;
        for (int i = 0; i < lanes; ++i) {
            if (done[i] || (held[i] && slot_full[i])) {
                stalled[i] = true;
                continue;
            }
            stalled[i] = false;
            if (control[i] == kPayload)
                before_merge[i] = held[i] ? model[i]->advance(symbol[i], after_step[i]) : after_step[i];
            if (!input_full[i] && (control[i] != kPayload || payload[i] > 0)) {
                if (budget == 0) {
                    stalled[i] = true;
                    continue;
                }
                if (pos >= length) return false;  // input ran out
                input_byte[i] = raw[pos++];
                input_full[i] = true;
                --budget;
            }
            if (held[i]) {
                slot_full[i] = true;
                slot[i] = static_cast<uint8_t>(symbol[i]);
                held[i] = false;
            }
        }

        // Step each lane's state machine.
        for (int i = 0; i < lanes; ++i) {
            if (done[i] || stalled[i]) continue;
            if (control[i] == kHeader0) {
                if (!input_full[i]) continue;
                const uint8_t h = input_byte[i];
                q[i] = h & 15;
                flush[i] = (h >> 4) & 1;
                three_byte[i] = (h >> 5) & 1;
                width[i] = spec.widths[i] - (h >> 6);
                input_full[i] = false;
                control[i] = kHeader1;
                zeros[i] = 0;
            } else if (control[i] == kHeader1) {
                if (!input_full[i]) continue;
                input_full[i] = false;
                if (width[i] < 1 || width[i] > kMaxWidth) return false;
                model[i] = &g_models.m[width[i] - 1][q[i]];
                if (three_byte[i]) {
                    payload[i] = input_byte[i];
                    control[i] = kHeader2;
                } else {
                    payload[i] = input_byte[i] + 1;
                    control[i] = kPayload;
                }
            } else if (control[i] == kHeader2) {
                if (!input_full[i]) continue;
                payload[i] = ((input_byte[i] << 8) | (payload[i] & 0xFF)) + 257;
                control[i] = kPayload;
                input_full[i] = false;
            } else {
                uint64_t state = before_merge[i];
                if (state < kRansL) {
                    if (payload[i] > 0) {
                        if (input_full[i]) {
                            state = (state << 8) | input_byte[i];
                            --payload[i];
                            input_full[i] = false;
                            if (payload[i] == 0 && flush[i])
                                while (state < kRansL && state != 0 && zeros[i] < kMaxVirtualZeros) {
                                    state <<= 8;
                                    ++zeros[i];
                                }
                        }
                    } else if (flush[i]) {
                        if (state == 0 || zeros[i] >= kMaxVirtualZeros) return false;  // not at its home state
                        while (state < kRansL && state != 0 && zeros[i] < kMaxVirtualZeros) {
                            state <<= 8;
                            ++zeros[i];
                        }
                    } else {
                        after_step[i] = state;
                        if (remaining[i] > 0)
                            control[i] = kHeader0;
                        else
                            done[i] = true;
                        continue;
                    }
                }

                const bool next = state > kRansL && remaining[i] > 0;
                if (next) {
                    const int s = model[i]->lookup(static_cast<uint32_t>(state & 0xFFFF));
                    symbol[i] = s;
                    --remaining[i];
                    if (s >> width[i]) return false;  // a symbol outside its width
                }
                held[i] = next;
                after_step[i] = state;

                if (payload[i] == 0) {
                    if (flush[i]) {
                        if (state == kRansL) {
                            if (remaining[i] > 0) {
                                control[i] = kHeader0;
                                before_merge[i] = 0;
                                after_step[i] = 0;
                            } else {
                                done[i] = true;
                            }
                        } else if (state > kRansL && !next) {
                            return false;
                        }
                    } else if (remaining[i] == 0 && !next) {
                        done[i] = true;
                    } else if (state == kRansL) {
                        return false;
                    }
                } else if (state == kRansL || (remaining[i] == 0 && !next && state > kRansL)) {
                    return false;
                }
            }
        }

        // Retire: the held symbols leave together, once every lane that isn't finished has one.
        bool ready = true, any = false;
        for (int i = 0; i < lanes; ++i) {
            if (slot_full[i])
                any = true;
            else if (!(done[i] && !held[i])) {
                ready = false;
                break;
            }
        }
        if (ready && any)
            for (int i = 0; i < lanes; ++i)
                if (slot_full[i]) {
                    if (out_count[i] >= per_lane) return false;  // more symbols than expected
                    store(i, slot[i]);
                    slot_full[i] = false;
                }
        for (int i = 0; i < lanes; ++i)
            if (done[i] && held[i] && !slot_full[i]) {
                slot_full[i] = true;
                slot[i] = static_cast<uint8_t>(symbol[i]);
                held[i] = false;
            }
    }

    for (int i = 0; i < lanes; ++i)
        if (out_count[i] != per_lane) return false;
    // Only zero padding may follow the last fragment.
    for (size_t k = pos; k < length; ++k)
        if (raw[k]) return false;
    return true;
}

// ---- records: references and residuals

// Two 4-bit symbols as one byte: `a` on the even bits, `b` on the odd.
inline uint8_t interleave4(uint8_t a, uint8_t b) {
    uint8_t v = 0;
    for (int k = 0; k < 4; ++k) v |= static_cast<uint8_t>(((a >> k) & 1) << (2 * k) | ((b >> k) & 1) << (2 * k + 1));
    return v;
}

// The record's endpoint and index references: a reference r is 0 for none, else r blocks back. The index one is
// coded directly, the endpoint one as a zigzag delta from it.
struct Refs {
    int endpoints, indices;
};
inline Refs references(const uint8_t* s, bool paired) {
    const uint8_t d = paired ? interleave4(s[0], s[1]) : s[0];
    const uint8_t ix = paired ? interleave4(s[2], s[3]) : s[1];
    const uint8_t delta = static_cast<uint8_t>((d >> 1) ^ (0u - (d & 1u)));
    return {static_cast<uint8_t>(ix + delta), ix};
}

// Block i's reference r: none (`zero`), a block of the history, or a dictionary entry before the first block.
template <typename T>
const T& pick(size_t i, int r, const T* history, const T* dictionary, const T& zero) {
    if (r == 0) return zero;
    if (static_cast<size_t>(r) <= i) return history[(i - r) % kHistory];
    return dictionary[static_cast<size_t>(r) - i - 1];
}

// Zigzag residual (0, -1, 1, -2, ...) of `bits` bits.
inline uint8_t zigzag(uint8_t v, int bits) {
    const uint8_t mask = static_cast<uint8_t>((1u << bits) - 1);
    return static_cast<uint8_t>(((v >> 1) ^ ((v & 1) ? mask : 0)) & mask);
}

// Zigzag of a whole 16-bit value (or a byte widened to 16 bits).
inline uint16_t zigzag16(uint16_t v) { return static_cast<uint16_t>((v >> 1) ^ (0u - (v & 1u))); }

// A byte's even and odd bits, each a zigzag-coded 4-bit index residual (sign-extended into 16 bits).
inline void nibbles(uint8_t b, bool odd_first, uint16_t out[2]) {
    uint8_t even = 0, odd = 0;
    for (int k = 0; k < 4; ++k) {
        even |= static_cast<uint8_t>(((b >> (2 * k)) & 1) << k);
        odd |= static_cast<uint8_t>(((b >> (2 * k + 1)) & 1) << k);
    }
    const uint16_t e = zigzag16(even), o = zigzag16(odd);
    out[0] = odd_first ? o : e;
    out[1] = odd_first ? e : o;
}

// An index residual of `bits` bits plus its reference of `ref_bits` bits: both left-aligned to 16 bits, summed,
// back to `bits` bits. Indices are kept modulo 2^bits (what the packed block takes).
inline uint16_t add_index(uint16_t residual, int bits, uint16_t ref, int ref_bits) {
    if (bits == 0) return 0;
    auto up = [](uint16_t v, int b) { return b == 0 || b > 16 ? 0u : static_cast<uint16_t>(v << (16 - b)); };
    return static_cast<uint16_t>(static_cast<uint16_t>(up(residual, bits) + up(ref, ref_bits)) >> (16 - bits));
}

// Little-endian bit writer for one 128-bit block.
struct Bits {
    uint64_t v[2] = {};
    int at = 0;
    void put(uint64_t value, int n) {  // n <= 32
        if (n <= 0) return;
        if (at < 128) {
            value &= (1ull << n) - 1;
            const int word = at >> 6, bit = at & 63;
            v[word] |= value << bit;
            if (bit + n > 64 && word == 0) v[1] |= value >> (64 - bit);
        }
        at += n;
    }
    void store(uint8_t* out) const { std::memcpy(out, v, 16); }
};

// ---- BC1, BC3, BC4, BC5

struct PlainFormat {
    uint8_t ep_bits[8];
    int ix3, ix2;  // the first ix3 indices have 3 bits, the next ix2 two
    bool paired;
    const t::Plain* dictionary;
};
constexpr PlainFormat kBc1 = {{5, 6, 5, 5, 6, 5, 0, 0}, 0, 16, false, t::kDictBc1};
constexpr PlainFormat kBc3 = {{5, 6, 5, 5, 6, 5, 8, 8}, 16, 16, true, t::kDictBc3};
constexpr PlainFormat kBc4 = {{8, 8, 0, 0, 0, 0, 0, 0}, 16, 0, false, t::kDictBc4};
constexpr PlainFormat kBc5 = {{8, 8, 8, 8, 0, 0, 0, 0}, 32, 0, true, t::kDictBc5};

// 2-bit index residuals: one symbol per 2x2 quad of the block.
void quads(const uint8_t* rank, const uint8_t* s, uint8_t* ix) {
    for (int q = 0; q < 4; ++q) {
        const uint8_t v = rank[s[q]];
        const int base = (q >> 1) * 8 + (q & 1) * 2;
        static constexpr int kAt[4] = {0, 1, 4, 5};
        for (int k = 0; k < 4; ++k) ix[base + kAt[k]] = (v >> (2 * k)) & 3;
    }
}

// 3-bit index residuals: one symbol per two pixels, raster order.
void pairs(const uint8_t* rank, const uint8_t* s, int count, uint8_t* ix) {
    for (int k = 0; k < count; ++k) {
        const uint8_t v = rank[s[k] & 63];
        ix[2 * k] = v & 7;
        ix[2 * k + 1] = (v >> 4) & 7;
    }
}

t::Plain residual(int format, const uint8_t* s) {
    t::Plain v{};
    switch (format) {
        case 1:
            for (int k = 0; k < 6; ++k) v.ep[k] = zigzag(s[2 + k], kBc1.ep_bits[k]);
            quads(t::kRank1, s + 8, v.ix);
            break;
        case 3: {
            // The colour lanes carry each channel's two endpoints together.
            static constexpr int kFrom[8] = {14, 16, 18, 15, 17, 19, 4, 5};
            for (int k = 0; k < 8; ++k) v.ep[k] = zigzag(s[kFrom[k]], kBc3.ep_bits[k]);
            pairs(t::kRank3Alpha, s + 6, 8, v.ix);
            quads(t::kRank3Colour, s + 20, v.ix + 16);
            break;
        }
        case 4:
            v.ep[0] = zigzag(s[2], 8);
            v.ep[1] = zigzag(s[3], 8);
            pairs(t::kRank4, s + 4, 8, v.ix);
            break;
        default:  // 5
            for (int k = 0; k < 4; ++k) v.ep[k] = zigzag(s[4 + k], 8);
            pairs(t::kRank5, s + 8, 16, v.ix);
            break;
    }
    return v;
}

// Index along the endpoint line -> the BC1 / BC4 code.
constexpr uint8_t kBc1Code[4] = {0, 2, 3, 1};
constexpr uint8_t kBc4Code[8] = {0, 2, 3, 4, 5, 6, 7, 1};

void bc1_colour(const uint8_t* ep, const uint8_t* ix, uint8_t* out) {
    const uint16_t c0 = static_cast<uint16_t>(ep[0] << 11 | ep[1] << 5 | ep[2]);
    const uint16_t c1 = static_cast<uint16_t>(ep[3] << 11 | ep[4] << 5 | ep[5]);
    uint32_t bits = 0;
    for (int p = 0; p < 16; ++p) bits |= static_cast<uint32_t>(kBc1Code[ix[p] & 3]) << (2 * p);
    std::memcpy(out, &c0, 2);
    std::memcpy(out + 2, &c1, 2);
    std::memcpy(out + 4, &bits, 4);
}

// A BC4 block; its endpoints are kept 128 below their stored value.
void bc4_block(uint8_t e0, uint8_t e1, const uint8_t* ix, uint8_t* out) {
    uint64_t v = static_cast<uint64_t>(e0 ^ 0x80) | static_cast<uint64_t>(e1 ^ 0x80) << 8;
    for (int p = 0; p < 16; ++p) v |= static_cast<uint64_t>(kBc4Code[ix[p] & 7]) << (16 + 3 * p);
    std::memcpy(out, &v, 8);
}

void pack_plain(int format, const t::Plain& v, uint8_t* out) {
    switch (format) {
        case 1: bc1_colour(v.ep, v.ix, out); break;
        case 3:
            bc4_block(v.ep[6], v.ep[7], v.ix, out);
            bc1_colour(v.ep, v.ix + 16, out + 8);
            break;
        case 4: bc4_block(v.ep[0], v.ep[1], v.ix, out); break;
        default:
            bc4_block(v.ep[0], v.ep[1], v.ix, out);
            bc4_block(v.ep[2], v.ep[3], v.ix + 16, out + 8);
            break;
    }
}

void rebuild_plain(int format, const PlainFormat& f, const uint8_t* records, size_t stride, size_t blocks,
                   uint32_t block_bytes, uint8_t* out) {
    std::vector<t::Plain> history(kHistory);
    const t::Plain zero{};
    uint8_t ix_bits[32];
    for (int k = 0; k < 32; ++k) ix_bits[k] = k < f.ix3 ? 3 : k < f.ix3 + f.ix2 ? 2 : 0;
    for (size_t i = 0; i < blocks; ++i) {
        const uint8_t* s = records + i * stride;
        const Refs r = references(s, f.paired);
        const t::Plain& ep_ref = pick(i, r.endpoints, history.data(), f.dictionary, zero);
        const t::Plain& ix_ref = pick(i, r.indices, history.data(), f.dictionary, zero);
        t::Plain v = residual(format, s);
        for (int k = 0; k < 8; ++k)
            v.ep[k] = static_cast<uint8_t>((v.ep[k] + ep_ref.ep[k]) & ((1u << f.ep_bits[k]) - 1));
        for (int k = 0; k < 32; ++k)
            v.ix[k] = static_cast<uint8_t>((v.ix[k] + ix_ref.ix[k]) & ((1u << ix_bits[k]) - 1));
        history[i % kHistory] = v;
        pack_plain(format, v, out + i * block_bytes);
    }
}

// ---- BC7

// Anchor pixels of the second subset of two, and of the second and third of three (the BC7 specification's).
constexpr uint8_t kAnchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                                  15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
                                  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,
                                  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};
constexpr uint8_t kAnchor3a[64] = {3, 3,  15, 15, 8,  3,  15, 15, 8,  8,  6,  6,  6, 5,  3,  3,
                                   3, 3,  8,  15, 3,  3,  6,  10, 5,  8,  8,  6,  8, 5,  15, 15,
                                   8, 15, 3,  5,  6,  10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15,
                                   3, 15, 5,  5,  5,  8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3};
constexpr uint8_t kAnchor3b[64] = {15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,
                                   15, 8,  15, 3,  15, 8,  15, 8,  3,  15, 6,  10, 15, 15, 10, 8,
                                   15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,
                                   15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};

// Endpoint bits of R G B A, and index bits of the two index sets, per mode (BC7 specification).
constexpr uint8_t kBc7Endpoint[8][4] = {{4, 4, 4, 0}, {6, 6, 6, 0}, {5, 5, 5, 0}, {7, 7, 7, 0},
                                        {5, 5, 5, 6}, {7, 7, 7, 8}, {7, 7, 7, 7}, {5, 5, 5, 5}};
constexpr uint8_t kBc7Index[8][2] = {{3, 0}, {3, 0}, {2, 0}, {2, 0}, {3, 2}, {2, 2}, {4, 0}, {2, 0}};

inline bool has_pbits(int mode) { return mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7; }

// Channel precisions as the endpoint rows keep them: the p-bit is red's lowest bit, and a rotation swaps alpha with
// the rotated channel.
void bc7_precision(uint16_t control, uint8_t p[4]) {
    const int mode = control & 7, rotation = (control >> 3) & 3;
    std::memcpy(p, kBc7Endpoint[mode], 4);
    if (rotation) {
        const uint8_t a = p[3];
        p[3] = p[rotation - 1];
        p[rotation - 1] = a;
    } else if (has_pbits(mode)) {
        p[0] += 1;
    }
}

void bc7_pack(const t::Bc7& b, const uint8_t prec[4], uint8_t* out) {
    const int mode = b.control & 7, subsets = (b.control & 0xFF) >> 6, partition = (b.control >> 8) & 63;
    const int rotation = (b.control >> 3) & 3;
    Bits w;
    w.put(1ull << mode, mode + 1);
    if (mode == 0)
        w.put(partition, 4);
    else if (mode == 1 || mode == 2 || mode == 3 || mode == 7)
        w.put(partition, 6);
    else if (mode == 4) {
        w.put(rotation, 2);
        w.put((b.control >> 5) & 1, 1);
    } else if (mode == 5)
        w.put(rotation, 2);

    const int ends = 2 * subsets;
    auto field = [&](int row, int column, int bits) -> uint64_t { return b.rows[row][column] >> (16 - bits); };
    int order[4] = {0, 1, 2, 3};
    if (rotation) {
        order[3] = rotation - 1;
        order[rotation - 1] = 3;
    }
    for (int ch : order) {
        const int p = prec[ch];
        if (!p) continue;
        const int drop = ch == 0 && has_pbits(mode) ? 1 : 0;
        for (int e = 0; e < ends; ++e) w.put(field(e / 2, 2 * ch + e % 2, p) >> drop, p - drop);
    }
    if (mode == 1)
        for (int row = 0; row < subsets; ++row) w.put(field(row, 0, prec[0]) & 1, 1);
    else if (has_pbits(mode))
        for (int e = 0; e < ends; ++e) w.put(field(e / 2, e % 2, prec[0]) & 1, 1);

    int anchors[3] = {0, -1, -1};
    if (subsets == 2) anchors[1] = kAnchor2[partition];
    if (subsets == 3) anchors[1] = kAnchor3a[partition], anchors[2] = kAnchor3b[partition];
    auto indices = [&](int set) {
        for (int p = 0; p < 16; ++p) {
            const bool anchor = p == anchors[0] || p == anchors[1] || p == anchors[2];
            w.put(b.ix[set][p], b.bits[set] - (anchor ? 1 : 0));
        }
    };
    if (mode == 4) {
        indices(1);
        indices(0);
    } else if (mode == 5) {
        indices(0);
        indices(1);
    } else {
        indices(0);
    }
    w.store(out);
}

bool rebuild_bc7(const uint8_t* records, size_t stride, size_t blocks, uint8_t* out) {
    std::vector<t::Bc7> history(kHistory);
    const t::Bc7 zero{};
    for (size_t i = 0; i < blocks; ++i) {
        const uint8_t* s = records + i * stride;
        const Refs refs = references(s, true);
        const uint16_t control = t::kControl7[(s[31] & 15) << 5 | (s[30] & 31)];
        if (control == 0) return false;  // an unused control code
        const int mode = control & 7, subsets = (control & 0xFF) >> 6;
        uint8_t prec[4];
        bc7_precision(control, prec);
        const bool three = subsets == 3;
        // Residuals per subset row, R0 R1 G0 G1 B0 B1 A0 A1; with three subsets the third row's G and B ride in the
        // alpha columns of the first two (no mode of three subsets has alpha).
        const uint8_t stored[3][8] = {
            {s[4], s[5], s[6], s[7], s[8], s[9], three ? uint8_t(0) : s[10], three ? uint8_t(0) : s[11]},
            {s[12], s[13], s[14], s[15], s[16], s[17], three ? uint8_t(0) : s[18], three ? uint8_t(0) : s[19]},
            {s[20], s[21], three ? s[10] : uint8_t(0), three ? s[11] : uint8_t(0), three ? s[18] : uint8_t(0),
             three ? s[19] : uint8_t(0), 0, 0},
        };
        const t::Bc7& r = pick(i, refs.endpoints, history.data(), t::kDictBc7, zero);
        t::Bc7 b{};
        b.control = control;
        // Rows the reference block lacks build on its first row.
        const int common = std::min(subsets, (r.control & 0xFF) >> 6);
        const int columns = prec[3] ? 8 : 6;
        for (int c = 0; c < columns; ++c) {
            const int p = prec[c / 2];
            if (!p) continue;
            const uint16_t mask = static_cast<uint16_t>(0xFFFFu << (16 - p));
            for (int row = 0; row < subsets; ++row) {
                const uint16_t res = static_cast<uint16_t>(zigzag16(stored[row][c]) << (16 - p));
                const uint16_t base = r.rows[row < common ? row : 0][c] & mask;
                b.rows[row][c] = static_cast<uint16_t>(base + res);
            }
        }
        // Indices: set 0 from record symbols 22-29, set 1 (modes 4 and 5) from 12-17 and 20-21; each symbol pair
        // holds four residuals.
        const t::Bc7& rx = pick(i, refs.indices, history.data(), t::kDictBc7, zero);
        b.bits[0] = kBc7Index[mode][0];
        b.bits[1] = kBc7Index[mode][1];
        static constexpr int kFrom[2][4] = {{22, 24, 26, 28}, {12, 14, 16, 20}};
        for (int set = 0; set < 2; ++set) {
            if (!b.bits[set]) continue;
            for (int k = 0; k < 4; ++k)
                for (int half = 0; half < 2; ++half) {
                    uint16_t pair[2];
                    nibbles(s[kFrom[set][k] + half], k < 3, pair);
                    for (int h = 0; h < 2; ++h) {
                        const int p = 4 * k + 2 * half + h;
                        b.ix[set][p] = add_index(pair[h], b.bits[set], rx.ix[set][p], rx.bits[set]);
                    }
                }
        }
        history[i % kHistory] = b;
        bc7_pack(b, prec, out + i * 16);
    }
    return true;
}

// ---- BC6H

// Bits of endpoint 0, then of the other endpoints' R, G, B, per mode 1-14 (mode = the BC6H specification's 1-14).
constexpr uint8_t kBc6Bits[15][4] = {{0, 0, 0, 0},    {10, 5, 5, 5},  {7, 6, 6, 6},   {11, 5, 4, 4},
                                     {11, 4, 5, 4},   {11, 4, 4, 5},  {9, 5, 5, 5},   {8, 6, 5, 5},
                                     {8, 5, 6, 5},    {8, 5, 5, 6},   {6, 6, 6, 6},   {10, 10, 10, 10},
                                     {11, 9, 9, 9},   {12, 8, 8, 8},  {16, 4, 4, 4}};
// Mode code and its width, modes 1-14.
constexpr uint8_t kBc6Code[15][2] = {{0, 0},  {0, 2},  {1, 2},  {2, 5},  {6, 5},  {10, 5}, {14, 5}, {18, 5},
                                     {22, 5}, {26, 5}, {30, 5}, {3, 5},  {7, 5},  {11, 5}, {15, 5}};

// The block header after the mode bits (BC6H specification): runs of `len` bits of channel `ch` (0-2 R G B; 3 = the
// partition, 5 bits) of endpoint `ep`, from bit `first` up. Modes 13 and 14 write their high base bits ascending.
struct Run {
    uint8_t ch, ep, first, len;
};
constexpr uint8_t R = 0, G = 1, B = 2;
constexpr Run D = {3, 0, 0, 5};
constexpr Run kL1[] = {{G, 2, 4, 1}, {B, 2, 4, 1}, {B, 3, 4, 1}, {R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10},
                       {R, 1, 0, 5}, {G, 3, 4, 1}, {G, 2, 0, 4}, {G, 1, 0, 5},  {B, 3, 0, 1},  {G, 3, 0, 4},
                       {B, 1, 0, 5}, {B, 3, 1, 1}, {B, 2, 0, 4}, {R, 2, 0, 5},  {B, 3, 2, 1},  {R, 3, 0, 5},
                       {B, 3, 3, 1}, D};
constexpr Run kL2[] = {{G, 2, 5, 1}, {G, 3, 4, 1}, {G, 3, 5, 1}, {R, 0, 0, 7}, {B, 3, 0, 1}, {B, 3, 1, 1},
                       {B, 2, 4, 1}, {G, 0, 0, 7}, {B, 2, 5, 1}, {B, 3, 2, 1}, {G, 2, 4, 1}, {B, 0, 0, 7},
                       {B, 3, 3, 1}, {B, 3, 5, 1}, {B, 3, 4, 1}, {R, 1, 0, 6}, {G, 2, 0, 4}, {G, 1, 0, 6},
                       {G, 3, 0, 4}, {B, 1, 0, 6}, {B, 2, 0, 4}, {R, 2, 0, 6}, {R, 3, 0, 6}, D};
constexpr Run kL3[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 5}, {R, 0, 10, 1}, {G, 2, 0, 4},
                       {G, 1, 0, 4},  {G, 0, 10, 1}, {B, 3, 0, 1},  {G, 3, 0, 4}, {B, 1, 0, 4},  {B, 0, 10, 1},
                       {B, 3, 1, 1},  {B, 2, 0, 4},  {R, 2, 0, 5},  {B, 3, 2, 1}, {R, 3, 0, 5},  {B, 3, 3, 1}, D};
constexpr Run kL4[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 4}, {R, 0, 10, 1}, {G, 3, 4, 1},
                       {G, 2, 0, 4},  {G, 1, 0, 5},  {G, 0, 10, 1}, {G, 3, 0, 4}, {B, 1, 0, 4},  {B, 0, 10, 1},
                       {B, 3, 1, 1},  {B, 2, 0, 4},  {R, 2, 0, 4},  {B, 3, 0, 1}, {B, 3, 2, 1},  {R, 3, 0, 4},
                       {G, 2, 4, 1},  {B, 3, 3, 1},  D};
constexpr Run kL5[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 4}, {R, 0, 10, 1}, {B, 2, 4, 1},
                       {G, 2, 0, 4},  {G, 1, 0, 4},  {G, 0, 10, 1}, {B, 3, 0, 1}, {G, 3, 0, 4},  {B, 1, 0, 5},
                       {B, 0, 10, 1}, {B, 2, 0, 4},  {R, 2, 0, 4},  {B, 3, 1, 1}, {B, 3, 2, 1},  {R, 3, 0, 4},
                       {B, 3, 4, 1},  {B, 3, 3, 1},  D};
constexpr Run kL6[] = {{R, 0, 0, 9}, {B, 2, 4, 1}, {G, 0, 0, 9}, {G, 2, 4, 1}, {B, 0, 0, 9}, {B, 3, 4, 1},
                       {R, 1, 0, 5}, {G, 3, 4, 1}, {G, 2, 0, 4}, {G, 1, 0, 5}, {B, 3, 0, 1}, {G, 3, 0, 4},
                       {B, 1, 0, 5}, {B, 3, 1, 1}, {B, 2, 0, 4}, {R, 2, 0, 5}, {B, 3, 2, 1}, {R, 3, 0, 5},
                       {B, 3, 3, 1}, D};
constexpr Run kL7[] = {{R, 0, 0, 8}, {G, 3, 4, 1}, {B, 2, 4, 1}, {G, 0, 0, 8}, {B, 3, 2, 1}, {G, 2, 4, 1},
                       {B, 0, 0, 8}, {B, 3, 3, 1}, {B, 3, 4, 1}, {R, 1, 0, 6}, {G, 2, 0, 4}, {G, 1, 0, 5},
                       {B, 3, 0, 1}, {G, 3, 0, 4}, {B, 1, 0, 5}, {B, 3, 1, 1}, {B, 2, 0, 4}, {R, 2, 0, 6},
                       {R, 3, 0, 6}, D};
constexpr Run kL8[] = {{R, 0, 0, 8}, {B, 3, 0, 1}, {B, 2, 4, 1}, {G, 0, 0, 8}, {G, 2, 5, 1}, {G, 2, 4, 1},
                       {B, 0, 0, 8}, {G, 3, 5, 1}, {B, 3, 4, 1}, {R, 1, 0, 5}, {G, 3, 4, 1}, {G, 2, 0, 4},
                       {G, 1, 0, 6}, {G, 3, 0, 4}, {B, 1, 0, 5}, {B, 3, 1, 1}, {B, 2, 0, 4}, {R, 2, 0, 5},
                       {B, 3, 2, 1}, {R, 3, 0, 5}, {B, 3, 3, 1}, D};
constexpr Run kL9[] = {{R, 0, 0, 8}, {B, 3, 1, 1}, {B, 2, 4, 1}, {G, 0, 0, 8}, {B, 2, 5, 1}, {G, 2, 4, 1},
                       {B, 0, 0, 8}, {B, 3, 5, 1}, {B, 3, 4, 1}, {R, 1, 0, 5}, {G, 3, 4, 1}, {G, 2, 0, 4},
                       {G, 1, 0, 5}, {B, 3, 0, 1}, {G, 3, 0, 4}, {B, 1, 0, 6}, {B, 2, 0, 4}, {R, 2, 0, 5},
                       {B, 3, 2, 1}, {R, 3, 0, 5}, {B, 3, 3, 1}, D};
constexpr Run kL10[] = {{R, 0, 0, 6}, {G, 3, 4, 1}, {B, 3, 0, 1}, {B, 3, 1, 1}, {B, 2, 4, 1}, {G, 0, 0, 6},
                        {G, 2, 5, 1}, {B, 2, 5, 1}, {B, 3, 2, 1}, {G, 2, 4, 1}, {B, 0, 0, 6}, {G, 3, 5, 1},
                        {B, 3, 3, 1}, {B, 3, 5, 1}, {B, 3, 4, 1}, {R, 1, 0, 6}, {G, 2, 0, 4}, {G, 1, 0, 6},
                        {G, 3, 0, 4}, {B, 1, 0, 6}, {B, 2, 0, 4}, {R, 2, 0, 6}, {R, 3, 0, 6}, D};
constexpr Run kL11[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 10}, {G, 1, 0, 10}, {B, 1, 0, 10}};
constexpr Run kL12[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 9}, {R, 0, 10, 1},
                        {G, 1, 0, 9},  {G, 0, 10, 1}, {B, 1, 0, 9},  {B, 0, 10, 1}};
constexpr Run kL13[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 8}, {R, 0, 10, 2},
                        {G, 1, 0, 8},  {G, 0, 10, 2}, {B, 1, 0, 8},  {B, 0, 10, 2}};
constexpr Run kL14[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 4}, {R, 0, 10, 6},
                        {G, 1, 0, 4},  {G, 0, 10, 6}, {B, 1, 0, 4},  {B, 0, 10, 6}};

struct Layout {
    const Run* runs;
    int count;
};
template <size_t N>
constexpr Layout layout(const Run (&r)[N]) {
    return {r, static_cast<int>(N)};
}
constexpr Layout kBc6Layout[15] = {{nullptr, 0}, layout(kL1),  layout(kL2),  layout(kL3),  layout(kL4),
                                   layout(kL5),  layout(kL6),  layout(kL7),  layout(kL8),  layout(kL9),
                                   layout(kL10), layout(kL11), layout(kL12), layout(kL13), layout(kL14)};

inline int bc6_index_bits(int mode) { return mode >= 1 && mode <= 10 ? 3 : mode >= 11 && mode <= 14 ? 4 : 0; }

// Two 8-bit symbols as 16 bits: `a` on the even bits, `b` on the odd.
inline uint16_t interleave8(uint8_t a, uint8_t b) {
    uint16_t v = 0;
    for (int k = 0; k < 8; ++k) v |= static_cast<uint16_t>(((a >> k) & 1) << (2 * k) | ((b >> k) & 1) << (2 * k + 1));
    return v;
}

bool rebuild_bc6(const uint8_t* records, size_t stride, size_t blocks, uint8_t* out) {
    std::vector<t::Bc6> history(kHistory);
    t::Bc6 zero{};
    zero.mode = 11;
    for (size_t i = 0; i < blocks; ++i) {
        const uint8_t* s = records + i * stride;
        const Refs refs = references(s, true);
        const int mode = s[30] + 1;
        if (mode < 1 || mode > 14) return false;
        const bool two = mode < 11;
        int shift[4];
        for (int k = 0; k < 4; ++k) shift[k] = 16 - kBc6Bits[mode][k];
        uint16_t res[4][3];
        for (int c = 0; c < 3; ++c) {
            res[0][c] = zigzag16(interleave8(s[4 + 2 * c], s[5 + 2 * c]));
            res[1][c] = zigzag16(interleave8(s[10 + 2 * c], s[11 + 2 * c]));
            res[2][c] = two ? zigzag16(s[16 + 2 * c]) : res[0][c];
            res[3][c] = two ? zigzag16(s[17 + 2 * c]) : res[1][c];
        }
        const t::Bc6& r = pick(i, refs.endpoints, history.data(), t::kDictBc6, zero);
        t::Bc6 b{};
        b.mode = static_cast<uint8_t>(mode);
        // Endpoint 0 at the base precision, the others per channel; the reference is cut by the mask after the sum.
        auto mask = [](int sh) { return static_cast<uint16_t>(0xFFFFu << sh); };
        for (int c = 0; c < 3; ++c)
            b.e[0][c] = static_cast<uint16_t>((r.e[0][c] + (res[0][c] << shift[0])) & mask(shift[0]));
        for (int k = 1; k <= (two ? 3 : 1); ++k)
            for (int c = 0; c < 3; ++c)
                b.e[k][c] = static_cast<uint16_t>((r.e[k][c] + (res[k][c] << shift[1 + c])) & mask(shift[1 + c]));
        if (!two) {
            for (int c = 0; c < 3; ++c) b.e[2][c] = static_cast<uint16_t>((b.e[0][c] >> shift[0]) << shift[1]);
            std::memcpy(b.e[3], b.e[1], sizeof b.e[3]);
        }
        const t::Bc6& rx = pick(i, refs.indices, history.data(), t::kDictBc6, zero);
        const int bits = bc6_index_bits(mode);
        for (int k = 0; k < 8; ++k) {
            uint16_t pair[2];
            nibbles(s[22 + k], k < 6, pair);
            for (int h = 0; h < 2; ++h) {
                const int p = 2 * k + h;
                b.ix[p] = add_index(pair[h], bits, rx.ix[p], bc6_index_bits(rx.mode));
            }
        }
        history[i % kHistory] = b;

        // The block: the working values have their top bit flipped.
        auto field = [&](int k, int c) -> uint64_t {
            const int sh = k == 0 ? shift[0] : shift[1 + c];
            return static_cast<uint16_t>(b.e[k][c] ^ 0x8000) >> sh;
        };
        Bits w;
        w.put(kBc6Code[mode][0], kBc6Code[mode][1]);
        const int partition = s[31] & 31;
        const Layout& l = kBc6Layout[mode];
        for (int k = 0; k < l.count; ++k) {
            const Run& run = l.runs[k];
            if (run.ch == 3)
                w.put(partition, 5);
            else
                w.put(field(run.ep, run.ch) >> run.first, run.len);
        }
        const int anchor = two ? kAnchor2[partition] : 0;
        for (int p = 0; p < 16; ++p) {
            const bool short_index = p == 0 || p == anchor;
            w.put(static_cast<uint16_t>(b.ix[p] + (1u << (bits - 1))), bits - (short_index ? 1 : 0));
        }
        w.store(out + i * 16);
    }
    return true;
}

}  // namespace

uint32_t block_bytes(int format) { return format >= 0 && format < 8 ? kSpecs[format].block_bytes : 0; }

bool supported(int format) { return block_bytes(format) && g_models.ok; }

const char* name(int format) {
    static const char* const names[8] = {"none", "BC1", "BC2", "BC3", "BC4", "BC5", "BC6H", "BC7"};
    return format >= 0 && format < 8 ? names[format] : "?";
}

Result decode(const uint8_t* packed, size_t packed_size, int format, uint8_t* out, size_t size) {
    if (!supported(format)) return Result::unsupported;
    const Spec& spec = kSpecs[format];
    if (size == 0 || size > kMaxOutput || size % spec.block_bytes) return Result::unsupported;
    const size_t blocks = size / spec.block_bytes;
    const int per = static_cast<int>(spec.block_bytes / 8);
    const size_t stride = static_cast<size_t>(spec.lanes) * per;
    std::vector<uint8_t> records(blocks * stride);
    if (!decode_lanes(packed, packed_size, spec, blocks, per, records.data())) return Result::bad_stream;
    switch (format) {
        case 1: rebuild_plain(1, kBc1, records.data(), stride, blocks, 8, out); break;
        case 3: rebuild_plain(3, kBc3, records.data(), stride, blocks, 16, out); break;
        case 4: rebuild_plain(4, kBc4, records.data(), stride, blocks, 8, out); break;
        case 5: rebuild_plain(5, kBc5, records.data(), stride, blocks, 16, out); break;
        case 6:
            if (!rebuild_bc6(records.data(), stride, blocks, out)) return Result::bad_stream;
            break;
        default:
            if (!rebuild_bc7(records.data(), stride, blocks, out)) return Result::bad_stream;
            break;
    }
    return Result::ok;
}

}  // namespace rt::bcpack
