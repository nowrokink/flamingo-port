// BCPack: the console's lossless packing of BC1-BC7 block streams, which its DirectStorage hardware unpacks (a
// request's options bits 1-4 name the format). Decode only: BC1, BC3, BC4, BC5, BC6H and BC7 (bcpack.cpp, tables in
// bcpack_tables.h).
#pragma once
#include <cstddef>
#include <cstdint>

namespace rt::bcpack {

enum class Result {
    ok,
    unsupported,  // a format without a decoder (BC2, none) or a size that isn't whole blocks
    bad_stream,   // the packed data doesn't decode to `size` bytes
};

// Bytes per block of `format` (8 or 16), or 0 if there's no such format.
uint32_t block_bytes(int format);
bool supported(int format);
const char* name(int format);

// Decodes a packed stream of `format` (1-7) into exactly `size` bytes at `out`.
Result decode(const uint8_t* packed, size_t packed_size, int format, uint8_t* out, size_t size);

}  // namespace rt::bcpack
