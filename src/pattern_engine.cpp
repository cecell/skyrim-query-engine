#include "pattern_engine.h"

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <cmath>
#include <cstring>

namespace skqe {
namespace {

float BitsToFloat(uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

}  // namespace

enum class PType : uint8_t { Prim, Seq, Repeat, Union, Opt, Skip, Bytes, Window, Bitfield, Packed, PackedMember };

// What a member of a `packed` word holds.
enum class MemberKind : uint8_t { Uint, Int, Bool, Enum, Flags, Rest };

enum class Prim : uint8_t {
    U8, U16, U32, I8, I16, I32, F32, Bool, HexU8, EmptyMarker, Lstring, Formid,
    I32Div100, I16Div100, U16Div65535, U8Div100, F32Div100, U32Div100,
    FloatRad2Deg, FloatDefault, FloatDefaultRad2Deg, FloatColor255, Scaled, ValueRef,
    ColorRgb, ColorRgba, Zstring, Str8, Str16, Str32, EnumU8, EnumU16, EnumU32,
    Fourcc, U16Div100,
    U64, I64,  // format 2
};

struct Pattern {
    PType type = PType::Prim;

    // Primitive (and bitfield's "as")
    Prim prim = Prim::U8;
    std::optional<std::string> bindAs;
    std::optional<std::map<std::string, std::string>> enumMap;
    std::string scaledRead = "uint8";
    double scaledOffset = 0.0;
    double scaledDivisor = 1.0;
    // Format 2 leaf attributes for writers; decoding ignores them.
    std::optional<Value> leafDefault;               // "default": the value a new field takes
    std::optional<double> rangeLo, rangeHi;         // "range": [lo, hi], either may be null
    bool hasRange = false;
    std::vector<std::string> target;                // "target": record types a FormID may point at
    bool fourccTrim = false;  // format 2 fourcc "trim": no trailing zero bytes
    bool fourccHex = false;   // format 2 fourcc "other":"hex": unprintable codes as 0xXXXXXXXX
    std::optional<Value> refExpr;
    bool refAsFormid = false;

    // Seq
    std::vector<std::pair<std::shared_ptr<const Pattern>, std::optional<std::string>>> items;
    bool unwrap = false;

    // Repeat / Opt / Window body; Repeat / Skip / Bytes / Window count
    std::shared_ptr<const Pattern> body;
    std::optional<Value> n;
    int nPfx = 0;  // 0 = none, else 1, 2 or 4 bytes

    // Union
    std::string on;
    std::optional<int64_t> mask;  // cases are looked up on (value & mask)
    std::map<std::string, std::shared_ptr<const Pattern>> cases;
    std::shared_ptr<const Pattern> deflt;
    // Format 2: case keys "lo..hi", "..hi", "lo.." (inclusive), tried after
    // the exact keys when the value is an integer.
    struct CaseRange {
        std::optional<int64_t> lo, hi;
        std::shared_ptr<const Pattern> arm;
    };
    std::vector<CaseRange> ranges;
    std::string write;     // format 2: the arm for a new field when `on` is $_remaining
    int peekWidth = 0;     // format 2: `on` is $_peek_uW+N, read N bytes ahead
    size_t peekOffset = 0;

    // Bitfield; Packed and PackedMember: `size` is the word's width in bytes
    int size = 4;
    std::map<int, std::string> bits;
    bool showUnnamed = false;  // format 2 "unnamed":"show": set bits without a name read as "bit<N>"
    bool bitNames = false;     // format 2: "bit<N>" is accepted as a flag name on encode

