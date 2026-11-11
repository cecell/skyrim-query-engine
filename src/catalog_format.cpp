// Format 2 of the pattern catalogue: the "$format" marker, the top-level
// members it allows, and the strict check of every pattern node (which keys
// a node may have, of which JSON type, which names are bound where). The
// check runs on the JSON tree before it is compiled; format 1 never comes
// here.
#include <algorithm>
#include <map>
#include <set>

#include "pattern_engine.h"

namespace skqe {
namespace {

// A load error: the first one found ends the load.
struct LoadError {
    std::string code, key, path, message;
};

std::string FindingText(const std::string& code, const std::string& key, const std::string& path, const std::string& msg) {
    std::string s;
    if (!key.empty()) s += key + ": ";
    if (!path.empty()) s += path + ": ";
    return s + code + " " + msg;
}

std::string Join(const std::string& path, const std::string& seg) {
    if (seg.empty() || seg[0] == '[') return path + seg;
    return path.empty() ? seg : path + "." + seg;
}

std::string Idx(size_t i) { return "[" + std::to_string(i) + "]"; }

bool IsCanonicalDecimal(const std::string& k) {
    if (k.empty() || k.size() > 10 || k.find_first_not_of("0123456789") != std::string::npos) return false;
    return k.size() == 1 || k[0] != '0';
}

int WidthOf(const std::string& s) { return s == "uint8" ? 1 : s == "uint16" ? 2 : s == "uint32" ? 4 : 0; }

const char* const kBuiltinContext[] = {"RT", "FSIG", "FormVersion", "Localized"};

// The kinds of primitive, for the keys they take.
enum class PrimKind { Number, Formid, Text, Enum, Scaled, ValueRef, Fourcc };

PrimKind KindOf(const std::string& t) {
    if (t == "formid") return PrimKind::Formid;
    if (t == "enum_uint8" || t == "enum_uint16" || t == "enum_uint32") return PrimKind::Enum;
    if (t == "scaled") return PrimKind::Scaled;
    if (t == "value_ref") return PrimKind::ValueRef;
    if (t == "fourcc") return PrimKind::Fourcc;
    if (t == "lstring" || t == "zstring" || t == "str8" || t == "str16" || t == "str32" || t == "bool" ||
        t == "hex_uint8" || t == "color_rgb" || t == "color_rgba" || t == "empty_marker") {
        return PrimKind::Text;
    }
    return PrimKind::Number;
}

class NodeChecker {
public:
    NodeChecker(const std::string& key, const std::set<std::string>& contextNames, std::vector<CatalogFinding>* warnings)
        : key_(key), rt_(key.substr(0, key.find('.'))), ctxNames_(contextNames), warnings_(warnings) {}

    // False with err_ set at the first error.
    bool Check(const Value& node, const std::string& path, std::set<std::string>* bound) {
        switch (node.kind()) {
            case Value::Kind::String:
                if (node.AsString() == "bytes" || IsPrimitiveName(node.AsString())) {
                    if (node.AsString() == "lstring") hasLstring_ = true;
                    if (node.AsString().rfind("enum_uint", 0) == 0) Warn("W108", path, "enum without a value map");
                    return true;
                }
                return Err("E010", path, "unknown primitive type: " + node.AsString());
            case Value::Kind::List: return CheckSeq(node, node, path, bound);
            case Value::Kind::Map: break;
            default: return Err("E013", path, "a pattern is a type name, a list or an object");
        }
        const Value* tv = node.Find("t");
        if (!tv) return Err("E012", path, "node without 't'");
        if (!tv->IsString()) return Err("E013", path, "'t' must be text");
        const std::string& t = tv->AsString();
        if (const Value* id = node.Find("id"); id && !id->IsString()) return Err("E013", Join(path, "id"), "'id' must be text");
        if (IsPrimitiveName(t)) return CheckPrim(node, t, path, bound);
        if (t == "seq") {
            if (!Keys(node, path, {"do", "unwrap"})) return false;
            const Value* d = node.Find("do");
            if (!d) return Err("E012", path, "seq without 'do'");
            if (d->kind() != Value::Kind::List) return Err("E013", Join(path, "do"), "seq 'do' must be a list");
            if (const Value* u = node.Find("unwrap"); u && u->kind() != Value::Kind::Bool) {
                return Err("E013", Join(path, "unwrap"), "'unwrap' must be true or false");
            }
            return CheckSeq(node, *d, Join(path, "do"), bound);
        }
        if (t == "repeat") {
            if (!Keys(node, path, {"do", "n", "n_pfx"})) return false;
            const Value* d = node.Find("do");
            if (!d) return Err("E012", path, "repeat without 'do'");
            if (node.Find("n") && node.Find("n_pfx")) return Err("E011", path, "repeat takes 'n' or 'n_pfx', not both");
            if (!CheckCount(node, path, *bound, false)) return false;
            std::set<std::string> inner = *bound;  // an element's names end with the element
            return Check(*d, Join(path, "do"), &inner);
        }
        if (t == "union") return CheckUnion(node, path, bound);
        if (t == "opt") {
            if (!Keys(node, path, {"do"})) return false;
            const Value* d = node.Find("do");
            if (!d) return Err("E012", path, "opt without 'do'");
            return Check(*d, Join(path, "do"), bound);
        }
        if (t == "skip") {
            if (!Keys(node, path, {"n"})) return false;
            if (!node.Find("n")) return Err("E012", path, "skip without 'n'");
            return CheckCount(node, path, *bound, false);
        }
        if (t == "bytes") {
            if (!Keys(node, path, {"n"})) return false;
            return CheckCount(node, path, *bound, false);
        }
        if (t == "window") {
            if (!Keys(node, path, {"do", "n", "n_pfx"})) return false;
            const Value* d = node.Find("do");
            if (!d) return Err("E012", path, "window without 'do'");
            if (!node.Find("n") && !node.Find("n_pfx")) return Err("E012", path, "window without 'n' or 'n_pfx'");
            if (node.Find("n") && node.Find("n_pfx")) return Err("E011", path, "window takes 'n' or 'n_pfx', not both");
            if (!CheckCount(node, path, *bound, false)) return false;
            return Check(*d, Join(path, "do"), bound);
        }
        if (t == "bitfield") return CheckBitfield(node, path, bound);
        if (t == "packed") return CheckPacked(node, path, bound);
        return Err("E010", path, "unknown node type: " + t);
    }

