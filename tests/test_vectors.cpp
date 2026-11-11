// Runs the pattern-language test vectors (a JSON file kept beside the language
// specification, not in this repository) against the engine.
//
//   skqe_vectors <test_vectors.json> [--features a,b,...] [--verbose]
//
// Only vectors whose feature is enabled run; the default list grows as the
// engine gains the features. Field vectors: decode (value, bytes after the
// pattern, or the error and its overread flag), then every traced leaf
// encoded back from its value over its own bytes must give those bytes, and
// a pattern that is one leaf must encode its value alone to encode_plain_hex.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "pattern_engine.h"
#include "pattern_value.h"

using namespace skqe;

namespace {

// The features this engine implements.
const char* const kDefaultFeatures[] = {"current", "context"};

int g_passed = 0, g_failed = 0, g_skipped = 0;
bool g_verbose = false;

void Fail(const std::string& id, const std::string& why) {
    ++g_failed;
    fprintf(stderr, "FAIL %s: %s\n", id.c_str(), why.c_str());
}

void Pass(const std::string& id) {
    ++g_passed;
    if (g_verbose) printf("ok   %s\n", id.c_str());
}

std::string Json(const Value& v) {
    std::string s;
    WriteJson(v, &s);
    return s;
}

// Exact equality: kinds, map order, float bits as doubles.
bool Same(const Value& a, const Value& b) {
    if (a.kind() != b.kind()) return false;
    switch (a.kind()) {
        case Value::Kind::Null: return true;
        case Value::Kind::Bool: return a.AsBool() == b.AsBool();
        case Value::Kind::Int: return a.AsInt() == b.AsInt();
        case Value::Kind::Float: return a.AsFloat() == b.AsFloat();
        case Value::Kind::String: return a.AsString() == b.AsString();
        case Value::Kind::List: {
            if (a.Items().size() != b.Items().size()) return false;
            for (size_t i = 0; i < a.Items().size(); ++i)
                if (!Same(a.Items()[i], b.Items()[i])) return false;
            return true;
        }
        case Value::Kind::Map: {
            if (a.Members().size() != b.Members().size()) return false;
            for (size_t i = 0; i < a.Members().size(); ++i) {
                if (a.Members()[i].first != b.Members()[i].first) return false;
                if (!Same(a.Members()[i].second, b.Members()[i].second)) return false;
            }
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> FromHex(const std::string& s) {
    std::vector<uint8_t> out;
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(static_cast<uint8_t>(nib(s[i]) << 4 | nib(s[i + 1])));
    return out;
}

std::string ToHex(const uint8_t* b, size_t n) {
    static const char k[] = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s.push_back(k[b[i] >> 4]);
        s.push_back(k[b[i] & 15]);
    }
    return s;
}

std::string Str(const Value& obj, const char* key) {
    const Value* v = obj.Find(key);
    return v && v->IsString() ? v->AsString() : std::string();
}

DecodeContext ContextOf(const Value* c) {
    DecodeContext ctx;
    if (!c) return ctx;
    ctx.rt = Str(*c, "rt");
    ctx.fsig = Str(*c, "fsig");
    if (const Value* fv = c->Find("form_version")) fv->ToInt(&ctx.formVersion);
    if (const Value* l = c->Find("localized"); l && l->kind() == Value::Kind::Bool) ctx.localized = l->AsBool();
    if (const Value* vars = c->Find("vars"); vars && vars->kind() == Value::Kind::Map) {
        for (const auto& [name, v] : vars->Members()) ctx.vars.emplace_back(name, v);
    }
    return ctx;
}

void RunField(const Value& v) {
    std::string id = Str(v, "id");
    const Value* pj = v.Find("pattern");
    std::shared_ptr<const Pattern> pat;
    try {
        pat = CompilePattern(*pj);
    } catch (const PatternError& e) {
        return Fail(id, std::string("pattern does not compile: ") + e.what());
    }
    DecodeContext ctx = ContextOf(v.Find("ctx"));
    std::vector<uint8_t> bytes = FromHex(Str(v, "hex"));
    const Value* wantErr = v.Find("decode_error");
    std::vector<TraceLeaf> leaves;
    Value got;
    try {
        got = TraceSubrecord(*pat, bytes.data(), bytes.size(), ctx, &leaves);
    } catch (const PatternError& e) {
        if (!wantErr) return Fail(id, std::string("decode error: ") + e.what());
        const Value* ov = wantErr->Find("overread");
        if (ov && ov->AsBool() != e.overread()) return Fail(id, std::string("overread flag differs: ") + e.what());
        std::string hint = Str(*wantErr, "message_contains");
        if (!hint.empty() && std::string(e.what()).find(hint) == std::string::npos) {
            fprintf(stderr, "note %s: message \"%s\", hint \"%s\"\n", id.c_str(), e.what(), hint.c_str());
        }
        return Pass(id);
    }
    if (wantErr) return Fail(id, "decoded, but an error was expected: " + Json(got));
    Value plain = DecodeSubrecord(*pat, bytes.data(), bytes.size(), ctx);
    if (!Same(plain, got)) return Fail(id, "trace and decode differ");
    if (!Same(got, *v.Find("decoded"))) return Fail(id, "decoded " + Json(got) + ", expected " + Json(*v.Find("decoded")));
    // Bytes after the pattern: past the last leaf.
    size_t end = 0;
    for (const TraceLeaf& l : leaves) end = std::max(end, l.offset + l.size);
    std::string tail = ToHex(bytes.data() + end, bytes.size() - end);
    if (tail != Str(v, "tail_hex")) return Fail(id, "tail " + tail + ", expected " + Str(v, "tail_hex"));
    // Every leaf back from its value, over its own bytes.
    for (const TraceLeaf& l : leaves) {
        if (!l.pattern) continue;
        if (LeafTypeName(*l.pattern) == "value_ref") continue;
        std::vector<uint8_t> enc;
        std::string err;
        if (!EncodeLeaf(*l.pattern, l.value, bytes.data() + l.offset, l.size, ctx.localized, &enc, &err)) {
            return Fail(id, "leaf " + l.path + " does not encode: " + err);
        }
        if (enc.size() != l.size || (l.size && std::memcmp(enc.data(), bytes.data() + l.offset, l.size) != 0)) {
            return Fail(id, "leaf " + l.path + " encodes to " + ToHex(enc.data(), enc.size()) + ", not " +
                                ToHex(bytes.data() + l.offset, l.size));
        }
    }
    // A one-leaf pattern from its value alone.
    if (PatternIsLeaf(*pat) && v.Find("encode_plain_hex")) {
        std::vector<uint8_t> enc;
        std::string err;
        if (!EncodeLeaf(*pat, got, nullptr, 0, ctx.localized, &enc, &err)) return Fail(id, "value-only encode: " + err);
        if (ToHex(enc.data(), enc.size()) != Str(v, "encode_plain_hex")) {
            return Fail(id, "value-only encode " + ToHex(enc.data(), enc.size()) + ", expected " + Str(v, "encode_plain_hex"));
        }
    }
    Pass(id);
}

// The vector's catalogue as file text. A format 1 catalogue holds each
// pattern as JSON text in a string, as the published file does.
std::string CatalogText(const Value& cat) {
    if (cat.Find("$format")) return Json(cat);
    Value out = Value::Map();
    for (const auto& [key, val] : cat.Members()) {
        if (!key.empty() && key[0] != '_' && !val.IsString()) out.Set(key, Value::Str(Json(val)));
        else out.Set(key, val);
    }
    return Json(out);
}

void RunRecord(const Value& v) {
    std::string id = Str(v, "id");
    PatternCatalog cat;
    std::string err;
    if (!cat.Load(CatalogText(*v.Find("catalog")), &err)) return Fail(id, "catalogue does not load: " + err);
    std::string rt = Str(v, "record_type");
    int64_t fv = 0;
    v.Find("form_version")->ToInt(&fv);
    const Value* loc = v.Find("localized");
    RecordContext rc(cat, rt, fv, !loc || loc->AsBool());
    const auto& fields = v.Find("fields")->Items();
    const auto& expect = v.Find("expect_fields")->Items();
    if (fields.size() != expect.size()) return Fail(id, "fields and expect_fields differ in length");
    for (size_t i = 0; i < fields.size(); ++i) {
        std::string fs = Str(fields[i], "sig");
        std::vector<uint8_t> bytes = FromHex(Str(fields[i], "hex"));
        const Value& want = expect[i];
        std::string at = "field " + std::to_string(i) + " " + fs + ": ";
        const DecodeContext& ctx = rc.Before(fs);
        if (const Value* p = want.Find("prev"); p && p->AsString() != rc.Prev()) {
            return Fail(id, at + "prev \"" + rc.Prev() + "\", expected \"" + p->AsString() + "\"");
        }
        if (const Value* vars = want.Find("vars")) {
            for (const auto& [name, val] : vars->Members()) {
                const Value* have = nullptr;
                for (const auto& cv : ctx.vars)
                    if (cv.first == name) have = &cv.second;
                if (!have || !Same(*have, val)) {
                    return Fail(id, at + "$@" + name + " is " + (have ? Json(*have) : "unbound") + ", expected " + Json(val));
                }
            }
        }
        const Pattern* pat = cat.Find(rt, fs);
        if (want.Find("no_pattern")) {
            if (pat) return Fail(id, at + "has a pattern, expected none");
        } else if (!pat) {
            return Fail(id, at + "no pattern");
        } else {
            try {
                Value got = DecodeSubrecord(*pat, bytes.data(), bytes.size(), ctx);
                if (want.Find("decode_error")) return Fail(id, at + "decoded, an error was expected");
                if (!Same(got, *want.Find("decoded"))) {
                    return Fail(id, at + "decoded " + Json(got) + ", expected " + Json(*want.Find("decoded")));
                }
            } catch (const PatternError& e) {
                const Value* we = want.Find("decode_error");
                if (!we) return Fail(id, at + "decode error: " + e.what());
                const Value* ov = we->Find("overread");
                if (ov && ov->AsBool() != e.overread()) return Fail(id, at + "overread flag differs: " + e.what());
            }
        }
        rc.After(fs, bytes.data(), bytes.size());
    }
    Pass(id);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: skqe_vectors <test_vectors.json> [--features a,b,...] [--verbose]\n");
        return 2;
    }
    std::set<std::string> features(std::begin(kDefaultFeatures), std::end(kDefaultFeatures));
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--verbose")) g_verbose = true;
        else if (!strcmp(argv[i], "--features") && i + 1 < argc) {
            features.clear();
            std::stringstream ss(argv[++i]);
            for (std::string f; std::getline(ss, f, ',');) features.insert(f);
        }
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Value root;
    std::string err;
    if (!ParseJson(text, &root, &err)) {
        fprintf(stderr, "cannot read %s: %s\n", argv[1], err.c_str());
        return 2;
    }
    auto enabled = [&](const Value& v) {
        std::string f = Str(v, "feature");
        // "context+range": every part must be enabled.
        std::stringstream ss(f);
        for (std::string part; std::getline(ss, part, '+');)
            if (!features.count(part)) return false;
        return true;
    };
    if (const Value* fv = root.Find("field_vectors")) {
        for (const Value& v : fv->Items()) {
            if (enabled(v)) RunField(v);
            else ++g_skipped;
        }
    }
    if (const Value* rv = root.Find("record_vectors")) {
        for (const Value& v : rv->Items()) {
            // A format 2 catalogue needs the format 2 loader and its context.
            bool v2 = v.Find("catalog") && v.Find("catalog")->Find("$format");
            if (enabled(v) && (!v2 || features.count("format2"))) RunRecord(v);
            else ++g_skipped;
        }
    }
    for (const char* sec : {"encode_vectors", "load_vectors"}) {
        if (const Value* s = root.Find(sec)) g_skipped += static_cast<int>(s->Items().size());
    }
    printf("%d passed, %d failed, %d not run\n", g_passed, g_failed, g_skipped);
    return g_failed ? 1 : 0;
}