    // Packed: the members in order, and the bits none of them owns
    std::vector<std::shared_ptr<const Pattern>> members;
    uint32_t restMask = 0;
    // PackedMember (also the "_rest" pseudo-member): its bits of the word
    MemberKind member = MemberKind::Uint;
    std::string memberId;
    int lo = 0, nbits = 0;
    uint32_t memberMask = 0;  // the bits it owns, in place
};

namespace {

[[noreturn]] void BadShape(const std::string& what) { throw PatternError(false, "Bad pattern shape: " + what); }

// Compiling a format 2 catalogue: its features are on. Format 1 compiles
// exactly as it always did.
thread_local bool g_format2 = false;
// Format 2 "_options": {"unnamed_bits": "show"}, for bitfields without their own "unnamed".
thread_local bool g_unnamedShow = false;
// Format 2 "_shared": name -> pattern, and each one compiled once.
thread_local const Value* g_shared = nullptr;
thread_local std::map<std::string, std::shared_ptr<const Pattern>>* g_sharedCompiled = nullptr;

const std::map<std::string, Prim>& PrimNames() {
    static const std::map<std::string, Prim> names = {
        {"uint8", Prim::U8}, {"uint16", Prim::U16}, {"uint32", Prim::U32},
        {"int8", Prim::I8}, {"int16", Prim::I16}, {"int32", Prim::I32},
        {"float32", Prim::F32}, {"float32_disp_rad2deg", Prim::F32},
        {"bool", Prim::Bool}, {"hex_uint8", Prim::HexU8}, {"empty_marker", Prim::EmptyMarker},
        {"lstring", Prim::Lstring}, {"formid", Prim::Formid},
        {"int32_div100", Prim::I32Div100}, {"int16_div100", Prim::I16Div100},
        {"uint16_div65535", Prim::U16Div65535}, {"uint8_div100", Prim::U8Div100},
        {"uint16_div100", Prim::U16Div100}, {"fourcc", Prim::Fourcc},
        {"float32_div100", Prim::F32Div100}, {"uint32_div100", Prim::U32Div100},
        {"float_rad2deg", Prim::FloatRad2Deg}, {"float_default", Prim::FloatDefault},
        {"float_default_rad2deg", Prim::FloatDefaultRad2Deg}, {"float_color255", Prim::FloatColor255},
        {"scaled", Prim::Scaled}, {"value_ref", Prim::ValueRef},
        {"color_rgb", Prim::ColorRgb}, {"color_rgba", Prim::ColorRgba},
        {"zstring", Prim::Zstring}, {"str8", Prim::Str8}, {"str16", Prim::Str16}, {"str32", Prim::Str32},
        {"enum_uint8", Prim::EnumU8}, {"enum_uint16", Prim::EnumU16}, {"enum_uint32", Prim::EnumU32},
        {"uint64", Prim::U64}, {"int64", Prim::I64},
    };
    return names;
}

int Width(const Value* v) {
    if (!v || !v->IsString()) return 0;
    const std::string& s = v->AsString();
    if (s == "uint8") return 1;
    if (s == "uint16") return 2;
    if (s == "uint32") return 4;
    BadShape("expected uint8/uint16/uint32, got " + s);
}

std::optional<std::string> IdOf(const Value& node) {
    if (node.kind() == Value::Kind::Map) {
        const Value* id = node.Find("id");
        if (id && id->IsString()) return id->AsString();
    }
    return std::nullopt;
}

std::shared_ptr<const Pattern> Compile(const Value& j);

// Primitives format 1 does not know.
bool IsFormat2Prim(Prim p) { return p == Prim::U64 || p == Prim::I64; }

// "default", "range", "target" on a leaf (format 2; the catalogue check
// has refused them where they do not belong).
void LeafAttrs(const Value& j, Pattern* p) {
    if (!g_format2 || j.kind() != Value::Kind::Map) return;
    if (const Value* d = j.Find("default")) p->leafDefault = *d;
    if (const Value* r = j.Find("range"); r && r->kind() == Value::Kind::List && r->Items().size() == 2) {
        p->hasRange = true;
        auto num = [](const Value& v, std::optional<double>* out) {
            if (v.kind() == Value::Kind::Int) *out = static_cast<double>(v.AsInt());
            else if (v.kind() == Value::Kind::Float) *out = v.AsFloat();
        };
        num(r->Items()[0], &p->rangeLo);
        num(r->Items()[1], &p->rangeHi);
    }
    if (const Value* t = j.Find("target"); t && t->kind() == Value::Kind::List) {
        for (const auto& x : t->Items())
            if (x.IsString()) p->target.push_back(x.AsString());
    }
}

}  // namespace

// "lo..hi", "..hi", "lo..": decimal integers, a leading '-' allowed.
bool ParseCaseRange(const std::string& key, std::optional<int64_t>* lo, std::optional<int64_t>* hi) {
    size_t dots = key.find("..");
    if (dots == std::string::npos) return false;
    auto num = [](const std::string& s, std::optional<int64_t>* out) {
        if (s.empty()) return true;
        size_t i = s[0] == '-' ? 1 : 0;
        if (i == s.size() || s.size() - i > 18 || s.find_first_not_of("0123456789", i) != std::string::npos) return false;
        *out = std::stoll(s);
        return true;
    };
    lo->reset();
    hi->reset();
    return num(key.substr(0, dots), lo) && num(key.substr(dots + 2), hi);
}

// "_peek_u8", "_peek_u16+4": the width in bytes and the offset.
bool ParsePeek(const std::string& name, int* width, size_t* offset) {
    static const char* const kNames[] = {"_peek_u8", "_peek_u16", "_peek_u32"};
    static const int kWidths[] = {1, 2, 4};
    for (int i = 0; i < 3; ++i) {
        std::string n = kNames[i];
        if (name.compare(0, n.size(), n) != 0) continue;
        std::string rest = name.substr(n.size());
        if (rest.empty()) {
            *width = kWidths[i];
            *offset = 0;
            return true;
        }
        if (rest[0] != '+' || rest.size() < 2 || rest.size() > 7 || rest.find_first_not_of("0123456789", 1) != std::string::npos) return false;
        *width = kWidths[i];
        *offset = static_cast<size_t>(std::stoul(rest.substr(1)));
        return true;
    }
    return false;
}

namespace {

std::vector<std::pair<std::shared_ptr<const Pattern>, std::optional<std::string>>> CompileItems(const Value& list) {
    std::vector<std::pair<std::shared_ptr<const Pattern>, std::optional<std::string>>> items;
    for (const auto& n : list.Items()) items.emplace_back(Compile(n), IdOf(n));
    return items;
}

const Value& Required(const Value& obj, const char* key, const char* node) {
    const Value* v = obj.Find(key);
    if (!v) BadShape(std::string(node) + " missing '" + key + "'");
    return *v;
}

// A `packed` word (format 2): members over bit ranges of one integer. The
// catalogue check has already refused overlaps and bad widths.
void CompilePacked(const Value& j, Pattern* p) {
    p->type = PType::Packed;
    int w = Width(j.Find("size"));
    if (!w) BadShape("packed missing 'size'");
    p->size = w;
    uint32_t wordMask = w == 4 ? 0xFFFFFFFFu : (1u << (8 * w)) - 1;
    uint32_t owned = 0;
    const Value* d = j.Find("do");
    if (!d || d->kind() != Value::Kind::List) BadShape("packed missing 'do'");
    for (const Value& m : d->Items()) {
        auto mp = std::make_shared<Pattern>();
        mp->type = PType::PackedMember;
        mp->size = w;
        if (const Value* id = m.Find("id"); id && id->IsString()) mp->memberId = id->AsString();
        std::string kind = "uint";
        if (const Value* k = m.Find("t"); k && k->IsString()) kind = k->AsString();
        mp->member = kind == "int" ? MemberKind::Int : kind == "bool" ? MemberKind::Bool : kind == "enum" ? MemberKind::Enum
                   : kind == "flags" ? MemberKind::Flags : MemberKind::Uint;
        if (const Value* as = m.Find("as"); as && as->IsString()) mp->bindAs = as->AsString();
        LeafAttrs(m, mp.get());
        if (mp->member == MemberKind::Flags) {
            if (const Value* bs = m.Find("bits"); bs && bs->kind() == Value::Kind::Map) {
                for (const auto& b : bs->Members()) {
                    int bit = std::stoi(b.first);
                    mp->bits[bit] = b.second.AsString();
                    mp->memberMask |= 1u << bit;
                }
            }
        } else {
            int64_t lo = 0, n = 0;
            if (const Value* v = m.Find("lo")) v->ToInt(&lo);
            if (const Value* v = m.Find("n")) v->ToInt(&n);
            if (n < 1 || lo < 0 || lo + n > 8 * w) BadShape("packed member out of the word");
            mp->lo = static_cast<int>(lo);
            mp->nbits = static_cast<int>(n);
            mp->memberMask = static_cast<uint32_t>(((n == 32 ? 0xFFFFFFFFull : (1ull << n) - 1)) << lo);
            if (mp->member == MemberKind::Enum) {
                mp->enumMap.emplace();
                if (const Value* e = m.Find("enum"); e && e->kind() == Value::Kind::Map)
                    for (const auto& x : e->Members()) (*mp->enumMap)[x.first] = x.second.AsString();
            }
        }
        if (owned & mp->memberMask) BadShape("packed members overlap");
        owned |= mp->memberMask;
        p->members.push_back(mp);
    }
    p->restMask = wordMask & ~owned;
    if (const Value* as = j.Find("as"); as && as->IsString()) p->bindAs = as->AsString();
}

// The "_rest" pseudo-member of a packed word: the bits no member owns.
std::shared_ptr<const Pattern> RestMember(const Pattern& packed) {
    auto r = std::make_shared<Pattern>();
    r->type = PType::PackedMember;
    r->member = MemberKind::Rest;
    r->memberId = "_rest";
    r->size = packed.size;
    r->memberMask = packed.restMask;
    return r;
}

std::shared_ptr<const Pattern> Compile(const Value& j) {
    auto p = std::make_shared<Pattern>();
    switch (j.kind()) {
        case Value::Kind::String: {
            if (j.AsString() == "bytes") {
                p->type = PType::Bytes;
                return p;
            }
            auto it = PrimNames().find(j.AsString());
            if (it == PrimNames().end() || (!g_format2 && IsFormat2Prim(it->second))) {
                throw PatternError(false, "Unknown primitive type: " + j.AsString());
            }
            p->prim = it->second;
            return p;
        }
        case Value::Kind::List:
            p->type = PType::Seq;
            p->items = CompileItems(j);
            return p;
        case Value::Kind::Map: break;
        default: BadShape("pattern must be string/list/dict");
    }

    const Value* tv = j.Find("t");
    if (!tv || !tv->IsString()) BadShape("dict node missing 't' key");
    const std::string& t = tv->AsString();
    if (t == "ref" && g_format2) {  // the shared pattern itself: one object for every use
        const Value* to = j.Find("to");
        if (!to || !to->IsString()) BadShape("ref missing 'to'");
        if (g_sharedCompiled) {
            auto it = g_sharedCompiled->find(to->AsString());
            if (it != g_sharedCompiled->end()) return it->second;
        }
        const Value* body = g_shared ? g_shared->Find(to->AsString()) : nullptr;
        if (!body) BadShape("ref to an unknown shared pattern: " + to->AsString());
        std::shared_ptr<const Pattern> sp = Compile(*body);
        if (g_sharedCompiled) (*g_sharedCompiled)[to->AsString()] = sp;
        return sp;
    }
    auto prim = PrimNames().find(t);
    if (prim != PrimNames().end() && (g_format2 || !IsFormat2Prim(prim->second))) {
        p->prim = prim->second;
        LeafAttrs(j, p.get());
        if (g_format2 && p->prim == Prim::Fourcc) {
            if (const Value* tr = j.Find("trim"); tr && tr->kind() == Value::Kind::Bool) p->fourccTrim = tr->AsBool();
            if (const Value* o = j.Find("other"); o && o->IsString()) p->fourccHex = o->AsString() == "hex";
        }
        if (const Value* as = j.Find("as"); as && as->IsString()) p->bindAs = as->AsString();
        if (const Value* e = j.Find("enum"); e && e->kind() == Value::Kind::Map) {
            p->enumMap.emplace();
            for (const auto& m : e->Members()) {
                if (m.second.IsString()) (*p->enumMap)[m.first] = m.second.AsString();
            }
        }
        if (p->prim == Prim::Scaled) {
            if (const Value* r = j.Find("read"); r && r->IsString()) p->scaledRead = r->AsString();
            auto num = [&j](const char* key, double dflt) {
                const Value* v = j.Find(key);
                if (!v) return dflt;
                if (v->kind() == Value::Kind::Int) return static_cast<double>(v->AsInt());
                if (v->kind() == Value::Kind::Float) return v->AsFloat();
                return dflt;
            };
            p->scaledOffset = num("offset", 0.0);
            p->scaledDivisor = num("divisor", 1.0);
        }
        if (p->prim == Prim::ValueRef) {
            if (const Value* r = j.Find("ref")) p->refExpr = *r;
            if (const Value* f = j.Find("as_formid"); f && f->kind() == Value::Kind::Bool) p->refAsFormid = f->AsBool();
        }
        return p;
    }

    if (t == "seq") {
        const Value& d = Required(j, "do", "seq");
        if (d.kind() != Value::Kind::List) BadShape("seq 'do' must be a list");
        p->type = PType::Seq;
        p->items = CompileItems(d);
        if (const Value* u = j.Find("unwrap"); u && u->kind() == Value::Kind::Bool) p->unwrap = u->AsBool();
    } else if (t == "repeat") {
        p->type = PType::Repeat;
        p->body = Compile(Required(j, "do", "repeat"));
        p->nPfx = Width(j.Find("n_pfx"));
        if (const Value* n = j.Find("n")) p->n = *n;
    } else if (t == "union") {
        p->type = PType::Union;
        const Value* on = j.Find("on");
        if (!on || !on->IsString()) BadShape("union missing 'on'");
        p->on = on->AsString();
        if (const Value* mask = j.Find("mask")) {
            int64_t m;
            if (!mask->ToInt(&m)) BadShape("union 'mask' must be an integer");
            p->mask = m;
        }
        if (const Value* cs = j.Find("cases"); cs && cs->kind() == Value::Kind::Map) {
            for (const auto& m : cs->Members()) {
                Pattern::CaseRange r;
                if (g_format2 && ParseCaseRange(m.first, &r.lo, &r.hi)) {
                    r.arm = Compile(m.second);
                    p->ranges.push_back(std::move(r));
                } else {
                    p->cases[m.first] = Compile(m.second);
                }
            }
        }
        if (const Value* d = j.Find("default")) p->deflt = Compile(*d);
        if (g_format2) {
            if (const Value* w = j.Find("write"); w && w->IsString()) p->write = w->AsString();
            int width;
            size_t off;
            if (ParsePeek(p->on.substr(1), &width, &off) && p->on[0] == '$') {
                p->peekWidth = width;
                p->peekOffset = off;
            }
        }
    } else if (t == "opt") {
        p->type = PType::Opt;
        p->body = Compile(Required(j, "do", "opt"));
    } else if (t == "skip") {
        p->type = PType::Skip;
        p->n = Required(j, "n", "skip");
    } else if (t == "bytes") {
        p->type = PType::Bytes;
        if (const Value* n = j.Find("n")) p->n = *n;
        LeafAttrs(j, p.get());
    } else if (t == "window") {
        p->type = PType::Window;
        p->body = Compile(Required(j, "do", "window"));
        p->nPfx = Width(j.Find("n_pfx"));
        if (const Value* n = j.Find("n")) p->n = *n;
    } else if (t == "bitfield") {
        p->type = PType::Bitfield;
        int w = Width(j.Find("size"));
        p->size = w ? w : 4;
        if (const Value* bs = j.Find("bits"); bs && bs->kind() == Value::Kind::Map) {
            for (const auto& m : bs->Members()) {
                const std::string& k = m.first;
                if (k.empty() || k.size() > 3 || k.find_first_not_of("0123456789") != std::string::npos ||
                    std::stoi(k) > 255) {
                    BadShape("bitfield bit key not a uint: " + k);
                }
                if (!m.second.IsString()) BadShape("bitfield value not a string for bit " + k);
                p->bits[std::stoi(k)] = m.second.AsString();
            }
        }
        if (const Value* as = j.Find("as"); as && as->IsString()) p->bindAs = as->AsString();
        LeafAttrs(j, p.get());
        if (g_format2) {
            p->bitNames = true;
            const Value* u = j.Find("unnamed");
            p->showUnnamed = u && u->IsString() ? u->AsString() == "show" : g_unnamedShow;
        }
    } else if (t == "packed" && g_format2) {
        CompilePacked(j, p.get());
        if (p->restMask) p->body = RestMember(*p);  // the "_rest" leaf's pattern
    } else {
        throw PatternError(false, "Unknown structural node type: " + t);
    }
    return p;
}

// ---- decoding ---------------------------------------------------------------

struct Cursor {
    const uint8_t* data;
    size_t pos;
    size_t end;

