#include "inflate.h"

#include <cstring>

namespace rt::inflate {
namespace {

struct Bits {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buf = 0;
    int count = 0;
    int pad = 0;  // zero bytes read past the end
    bool over = false;

    void fill() {
        while (count <= 56) {
            uint64_t b = 0;
            if (p < end) b = *p++;
            else if (++pad > 16) over = true;
            buf |= b << count;
            count += 8;
        }
    }
    uint32_t get(int n) {
        if (count < n) fill();
        uint32_t v = static_cast<uint32_t>(buf & ((uint64_t{1} << n) - 1));
        buf >>= n;
        count -= n;
        return v;
    }
    void align() {
        int drop = count & 7;
        buf >>= drop;
        count -= drop;
    }
};

// A canonical Huffman code: counts per length and symbols by code order, decoded bit by bit with a fast first
// lookup of 9 bits.
struct Huffman {
    uint16_t count[16];
    uint16_t symbol[288];
    int16_t fast[512];  // symbol | length << 9, or -1
    bool build(const uint8_t* lengths, int n) {
        std::memset(count, 0, sizeof count);
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left = (left << 1) - count[len];
            if (left < 0) return false;
        }
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + count[len];
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
        // fast table: walk codes in canonical order
        for (int16_t& f : fast) f = -1;
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 9; ++len) {
            for (int k = 0; k < count[len]; ++k, ++code, ++index) {
                // code is MSB-first; the stream is LSB-first, so reverse it
                int rev = 0;
                for (int b = 0; b < len; ++b) rev |= ((code >> b) & 1) << (len - 1 - b);
                for (int fill = rev; fill < 512; fill += 1 << len)
                    fast[fill] = static_cast<int16_t>(symbol[index] | len << 9);
            }
            first = (first + count[len]) << 1;
            code <<= 1;
        }
        (void)first;
        return true;
    }
    int decode(Bits& b) const {
        if (b.count < 16) b.fill();
        int f = fast[b.buf & 511];
        if (f >= 0) {
            int len = f >> 9;
            b.buf >>= len;
            b.count -= len;
            return f & 511;
        }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(b.get(1));
            int n = count[len];
            if (code - n < first) return symbol[index + (code - first)];
            index += n;
            first += n;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

constexpr uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                   35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Bits& b, const Huffman& lit, const Huffman& dist, uint8_t* out, size_t size, size_t& at) {
    for (;;) {
        int s = lit.decode(b);
        if (s < 0 || b.over) return false;
        if (s < 256) {
            if (at >= size) return false;
            out[at++] = static_cast<uint8_t>(s);
        } else if (s == 256) {
            return true;
        } else {
            s -= 257;
            if (s >= 29) return false;
            size_t len = kLenBase[s] + b.get(kLenExtra[s]);
            int d = dist.decode(b);
            if (d < 0 || d >= 30) return false;
            size_t back = kDistBase[d] + b.get(kDistExtra[d]);
            if (back > at || at + len > size) return false;
            uint8_t* o = out + at;
            const uint8_t* from = o - back;
            if (back >= len) std::memcpy(o, from, len);
            else for (size_t i = 0; i < len; ++i) o[i] = from[i];
            at += len;
        }
    }
}

// The fixed codes, built at startup (the runtime has no thread-safe statics).
Huffman fixed_lit, fixed_dist;
[[maybe_unused]] const bool g_fixed_ready = [] {
    uint8_t l[288];
    for (int i = 0; i < 144; ++i) l[i] = 8;
    for (int i = 144; i < 256; ++i) l[i] = 9;
    for (int i = 256; i < 280; ++i) l[i] = 7;
    for (int i = 280; i < 288; ++i) l[i] = 8;
    fixed_lit.build(l, 288);
    uint8_t d[30];
    for (uint8_t& x : d) x = 5;
    fixed_dist.build(d, 30);
    return true;
}();

}  // namespace

int64_t raw(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size) {
    Bits b{in, in + in_size};
    size_t at = 0;
    for (;;) {
        uint32_t last = b.get(1), type = b.get(2);
        if (type == 0) {
            b.align();
            uint32_t len = b.get(16), nlen = b.get(16);
            if ((len ^ 0xFFFF) != nlen || at + len > out_size) return -1;
            for (uint32_t i = 0; i < len; ++i) out[at++] = static_cast<uint8_t>(b.get(8));
        } else if (type == 1) {
            if (!codes(b, fixed_lit, fixed_dist, out, out_size, at)) return -1;
        } else if (type == 2) {
            uint32_t hlit = b.get(5) + 257, hdist = b.get(5) + 1, hclen = b.get(4) + 4;
            static constexpr uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {};
            for (uint32_t i = 0; i < hclen; ++i) cl[order[i]] = static_cast<uint8_t>(b.get(3));
            Huffman clh;
            if (!clh.build(cl, 19)) return -1;
            uint8_t lens[320] = {};
            uint32_t n = 0;
            while (n < hlit + hdist) {
                int s = clh.decode(b);
                if (s < 0) return -1;
                if (s < 16) {
                    lens[n++] = static_cast<uint8_t>(s);
                } else {
                    uint32_t rep;
                    uint8_t v = 0;
                    if (s == 16) {
                        if (!n) return -1;
                        v = lens[n - 1];
                        rep = 3 + b.get(2);
                    } else if (s == 17) {
                        rep = 3 + b.get(3);
                    } else {
                        rep = 11 + b.get(7);
                    }
                    if (n + rep > hlit + hdist) return -1;
                    while (rep--) lens[n++] = v;
                }
            }
            Huffman lit, dist;
            if (!lit.build(lens, static_cast<int>(hlit)) || !dist.build(lens + hlit, static_cast<int>(hdist))) return -1;
            if (!codes(b, lit, dist, out, out_size, at)) return -1;
        } else {
            return -1;
        }
        if (last) return static_cast<int64_t>(at);
    }
}

int64_t zlib(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size) {
    if (in_size < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 0x20)) return -1;
    return raw(in + 2, in_size - 2, out, out_size);
}

}  // namespace rt::inflate
