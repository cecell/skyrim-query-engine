#include "pattern_engine.h"

#include <algorithm>
#include <bitset>
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

enum class PType : uint8_t { Prim, Seq, Repeat, Union, Opt, Skip, Bytes, Window, Bitfield };

enum class Prim : uint8_t {
    U8, U16, U32, I8, I16, I32, F32, Bool, HexU8, EmptyMarker, Lstring, Formid,
    I32Div100, I16Div100, U16Div65535, U8Div100, F32Div100, U32Div100,
    FloatRad2Deg, FloatDefault, FloatDefaultRad2Deg, FloatColor255, Scaled, ValueRef,
    ColorRgb, ColorRgba, Zstring, Str8, Str16, Str32, EnumU8, EnumU16, EnumU32,
    Fourcc, U16Div100,
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

    // Bitfield
    int size = 4;
    std::map<int, std::string> bits;
};

namespace {

[[noreturn]] void BadShape(const std::string& what) { throw PatternError(false, "Bad pattern shape: " + what); }

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

std::shared_ptr<const Pattern> Compile(const Value& j) {
    auto p = std::make_shared<Pattern>();
    switch (j.kind()) {
        case Value::Kind::String: {
            if (j.AsString() == "bytes") {
                p->type = PType::Bytes;
                return p;
            }
            auto it = PrimNames().find(j.AsString());
            if (it == PrimNames().end()) throw PatternError(false, "Unknown primitive type: " + j.AsString());
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
    auto prim = PrimNames().find(t);
    if (prim != PrimNames().end()) {
        p->prim = prim->second;
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
            for (const auto& m : cs->Members()) p->cases[m.first] = Compile(m.second);
        }
        if (const Value* d = j.Find("default")) p->deflt = Compile(*d);
    } else if (t == "opt") {
        p->type = PType::Opt;
        p->body = Compile(Required(j, "do", "opt"));
    } else if (t == "skip") {
        p->type = PType::Skip;
        p->n = Required(j, "n", "skip");
    } else if (t == "bytes") {
        p->type = PType::Bytes;
        if (const Value* n = j.Find("n")) p->n = *n;
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
        if (ctx.epft) root.emplace_back("@EPFT", Value::Int(*ctx.epft));
        root.emplace_back("@EDID0", Value::Str(ctx.edidPrefix));
        root.emplace_back("@ANAM", Value::Str(ctx.lastAnam));
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
            v = Value::Str(code);
            break;
        }
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
            Value on = scope.Resolve(Value::Str(p.on), cur);
            if (p.mask) {
                int64_t i;
                if (!on.ToInt(&i)) BadShape("union 'mask' on a value that is not an integer");
                on = Value::Int(i & *p.mask);
            }
            std::string key = on.LookupKey();
            auto it = p.cases.find(key);
            const Pattern* c = it != p.cases.end() ? it->second.get() : p.deflt.get();
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

        case PType::Bitfield: {
            size_t start = cur.pos;
            uint32_t raw = cur.ReadUint(p.size);
            std::vector<Value> flags;
            for (const auto& [bit, name] : p.bits) {
                if (bit < 32 && (raw & (1u << bit))) flags.push_back(Value::Str(name));
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

Value DecodeSubrecord(const Pattern& pattern, const uint8_t* data, size_t size, const DecodeContext& ctx) {
    Cursor cur{data, 0, size};
    Scope scope(ctx);
    std::optional<Value> v = Exec(pattern, cur, scope);
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
            for (const auto& b : p.bits)
                if (b.first < 32) raw &= ~(1u << b.first);
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
            if (!c.empty() && c.size() != 4) return Fail(err, "expected four characters");
            c.resize(4, '\0');
            out->insert(out->end(), c.begin(), c.end());
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
bool LeafIsByteRun(const Pattern& p) { return p.type == PType::Bytes; }
std::string LeafTypeName(const Pattern& p) {
    if (p.type == PType::Bytes) return "bytes";
    if (p.type == PType::Bitfield) return "flags";
    if (p.type != PType::Prim) return "structure";
    for (const auto& e : PrimNames())
        if (e.second == p.prim && e.first != "float32_disp_rad2deg") return e.first;
    return "?";
}
bool PatternIsLeaf(const Pattern& p) { return p.type == PType::Prim || p.type == PType::Bytes || p.type == PType::Bitfield; }

bool PatternCatalog::Load(const std::string& jsonText, std::string* error) {
    Value root;
    if (!ParseJson(jsonText, &root, error)) return false;
    if (root.kind() != Value::Kind::Map) {
        if (error) *error = "patterns file is not a JSON object";
        return false;
    }
    byKey_.clear();
    lstringClass_.clear();
    display_.clear();
    names_.clear();
    multi_.clear();
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
    }
    return Leaf(K::Unknown);
}

}  // namespace

Shape ShapeOf(const Pattern& pattern) {
    std::optional<Shape> s = Walk(pattern);
    return s ? *s : Leaf(K::Unknown);
}

}  // namespace skqe