    size_t Remaining() const { return end > pos ? end - pos : 0; }
    bool AtEnd() const { return pos >= end; }
    const uint8_t* Read(size_t n) {
        if (pos + n > end) {
            throw PatternError(true, "Overread: need " + std::to_string(n) + " bytes at " + std::to_string(pos) +
                                         ", window end " + std::to_string(end));
        }
        const uint8_t* p = data + pos;
        pos += n;
        return p;
    }
    uint8_t U8() { return *Read(1); }
    uint16_t U16() { const uint8_t* b = Read(2); return static_cast<uint16_t>(b[0] | (b[1] << 8)); }
    uint32_t U32() {
        const uint8_t* b = Read(4);
        return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
               (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    }
    float F32() { return BitsToFloat(U32()); }
    uint32_t ReadUint(int width) { return width == 1 ? U8() : width == 2 ? U16() : U32(); }
    uint64_t U64() {
        const uint8_t* b = Read(8);
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = v << 8 | b[i];
        return v;
    }
};

class Scope {
public:
    explicit Scope(const DecodeContext& ctx) {
        frames_.emplace_back();
        auto& root = frames_.back();
        root.emplace_back("@RT", Value::Str(ctx.rt));
        root.emplace_back("@FSIG", Value::Str(ctx.fsig));
        root.emplace_back("@FormVersion", Value::Int(ctx.formVersion));
        root.emplace_back("@Localized", Value::Bool(ctx.localized));
        if (ctx.prev) root.emplace_back("@Prev", Value::Str(*ctx.prev));
        for (const auto& [name, v] : ctx.vars) root.emplace_back("@" + name, v);
    }
    void Bind(const std::string& name, Value v) { frames_.back().emplace_back(name, std::move(v)); }
    const Value* Lookup(std::string_view name) const {
        for (auto f = frames_.rbegin(); f != frames_.rend(); ++f) {
            for (auto it = f->rbegin(); it != f->rend(); ++it) {
                if (it->first == name) return &it->second;
            }
        }
        return nullptr;
    }
    const Value& Require(std::string_view name) const {
        const Value* v = Lookup(name);
        if (!v) throw PatternError(false, "Undefined $name in scope: " + std::string(name));
        return *v;
    }
    void Push() { frames_.emplace_back(); }
    void Pop() {
        if (frames_.size() > 1) frames_.pop_back();
    }
    std::vector<TraceLeaf>* trace = nullptr;  // set by TraceSubrecord: also report where each leaf's bytes are

    // A literal, a "$name" / "$@KEY" lookup, or one of the meta-refs.
    Value Resolve(const Value& raw, const Cursor& cur) const {
        if (raw.IsString() && !raw.AsString().empty() && raw.AsString()[0] == '$') {
            std::string_view name(raw.AsString());
            name.remove_prefix(1);
            if (name == "_remaining") return Value::Int(static_cast<int64_t>(cur.Remaining()));
            if (name == "_pos") return Value::Int(static_cast<int64_t>(cur.pos));
            int peek = name == "_peek_u8" ? 1 : name == "_peek_u16" ? 2 : name == "_peek_u32" ? 4 : 0;
            if (peek) {
                if (cur.Remaining() < static_cast<size_t>(peek)) {
                    throw PatternError(true, "Overread: peek " + std::to_string(peek) + " at " + std::to_string(cur.pos));
                }
                Cursor c = cur;
                return Value::Int(c.ReadUint(peek));
            }
            if (name.substr(0, 10) == "_popcount_") {
                const Value& v = Require(name.substr(10));
                int64_t i;
                if (!v.ToInt(&i)) BadShape("$_popcount_: scope variable is not an integer");
                return Value::Int(static_cast<int64_t>(std::bitset<64>(static_cast<uint64_t>(i)).count()));
            }
            return Require(name);
        }
        if (raw.kind() == Value::Kind::List || raw.kind() == Value::Kind::Map) BadShape("cannot resolve as scalar");
        return raw;
    }

private:
    std::vector<std::vector<std::pair<std::string, Value>>> frames_;
};

int64_t ResolveCount(const Value& n, Scope& scope, const Cursor& cur, const char* what) {
    Value r = scope.Resolve(n, cur);
    int64_t i;
    if (!r.ToInt(&i)) BadShape(std::string(what) + " 'n' must resolve to an integer");
    if (i < 0) BadShape(std::string(what) + " 'n' negative");
    return i;
}

std::string HexBytes(const uint8_t* b, size_t n) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(kHex[b[i] >> 4]);
        s.push_back(kHex[b[i] & 15]);
    }
    return s;
}

std::string FormidText(uint32_t raw) {
    char buf[16];
    snprintf(buf, sizeof(buf), "[%08X]", raw);
    return buf;
}

std::string ReadZ(Cursor& cur) {
    size_t start = cur.pos;
    size_t z = start;
    while (z < cur.end && cur.data[z] != 0) ++z;
    std::string s = TextToUtf8(cur.data + start, z - start);
    cur.pos = z < cur.end ? z + 1 : z;
    return s;
}

// f32 degrees from f32 radians, as xEdit (and the Rust engine) compute them.
double Rad2Deg(float rad) {
    float deg = rad * (180.0f / 3.14159265358979323846f);
    if (deg < 0.0f) deg += 360.0f;
    if (deg == 0.0f) deg = 0.0f;
    return deg;
}

Value ExecPrim(const Pattern& node, Cursor& cur, Scope& scope) {
    std::optional<int64_t> rawInt;  // what "as" binds for enum/formid/lstring
    Value v;
    switch (node.prim) {
        case Prim::U8: v = Value::Int(cur.U8()); break;
        case Prim::U16: v = Value::Int(cur.U16()); break;
        case Prim::U32: v = Value::Int(cur.U32()); break;
        case Prim::I8: v = Value::Int(static_cast<int8_t>(cur.U8())); break;
        case Prim::I16: v = Value::Int(static_cast<int16_t>(cur.U16())); break;
        case Prim::I32: v = Value::Int(static_cast<int32_t>(cur.U32())); break;
        case Prim::F32: v = Value::Float(cur.F32()); break;
        case Prim::FloatRad2Deg: v = Value::Float(Rad2Deg(cur.F32())); break;
        case Prim::FloatDefault:
        case Prim::FloatDefaultRad2Deg: {
            uint32_t raw = cur.U32();
            if (raw == 0x7F7FFFFF) v = Value::Str("Default");
            else if (node.prim == Prim::FloatDefault) v = Value::Float(BitsToFloat(raw));
            else v = Value::Float(Rad2Deg(BitsToFloat(raw)));
            break;
        }
        case Prim::FloatColor255: {
            float r = std::round(cur.F32() * 255.0f);
            // Rust `as i64`: saturating, NaN -> 0.
            int64_t i = std::isnan(r) ? 0 : r >= 9.2233720368547758e18f ? INT64_MAX
                      : r <= -9.2233720368547758e18f ? INT64_MIN : static_cast<int64_t>(r);
            v = Value::Int(i);
            break;
        }
        case Prim::Bool: v = Value::Bool(cur.U8() != 0); break;
        case Prim::HexU8: {
            char buf[8];
            snprintf(buf, sizeof(buf), "0x%02X", cur.U8());
            v = Value::Str(buf);
            break;
        }
        case Prim::EmptyMarker: break;
        case Prim::Lstring: {
            const Value* loc = scope.Lookup("@Localized");
            bool localized = loc && loc->kind() == Value::Kind::Bool ? loc->AsBool() : true;
            if (localized) {
                uint32_t raw = cur.U32();
                rawInt = raw;
                char buf[32];
                snprintf(buf, sizeof(buf), "[LSTRING:0x%08X]", raw);
                v = Value::Str(buf);
            } else {
                v = Value::Str(ReadZ(cur));
            }
            break;
        }
        case Prim::Formid: {
            uint32_t raw = cur.U32();
            rawInt = raw;
            v = Value::Str(FormidText(raw));
            break;
        }
        case Prim::I32Div100: v = Value::Float(static_cast<int32_t>(cur.U32()) / 100.0); break;
        case Prim::I16Div100: v = Value::Float(static_cast<int16_t>(cur.U16()) / 100.0); break;
        case Prim::U16Div65535: v = Value::Float(cur.U16() / 65535.0); break;
        case Prim::U8Div100: v = Value::Float(cur.U8() / 100.0); break;
        case Prim::U16Div100: v = Value::Float(cur.U16() / 100.0); break;
        case Prim::Fourcc: {
            // Four characters; all zero bytes read as "" (no code).
            const uint8_t* b = cur.Read(4);
            std::string code(reinterpret_cast<const char*>(b), 4);
            if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) code.clear();
            if (!code.empty() && node.fourccTrim) {
                while (!code.empty() && code.back() == '\0') code.pop_back();
            }
            if (!code.empty() && node.fourccHex &&
                std::any_of(code.begin(), code.end(), [](char c) { return c < 0x20 || c > 0x7E; })) {
                char buf[16];
                snprintf(buf, sizeof(buf), "0x%08X", static_cast<uint32_t>(b[0] | b[1] << 8 | b[2] << 16) | static_cast<uint32_t>(b[3]) << 24);
                code = buf;
            }
            v = Value::Str(code);
            break;
        }
        case Prim::U64: {
            uint64_t u = cur.U64();
            if (u <= static_cast<uint64_t>(INT64_MAX)) {
                v = Value::Int(static_cast<int64_t>(u));
            } else {  // beyond the engine's signed integer: hex text
                char buf[24];
                snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(u));
                v = Value::Str(buf);
            }
            break;
        }
        case Prim::I64: v = Value::Int(static_cast<int64_t>(cur.U64())); break;
        case Prim::F32Div100: v = Value::Float(static_cast<double>(cur.F32()) / 100.0); break;
        case Prim::U32Div100: v = Value::Float(cur.U32() / 100.0); break;
        case Prim::Scaled: {
            const std::string& r = node.scaledRead;
            double raw;
            if (r == "uint8") raw = cur.U8();
            else if (r == "int8") raw = static_cast<int8_t>(cur.U8());
            else if (r == "uint16") raw = cur.U16();
            else if (r == "int16") raw = static_cast<int16_t>(cur.U16());
            else if (r == "uint32") raw = cur.U32();
            else if (r == "int32") raw = static_cast<int32_t>(cur.U32());
            else if (r == "float32") raw = cur.F32();
            else throw PatternError(false, "scaled: unknown 'read' type: " + r);
            v = Value::Float(node.scaledDivisor != 0.0 ? (raw - node.scaledOffset) / node.scaledDivisor
                                                       : raw - node.scaledOffset);
            break;
        }
        case Prim::ValueRef: {
            if (!node.refExpr) throw PatternError(false, "value_ref missing 'ref'");
            if (!node.refExpr->IsString() || node.refExpr->AsString().empty() || node.refExpr->AsString()[0] != '$') {
                throw PatternError(false, "value_ref requires 'ref' starting with $");
            }
            v = scope.Resolve(*node.refExpr, cur);
            int64_t i;
            if (node.refAsFormid && v.ToInt(&i) && i >= 0 && i <= 0xFFFFFFFFLL) {
                v = Value::Str(FormidText(static_cast<uint32_t>(i)));
            }
            break;
        }
        case Prim::ColorRgb:
        case Prim::ColorRgba: {
            size_t n = node.prim == Prim::ColorRgb ? 3 : 4;
            const uint8_t* b = cur.Read(n);
            std::vector<Value> c;
            for (size_t k = 0; k < n; ++k) c.push_back(Value::Int(b[k]));
            v = Value::List(std::move(c));
            break;
        }
        case Prim::Zstring: v = Value::Str(ReadZ(cur)); break;
        case Prim::Str8:
        case Prim::Str16:
        case Prim::Str32: {
            size_t n = cur.ReadUint(node.prim == Prim::Str8 ? 1 : node.prim == Prim::Str16 ? 2 : 4);
            const uint8_t* b = cur.Read(n);
            v = Value::Str(TextToUtf8(b, n));
            break;
        }
        case Prim::EnumU8:
        case Prim::EnumU16:
        case Prim::EnumU32: {
            uint32_t raw = cur.ReadUint(node.prim == Prim::EnumU8 ? 1 : node.prim == Prim::EnumU16 ? 2 : 4);
            rawInt = raw;
            if (node.enumMap) {
                auto it = node.enumMap->find(std::to_string(raw));
                if (it != node.enumMap->end()) {
                    v = Value::Str(it->second);
                } else {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "0x%X", raw);
                    v = Value::Str(buf);
                }
            } else {
                v = Value::Int(raw);
            }
            break;
        }
    }
    if (node.bindAs) scope.Bind(*node.bindAs, rawInt ? Value::Int(*rawInt) : v);
    return v;
}

