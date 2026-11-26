// DEFLATE (RFC 1951) and zlib streams (RFC 1950), for the console's DirectStorage requests.
#pragma once
#include <cstddef>
#include <cstdint>

namespace rt::inflate {

// Inflates `in` into `out` (at most `out_size` bytes). Returns the bytes written, or -1 for a bad stream or one that
// doesn't fit. zlib: a 2-byte header first (its Adler-32 trailer isn't checked); raw: DEFLATE data only.
int64_t zlib(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size);
int64_t raw(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size);

}  // namespace rt::inflate