    const CatalogFinding& error() const { return err_; }
    bool hasLstring() const { return hasLstring_; }
    const std::set<std::string>& contextRead() const { return ctxRead_; }

private:
    bool Err(const char* code, const std::string& path, const std::string& msg) {
        err_ = {code, key_, path, msg};
        return false;
    }
    void Warn(const char* code, const std::string& path, const std::string& msg) {
        if (warnings_) warnings_->push_back({code, key_, path, msg});
    }

    // Only `allowed` (plus t, id, note) may appear.
    bool Keys(const Value& node, const std::string& path, std::initializer_list<const char*> allowed) {
        for (const auto& m : node.Members()) {
            const std::string& k = m.first;
            if (k == "t" || k == "id" || k == "note") continue;
            bool ok = false;
            for (const char* a : allowed) ok = ok || k == a;
            if (!ok) return Err("E011", Join(path, k), "unknown key '" + k + "'");
        }
        return true;
    }

    bool CheckAs(const Value& node, const std::string& path, std::set<std::string>* bound) {
        const Value* as = node.Find("as");
        if (!as) return true;
        if (!as->IsString() || as->AsString().empty()) return Err("E013", Join(path, "as"), "'as' must be a name");
        bound->insert(as->AsString());
        return true;
    }

    // A "$..." reference: bound earlier, a built-in, or a declared variable.
    bool CheckRef(const std::string& ref, const std::string& path, const std::set<std::string>& bound) {
        std::string name = ref.substr(1);
        if (!name.empty() && name[0] == '@') {
            std::string v = name.substr(1);
            for (const char* b : kBuiltinContext)
                if (v == b) return true;
            if (v == "Prev") return true;
            if (ctxNames_.count(v)) {
                ctxRead_.insert(v);
                return true;
            }
            return Err("E019", path, "context variable $@" + v + " is not declared for " + rt_);
        }
        int pw;
        size_t po;
        if (name == "_remaining" || name == "_pos" || ParsePeek(name, &pw, &po)) return true;
        if (name.rfind("_popcount_", 0) == 0) name = name.substr(10);
        else if (!name.empty() && name[0] == '_') return Err("E019", path, "unknown reference " + ref);
        if (!bound.count(name)) return Err("E019", path, "$" + name + " is used before an 'as' binds it");
        return true;
    }

    // `n` (and `n_pfx`): a non-negative integer or a reference.
    bool CheckCount(const Value& node, const std::string& path, const std::set<std::string>& bound, bool) {
        if (const Value* p = node.Find("n_pfx"); p && (!p->IsString() || !WidthOf(p->AsString()))) {
            return Err("E013", Join(path, "n_pfx"), "'n_pfx' must be uint8, uint16 or uint32");
        }
        const Value* n = node.Find("n");
        if (!n) return true;
        if (n->kind() == Value::Kind::Int) {
            if (n->AsInt() < 0) return Err("E013", Join(path, "n"), "'n' must not be negative");
            return true;
        }
        if (n->IsString() && !n->AsString().empty() && n->AsString()[0] == '$') return CheckRef(n->AsString(), Join(path, "n"), bound);
        return Err("E013", Join(path, "n"), "'n' must be a non-negative integer or a $ reference");
    }