// Puts a seq's or repeat's segment in front of the paths of the leaves its item
// reported, the way skqe's Select walks the decoded value.
void PrefixLeaves(std::vector<TraceLeaf>* leaves, size_t from, size_t to, const std::string& seg) {
    for (size_t i = from; i < to; ++i) {
        std::string& path = (*leaves)[i].path;
        if (path.empty()) path = seg;
        else if (path[0] == '[') path = seg + path;
        else path = seg + "." + path;
    }
}

// A packed member's bits, unsigned and shifted down.
uint32_t MemberBits(const Pattern& m, uint32_t word) { return (word & m.memberMask) >> m.lo; }

int64_t MemberSigned(const Pattern& m, uint32_t word) {
    int64_t v = MemberBits(m, word);
    if (m.nbits < 64 && (v >> (m.nbits - 1)) & 1) v -= int64_t{1} << m.nbits;
    return v;
}

// What `as` binds for a member.
Value MemberNumber(const Pattern& m, uint32_t word) {
    switch (m.member) {
        case MemberKind::Int: return Value::Int(MemberSigned(m, word));
        case MemberKind::Flags:
        case MemberKind::Rest: return Value::Int(word & m.memberMask);
        default: return Value::Int(MemberBits(m, word));
    }
}

// A member's decoded value.
Value MemberValue(const Pattern& m, uint32_t word) {
    switch (m.member) {
        case MemberKind::Uint: return Value::Int(MemberBits(m, word));
        case MemberKind::Int: return Value::Int(MemberSigned(m, word));
        case MemberKind::Bool: return Value::Bool(MemberBits(m, word) != 0);
        case MemberKind::Enum: {
            uint32_t raw = MemberBits(m, word);
            auto it = m.enumMap->find(std::to_string(raw));
            if (it != m.enumMap->end()) return Value::Str(it->second);
            char buf[16];
            snprintf(buf, sizeof(buf), "0x%X", raw);
            return Value::Str(buf);
        }
        case MemberKind::Flags: {
            std::vector<Value> flags;
            for (const auto& [bit, name] : m.bits)
                if (word & (1u << bit)) flags.push_back(Value::Str(name));
            return Value::List(std::move(flags));
        }
        case MemberKind::Rest: return Value::Int(word & m.memberMask);
    }
    return Value();
}

// nullopt = a skip, which an enclosing seq drops.
std::optional<Value> Exec(const Pattern& p, Cursor& cur, Scope& scope) {
    switch (p.type) {
        case PType::Prim: {
            size_t start = cur.pos;
            Value v = ExecPrim(p, cur, scope);
            if (scope.trace) scope.trace->push_back({"", start, cur.pos - start, &p, v, p.bindAs.has_value()});
            return v;
        }

        case PType::Seq: {
            std::vector<Value> results;
            std::vector<const std::optional<std::string>*> ids;
            bool labeled = false;
            std::vector<std::pair<size_t, size_t>> ranges;  // leaves each result reported
            for (const auto& item : p.items) {
                size_t mark = scope.trace ? scope.trace->size() : 0;
                std::optional<Value> r = Exec(*item.first, cur, scope);
                if (!r) continue;
                results.push_back(std::move(*r));
                ids.push_back(&item.second);
                if (item.second) labeled = true;
                if (scope.trace) ranges.emplace_back(mark, scope.trace->size());
            }
            if (scope.trace && (labeled || !p.unwrap || results.size() != 1)) {
                for (size_t i = 0; i < ranges.size(); ++i) {
                    std::string seg = labeled ? (*ids[i] ? **ids[i] : "_" + std::to_string(i)) : "[" + std::to_string(i) + "]";
                    PrefixLeaves(scope.trace, ranges[i].first, ranges[i].second, seg);
                }
            }
            if (labeled) {
                Value m = Value::Map();
                for (size_t i = 0; i < results.size(); ++i) {
                    m.Set(*ids[i] ? **ids[i] : "_" + std::to_string(i), std::move(results[i]));
                }
                return m;
            }
            if (p.unwrap && results.size() == 1) return std::move(results[0]);
            return Value::List(std::move(results));
        }

        case PType::Repeat: {
            std::optional<int64_t> count;
            if (p.nPfx) {
                size_t start = cur.pos;
                count = cur.ReadUint(p.nPfx);
                // Not part of the decoded value, but an edit of the list must know it.
                if (scope.trace) scope.trace->push_back({"#count", start, cur.pos - start, nullptr, Value::Int(*count), true});
            } else if (p.n) count = ResolveCount(*p.n, scope, cur, "repeat");
            std::vector<Value> items;
            if (count) {
                // The count comes from the data. Far more items than bytes
                // left means damaged data (an element that reads nothing would
                // otherwise repeat billions of times).
                if (*count > static_cast<int64_t>(cur.Remaining()) + 1024) {
                    throw PatternError(false, "repeat: count " + std::to_string(*count) + " exceeds the data");
                }
                for (int64_t k = 0; k < *count; ++k) {
                    scope.Push();
                    std::optional<Value> v;
                    size_t mark = scope.trace ? scope.trace->size() : 0;
                    try {
                        v = Exec(*p.body, cur, scope);
                    } catch (...) {
                        scope.Pop();
                        throw;
                    }
                    scope.Pop();
                    if (scope.trace) PrefixLeaves(scope.trace, mark, scope.trace->size(), "[" + std::to_string(k) + "]");
                    items.push_back(v ? std::move(*v) : Value());
                }
            } else {
                // Until the window ends; a short final element ends the list
                // (rolled back), it is not an error.
                while (!cur.AtEnd()) {
                    size_t start = cur.pos;
                    scope.Push();
                    std::optional<Value> v;
                    size_t mark = scope.trace ? scope.trace->size() : 0;
                    try {
                        v = Exec(*p.body, cur, scope);
                    } catch (const PatternError& e) {
                        scope.Pop();
                        if (!e.overread()) throw;
                        cur.pos = start;
                        if (scope.trace) scope.trace->resize(mark);  // rolled back
                        break;
                    }
                    scope.Pop();
                    if (scope.trace) {
                        PrefixLeaves(scope.trace, mark, scope.trace->size(), "[" + std::to_string(items.size()) + "]");
                    }
                    items.push_back(v ? std::move(*v) : Value());
                    if (cur.pos == start) break;
                }
            }
            return Value::List(std::move(items));
        }

        case PType::Union: {
            Value on;
            if (p.peekWidth) {  // format 2 read-ahead, possibly at an offset
                if (cur.Remaining() < p.peekOffset + p.peekWidth) {
                    throw PatternError(true, "Overread: peek " + std::to_string(p.peekWidth) + " at " +
                                                 std::to_string(cur.pos + p.peekOffset));
                }
                Cursor c = cur;
                c.pos += p.peekOffset;
                on = Value::Int(c.ReadUint(p.peekWidth));
            } else {
                on = scope.Resolve(Value::Str(p.on), cur);
            }
            if (p.mask) {
                int64_t i;
                if (!on.ToInt(&i)) BadShape("union 'mask' on a value that is not an integer");
                on = Value::Int(i & *p.mask);
            }
            std::string key = on.LookupKey();
            auto it = p.cases.find(key);
            const Pattern* c = it != p.cases.end() ? it->second.get() : nullptr;
            if (!c && on.kind() == Value::Kind::Int) {
                for (const auto& r : p.ranges) {
                    if ((!r.lo || on.AsInt() >= *r.lo) && (!r.hi || on.AsInt() <= *r.hi)) {
                        c = r.arm.get();
                        break;
                    }
                }
            }
            if (!c) c = p.deflt.get();
            if (!c) BadShape("union: no case for key " + key);
            return Exec(*c, cur, scope);
        }

        case PType::Opt:
            if (cur.AtEnd()) return Value();
            return Exec(*p.body, cur, scope);

        case PType::Skip: {
            int64_t n = ResolveCount(*p.n, scope, cur, "skip");
            if (n > 0) cur.Read(static_cast<size_t>(n));
            return std::nullopt;
        }

        case PType::Bytes: {
            size_t n = p.n ? static_cast<size_t>(ResolveCount(*p.n, scope, cur, "bytes")) : cur.Remaining();
            size_t start = cur.pos;
            const uint8_t* b = cur.Read(n);
            Value v = Value::Str(HexBytes(b, n));
            if (scope.trace) scope.trace->push_back({"", start, n, &p, v, false});
            return v;
        }

        case PType::Window: {
            size_t len;
            if (p.nPfx) len = cur.ReadUint(p.nPfx);
            else if (p.n) len = static_cast<size_t>(ResolveCount(*p.n, scope, cur, "window"));
            else BadShape("window: need 'n' or 'n_pfx'");
            size_t subEnd = cur.pos + len;
            if (subEnd > cur.end) BadShape("window: subwindow extends past parent end");
            size_t savedEnd = cur.end;
            cur.end = subEnd;
            std::optional<Value> r;
            try {
                r = Exec(*p.body, cur, scope);  // parent scope, no push
            } catch (...) {
                cur.end = savedEnd;
                cur.pos = subEnd;
                throw;
            }
            cur.end = savedEnd;
            cur.pos = subEnd;  // always skip the whole window
            return r ? std::move(*r) : Value();
        }

        case PType::Packed: {
            size_t start = cur.pos;
            uint32_t word = cur.ReadUint(p.size);
            Value m = Value::Map();
            for (const auto& mp : p.members) {
                Value v = MemberValue(*mp, word);
                if (mp->bindAs) scope.Bind(*mp->bindAs, MemberNumber(*mp, word));
                if (scope.trace) {
                    scope.trace->push_back({mp->memberId, start, cur.pos - start, mp.get(), v,
                                            mp->bindAs.has_value() || p.bindAs.has_value()});
                }
                m.Set(mp->memberId, std::move(v));
            }
            if (word & p.restMask) {
                Value v = Value::Int(word & p.restMask);
                if (scope.trace) scope.trace->push_back({"_rest", start, cur.pos - start, p.body.get(), v, p.bindAs.has_value()});
                m.Set("_rest", std::move(v));
            }
            if (p.bindAs) scope.Bind(*p.bindAs, Value::Int(word));
            return m;
        }

        case PType::PackedMember: BadShape("a packed member outside its word");

        case PType::Bitfield: {
            size_t start = cur.pos;
            uint32_t raw = cur.ReadUint(p.size);
            std::vector<Value> flags;
            if (p.showUnnamed) {  // every set bit, in bit order; a bit without a name as bit<N>
                for (int bit = 0; bit < 8 * p.size; ++bit) {
                    if (!(raw & (1u << bit))) continue;
                    auto it = p.bits.find(bit);
                    flags.push_back(Value::Str(it != p.bits.end() ? it->second : "bit" + std::to_string(bit)));
                }
            } else {
                for (const auto& [bit, name] : p.bits) {
                    if (bit < 32 && (raw & (1u << bit))) flags.push_back(Value::Str(name));
                }
            }
            if (p.bindAs) scope.Bind(*p.bindAs, Value::Int(raw));
            Value v = Value::List(std::move(flags));
            if (scope.trace) scope.trace->push_back({"", start, cur.pos - start, &p, v, p.bindAs.has_value()});
            return v;
        }
    }
    return Value();
}

}  // namespace

