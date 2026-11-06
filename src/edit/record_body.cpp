#include "edit/record_body.h"

#include <cstring>

#include "zlib.h"

namespace spf::edit {
namespace {

uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void Header(std::vector<uint8_t>* out, const char* sig, uint16_t size) {
    out->insert(out->end(), sig, sig + 4);
    out->push_back(static_cast<uint8_t>(size));
    out->push_back(static_cast<uint8_t>(size >> 8));
}

}  // namespace

const char* BodyIssueName(BodyIssue i) {
    switch (i) {
        case BodyIssue::None: return "none";
        case BodyIssue::TrailingBytes: return "trailing_bytes";
        case BodyIssue::Overrun: return "subrecord_overrun";
        case BodyIssue::BadXxxx: return "bad_xxxx";
    }
    return "?";
}

ParsedBody ParseBody(const uint8_t* data, size_t size) {
    ParsedBody out;
    size_t q = 0;
    while (q < size) {
        if (size - q < 6) {
            out.issue = BodyIssue::TrailingBytes;
            out.issueOffset = q;
            return out;
        }
        SubView s;
        std::memcpy(s.sig.data(), data + q, 4);
        size_t start = q;
        uint16_t field = U16(data + q + 4);
        size_t len = field;
        q += 6;
        if (std::memcmp(s.sig.data(), "XXXX", 4) == 0) {
            if (len != 4 || size - q < 4 + 6) {
                out.issue = BodyIssue::BadXxxx;
                out.issueOffset = start;
                return out;
            }
            len = U32(data + q);
            q += 4;
            std::memcpy(s.sig.data(), data + q, 4);
            field = U16(data + q + 4);
            q += 6;
            s.viaXxxx = true;
        }
        if (len > size - q) {
            out.issue = BodyIssue::Overrun;
            out.issueOffset = start;
            return out;
        }
        s.sizeField = field;
        s.data = data + q;
        s.size = len;
        q += len;
        out.subs.push_back(s);
    }
    return out;
}

void EmitBody(const std::vector<SubView>& subs, std::vector<uint8_t>* out) {
    for (const SubView& s : subs) {
        if (s.viaXxxx || s.size > 0xFFFF) {
            Header(out, "XXXX", 4);
            uint32_t n = static_cast<uint32_t>(s.size);
            for (int i = 0; i < 4; ++i) out->push_back(static_cast<uint8_t>(n >> (8 * i)));
            // A parsed subrecord keeps the u16 it had; a new one writes 0, as
            // the Creation Kit does.
            Header(out, s.sig.data(), s.viaXxxx ? s.sizeField : 0);
        } else {
            Header(out, s.sig.data(), static_cast<uint16_t>(s.size));
        }
        if (s.size) out->insert(out->end(), s.data, s.data + s.size);
    }
}

bool ZlibDeflate(const uint8_t* src, size_t size, std::vector<uint8_t>* out) {
    uLongf cap = compressBound(static_cast<uLong>(size));
    out->resize(cap);
    int rc = compress2(out->data(), &cap, src, static_cast<uLong>(size), 9);
    if (rc != Z_OK) return false;
    out->resize(cap);
    return true;
}

}  // namespace spf::edit