    bool CheckSeq(const Value& node, const Value& items, const std::string& path, std::set<std::string>* bound) {
        (void)node;
        std::set<std::string> ids;
        bool labelled = false, unnamed = false;
        size_t i = 0;
        for (const Value& item : items.Items()) {
            std::string ip = Join(path, Idx(i++));
            if (!Check(item, ip, bound)) return false;
            const Value* id = item.kind() == Value::Kind::Map ? item.Find("id") : nullptr;
            if (id) {
                labelled = true;
                if (!ids.insert(id->AsString()).second) return Err("E015", Join(ip, "id"), "duplicate id '" + id->AsString() + "'");
            } else {
                const Value* t = item.kind() == Value::Kind::Map ? item.Find("t") : nullptr;
                if (!(t && t->IsString() && t->AsString() == "skip")) unnamed = true;
            }
        }
        if (labelled && unnamed) Warn("W102", path, "an item without 'id' in a labelled sequence is named _<n>");
        return true;
    }

    bool CheckPrim(const Value& node, const std::string& t, const std::string& path, std::set<std::string>* bound) {
        PrimKind k = KindOf(t);
        switch (k) {
            case PrimKind::Number:
            case PrimKind::Formid:
            case PrimKind::Text:
            case PrimKind::Fourcc:
                if (!Keys(node, path, {"as"})) return false;
                break;
            case PrimKind::Enum:
                if (!Keys(node, path, {"as", "enum"})) return false;
                break;
            case PrimKind::Scaled:
                if (!Keys(node, path, {"as", "read", "offset", "divisor"})) return false;
                if (const Value* r = node.Find("read")) {
                    static const std::set<std::string> reads = {"uint8", "int8", "uint16", "int16", "uint32", "int32", "float32"};
                    if (!r->IsString() || !reads.count(r->AsString())) return Err("E013", Join(path, "read"), "bad 'read' type");
                }
                for (const char* nk : {"offset", "divisor"}) {
                    const Value* v = node.Find(nk);
                    if (v && v->kind() != Value::Kind::Int && v->kind() != Value::Kind::Float) {
                        return Err("E013", Join(path, nk), std::string("'") + nk + "' must be a number");
                    }
                }
                break;
            case PrimKind::ValueRef: {
                if (!Keys(node, path, {"as", "ref", "as_formid"})) return false;
                const Value* r = node.Find("ref");
                if (!r) return Err("E012", path, "value_ref without 'ref'");
                if (!r->IsString() || r->AsString().empty() || r->AsString()[0] != '$') {
                    return Err("E013", Join(path, "ref"), "'ref' must be a $ reference");
                }
                if (!CheckRef(r->AsString(), Join(path, "ref"), *bound)) return false;
                if (const Value* f = node.Find("as_formid"); f && f->kind() != Value::Kind::Bool) {
                    return Err("E013", Join(path, "as_formid"), "'as_formid' must be true or false");
                }
                break;
            }
        }
        if (t == "lstring") hasLstring_ = true;
        if (k == PrimKind::Enum) {
            const Value* e = node.Find("enum");
            if (!e) {
                Warn("W108", path, "enum without a value map");
            } else {
                if (e->kind() != Value::Kind::Map) return Err("E013", Join(path, "enum"), "'enum' must be an object");
                int width = t == "enum_uint8" ? 1 : t == "enum_uint16" ? 2 : 4;
                std::set<std::string> names;
                for (const auto& [ek, ev] : e->Members()) {
                    std::string ep = Join(Join(path, "enum"), ek);
                    if (!IsCanonicalDecimal(ek) || std::stoull(ek) >= (uint64_t{1} << (8 * width))) {
                        return Err("E014", ep, "enum key must be a decimal number in range, without leading zeros");
                    }
                    if (!ev.IsString()) return Err("E013", ep, "an enum name must be text");
                    if (!names.insert(ev.AsString()).second) return Err("E015", ep, "duplicate enum name '" + ev.AsString() + "'");
                }
            }
        }
        return CheckAs(node, path, bound);
    }

    bool CheckBitfield(const Value& node, const std::string& path, std::set<std::string>* bound) {
        if (!Keys(node, path, {"size", "bits", "as"})) return false;
        const Value* size = node.Find("size");
        if (!size) return Err("E012", path, "bitfield without 'size'");
        if (!size->IsString() || !WidthOf(size->AsString())) return Err("E013", Join(path, "size"), "'size' must be uint8, uint16 or uint32");
        const Value* bits = node.Find("bits");
        if (!bits) return Err("E012", path, "bitfield without 'bits'");
        if (bits->kind() != Value::Kind::Map) return Err("E013", Join(path, "bits"), "'bits' must be an object");
        int width = WidthOf(size->AsString()) * 8;
        std::set<std::string> names;
        for (const auto& [bk, bv] : bits->Members()) {
            std::string bp = Join(Join(path, "bits"), bk);
            if (!IsCanonicalDecimal(bk) || std::stoull(bk) >= static_cast<uint64_t>(width)) {
                return Err("E014", bp, "bit number must be a decimal number below the width, without leading zeros");
            }
            if (!bv.IsString()) return Err("E013", bp, "a bit name must be text");
            if (!names.insert(bv.AsString()).second) return Err("E015", bp, "duplicate bit name '" + bv.AsString() + "'");
        }
        return CheckAs(node, path, bound);
    }