std::shared_ptr<const Pattern> CompilePattern(const Value& json) { return Compile(json); }

std::shared_ptr<const Pattern> CompilePattern(const Value& json, bool format2) { return CompilePattern(json, format2, false); }

std::shared_ptr<const Pattern> CompilePattern(const Value& json, bool format2, bool unnamedShow) {
    return CompilePattern(json, format2, unnamedShow, nullptr, nullptr);
}

std::shared_ptr<const Pattern> CompilePattern(const Value& json, bool format2, bool unnamedShow, const Value* shared,
                                              std::map<std::string, std::shared_ptr<const Pattern>>* sharedCompiled) {
    struct Flag {
        bool saved = g_format2, savedShow = g_unnamedShow;
        const Value* savedShared = g_shared;
        std::map<std::string, std::shared_ptr<const Pattern>>* savedCompiled = g_sharedCompiled;
        ~Flag() {
            g_format2 = saved;
            g_unnamedShow = savedShow;
            g_shared = savedShared;
            g_sharedCompiled = savedCompiled;
        }
    } flag;
    g_format2 = format2;
    g_unnamedShow = unnamedShow;
    g_shared = shared;
    g_sharedCompiled = sharedCompiled;
    return Compile(json);
}

Value DecodeSubrecord(const Pattern& pattern, const uint8_t* data, size_t size, const DecodeContext& ctx,
                      size_t* consumed) {
    Cursor cur{data, 0, size};
    Scope scope(ctx);
    std::optional<Value> v = Exec(pattern, cur, scope);
    if (consumed) *consumed = cur.pos;
    return v ? std::move(*v) : Value();
}

Value TraceSubrecord(const Pattern& pattern, const uint8_t* data, size_t size, const DecodeContext& ctx,
                     std::vector<TraceLeaf>* leaves) {
    Cursor cur{data, 0, size};
    Scope scope(ctx);
    scope.trace = leaves;
    std::optional<Value> v = Exec(pattern, cur, scope);
    return v ? std::move(*v) : Value();
}

// ---- encoding one leaf -------------------------------------------------------

namespace {

bool Fail(std::string* err, std::string why) {
    *err = std::move(why);
    return false;
}

bool ToDouble(const Value& v, double* out) {
    switch (v.kind()) {
        case Value::Kind::Int: *out = static_cast<double>(v.AsInt()); return true;
        case Value::Kind::Float: *out = v.AsFloat(); return true;
        case Value::Kind::Bool: *out = v.AsBool() ? 1 : 0; return true;
        case Value::Kind::String: {
            char* end = nullptr;
            const std::string& s = v.AsString();
            double d = strtod(s.c_str(), &end);
            if (s.empty() || *end) return false;
            *out = d;
            return true;
        }
        default: return false;
    }
}

bool ToInteger(const Value& v, int64_t* out) {
    if (v.ToInt(out)) return true;
    if (v.IsString()) {
        const std::string& s = v.AsString();
        char* end = nullptr;
        long long i = strtoll(s.c_str(), &end, 0);  // decimal or 0x...
        if (s.empty() || *end) return false;
        *out = i;
        return true;
    }
    return false;
}

void PutLE(uint64_t v, int width, std::vector<uint8_t>* out) {
    for (int i = 0; i < width; ++i) out->push_back(static_cast<uint8_t>(v >> (8 * i)));
}

bool PutInt(const Value& in, int width, bool isSigned, std::vector<uint8_t>* out, std::string* err) {
    int64_t i;
    if (!ToInteger(in, &i)) return Fail(err, "expected an integer");
    int64_t lo = isSigned ? -(int64_t{1} << (width * 8 - 1)) : 0;
    int64_t hi = isSigned ? (int64_t{1} << (width * 8 - 1)) - 1 : (int64_t{1} << (width * 8)) - 1;
    if (i < lo || i > hi) return Fail(err, std::to_string(i) + " does not fit " + std::to_string(width) + " bytes");
    PutLE(static_cast<uint64_t>(i), width, out);
    return true;
}

bool PutRounded(double d, int width, bool isSigned, std::vector<uint8_t>* out, std::string* err) {
    if (std::isnan(d) || std::isinf(d)) return Fail(err, "not a finite number");
    return PutInt(Value::Int(static_cast<int64_t>(std::llround(d))), width, isSigned, out, err);
}

void PutFloat(double d, std::vector<uint8_t>* out) {
    float f = static_cast<float>(d);
    uint32_t u;
    std::memcpy(&u, &f, 4);
    PutLE(u, 4, out);
}

// UTF-8 -> what a plugin stores: the encoding the old text was in when that
// shows (non-ASCII bytes that are valid UTF-8 stay UTF-8), otherwise
// Windows-1252 when every character has a place there (what the game's English
// text uses), otherwise UTF-8.
std::string TextToFile(const std::string& utf8, const uint8_t* old, size_t oldSize) {
    if (std::all_of(utf8.begin(), utf8.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; })) return utf8;
    if (old && std::any_of(old, old + oldSize, [](uint8_t b) { return b >= 0x80; }) &&
        IsValidUtf8(old, oldSize)) {
        return utf8;
    }
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        size_t n = c < 0x80 ? 1 : c >> 5 == 6 ? 2 : c >> 4 == 14 ? 3 : c >> 3 == 30 ? 4 : 0;
        if (!n || i + n > utf8.size()) return utf8;
        uint32_t cp = n == 1 ? c : n == 2 ? c & 0x1Fu : n == 3 ? c & 0x0Fu : c & 0x07u;
        for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
        i += n;
        if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) {
            out.push_back(static_cast<char>(cp));
            continue;
        }
        size_t slot = 32;
        for (size_t k = 0; k < 32; ++k)
            if (kCp1252High[k] == cp) slot = k;
        if (slot == 32) return utf8;
        out.push_back(static_cast<char>(0x80 + slot));
    }
    return out;
}

