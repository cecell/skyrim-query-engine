#include "edit/roundtrip.h"

#include <windows.h>

#include <cstring>

#include "edit/document.h"

namespace spf::edit {
namespace {

std::string FirstDifference(const Bytes& out, const uint8_t* in, size_t inSize) {
    if (out.size() != inSize) {
        return "size " + std::to_string(out.size()) + " instead of " + std::to_string(inSize);
    }
    for (size_t i = 0; i < inSize; ++i)
        if (out[i] != in[i]) return "first difference at offset " + std::to_string(i);
    return {};
}

void Scan(const Node& n, bool isTes4, const RoundTripOptions& opt, RoundTripResult* r, Bytes* scratch) {
    if (n.isGroup) {
        ++r->groups;
        for (const auto& c : n.children) Scan(*c, false, opt, r, scratch);
        return;
    }
    if (!isTes4) ++r->records;
    const uint8_t* body;
    size_t size;
    auto note = [&](const std::string& why) {
        ++r->opaque;
        ++r->opaqueBy[why];
        if (r->firstOpaque.empty()) {
            char id[16];
            snprintf(id, sizeof(id), "%08X ", n.formId());
            r->firstOpaque = SigText(n.sig()) + " " + id + why;
        }
    };
    if (n.compressed()) ++r->compressed;
    if (!Document::PlainBody(n, scratch, &body, &size)) {
        ++r->inflateFailures;
        note("inflate_failed");
        return;
    }
    ParsedBody parsed = ParseBody(body, size);
    r->subrecords += parsed.subs.size();
    for (const SubView& s : parsed.subs) {
        if (s.viaXxxx) {
            ++r->xxxx;
            if (s.sizeField != 0 || s.size <= 0xFFFF) ++r->xxxxOddSize;
        }
    }
    if (parsed.issue != BodyIssue::None) note(BodyIssueName(parsed.issue));
    if (opt.recompress && n.compressed()) {
        Bytes z;
        if (ZlibDeflate(body, size, &z)) {
            if (z.size() + 4 >= size) ++r->recompressNotSmaller;
            if (z.size() + 4 == n.storedSize && std::memcmp(z.data(), n.stored + 4, z.size()) == 0) ++r->recompressSame;
            else ++r->recompressDifferent;
        }
    }
}

}  // namespace

RoundTripResult RoundTrip(const std::wstring& path, const RoundTripOptions& opt) {
    RoundTripResult r;
    r.path = path;
    LARGE_INTEGER start, stop, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    std::string err;
    std::unique_ptr<Document> d = Document::Open(path, &err);
    if (!d) {
        r.refused = err;
        QueryPerformanceCounter(&stop);
        r.ms = 1000.0 * (stop.QuadPart - start.QuadPart) / freq.QuadPart;
        return r;
    }
    r.opened = true;
    r.size = d->inputSize();
    r.trailingBytes = d->trailingSize();

    Bytes out;
    std::string why;
    auto level = [&](const WriteOptions& w, const char* name) {
        bool ok = d->Serialize(&out, w, &why);
        std::string diff = ok ? FirstDifference(out, d->input(), d->inputSize()) : why;
        if (!diff.empty() && r.failure.empty()) r.failure = std::string(name) + ": " + diff;
        return diff.empty();
    };
    r.l0 = level({}, "L0");
    r.l1 = level({true, false}, "L1");
    r.l2 = level({true, true}, "L2");
    Bytes().swap(out);

    Bytes scratch;
    for (size_t i = 0; i < d->root().children.size(); ++i) Scan(*d->root().children[i], i == 0, opt, &r, &scratch);
    r.computedCount = d->CountForHedr();
    const uint8_t* body;
    size_t size;
    if (Document::PlainBody(*d->tes4(), &scratch, &body, &size)) {
        ParsedBody p = ParseBody(body, size);
        for (const SubView& s : p.subs) {
            if (std::memcmp(s.sig.data(), "HEDR", 4) == 0 && s.size >= 8) {
                std::memcpy(&r.hedrCount, s.data + 4, 4);
                r.hedrCountMatches = r.hedrCount == r.computedCount;
                break;
            }
        }
    }
    for (const auto& e : d->Index())
        if (e.second.size() > 1) r.duplicateFormIds += e.second.size() - 1;
    QueryPerformanceCounter(&stop);
    r.ms = 1000.0 * (stop.QuadPart - start.QuadPart) / freq.QuadPart;
    return r;
}

}  // namespace spf::edit
