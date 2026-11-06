// Measures whether the model loses anything: reads a plugin, writes it back at
// three levels, and compares each with the input. Writes nothing to disk.
//
//   L0  untouched: spans copied. Only shows the walker covers the file.
//   L1  groups rebuilt: every group size recomputed, every header re-emitted.
//   L2  records rebuilt: also every record split into subrecords and emitted
//       again (compressed ones through inflate, comparing inflated bytes).
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace skqe::edit {

struct RoundTripOptions {
    bool recompress = false;  // deflate each compressed record, count those that match byte for byte
};

struct RoundTripResult {
    std::wstring path;
    uint64_t size = 0;
    bool opened = false;
    std::string refused;  // why the structure was refused
    bool l0 = false, l1 = false, l2 = false;
    std::string failure;  // first difference, when a level failed
    double ms = 0;

    uint64_t records = 0, groups = 0, compressed = 0;
    uint64_t subrecords = 0, xxxx = 0, xxxxOddSize = 0;
    uint64_t opaque = 0;                     // records whose body cannot be split exactly
    std::map<std::string, uint64_t> opaqueBy;  // reason -> count
    std::string firstOpaque;                 // "XXXXXXXX reason", for looking at one
    uint64_t inflateFailures = 0;
    uint64_t trailingBytes = 0;
    uint64_t duplicateFormIds = 0;

    // Facts the writer has to respect, not failures.
    uint32_t hedrCount = 0, computedCount = 0;
    bool hedrCountMatches = false;
    uint64_t recompressSame = 0, recompressDifferent = 0, recompressNotSmaller = 0;
};

RoundTripResult RoundTrip(const std::wstring& path, const RoundTripOptions& opt);

}  // namespace skqe::edit