// Puts one packed member's value into `word`. `edit`: flags also take
// {"add":[...],"remove":[...]}.
bool PutMember(const Pattern& m, const Value& in, uint32_t* word, bool edit, std::string* err) {
    uint32_t bits = 0;
    switch (m.member) {
        case MemberKind::Uint:
        case MemberKind::Int:
        case MemberKind::Enum:
        case MemberKind::Bool: {
            int64_t i = 0;
            if (m.member == MemberKind::Bool && in.kind() == Value::Kind::Bool) {
                i = in.AsBool() ? 1 : 0;
            } else if (m.member == MemberKind::Enum && in.IsString() && m.enumMap &&
                       std::any_of(m.enumMap->begin(), m.enumMap->end(), [&](const auto& e) { return e.second == in.AsString(); })) {
                for (const auto& e : *m.enumMap)
                    if (e.second == in.AsString()) i = std::stoll(e.first);
            } else if (!ToInteger(in, &i)) {
                return Fail(err, m.member == MemberKind::Enum ? "expected a name from the list or a number" : "expected an integer");
            }
            int64_t lo = 0, hi = (int64_t{1} << m.nbits) - 1;
            if (m.member == MemberKind::Int) {
                lo = -(int64_t{1} << (m.nbits - 1));
                hi = (int64_t{1} << (m.nbits - 1)) - 1;
            }
            if (m.member == MemberKind::Bool) hi = 1;
            if (i < lo || i > hi) return Fail(err, std::to_string(i) + " does not fit " + std::to_string(m.nbits) + " bits");
            bits = (static_cast<uint32_t>(static_cast<uint64_t>(i)) << m.lo) & m.memberMask;
            break;
        }
        case MemberKind::Flags: {
            auto bitOf = [&m](const Value& n, uint32_t* b) {
                if (!n.IsString()) return false;
                for (const auto& [bit, name] : m.bits)
                    if (name == n.AsString()) {
                        *b = 1u << bit;
                        return true;
                    }
                return false;
            };
            uint32_t cur = *word & m.memberMask;
            if (edit && in.kind() == Value::Kind::Map) {
                for (const char* part : {"add", "remove"}) {
                    const Value* list = in.Find(part);
                    if (!list) continue;
                    for (const auto& n : list->Items()) {
                        uint32_t b;
                        if (!bitOf(n, &b)) return Fail(err, "no flag named " + (n.IsString() ? n.AsString() : std::string("?")));
                        cur = part[0] == 'a' ? cur | b : cur & ~b;
                    }
                }
                bits = cur;
            } else if (in.kind() == Value::Kind::List) {
                for (const auto& n : in.Items()) {
                    uint32_t b;
                    if (!bitOf(n, &b)) return Fail(err, "no flag named " + (n.IsString() ? n.AsString() : std::string("?")));
                    bits |= b;
                }
            } else {
                return Fail(err, "expected flag names");
            }
            break;
        }
        case MemberKind::Rest: {
            int64_t i;
            if (!ToInteger(in, &i)) return Fail(err, "expected an integer");
            if (i < 0 || (static_cast<uint64_t>(i) & ~static_cast<uint64_t>(m.memberMask)) != 0) return Fail(err, "_rest has bits a member owns");
            bits = static_cast<uint32_t>(i);
            break;
        }
    }
    *word = (*word & ~m.memberMask) | bits;
    return true;
}

bool FromHex(const std::string& s, std::vector<uint8_t>* out) {
    if (s.size() % 2) return false;
    auto nib = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = nib(s[i]), b = nib(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out->push_back(static_cast<uint8_t>(a << 4 | b));
    }
    return true;
}

}  // namespace

bool EncodeLeaf(const Pattern& p, const Value& in, const uint8_t* cur, size_t curSize, bool localized,
                std::vector<uint8_t>* out, std::string* err) {
    out->clear();
    if (in.IsString() && in.AsString().rfind("raw:", 0) == 0) {
        if (!FromHex(in.AsString().substr(4), out)) return Fail(err, "raw: needs hex digits");
        return true;
    }
    if (p.type == PType::PackedMember) {  // the whole word, with only this member's bits replaced
        uint32_t word = 0;
        for (size_t i = 0; i < curSize && i < static_cast<size_t>(p.size); ++i) word |= static_cast<uint32_t>(cur[i]) << (8 * i);
        if (!PutMember(p, in, &word, true, err)) return false;
        PutLE(word, p.size, out);
        return true;
    }
    if (p.type == PType::Packed) {  // a map of members, as decoded
        if (!in.IsNull() && in.kind() != Value::Kind::Map) return Fail(err, "expected an object of members");
        uint32_t word = 0;
        for (const auto& mp : p.members) {
            const Value* v = in.IsNull() ? nullptr : in.Find(mp->memberId);
            if (!v || v->IsNull()) {
                if (mp->member == MemberKind::Flags) continue;
                return Fail(err, "a value is missing: " + mp->memberId);
            }
            if (!PutMember(*mp, *v, &word, false, err)) return Fail(err, mp->memberId + ": " + *err);
        }
        if (const Value* r = in.IsNull() ? nullptr : in.Find("_rest"); r && !r->IsNull()) {
            int64_t i;
            if (!ToInteger(*r, &i)) return Fail(err, "_rest: expected an integer");
            uint64_t wordMask = p.size == 4 ? 0xFFFFFFFFull : (1ull << (8 * p.size)) - 1;
            if (i < 0 || (static_cast<uint64_t>(i) & ~(static_cast<uint64_t>(p.restMask))) != 0 || static_cast<uint64_t>(i) > wordMask) {
                return Fail(err, "_rest has bits a member owns");
            }
            word |= static_cast<uint32_t>(i);
        }
        PutLE(word, p.size, out);
        return true;
    }
    if (p.type == PType::Bytes) {
        if (!in.IsString() || !FromHex(in.AsString(), out)) return Fail(err, "expected hex digits");
        return true;
    }
    if (p.type == PType::Bitfield) {
        uint32_t raw = 0;
        for (size_t i = 0; i < curSize && i < 4; ++i) raw |= static_cast<uint32_t>(cur[i]) << (8 * i);
        auto bitOf = [&p](const std::string& name, uint32_t* bit) {
            for (const auto& b : p.bits) {
                if (b.second == name && b.first < 32) {
                    *bit = 1u << b.first;
                    return true;
                }
            }
            // Format 2: bit<N> for any bit of the word.
            if (p.bitNames && name.size() > 3 && name.size() <= 5 && name.compare(0, 3, "bit") == 0 &&
                name.find_first_not_of("0123456789", 3) == std::string::npos && (name.size() == 4 || name[3] != '0')) {
                int n = std::stoi(name.substr(3));
                if (n < 8 * p.size) {
                    *bit = 1u << n;
                    return true;
                }
            }
            return false;
        };
        auto apply = [&](const Value& list, bool set) {
            for (const auto& n : list.Items()) {
                uint32_t bit = 0;
                if (!n.IsString() || !bitOf(n.AsString(), &bit)) {
                    return Fail(err, "no flag named " + (n.IsString() ? n.AsString() : std::string("?")));
                }
                raw = set ? raw | bit : raw & ~bit;
            }
            return true;
        };
        if (in.kind() == Value::Kind::Map) {  // {"add":[...], "remove":[...]}
            if (const Value* a = in.Find("add"); a && !apply(*a, true)) return false;
            if (const Value* r = in.Find("remove"); r && !apply(*r, false)) return false;
        } else if (in.kind() == Value::Kind::List) {  // the named bits become exactly these; unnamed bits stay
            if (p.showUnnamed) {
                raw = 0;  // the value shows every set bit: the list is the whole word
            } else {
                for (const auto& b : p.bits)
                    if (b.first < 32) raw &= ~(1u << b.first);
            }
            if (!apply(in, true)) return false;
        } else {
            int64_t i;
            if (!ToInteger(in, &i)) return Fail(err, "expected flag names or an integer");
            if (i < 0 || i >= (int64_t{1} << (p.size * 8))) return Fail(err, "value does not fit the flags");
            raw = static_cast<uint32_t>(i);
        }
        PutLE(raw, p.size, out);
        return true;
    }
    if (p.type != PType::Prim) return Fail(err, "not a single value");
    double d = 0;
    auto needNumber = [&]() { return ToDouble(in, &d) || Fail(err, "expected a number"); };
    auto putText = [&](bool zero) {
        if (!in.IsString()) return Fail(err, "expected text");
        std::string t = TextToFile(in.AsString(), cur, curSize);
        out->insert(out->end(), t.begin(), t.end());
        if (zero) out->push_back(0);
        return true;
    };
    const float kRadToDeg = 180.0f / 3.14159265358979323846f;
    switch (p.prim) {
        case Prim::U8: return PutInt(in, 1, false, out, err);
        case Prim::U16: return PutInt(in, 2, false, out, err);
        case Prim::U32: return PutInt(in, 4, false, out, err);
        case Prim::I8: return PutInt(in, 1, true, out, err);
        case Prim::I16: return PutInt(in, 2, true, out, err);
        case Prim::I32: return PutInt(in, 4, true, out, err);
        case Prim::F32:
            if (!needNumber()) return false;
            PutFloat(d, out);
            return true;
        case Prim::FloatRad2Deg:
            if (!needNumber()) return false;
            PutFloat(static_cast<float>(d) / kRadToDeg, out);
            return true;
        case Prim::FloatDefault:
        case Prim::FloatDefaultRad2Deg:
            if (in.IsString() && in.AsString() == "Default") {
                PutLE(0x7F7FFFFF, 4, out);
                return true;
            }
            if (!needNumber()) return false;
            PutFloat(p.prim == Prim::FloatDefault ? d : static_cast<float>(d) / kRadToDeg, out);
            return true;
        case Prim::FloatColor255:
            if (!needNumber()) return false;
            PutFloat(d / 255.0, out);
            return true;
        case Prim::Bool:
            if (!needNumber()) return false;
            out->push_back(d != 0 ? 1 : 0);
            return true;
        case Prim::HexU8: return PutInt(in, 1, false, out, err);
        case Prim::EmptyMarker: return true;
        case Prim::Lstring:
            if (!localized) return putText(true);
            if (in.IsString() && in.AsString().rfind("[LSTRING:0x", 0) == 0) {
                PutLE(strtoul(in.AsString().c_str() + 11, nullptr, 16), 4, out);
                return true;
            }
            if (in.IsString() && !ToDouble(in, &d)) return Fail(err, "this plugin is localized: give a string ID, not text");
            return PutInt(in, 4, false, out, err);
        case Prim::Formid:
            if (in.IsString() && in.AsString().size() == 10 && in.AsString()[0] == '[') {
                PutLE(strtoul(in.AsString().c_str() + 1, nullptr, 16), 4, out);
                return true;
            }
            return PutInt(in, 4, false, out, err);
        case Prim::I32Div100: return needNumber() && PutRounded(d * 100, 4, true, out, err);
        case Prim::I16Div100: return needNumber() && PutRounded(d * 100, 2, true, out, err);
        case Prim::U16Div65535: return needNumber() && PutRounded(d * 65535, 2, false, out, err);
        case Prim::U8Div100: return needNumber() && PutRounded(d * 100, 1, false, out, err);
        case Prim::U16Div100: return needNumber() && PutRounded(d * 100, 2, false, out, err);
        case Prim::U32Div100: return needNumber() && PutRounded(d * 100, 4, false, out, err);
        case Prim::F32Div100:
            if (!needNumber()) return false;
            PutFloat(d * 100, out);
            return true;
        case Prim::Fourcc: {
            if (!in.IsString()) return Fail(err, "expected four characters");
            std::string c = in.AsString();
            if (p.fourccHex && c.size() == 10 && c[0] == '0' && (c[1] == 'x' || c[1] == 'X') &&
                c.find_first_not_of("0123456789abcdefABCDEF", 2) == std::string::npos) {
                PutLE(std::stoul(c.substr(2), nullptr, 16), 4, out);
                return true;
            }
            if (p.fourccTrim) {
                if (c.size() > 4) return Fail(err, "expected at most four characters");
            } else if (!c.empty() && c.size() != 4) {
                return Fail(err, "expected four characters");
            }
            c.resize(4, '\0');
            out->insert(out->end(), c.begin(), c.end());
            return true;
        }
        case Prim::U64: {
            uint64_t u;
            if (in.kind() == Value::Kind::Int) {
                if (in.AsInt() < 0) return Fail(err, std::to_string(in.AsInt()) + " does not fit 8 bytes");
                u = static_cast<uint64_t>(in.AsInt());
            } else if (in.IsString() && !in.AsString().empty() && in.AsString()[0] != '-') {
                char* end = nullptr;
                errno = 0;
                u = strtoull(in.AsString().c_str(), &end, 0);
                if (*end || errno == ERANGE) return Fail(err, "expected an integer that fits 8 bytes");
            } else {
                return Fail(err, "expected an integer");
            }
            PutLE(u, 8, out);
            return true;
        }
        case Prim::I64: {
            int64_t i;
            if (in.IsString()) {
                char* end = nullptr;
                errno = 0;
                i = strtoll(in.AsString().c_str(), &end, 0);
                if (in.AsString().empty() || *end || errno == ERANGE) return Fail(err, "expected an integer that fits 8 bytes");
            } else if (!in.ToInt(&i)) {
                return Fail(err, "expected an integer");
            }
            PutLE(static_cast<uint64_t>(i), 8, out);
            return true;
        }
        case Prim::Scaled: {
            if (!needNumber()) return false;
            double raw = d * (p.scaledDivisor != 0.0 ? p.scaledDivisor : 1.0) + p.scaledOffset;
            const std::string& r = p.scaledRead;
            if (r == "float32") {
                PutFloat(raw, out);
                return true;
            }
            int width = r == "uint8" || r == "int8" ? 1 : r == "uint16" || r == "int16" ? 2 : 4;
            return PutRounded(raw, width, r[0] == 'i', out, err);
        }
        case Prim::ColorRgb:
        case Prim::ColorRgba: {
            size_t n = p.prim == Prim::ColorRgb ? 3 : 4;
            if (in.kind() != Value::Kind::List || in.Items().size() != n) return Fail(err, "expected " + std::to_string(n) + " numbers");
            for (const auto& c : in.Items())
                if (!PutInt(c, 1, false, out, err)) return false;
            return true;
        }
        case Prim::Zstring: return putText(true);
        case Prim::Str8:
        case Prim::Str16:
        case Prim::Str32: {
            if (!in.IsString()) return Fail(err, "expected text");
            std::string t = TextToFile(in.AsString(), cur, curSize);
            int width = p.prim == Prim::Str8 ? 1 : p.prim == Prim::Str16 ? 2 : 4;
            if (width < 4 && t.size() >= (size_t{1} << (width * 8))) return Fail(err, "text too long for its length prefix");
            PutLE(t.size(), width, out);
            out->insert(out->end(), t.begin(), t.end());
            return true;
        }
        case Prim::EnumU8:
        case Prim::EnumU16:
        case Prim::EnumU32: {
            int width = p.prim == Prim::EnumU8 ? 1 : p.prim == Prim::EnumU16 ? 2 : 4;
            if (in.IsString() && p.enumMap) {
                for (const auto& e : *p.enumMap)
                    if (e.second == in.AsString()) return PutInt(Value::Str(e.first), width, false, out, err);
            }
            return PutInt(in, width, false, out, err);
        }
        case Prim::ValueRef: return Fail(err, "a computed value has no bytes of its own");
    }
    return Fail(err, "unknown type");
}

