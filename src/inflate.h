// zlib (RFC 1950/1951) decompression for compressed plugin records.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace spf {

// The largest record body inflated (the biggest in the Bethesda plugins is a
// few MB).
constexpr size_t kMaxInflated = size_t{256} << 20;

// Inflates a zlib stream into exactly `expectedSize` bytes. Returns false on
// a malformed stream, a size mismatch, or a size above kMaxInflated or the
// most deflate can expand `srcLen` bytes to.
bool ZlibInflate(const uint8_t* src, size_t srcLen, size_t expectedSize, std::vector<uint8_t>* out);

}  // namespace spf
