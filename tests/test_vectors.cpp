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
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "pattern_engine.h"
#include "pattern_value.h"

using namespace skqe;

namespace {

// The features this engine implements.
const char* const kDefaultFeatures[] = {"current", "context", "load", "format2", "context_decl", "range", "peek_offset", "write_arm", "packed", "unnamed_bits", "shared", "int64", "fourcc", "defaults", "grammar"};

int g_passed = 0, g_failed = 0, g_skipped = 0, g_notApplicable = 0;
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
    if (const Value* p = c->Find("prev"); p && p->IsString()) ctx.prev = p->AsString();
    if (const Value* vars = c->Find("vars"); vars && vars->kind() == Value::Kind::Map) {
        for (const auto& [name, v] : vars->Members()) ctx.vars.emplace_back(name, v);
    }
    return ctx;
}

// A field vector's pattern. With "shared" or "options" it is loaded as the
// one key of a format 2 catalogue that carries them; otherwise compiled
// directly (as format 1 for today's behaviour).
std::shared_ptr<const Pattern> CompileVector(const Value& v) {
    const Value* pj = v.Find("pattern");
    if (!v.Find("shared") && !v.Find("options")) return CompilePattern(*pj, Str(v, "feature") != "current");
    Value cat = Value::Map();
    cat.Set("$format", Value::Str("spf-patterns/2"));
    if (const Value* s = v.Find("shared")) cat.Set("_shared", *s);
    if (const Value* o = v.Find("options")) cat.Set("_options", *o);
    cat.Set("TEST.TEST", *pj);
    PatternCatalog* c = new PatternCatalog;  // kept alive with the pattern below
    std::string err;
    if (!c->Load(Json(cat), &err)) throw PatternError(false, "catalogue: " + err);
    const Pattern* p = c->Find("TEST", "TEST");
    return std::shared_ptr<const Pattern>(std::shared_ptr<PatternCatalog>(c), p);
}