    // A word split into members (spec B.1).
    bool CheckPacked(const Value& node, const std::string& path, std::set<std::string>* bound) {
        if (!Keys(node, path, {"size", "do", "as"})) return false;
        const Value* size = node.Find("size");
        if (!size) return Err("E012", path, "packed without 'size'");
        if (!size->IsString() || !WidthOf(size->AsString())) return Err("E013", Join(path, "size"), "'size' must be uint8, uint16 or uint32");
        const Value* d = node.Find("do");
        if (!d) return Err("E012", path, "packed without 'do'");
        if (d->kind() != Value::Kind::List) return Err("E013", Join(path, "do"), "'do' must be a list of members");
        if (d->Items().empty()) return Err("E016", Join(path, "do"), "packed without members");
        int width = WidthOf(size->AsString()) * 8;
        std::set<std::string> ids;
        uint64_t owned = 0;
        size_t i = 0;
        for (const Value& m : d->Items()) {
            std::string mp = Join(Join(path, "do"), Idx(i++));
            if (m.kind() != Value::Kind::Map) return Err("E013", mp, "a member is an object");
            static const std::set<std::string> kMemberKeys = {"id", "t", "lo", "n", "enum", "bits", "as", "note"};
            for (const auto& kv : m.Members())
                if (!kMemberKeys.count(kv.first)) return Err("E011", Join(mp, kv.first), "unknown key '" + kv.first + "'");
            const Value* id = m.Find("id");
            if (!id) return Err("E012", mp, "member without 'id'");
            if (!id->IsString() || id->AsString().empty()) return Err("E013", Join(mp, "id"), "'id' must be text");
            if (id->AsString() == "_rest") return Err("E015", Join(mp, "id"), "'_rest' is reserved");
            if (!ids.insert(id->AsString()).second) return Err("E015", Join(mp, "id"), "duplicate id '" + id->AsString() + "'");
            std::string kind = "uint";
            if (const Value* t = m.Find("t")) {
                if (!t->IsString()) return Err("E013", Join(mp, "t"), "member type must be text");
                kind = t->AsString();
                if (kind != "uint" && kind != "int" && kind != "bool" && kind != "enum" && kind != "flags") {
                    return Err("E013", Join(mp, "t"), "member type is uint, int, bool, enum or flags");
                }
            }
            uint64_t mask = 0;
            if (kind == "flags") {
                if (m.Find("lo") || m.Find("n")) return Err("E011", mp, "a flags member owns its 'bits', not 'lo'/'n'");
                if (m.Find("enum")) return Err("E011", Join(mp, "enum"), "'enum' is for enum members");
                const Value* bits = m.Find("bits");
                if (!bits) return Err("E012", mp, "flags member without 'bits'");
                if (bits->kind() != Value::Kind::Map) return Err("E013", Join(mp, "bits"), "'bits' must be an object");
                std::set<std::string> names;
                for (const auto& [bk, bv] : bits->Members()) {
                    std::string bp = Join(Join(mp, "bits"), bk);
                    if (!IsCanonicalDecimal(bk) || std::stoull(bk) >= static_cast<uint64_t>(width)) {
                        return Err("E014", bp, "bit number must be a decimal number below the width, without leading zeros");
                    }
                    if (!bv.IsString()) return Err("E013", bp, "a bit name must be text");
                    if (!names.insert(bv.AsString()).second) return Err("E015", bp, "duplicate bit name '" + bv.AsString() + "'");
                    mask |= uint64_t{1} << std::stoull(bk);
                }
            } else {
                if (m.Find("bits")) return Err("E011", Join(mp, "bits"), "'bits' is for flags members");
                if (kind != "enum" && m.Find("enum")) return Err("E011", Join(mp, "enum"), "'enum' is for enum members");
                const Value* lo = m.Find("lo");
                const Value* n = m.Find("n");
                if (!lo || !n) return Err("E012", mp, "member without 'lo' and 'n'");
                if (lo->kind() != Value::Kind::Int) return Err("E013", Join(mp, "lo"), "'lo' must be an integer");
                if (n->kind() != Value::Kind::Int) return Err("E013", Join(mp, "n"), "'n' must be an integer");
                if (n->AsInt() < 1 || lo->AsInt() < 0 || lo->AsInt() + n->AsInt() > width) {
                    return Err("E016", mp, "member bits leave the word");
                }
                if (kind == "bool" && n->AsInt() != 1) return Err("E016", Join(mp, "n"), "a bool member is 1 bit");
                mask = ((n->AsInt() == 64 ? ~uint64_t{0} : (uint64_t{1} << n->AsInt()) - 1)) << lo->AsInt();
                if (kind == "enum") {
                    const Value* e = m.Find("enum");
                    if (!e) return Err("E012", mp, "enum member without 'enum'");
                    if (e->kind() != Value::Kind::Map) return Err("E013", Join(mp, "enum"), "'enum' must be an object");
                    std::set<std::string> names;
                    for (const auto& [ek, ev] : e->Members()) {
                        std::string ep = Join(Join(mp, "enum"), ek);
                        if (!IsCanonicalDecimal(ek) || std::stoull(ek) >= (uint64_t{1} << n->AsInt())) {
                            return Err("E014", ep, "enum key must be a decimal number that fits the member");
                        }
                        if (!ev.IsString()) return Err("E013", ep, "an enum name must be text");
                        if (!names.insert(ev.AsString()).second) return Err("E015", ep, "duplicate enum name '" + ev.AsString() + "'");
                    }
                }
            }
            if (owned & mask) return Err("E016", mp, "members overlap");
            owned |= mask;
            if (!CheckAs(m, mp, bound)) return false;
        }
        return CheckAs(node, path, bound);
    }