bool LeafIsFormId(const Pattern& p) { return p.type == PType::Prim && p.prim == Prim::Formid; }

const Value* LeafDefault(const Pattern& leaf) { return leaf.leafDefault ? &*leaf.leafDefault : nullptr; }

bool LeafRange(const Pattern& leaf, std::optional<double>* lo, std::optional<double>* hi) {
    if (!leaf.hasRange) return false;
    *lo = leaf.rangeLo;
    *hi = leaf.rangeHi;
    return true;
}

const std::vector<std::string>& LeafTargets(const Pattern& leaf) { return leaf.target; }

bool CheckDefaults(const Pattern& p, std::string* err) {
    if (p.leafDefault) {
        std::vector<uint8_t> out;
        std::string e;
        if (!EncodeLeaf(p, *p.leafDefault, nullptr, 0, true, &out, &e)) {
            *err = "default " + [&] { std::string s; WriteJson(*p.leafDefault, &s); return s; }() + " does not encode: " + e;
            return false;
        }
    }
    for (const auto& item : p.items)
        if (!CheckDefaults(*item.first, err)) return false;
    for (const auto& c : p.cases)
        if (!CheckDefaults(*c.second, err)) return false;
    for (const auto& r : p.ranges)
        if (!CheckDefaults(*r.arm, err)) return false;
    for (const auto& m : p.members)
        if (!CheckDefaults(*m, err)) return false;
    if (p.deflt && !CheckDefaults(*p.deflt, err)) return false;
    if (p.body && p.type != PType::Packed && !CheckDefaults(*p.body, err)) return false;
    return true;
}
bool LeafIsByteRun(const Pattern& p) { return p.type == PType::Bytes; }
std::string LeafTypeName(const Pattern& p) {
    if (p.type == PType::Bytes) return "bytes";
    if (p.type == PType::Packed) return "packed";
    if (p.type == PType::PackedMember) {
        switch (p.member) {
            case MemberKind::Uint: return "packed_uint";
            case MemberKind::Int: return "packed_int";
            case MemberKind::Bool: return "packed_bool";
            case MemberKind::Enum: return "packed_enum";
            case MemberKind::Flags: return "packed_flags";
            case MemberKind::Rest: return "packed_rest";
        }
    }
    if (p.type == PType::Bitfield) return "flags";
    if (p.type != PType::Prim) return "structure";
    for (const auto& e : PrimNames())
        if (e.second == p.prim && e.first != "float32_disp_rad2deg") return e.first;
    return "?";
}
bool PatternIsLeaf(const Pattern& p) {
    return p.type == PType::Prim || p.type == PType::Bytes || p.type == PType::Bitfield || p.type == PType::Packed;
}

bool IsPrimitiveName(const std::string& name) { return PrimNames().count(name) != 0; }

void PatternCatalog::Clear() {
    format_ = 1;
    warnings_.clear();
    byKey_.clear();
    lstringClass_.clear();
    display_.clear();
    names_.clear();
    multi_.clear();
    shared_ = records_ = options_ = meta_ = Value();
    sharedCompiled_.clear();
    grammars_.clear();
    context_.clear();
}

bool PatternCatalog::Load(const std::string& jsonText, std::string* error) { return Load(jsonText, error, LoadOptions{}); }

bool PatternCatalog::Load(const std::string& jsonText, std::string* error, const LoadOptions& options) {
    Clear();
    Value root;
    std::string duplicate;
    if (!ParseJson(jsonText, &root, error, &duplicate)) return false;
    const Value* marker = root.kind() == Value::Kind::Map ? root.Find("$format") : nullptr;
    bool v2 = marker || options.strict;
    if (root.kind() != Value::Kind::Map) {
        if (error) *error = v2 ? "E001 the catalogue is not a JSON object" : "patterns file is not a JSON object";
        return false;
    }
    if (marker && (!marker->IsString() || marker->AsString() != "spf-patterns/2")) {
        if (error) *error = "$format: E003 unknown format (this engine reads spf-patterns/2)";
        return false;
    }
    if (v2) return LoadFormat2(root, duplicate, error);
    return LoadFormat1(root, error);
}

bool PatternCatalog::LoadFormat1(const Value& root, std::string* error) {
    for (const auto& [key, val] : root.Members()) {
        if (key == "_multi" && val.kind() == Value::Kind::List) {
            for (const auto& m : val.Items())
                if (m.IsString()) multi_.insert(m.AsString());
            continue;
        }
        if (key == "_names" && val.kind() == Value::Kind::Map) {
            for (const auto& entry : val.Members()) {
                if (entry.second.kind() != Value::Kind::Map) continue;
                for (const auto& where : entry.second.Members()) {
                    if (where.second.kind() != Value::Kind::List) continue;
                    auto& list = names_[entry.first][where.first];
                    for (const auto& n : where.second.Items()) list.push_back(n.IsString() ? n.AsString() : "");
                }
            }
            continue;
        }
        if (key == "_display" && val.kind() == Value::Kind::Map) {
            for (const auto& m : val.Members())
                if (m.second.IsString()) display_[m.first] = m.second.AsString();
            continue;
        }
        if (key == "_lstrings" && val.kind() == Value::Kind::Map) {
            for (const auto& m : val.Members()) {
                if (m.second.IsString()) lstringClass_[m.first] = m.second.AsString();
            }
            continue;
        }
        if (!key.empty() && key[0] == '_') continue;
        if (!val.IsString()) continue;
        if (!Override(key, val.AsString(), error)) return false;
    }
    return true;
}

bool PatternCatalog::Override(const std::string& key, const std::string& json, std::string* error) {
    // The value is itself JSON; when it does not parse it is a bare
    // primitive name such as formid.
    Value tree;
    if (!ParseJson(json, &tree, nullptr)) tree = Value::Str(json);
    try {
        byKey_[key] = CompilePattern(tree);
    } catch (const PatternError& e) {
        if (error) *error = key + ": " + e.what();
        return false;
    }
    return true;
}

const Pattern* PatternCatalog::Find(const std::string& rt, const std::string& fs) const {
    auto it = byKey_.find(rt + "." + fs);
    return it == byKey_.end() ? nullptr : it->second.get();
}

std::string PatternCatalog::LstringClass(const std::string& rt, const std::string& fs) const {
    auto it = lstringClass_.find(rt + "." + fs);
    return it == lstringClass_.end() ? std::string() : it->second;
}

std::vector<std::string> PatternCatalog::Keys() const {
    std::vector<std::string> keys;
    for (const auto& k : byKey_) keys.push_back(k.first);
    std::sort(keys.begin(), keys.end());
    return keys;
}

bool PatternCatalog::IsMulti(const std::string& rt, const std::string& fs) const {
    return multi_.count(rt + "." + fs) != 0;
}

const std::vector<std::string>* PatternCatalog::Names(const std::string& rt, const std::string& fs,
                                                      const std::string& where) const {
    for (const std::string& key : {rt + "." + fs, "*." + fs}) {
        auto it = names_.find(key);
        if (it == names_.end()) continue;
        auto w = it->second.find(where);
        if (w != it->second.end()) return &w->second;
    }
    return nullptr;
}