void RunField(const Value& v) {
    std::string id = Str(v, "id");
    std::shared_ptr<const Pattern> pat;
    try {
        pat = CompileVector(v);
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
    size_t end = 0;
    Value plain = DecodeSubrecord(*pat, bytes.data(), bytes.size(), ctx, &end);
    if (!Same(plain, got)) return Fail(id, "trace and decode differ");
    if (!Same(got, *v.Find("decoded"))) return Fail(id, "decoded " + Json(got) + ", expected " + Json(*v.Find("decoded")));
    // Bytes after the pattern.
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
            // A lossy leaf (a rounded display): the field must still read the same.
            std::vector<uint8_t> spliced(bytes.begin(), bytes.begin() + l.offset);
            spliced.insert(spliced.end(), enc.begin(), enc.end());
            spliced.insert(spliced.end(), bytes.begin() + l.offset + l.size, bytes.end());
            bool same = false;
            try {
                same = Same(DecodeSubrecord(*pat, spliced.data(), spliced.size(), ctx), got);
            } catch (const PatternError&) {
            }
            if (!same) {
                return Fail(id, "leaf " + l.path + " encodes to " + ToHex(enc.data(), enc.size()) + ", not " +
                                    ToHex(bytes.data() + l.offset, l.size));
            }
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

// An encode vector: a value with no capture events. This engine encodes
// whole fields only through EncodeLeaf, so a vector whose pattern is not a
// single leaf (or a packed word) does not apply.
void RunEncode(const Value& v) {
    std::string id = Str(v, "id");
    std::shared_ptr<const Pattern> pat;
    try {
        pat = CompilePattern(*v.Find("pattern"), true);
    } catch (const PatternError& e) {
        return Fail(id, std::string("pattern does not compile: ") + e.what());
    }
    if (!PatternIsLeaf(*pat)) {
        ++g_notApplicable;
        if (g_verbose) printf("n/a  %s (needs a whole-field encoder)\n", id.c_str());
        return;
    }
    DecodeContext ctx = ContextOf(v.Find("ctx"));
    std::vector<uint8_t> enc;
    std::string err;
    bool ok = EncodeLeaf(*pat, *v.Find("value"), nullptr, 0, ctx.localized, &enc, &err);
    std::string want = Str(v, "error_contains");
    if (!want.empty()) {
        if (ok) return Fail(id, "encoded " + ToHex(enc.data(), enc.size()) + ", expected an error");
        if (err.find(want) == std::string::npos) fprintf(stderr, "note %s: message \"%s\", hint \"%s\"\n", id.c_str(), err.c_str(), want.c_str());
        return Pass(id);
    }
    if (!ok) return Fail(id, "does not encode: " + err);
    if (ToHex(enc.data(), enc.size()) != Str(v, "hex")) return Fail(id, "encoded " + ToHex(enc.data(), enc.size()) + ", expected " + Str(v, "hex"));
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
    std::vector<std::string> sigs;
    std::vector<Value> values(fields.size());
    std::vector<size_t> sizes;
    for (size_t i = 0; i < fields.size(); ++i) {
        std::string fs = Str(fields[i], "sig");
        std::vector<uint8_t> bytes = FromHex(Str(fields[i], "hex"));
        const Value& want = expect[i];
        std::string at = "field " + std::to_string(i) + " " + fs + ": ";
        const DecodeContext& ctx = rc.Before(fs);
        if (cat.format() == 2 && (!ctx.prev || *ctx.prev != rc.Prev())) return Fail(id, at + "$@Prev not bound to the previous field");
        if (const Value* p = want.Find("prev"); p && p->AsString() != rc.Prev()) {
            return Fail(id, at + "prev \"" + rc.Prev() + "\", expected \"" + p->AsString() + "\"");
        }
        if (const Value* vars = want.Find("vars"); vars && cat.format() == 2 && vars->Members().size() != ctx.vars.size()) {
            return Fail(id, at + std::to_string(ctx.vars.size()) + " context variables, expected " +
                                std::to_string(vars->Members().size()));
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
                values[i] = got;
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
        sigs.push_back(fs);
        sizes.push_back(bytes.size());
    }
    if (const RecordGrammar* g = cat.Grammar(rt)) {
        std::vector<GrammarPlacement> placed;
        std::vector<GrammarDiag> diags;
        g->Match(sigs, &placed, &diags);
        for (size_t i = 0; i < expect.size(); ++i) {
            const Value* slot = expect[i].Find("slot");
            std::string want = slot && slot->IsString() ? slot->AsString() : "(none)";
            std::string got = placed[i].slot ? *placed[i].slot : "(none)";
            if (slot && want != got) return Fail(id, "field " + std::to_string(i) + ": slot " + got + ", expected " + want);
            if (const Value* grp = expect[i].Find("group"); grp && grp->AsString() != placed[i].group) {
                return Fail(id, "field " + std::to_string(i) + ": group " + placed[i].group + ", expected " + grp->AsString());
            }
        }
        Value dv = Value::List();
        for (const auto& d : diags) {
            Value x = Value::Map();
            x.Set("code", Value::Str(d.code));
            x.Set("what", Value::Str(d.what));
            if (d.code == "missing") x.Set("in", Value::Str(d.in));
            if (d.at) x.Set("at", Value::Int(static_cast<int64_t>(*d.at)));
            dv.Items().push_back(std::move(x));
        }
        if (const Value* wd = v.Find("expect_diagnostics"); wd && !Same(dv, *wd)) {
            return Fail(id, "diagnostics " + Json(dv) + ", expected " + Json(*wd));
        }
        std::vector<const Value*> ptrs;
        for (const Value& x : values) ptrs.push_back(x.IsNull() ? nullptr : &x);
        Value cv = Value::List();
        for (const auto& c : g->Counts(sigs, ptrs, sizes, placed)) {
            Value x = Value::Map();
            x.Set("count", Value::Str(c.count));
            x.Set("stored", c.stored ? Value::Int(*c.stored) : Value());
            x.Set("actual", Value::Int(c.actual));
            x.Set("ok", Value::Bool(c.ok));
            if (!c.ok) x.Set("code", Value::Str(c.code));
            cv.Items().push_back(std::move(x));
        }
        if (const Value* wc = v.Find("expect_counts"); wc && !Same(cv, *wc)) {
            return Fail(id, "counts " + Json(cv) + ", expected " + Json(*wc));
        }
    } else if (v.Find("catalog")->Find("_records")) {
        return Fail(id, "the catalogue's grammar did not load");
    }
    Pass(id);
}

// The feature a load vector needs besides the format 2 loader ("" = none).
std::string LoadFeature(const std::string& id) {
    if (id.rfind("load-packed-", 0) == 0) return "packed";
    if (id.rfind("load-range-", 0) == 0) return "range";
    if (id.rfind("load-ref-", 0) == 0) return "shared";
    if (id.rfind("load-context-", 0) == 0) return "context_decl";
    if (id.rfind("load-grammar-", 0) == 0) return "grammar";
    if (id == "load-published-catalogue-plus-marker") return "context_decl";
    return "";
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The context declarations that stand for the three old variables.
const char kLegacyContext[] =
    R"({"PERK":{"EPFT":{"init":0,"on":{"PRKE":{"set":0},"EPFT":{"read":"uint8"}}}},)"
    R"("GMST":{"EDID0":{"init":"","on":{"EDID":{"read":"char","lower":true}}}},)"
    R"("PACK":{"ANAM":{"init":"","on":{"ANAM":{"read":"zstring"}}}}})";

void RunLoad(const Value& v, const std::string& published) {
    std::string id = Str(v, "id");
    std::string text = Str(v, "catalog_text");
    const Value& want = *v.Find("format2");
    if (id == "load-published-catalogue" || id == "load-published-catalogue-plus-marker") {
        if (published.empty()) {
            ++g_skipped;
            return;
        }
        text = ReadFile(published);
        PatternCatalog cat;
        std::string err;
        if (id == "load-published-catalogue") {
            // The published file as format 1: without the marker and the
            // context declarations it carries since format 2.
            Value root;
            ParseJson(text, &root, &err);
            Value plain = Value::Map();
            for (const auto& m : root.Members())
                if (m.first != "$format" && m.first != "_context") plain.Set(m.first, m.second);
            text = Json(plain);
            if (!cat.Load(text, &err)) return Fail(id, "does not load: " + err);
            if (cat.format() != 1 || !cat.warnings().empty()) return Fail(id, "not a quiet format 1 load");
            printf("note %s: format 1, %zu keys\n", id.c_str(), cat.size());
            return Pass(id);
        }
        Value root;
        ParseJson(text, &root, &err);
        Value out = Value::Map();
        out.Set("$format", Value::Str("spf-patterns/2"));
        Value ctx;
        ParseJson(kLegacyContext, &ctx, &err);
        out.Set("_context", ctx);
        for (const auto& m : root.Members()) out.Set(m.first, m.second);
        if (!cat.Load(Json(out), &err)) return Fail(id, "does not load: " + err);
        std::map<std::string, size_t> codes;
        for (const auto& w : cat.warnings()) ++codes[w.code];
        std::string list;
        for (const auto& [c, n] : codes) list += " " + c + " " + std::to_string(n);
        printf("note %s: format 2, %zu keys, warnings%s\n", id.c_str(), cat.size(), list.c_str());
        return Pass(id);
    }
    PatternCatalog cat;
    std::string err;
    bool ok = cat.Load(text, &err);
    std::string result = Str(want, "result");
    if (result == "ok") {
        if (!ok) return Fail(id, "does not load: " + err);
        if (cat.format() != 2) return Fail(id, "not loaded as format 2");
        std::set<std::string> codes, expect;
        for (const auto& w : cat.warnings()) codes.insert(w.code);
        if (const Value* ws = want.Find("warnings"))
            for (const auto& w : ws->Items()) expect.insert(w.AsString());
        if (codes != expect) {
            std::string got;
            for (const auto& c : codes) got += " " + c;
            return Fail(id, "warnings:" + got);
        }
        return Pass(id);
    }
    if (result == "error") {
        if (ok) return Fail(id, "loads, expected " + Str(want, "code"));
        if (err.find(Str(want, "code")) == std::string::npos) return Fail(id, "error \"" + err + "\", expected " + Str(want, "code"));
        if (g_verbose) printf("     %s\n", err.c_str());
        return Pass(id);
    }
    ++g_skipped;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: skqe_vectors <test_vectors.json> [--features a,b,...] [--verbose]\n");
        return 2;
    }
    std::set<std::string> features(std::begin(kDefaultFeatures), std::end(kDefaultFeatures));
    std::string published;  // the published catalogue, for the load-published-* vectors
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--verbose")) g_verbose = true;
        else if (!strcmp(argv[i], "--published") && i + 1 < argc) published = argv[++i];
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
            // fourcc options anywhere in the vector need that feature too.
            bool fourcc = Json(v).find("\"trim\"") != std::string::npos;
            if (enabled(v) && (!fourcc || features.count("fourcc"))) RunField(v);
            else ++g_skipped;
        }
    }
    if (const Value* rv = root.Find("record_vectors")) {
        for (const Value& v : rv->Items()) {
            // A format 2 catalogue needs the format 2 loader and its context.
            bool v2 = v.Find("catalog") && v.Find("catalog")->Find("$format");
            // Range keys in a catalogue need the range feature too.
            std::string cj = v.Find("catalog") ? Json(*v.Find("catalog")) : "";
            bool ranges = cj.find("\"..") != std::string::npos || cj.find("..\"") != std::string::npos;
            if (enabled(v) && (!v2 || features.count("format2")) && (!ranges || features.count("range"))) RunRecord(v);
            else ++g_skipped;
        }
    }
    if (const Value* lv = root.Find("load_vectors")) {
        for (const Value& v : lv->Items()) {
            std::string need = LoadFeature(Str(v, "id"));
            if (features.count("load") && (need.empty() || features.count(need))) RunLoad(v, published);
            else ++g_skipped;
        }
    }
    if (const Value* ev = root.Find("encode_vectors")) {
        for (const Value& v : ev->Items()) {
            if (enabled(v)) RunEncode(v);
            else ++g_skipped;
        }
    }
    printf("%d passed, %d failed, %d not applicable, %d not run\n", g_passed, g_failed, g_notApplicable, g_skipped);
    return g_failed ? 1 : 0;
}