    // Does an arm begin by skipping (over the bytes a read-ahead looked at)?
    static bool StartsWithSkip(const Value& arm) {
        auto isSkip = [](const Value& v) {
            const Value* t = v.kind() == Value::Kind::Map ? v.Find("t") : nullptr;
            return t && t->IsString() && t->AsString() == "skip";
        };
        if (isSkip(arm)) {
            const Value* n = arm.Find("n");
            return !(n && n->kind() == Value::Kind::Int && n->AsInt() == 0);
        }
        const Value* items = arm.kind() == Value::Kind::List ? &arm : nullptr;
        if (!items && arm.kind() == Value::Kind::Map) {
            const Value* t = arm.Find("t");
            if (t && t->IsString() && t->AsString() == "seq") items = arm.Find("do");
        }
        return items && items->kind() == Value::Kind::List && !items->Items().empty() && isSkip(items->Items()[0]);
    }

    bool CheckUnion(const Value& node, const std::string& path, std::set<std::string>* bound) {
        if (!Keys(node, path, {"on", "cases", "mask", "default", "write"})) return false;
        const Value* on = node.Find("on");
        if (!on) return Err("E012", path, "union without 'on'");
        if (!on->IsString() || on->AsString().empty() || on->AsString()[0] != '$') {
            return Err("E013", Join(path, "on"), "'on' must be a $ reference");
        }
        if (!CheckRef(on->AsString(), Join(path, "on"), *bound)) return false;
        const Value* cases = node.Find("cases");
        if (!cases) return Err("E012", path, "union without 'cases'");
        if (cases->kind() != Value::Kind::Map) return Err("E013", Join(path, "cases"), "'cases' must be an object");
        if (const Value* m = node.Find("mask"); m && m->kind() != Value::Kind::Int) {
            return Err("E013", Join(path, "mask"), "'mask' must be an integer");
        }
        if (const Value* w = node.Find("write")) {
            if (!w->IsString()) return Err("E013", Join(path, "write"), "'write' must be a case key");
            if (!cases->Find(w->AsString())) return Err("E012", Join(path, "write"), "'write' names no case");
        }
        // Range keys: well formed, lo <= hi, no two sharing a value.
        std::vector<std::pair<int64_t, int64_t>> spans;
        for (const auto& m : cases->Members()) {
            if (m.first.find("..") == std::string::npos) continue;
            std::optional<int64_t> lo, hi;
            std::string kp = Join(Join(path, "cases"), m.first);
            if (!ParseCaseRange(m.first, &lo, &hi) || (!lo && !hi)) return Err("E017", kp, "a range is lo..hi, ..hi or lo..");
            int64_t a = lo ? *lo : INT64_MIN, b = hi ? *hi : INT64_MAX;
            if (a > b) return Err("E017", kp, "range runs backwards");
            for (const auto& s : spans)
                if (a <= s.second && s.first <= b) return Err("E017", kp, "ranges overlap");
            spans.emplace_back(a, b);
        }
        // Names an arm binds stay visible after the union, as at run time.
        std::set<std::string> after = *bound;
        int pw = 0;
        size_t po = 0;
        bool peek = ParsePeek(on->AsString().substr(1), &pw, &po) && po == 0;
        bool skipsDiscriminator = false;
        for (const auto& [ck, cv] : cases->Members()) {
            std::set<std::string> inner = *bound;
            if (!Check(cv, Join(Join(path, "cases"), ck), &inner)) return false;
            after.insert(inner.begin(), inner.end());
            skipsDiscriminator = skipsDiscriminator || (peek && StartsWithSkip(cv));
        }
        if (const Value* d = node.Find("default")) {
            std::set<std::string> inner = *bound;
            if (!Check(*d, Join(path, "default"), &inner)) return false;
            after.insert(inner.begin(), inner.end());
            skipsDiscriminator = skipsDiscriminator || (peek && StartsWithSkip(*d));
        } else {
            Warn("W101", path, "union without a default: a value no case names leaves the field raw");
        }
        if (skipsDiscriminator) Warn("W103", path, "a read-ahead arm skips over its own discriminator");
        *bound = std::move(after);
        return true;
    }

