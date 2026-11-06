// A record's subrecords, split without losing anything: re-emitting what
// ParseBody returned gives back the same bytes, including the odd spellings of
// XXXX some tools write. No catalog involved.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spf::edit {

using Sig = std::array<char, 4>;

inline Sig MakeSig(const char* s) { return {s[0], s[1], s[2], s[3]}; }
inline std::string SigText(const Sig& s) { return std::string(s.data(), 4); }

// One subrecord, viewing bytes owned by someone else.
struct SubView {
    Sig sig{};
    const uint8_t* data = nullptr;
    size_t size = 0;
    // How its size was spelled in the file. Without XXXX: the plain u16. With
    // XXXX: the u16 after it, normally 0. New subrecords leave these unset.
    bool viaXxxx = false;
    uint16_t sizeField = 0;
};

// Why a body cannot be split into subrecords exactly. A record with one is
// "opaque": kept byte for byte, field edits refused.
enum class BodyIssue {
    None,
    TrailingBytes,  // fewer than 6 bytes left over after the last subrecord
    Overrun,        // a subrecord claims more bytes than the record holds
    BadXxxx,        // XXXX whose payload is not 4 bytes, or with nothing after it
};
const char* BodyIssueName(BodyIssue i);

struct ParsedBody {
    std::vector<SubView> subs;
    BodyIssue issue = BodyIssue::None;
    size_t issueOffset = 0;  // where the unparsable tail starts
};

ParsedBody ParseBody(const uint8_t* data, size_t size);

// Appends the subrecords to `out`. Sizes above 65535 get an XXXX.
void EmitBody(const std::vector<SubView>& subs, std::vector<uint8_t>* out);

// zlib stream at level 9, the Creation Kit's level.
bool ZlibDeflate(const uint8_t* src, size_t size, std::vector<uint8_t>* out);

}  // namespace spf::edit