std::string PatternCatalog::Display(const std::string& rt, const std::string& fs) const {
    auto it = display_.find(rt + "." + fs);
    return it == display_.end() ? std::string() : it->second;
}

// ---- record context ---------------------------------------------------------

namespace {

ContextAction SetTo(Value v) {
    ContextAction a;
    a.kind = ContextAction::Kind::Set;
    a.set = std::move(v);
    return a;
}

ContextAction ReadAs(const char* type, bool lower = false) {
    ContextAction a;
    a.kind = ContextAction::Kind::Read;
    a.read = type;
    a.lower = lower;
    return a;
}

// A catalogue without declarations: the three variables the engine always
// had. $@EDID0 and $@ANAM exist for every record type (and change only in
// GMST and PACK), $@EPFT only in PERK.
const std::vector<ContextVar>& LegacyContextVars(const std::string& rt) {
    auto make = [](bool perk, bool gmst, bool pack) {
        std::vector<ContextVar> v;
        if (perk) v.push_back({"EPFT", Value::Int(0), {{"PRKE", SetTo(Value::Int(0))}, {"EPFT", ReadAs("uint8")}}});
        ContextVar edid{"EDID0", Value::Str(""), {}};
        if (gmst) edid.on.push_back({"EDID", ReadAs("char", true)});
        v.push_back(std::move(edid));
        ContextVar anam{"ANAM", Value::Str(""), {}};
        if (pack) anam.on.push_back({"ANAM", ReadAs("zstring")});
        v.push_back(std::move(anam));
        return v;
    };
    static const std::vector<ContextVar> perk = make(true, false, false), gmst = make(false, true, false),
                                         pack = make(false, false, true), other = make(false, false, false);
    return rt == "PERK" ? perk : rt == "GMST" ? gmst : rt == "PACK" ? pack : other;
}

void ApplyAction(const ContextAction& a, const uint8_t* data, size_t size, Value* var) {
    if (a.kind == ContextAction::Kind::Set) {
        *var = a.set;
        return;
    }
    const std::string& t = a.read;
    auto lower = [&a](char c) { return a.lower && c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (t == "zstring") {  // byte for byte, to the first NUL or the end
        std::string s;
        for (size_t i = a.at; i < size && data[i]; ++i) s.push_back(lower(static_cast<char>(data[i])));
        *var = Value::Str(std::move(s));
        return;
    }
    if (t == "char") {
        if (a.at >= size) return;  // too short: unchanged
        *var = Value::Str(std::string(1, lower(static_cast<char>(data[a.at]))));
        return;
    }
    size_t width = t == "uint8" || t == "int8" ? 1 : t == "uint16" || t == "int16" ? 2 : 4;
    if (a.at >= size || size - a.at < width) return;  // too short: unchanged
    uint32_t u = 0;
    for (size_t i = 0; i < width; ++i) u |= static_cast<uint32_t>(data[a.at + i]) << (8 * i);
    int64_t v = u;
    if (t[0] == 'i') v = width == 1 ? static_cast<int8_t>(u) : width == 2 ? static_cast<int16_t>(u) : static_cast<int32_t>(u);
    if (a.mask) v &= *a.mask;
    *var = Value::Int(v);
}

}  // namespace

RecordContext::RecordContext(const PatternCatalog& cat, std::string rt, int64_t formVersion, bool localized)
    : decl_(&cat.ContextVars(rt)), format2_(cat.format() == 2) {
    ctx_.rt = std::move(rt);
    ctx_.formVersion = formVersion;
    ctx_.localized = localized;
    ctx_.vars.reserve(decl_->size());
    for (const ContextVar& v : *decl_) ctx_.vars.emplace_back(v.name, v.init);
}

const DecodeContext& RecordContext::Before(const std::string& fsig) {
    ctx_.fsig = fsig;
    if (format2_) ctx_.prev = prev_;
    return ctx_;
}

void RecordContext::After(const std::string& fsig, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < decl_->size(); ++i) {
        for (const auto& [sig, action] : (*decl_)[i].on) {
            if (sig == fsig) ApplyAction(action, data, size, &ctx_.vars[i].second);
        }
    }
    prev_ = fsig;
}

// Format 2: what "_context" declares, nothing else. Format 1: the built-in table.
const std::vector<ContextVar>& PatternCatalog::ContextVars(const std::string& rt) const {
    return format_ == 2 ? DeclaredContext(rt) : LegacyContextVars(rt);
}

// ---- shapes -----------------------------------------------------------------

namespace {

using K = Shape::Kind;

Shape Leaf(K k) {
    Shape s;
    s.kind = k;
    return s;
}

// One shape that holds what either does (union arms).
Shape Merge(const Shape& a, const Shape& b) {
    if (a.kind == b.kind) {
        Shape m = a;
        if (a.kind == K::Map) {
            for (const auto& f : b.fields) {
                auto it = std::find_if(m.fields.begin(), m.fields.end(),
                                       [&](const auto& x) { return x.first == f.first; });
                if (it == m.fields.end()) m.fields.push_back(f);
                else it->second = Merge(it->second, f.second);
            }
        } else if (a.kind == K::Tuple) {
            for (size_t i = 0; i < b.fields.size(); ++i) {
                if (i < m.fields.size()) m.fields[i].second = Merge(m.fields[i].second, b.fields[i].second);
                else m.fields.push_back(b.fields[i]);
            }
        } else if (a.kind == K::List) {
            m.elem = {Merge(a.elem[0], b.elem[0])};
        }
        return m;
    }
    if ((a.kind == K::Int && b.kind == K::Real) || (a.kind == K::Real && b.kind == K::Int)) return Leaf(K::Real);
    return Leaf(K::Unknown);
}

// nullopt = a skip, which an enclosing seq drops (as Exec does).
std::optional<Shape> Walk(const Pattern& p) {
    switch (p.type) {
        case PType::Prim:
            switch (p.prim) {
                case Prim::U8: case Prim::U16: case Prim::U32: case Prim::I8: case Prim::I16: case Prim::I32:
                case Prim::U64: case Prim::I64:
                case Prim::FloatColor255:
                    return Leaf(K::Int);
                case Prim::F32: case Prim::FloatRad2Deg: case Prim::FloatDefault: case Prim::FloatDefaultRad2Deg:
                case Prim::I32Div100: case Prim::I16Div100: case Prim::U16Div65535: case Prim::U8Div100: case Prim::U16Div100:
                case Prim::F32Div100: case Prim::U32Div100: case Prim::Scaled:
                    return Leaf(K::Real);
                case Prim::Bool: return Leaf(K::Bool);
                case Prim::HexU8: case Prim::Zstring: case Prim::Str8: case Prim::Str16: case Prim::Str32: case Prim::Fourcc:
                    return Leaf(K::Text);
                case Prim::EmptyMarker: return Leaf(K::Marker);
                case Prim::Lstring: return Leaf(K::LString);
                case Prim::Formid: return Leaf(K::FormId);
                case Prim::ValueRef: return Leaf(p.refAsFormid ? K::FormId : K::Int);
                case Prim::ColorRgb:
                case Prim::ColorRgba: {
                    Shape t = Leaf(K::Tuple);
                    const char* names[4] = {"r", "g", "b", "a"};
                    for (int i = 0; i < (p.prim == Prim::ColorRgb ? 3 : 4); ++i) t.fields.emplace_back(names[i], Leaf(K::Int));
                    return t;
                }
                case Prim::EnumU8: case Prim::EnumU16: case Prim::EnumU32:
                    return Leaf(p.enumMap ? K::Text : K::Int);
            }
            return Leaf(K::Unknown);
        case PType::Seq: {
            std::vector<std::pair<Shape, const std::optional<std::string>*>> items;
            bool labeled = false;
            for (const auto& item : p.items) {
                std::optional<Shape> s = Walk(*item.first);
                if (!s) continue;
                items.emplace_back(std::move(*s), &item.second);
                if (item.second) labeled = true;
            }
            if (!labeled && p.unwrap && items.size() == 1) return items[0].first;
            Shape s = Leaf(labeled ? K::Map : K::Tuple);
            for (size_t i = 0; i < items.size(); ++i)
                s.fields.emplace_back(*items[i].second ? **items[i].second : "_" + std::to_string(i), items[i].first);
            return s;
        }
        case PType::Repeat: {
            Shape s = Leaf(K::List);
            std::optional<Shape> e = Walk(*p.body);
            s.elem = {e ? *e : Leaf(K::Unknown)};
            return s;
        }
        case PType::Union: {
            std::optional<Shape> m;
            bool anySkip = false;
            auto add = [&](const Pattern& c) {
                std::optional<Shape> s = Walk(c);
                if (!s) anySkip = true;
                else m = m ? Merge(*m, *s) : *s;
            };
            for (const auto& c : p.cases) add(*c.second);
            for (const auto& r : p.ranges) add(*r.arm);
            if (p.deflt) add(*p.deflt);
            if (!m && anySkip) return std::nullopt;
            return m ? *m : Leaf(K::Unknown);
        }
        case PType::Opt:
        case PType::Window: {
            std::optional<Shape> s = Walk(*p.body);
            return s ? *s : Leaf(K::Unknown);
        }
        case PType::Skip: return std::nullopt;
        case PType::Bytes: return Leaf(K::Bytes);
        case PType::Bitfield: return Leaf(K::Flags);
        case PType::Packed: {
            Shape s = Leaf(K::Map);
            for (const auto& m : p.members) {
                K k = m->member == MemberKind::Bool ? K::Bool : m->member == MemberKind::Enum ? K::Text
                    : m->member == MemberKind::Flags ? K::Flags : K::Int;
                s.fields.emplace_back(m->memberId, Leaf(k));
            }
            if (p.restMask) s.fields.emplace_back("_rest", Leaf(K::Int));
            return s;
        }
        case PType::PackedMember: return Leaf(p.member == MemberKind::Bool ? K::Bool : p.member == MemberKind::Enum ? K::Text
                                              : p.member == MemberKind::Flags ? K::Flags : K::Int);
    }
    return Leaf(K::Unknown);
}

}  // namespace

Shape ShapeOf(const Pattern& pattern) {
    std::optional<Shape> s = Walk(pattern);
    return s ? *s : Leaf(K::Unknown);
}

}  // namespace skqe