    std::string key_, rt_;
    const std::set<std::string>& ctxNames_;
    std::vector<CatalogFinding>* warnings_;
    CatalogFinding err_;
    bool hasLstring_ = false;
    std::set<std::string> ctxRead_;
};

bool KeyShape(const std::string& key) {
    size_t dot = key.find('.');
    if (dot != 4) return false;
    for (size_t i = 0; i < 4; ++i) {
        char c = key[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    std::string fs = key.substr(5);
    return fs.size() == 4 || fs == "Record Header";
}

bool IsRecordType(const std::string& rt) {
    if (rt.size() != 4) return false;
    for (char c : rt)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

}  // namespace

std::string CatalogFinding::Text() const { return FindingText(code, key, path, message); }

bool PatternCatalog::LoadFormat2(const Value& root, const std::string& duplicate, std::string* error) {
    auto fail = [&](const std::string& code, const std::string& key, const std::string& path, const std::string& msg) {
        if (error) *error = FindingText(code, key, path, msg);
        return false;
    };
    if (!duplicate.empty()) return fail("E002", "", "", "duplicate key '" + duplicate + "'");
    format_ = 2;
    // Pass 1: the top-level members and the keys.
    std::vector<std::pair<std::string, const Value*>> patterns;
    std::set<std::string> keys;
    static const std::set<std::string> kSections = {"$format", "_about", "_options", "_shared", "_context", "_records",
                                                    "_multi",  "_lstrings", "_display", "_names", "_meta"};
    for (const auto& [key, val] : root.Members()) {
        if (kSections.count(key)) continue;
        if (!key.empty() && (key[0] == '_' || key[0] == '$')) return fail("E004", key, "", "unknown top-level member");
        if (!KeyShape(key)) return fail("E021", key, "", "a key is RT.FS: a 4-character record type, a dot, a field signature");
        keys.insert(key);
        patterns.emplace_back(key, &val);
    }
    auto section = [&](const char* name) { return root.Find(name); };
    auto keyExists = [&](const std::string& k) { return keys.count(k) != 0; };

    if (const Value* a = section("_about"); a && !a->IsString()) return fail("E005", "_about", "", "must be text");
    if (const Value* o = section("_options")) {
        if (o->kind() != Value::Kind::Map) return fail("E005", "_options", "", "must be an object");
        for (const auto& [k, v] : o->Members()) {
            if (k != "unnamed_bits") return fail("E005", "_options", k, "unknown option");
            if (!v.IsString() || (v.AsString() != "hide" && v.AsString() != "show")) {
                return fail("E005", "_options", k, "must be \"hide\" or \"show\"");
            }
        }
        options_ = *o;
    }
    if (const Value* m = section("_multi")) {
        if (m->kind() != Value::Kind::List) return fail("E005", "_multi", "", "must be a list of keys");
        for (const auto& k : m->Items()) {
            if (!k.IsString()) return fail("E005", "_multi", "", "must be a list of keys");
            if (!keyExists(k.AsString())) return fail("E022", "_multi", k.AsString(), "names a key that does not exist");
            multi_.insert(k.AsString());
        }
    }
    if (const Value* l = section("_lstrings")) {
        if (l->kind() != Value::Kind::Map) return fail("E005", "_lstrings", "", "must be an object");
        for (const auto& [k, v] : l->Members()) {
            if (!v.IsString() || (v.AsString() != "strings" && v.AsString() != "dlstrings" && v.AsString() != "ilstrings")) {
                return fail("E005", "_lstrings", k, "must be strings, dlstrings or ilstrings");
            }
            if (!keyExists(k)) return fail("E022", "_lstrings", k, "names a key that does not exist");
            lstringClass_[k] = v.AsString();
        }
    }
    if (const Value* d = section("_display")) {
        if (d->kind() != Value::Kind::Map) return fail("E005", "_display", "", "must be an object");
        for (const auto& [k, v] : d->Members()) {
            if (!v.IsString()) return fail("E005", "_display", k, "must be text");
            if (!keyExists(k)) return fail("E022", "_display", k, "names a key that does not exist");
            display_[k] = v.AsString();
        }
    }
    if (const Value* n = section("_names")) {
        if (n->kind() != Value::Kind::Map) return fail("E005", "_names", "", "must be an object");
        for (const auto& [k, v] : n->Members()) {
            if (k.rfind("*.", 0) == 0) {
                std::string tail = k.substr(1);
                bool any = std::any_of(keys.begin(), keys.end(), [&](const std::string& x) {
                    return x.size() >= tail.size() && x.compare(x.size() - tail.size(), tail.size(), tail) == 0;
                });
                if (!any) return fail("E022", "_names", k, "matches no key");
            } else if (!keyExists(k)) {
                return fail("E022", "_names", k, "names a key that does not exist");
            }
            if (v.kind() != Value::Kind::Map) return fail("E005", "_names", k, "must be an object of lists");
            for (const auto& [where, list] : v.Members()) {
                if (list.kind() != Value::Kind::List) return fail("E005", "_names", k + "." + where, "must be a list of names");
                auto& out = names_[k][where];
                for (const auto& x : list.Items()) {
                    if (!x.IsString()) return fail("E005", "_names", k + "." + where, "must be a list of names");
                    out.push_back(x.AsString());
                }
            }
        }
    }
    if (const Value* s = section("_shared")) {
        if (s->kind() != Value::Kind::Map) return fail("E005", "_shared", "", "must be an object");
        shared_ = *s;
    }
    if (const Value* r = section("_records")) {
        if (r->kind() != Value::Kind::Map) return fail("E005", "_records", "", "must be an object");
        for (const auto& [rt, g] : r->Members()) {
            if (!IsRecordType(rt)) return fail("E005", "_records", rt, "not a record type");
            if (g.kind() != Value::Kind::Map) return fail("E005", "_records", rt, "must be an object");
        }
        records_ = *r;
    }
    if (const Value* m = section("_meta")) {
        if (m->kind() != Value::Kind::Map) return fail("E005", "_meta", "", "must be an object");
        for (const auto& [k, v] : m->Members()) {
            if (!keyExists(k)) return fail("E022", "_meta", k, "names a key that does not exist");
            if (v.kind() != Value::Kind::Map) return fail("E005", "_meta", k, "must be an object");
        }
        meta_ = *m;
    }
    if (const Value* c = section("_context")) {
        if (c->kind() != Value::Kind::Map) return fail("E005", "_context", "", "must be an object");
        std::string err;
        if (!LoadContext(*c, &err)) {
            if (error) *error = err;
            return false;
        }
    }

    // Pass 2: every pattern, checked, then compiled.
    std::map<std::string, std::vector<std::string>> byText;   // structured pattern text -> keys
    std::map<std::string, std::set<std::string>> byFs;        // field signature -> distinct pattern texts
    for (const auto& [key, val] : patterns) {
        Value tree;
        if (val->IsString()) {
            const std::string& s = val->AsString();
            if (s == "bytes" || IsPrimitiveName(s)) {
                tree = *val;
            } else {
                std::string dup, perr;
                if (!ParseJson(s, &tree, &perr, &dup)) return fail("E010", key, "", "unknown primitive type: " + s);
                if (!dup.empty()) return fail("E002", key, "", "duplicate key '" + dup + "'");
            }
        } else {
            tree = *val;
        }
        std::set<std::string> ctxNames;
        std::string rt = key.substr(0, 4);
        for (const ContextVar& v : DeclaredContext(rt)) ctxNames.insert(v.name);
        NodeChecker check(key, ctxNames, &warnings_);
        std::set<std::string> bound;
        if (!check.Check(tree, "", &bound)) {
            if (error) *error = check.error().Text();
            return false;
        }
        // A field may not read a variable its own field signature updates.
        for (const ContextVar& v : DeclaredContext(rt)) {
            if (!check.contextRead().count(v.name)) continue;
            for (const auto& action : v.on) {
                if (action.first == key.substr(5)) {
                    return fail("E020", key, "", "reads $@" + v.name + ", which " + action.first + " itself updates");
                }
            }
        }
        if (check.hasLstring() && !lstringClass_.count(key)) {
            warnings_.push_back({"W106", key, "", "an lstring leaf in a key with no _lstrings entry"});
        }
        try {
            byKey_[key] = CompilePattern(tree, true);
        } catch (const PatternError& e) {
            return fail("E010", key, "", e.what());
        }
        std::string text;
        WriteJson(tree, &text);
        if (tree.kind() != Value::Kind::String) byText[text].push_back(key);
        byFs[key.substr(5)].insert(text);
    }
    for (const auto& [text, ks] : byText) {
        if (ks.size() > 1) {
            std::string list;
            for (const auto& k : ks) list += (list.empty() ? "" : ", ") + k;
            warnings_.push_back({"W104", ks[0], "", std::to_string(ks.size()) + " keys have the same pattern: " + list});
        }
    }
    for (const auto& [fs, texts] : byFs) {
        if (texts.size() > 1) {
            warnings_.push_back({"W105", "*." + fs, "", std::to_string(texts.size()) + " different patterns across record types"});
        }
    }
    return true;
}

bool PatternCatalog::LoadContext(const Value& c, std::string* error) {
    auto fail = [&](const std::string& code, const std::string& path, const std::string& msg) {
        *error = FindingText(code, "_context", path, msg);
        return false;
    };
    static const std::set<std::string> kReads = {"uint8", "uint16", "uint32", "int8", "int16", "int32", "char", "zstring"};
    for (const auto& [rts, vars] : c.Members()) {
        std::vector<std::string> types;
        for (size_t s = 0; s <= rts.size();) {
            size_t e = rts.find(',', s);
            if (e == std::string::npos) e = rts.size();
            types.push_back(rts.substr(s, e - s));
            s = e + 1;
        }
        for (const auto& rt : types)
            if (!IsRecordType(rt)) return fail("E005", rts, "not a record type: " + rt);
        if (vars.kind() != Value::Kind::Map) return fail("E005", rts, "must be an object of variables");
        for (const auto& [name, decl] : vars.Members()) {
            std::string vp = rts + "." + name;
            for (const char* b : kBuiltinContext)
                if (name == b) return fail("E020", vp, "a built-in name");
            if (name == "Prev") return fail("E020", vp, "a built-in name");
            if (name.empty() || name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
                return fail("E020", vp, "a variable name is letters, digits and _");
            }
            if (decl.kind() != Value::Kind::Map) return fail("E005", vp, "must be an object");
            for (const auto& m : decl.Members()) {
                if (m.first != "init" && m.first != "on" && m.first != "note") return fail("E020", vp + "." + m.first, "unknown key");
            }
            ContextVar var;
            var.name = name;
            const Value* init = decl.Find("init");
            if (!init) return fail("E020", vp, "no 'init'");
            if (init->kind() != Value::Kind::Int && !init->IsString()) return fail("E020", vp + ".init", "must be an integer or text");
            var.init = *init;
            if (const Value* on = decl.Find("on")) {
                if (on->kind() != Value::Kind::Map) return fail("E020", vp + ".on", "must be an object");
                for (const auto& [sig, act] : on->Members()) {
                    std::string ap = vp + ".on." + sig;
                    if (sig.size() != 4) return fail("E020", ap, "not a field signature");
                    if (act.kind() != Value::Kind::Map) return fail("E020", ap, "an action is {\"set\":..} or {\"read\":..}");
                    ContextAction a;
                    if (const Value* set = act.Find("set")) {
                        if (act.Members().size() != 1) return fail("E020", ap, "'set' takes nothing else");
                        if (set->kind() != Value::Kind::Int && !set->IsString()) return fail("E020", ap, "'set' must be an integer or text");
                        a.kind = ContextAction::Kind::Set;
                        a.set = *set;
                    } else if (const Value* rd = act.Find("read")) {
                        a.kind = ContextAction::Kind::Read;
                        if (!rd->IsString() || !kReads.count(rd->AsString())) return fail("E020", ap, "bad 'read' type");
                        a.read = rd->AsString();
                        bool text = a.read == "char" || a.read == "zstring";
                        for (const auto& [ak, av] : act.Members()) {
                            if (ak == "read") continue;
                            if (ak == "at") {
                                if (av.kind() != Value::Kind::Int || av.AsInt() < 0) return fail("E020", ap, "'at' must be a non-negative integer");
                                a.at = static_cast<size_t>(av.AsInt());
                            } else if (ak == "mask") {
                                if (text || av.kind() != Value::Kind::Int) return fail("E020", ap, "'mask' is for integer reads");
                                a.mask = av.AsInt();
                            } else if (ak == "lower") {
                                if (!text || av.kind() != Value::Kind::Bool) return fail("E020", ap, "'lower' is for char and zstring");
                                a.lower = av.AsBool();
                            } else {
                                return fail("E020", ap, "unknown key '" + ak + "'");
                            }
                        }
                    } else {
                        return fail("E020", ap, "an action is {\"set\":..} or {\"read\":..}");
                    }
                    var.on.emplace_back(sig, std::move(a));
                }
            }
            for (const auto& rt : types) {
                auto& list = context_[rt];
                for (const auto& existing : list)
                    if (existing.name == name) return fail("E020", vp, "declared twice for " + rt);
                list.push_back(var);
            }
        }
    }
    return true;
}

const std::vector<ContextVar>& PatternCatalog::DeclaredContext(const std::string& rt) const {
    static const std::vector<ContextVar> kNone;
    auto it = context_.find(rt);
    return it == context_.end() ? kNone : it->second;
}

}  // namespace skqe
