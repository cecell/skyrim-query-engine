#include "grammar.h"

#include <algorithm>
#include <map>

#include "pattern_engine.h"

namespace skqe {
namespace {

bool IsSig(const std::string& s) { return s.size() == 4; }

// The text an item is reported by: its signature(s), or the group's name.
std::string ItemName(const GrammarItem& it) {
    if (it.isGroup) return it.group;
    std::string s;
    for (const auto& g : it.sigs) s += (s.empty() ? "" : "/") + g;
    return s;
}

// Could an instance of this item begin with `sig` (counts aside)?
bool StartsWith(const GrammarItem& it, const std::string& sig) {
    if (!it.isGroup) return std::find(it.sigs.begin(), it.sigs.end(), sig) != it.sigs.end();
    if (it.unordered) {
        for (const auto& c : it.fields)
            if (StartsWith(c, sig)) return true;
        return false;
    }
    return !it.fields.empty() && StartsWith(it.fields[0], sig);
}

struct Parser {
    std::string err;
    std::set<std::string>* allSigs;
    std::set<std::string>* groups;
    std::map<std::string, int>* sigItems;  // signature -> field items that have it
    std::set<std::string>* many;           // signatures that repeat by max "*" or a repeating group

    bool Fail(const std::string& where, const std::string& msg) {
        err = (where.empty() ? "" : where + ": ") + "E030 " + msg;
        return false;
    }

    bool Items(const Value& list, const std::string& where, bool inMany, std::vector<GrammarItem>* out) {
        if (list.kind() != Value::Kind::List || list.Items().empty()) return Fail(where, "'fields' must be a list of items, not empty");
        std::set<std::string> names;
        size_t i = 0;
        for (const Value& v : list.Items()) {
            std::string w = where + "[" + std::to_string(i++) + "]";
            if (v.kind() != Value::Kind::Map) return Fail(w, "an item is an object");
            GrammarItem it;
            const Value* sig = v.Find("sig");
            const Value* grp = v.Find("group");
            if ((sig != nullptr) == (grp != nullptr)) return Fail(w, "an item has 'sig' or 'group'");
            static const std::set<std::string> kField = {"sig", "min", "max", "label", "note"};
            static const std::set<std::string> kGroup = {"group", "fields", "min", "max", "unordered", "note"};
            for (const auto& m : v.Members()) {
                if (!(sig ? kField : kGroup).count(m.first)) return Fail(w, "unknown key '" + m.first + "'");
            }
            if (const Value* mn = v.Find("min")) {
                if (mn->kind() != Value::Kind::Int || (mn->AsInt() != 0 && mn->AsInt() != 1)) return Fail(w, "'min' is 0 or 1");
                it.min = static_cast<int>(mn->AsInt());
            }
            if (const Value* mx = v.Find("max")) {
                if (mx->IsString() && mx->AsString() == "*") it.many = true;
                else if (!(mx->kind() == Value::Kind::Int && mx->AsInt() == 1)) return Fail(w, "'max' is 1 or \"*\"");
            }
            if (sig) {
                if (sig->IsString()) {
                    it.sigs.push_back(sig->AsString());
                } else if (sig->kind() == Value::Kind::List && !sig->Items().empty()) {
                    for (const auto& s : sig->Items()) {
                        if (!s.IsString()) return Fail(w, "'sig' is a field signature or a list of them");
                        it.sigs.push_back(s.AsString());
                    }
                } else {
                    return Fail(w, "'sig' is a field signature or a list of them");
                }
                for (const auto& s : it.sigs) {
                    if (!IsSig(s)) return Fail(w, "not a field signature: " + s);
                    allSigs->insert(s);
                    ++(*sigItems)[s];
                    if (it.many || inMany) many->insert(s);
                }
                if (const Value* l = v.Find("label")) {
                    if (!l->IsString()) return Fail(w, "'label' is text");
                    it.label = l->AsString();
                }
            } else {
                it.isGroup = true;
                if (!grp->IsString() || grp->AsString().empty()) return Fail(w, "'group' is a name");
                it.group = grp->AsString();
                if (!names.insert(it.group).second) return Fail(w, "two groups named '" + it.group + "' side by side");
                groups->insert(it.group);
                if (const Value* u = v.Find("unordered")) {
                    if (u->kind() != Value::Kind::Bool) return Fail(w, "'unordered' is true or false");
                    it.unordered = u->AsBool();
                }
                const Value* f = v.Find("fields");
                if (!f) return Fail(w, "a group without 'fields'");
                if (!Items(*f, w + ".fields", inMany || it.many, &it.fields)) return false;
                if (!it.unordered && (it.fields[0].isGroup || it.fields[0].min != 1)) {
                    return Fail(w, "an ordered group starts with a field that has min 1 (its leader)");
                }
            }
            out->push_back(std::move(it));
        }
        return true;
    }
};

// A member path through a pattern's shape: "" is the value itself.
bool ShapeHasPath(const Shape& s, const std::string& path) {
    if (path.empty()) return true;
    if (s.kind == Shape::Kind::Unknown) return true;
    if (s.kind == Shape::Kind::List && !s.elem.empty()) return ShapeHasPath(s.elem[0], path);
    if (s.kind != Shape::Kind::Map) return false;
    size_t dot = path.find('.');
    std::string head = path.substr(0, dot);
    std::string rest = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (const auto& f : s.fields)
        if (f.first == head) return ShapeHasPath(f.second, rest);
    return false;
}

// The integer a count path holds in a decoded value (lists summed), or none.
std::optional<int64_t> ValueAt(const Value& v, const std::string& path) {
    if (!path.empty()) {
        size_t dot = path.find('.');
        std::string head = path.substr(0, dot);
        std::string rest = dot == std::string::npos ? "" : path.substr(dot + 1);
        if (v.kind() == Value::Kind::List) {
            int64_t sum = 0;
            bool any = false;
            for (const auto& x : v.Items()) {
                auto r = ValueAt(x, path);
                if (r) {
                    sum += *r;
                    any = true;
                }
            }
            return any ? std::optional<int64_t>(sum) : std::nullopt;
        }
        if (v.kind() != Value::Kind::Map) return std::nullopt;
        const Value* m = v.Find(head);
        return m ? ValueAt(*m, rest) : std::nullopt;
    }
    int64_t i;
    if (v.ToInt(&i)) return i;
    if (v.kind() == Value::Kind::List) {
        int64_t sum = 0;
        for (const auto& x : v.Items()) {
            auto r = ValueAt(x, "");
            if (r) sum += *r;
        }
        return sum;
    }
    return std::nullopt;
}

}  // namespace

bool RecordGrammar::Parse(const Value& json, const ShapeLookup& shapeOf, std::string* error) {
    items_.clear();
    counts_.clear();
    allSigs_.clear();
    multi_.clear();
    groups_.clear();
    std::map<std::string, int> sigItems;
    std::set<std::string> many;
    Parser p{"", &allSigs_, &groups_, &sigItems, &many};
    auto fail = [&](const std::string& msg) {
        *error = msg;
        return false;
    };
    if (json.kind() != Value::Kind::Map) return fail("E030 a record grammar is an object");
    for (const auto& m : json.Members()) {
        if (m.first != "fields" && m.first != "counts" && m.first != "note") return fail("E030 unknown key '" + m.first + "'");
    }
    const Value* fields = json.Find("fields");
    if (!fields) return fail("E030 no 'fields'");
    if (!p.Items(*fields, "fields", false, &items_)) return fail(p.err);
    for (const auto& [sig, n] : sigItems)
        if (n > 1) many.insert(sig);
    multi_ = many;
    if (const Value* counts = json.Find("counts")) {
        if (counts->kind() != Value::Kind::List) return fail("E030 'counts' is a list");
        size_t i = 0;
        for (const Value& c : counts->Items()) {
            std::string w = "counts[" + std::to_string(i++) + "]: ";
            if (c.kind() != Value::Kind::Map) return fail(w + "E030 a count is an object");
            for (const auto& m : c.Members()) {
                if (m.first != "count" && m.first != "of" && m.first != "create" && m.first != "note") {
                    return fail(w + "E030 unknown key '" + m.first + "'");
                }
            }
            GrammarCountRule r;
            const Value* cnt = c.Find("count");
            if (!cnt) return fail(w + "E030 no 'count'");
            if (cnt->IsString()) {
                r.paths.push_back(cnt->AsString());
            } else if (cnt->kind() == Value::Kind::List && !cnt->Items().empty()) {
                for (const auto& x : cnt->Items()) {
                    if (!x.IsString()) return fail(w + "E030 'count' is a path or a list of paths");
                    r.paths.push_back(x.AsString());
                }
            } else {
                return fail(w + "E030 'count' is a path or a list of paths");
            }
            for (const auto& path : r.paths) {
                r.text += (r.text.empty() ? "" : "+") + path;
                std::string sig = path.substr(0, 4);
                std::string member = path.size() > 5 && path[4] == '.' ? path.substr(5) : "";
                if (path.size() != 4 && member.empty()) return fail(w + "E030 a count path is SIG or SIG.member");
                const Shape* shape = shapeOf(sig);
                if (!shape) return fail(w + "E030 " + sig + " has no pattern to read the count from");
                if (!ShapeHasPath(*shape, member)) return fail(w + "E030 the pattern of " + sig + " has no " + member);
            }
            const Value* of = c.Find("of");
            if (!of || of->kind() != Value::Kind::Map) return fail(w + "E030 no 'of'");
            for (const auto& m : of->Members()) {
                if (m.first == "per") continue;
                if (m.first != "occurrences" && m.first != "groups" && m.first != "elements" && m.first != "bytes") {
                    return fail(w + "E030 'of' counts occurrences, groups, elements or bytes");
                }
                if (!r.of.empty()) return fail(w + "E030 'of' names one thing");
                if (!m.second.IsString()) return fail(w + "E030 'of' names a field signature or a group");
                r.of = m.first;
                r.what = m.second.AsString();
            }
            if (r.of.empty()) return fail(w + "E030 'of' counts occurrences, groups, elements or bytes");
            if (r.of == "groups" ? !groups_.count(r.what) : !allSigs_.count(r.what)) {
                return fail(w + "E030 '" + r.what + "' is not in this record's grammar");
            }
            if (const Value* per = of->Find("per")) {
                if (r.of != "bytes") return fail(w + "E030 'per' is for bytes");
                if (per->kind() != Value::Kind::Int || per->AsInt() < 1) return fail(w + "E030 'per' is at least 1");
                r.per = per->AsInt();
            }
            if (const Value* cr = c.Find("create")) {
                if (cr->kind() != Value::Kind::Bool) return fail(w + "E030 'create' is true or false");
                r.create = cr->AsBool();
            }
            counts_.push_back(std::move(r));
        }
    }
    return true;
}

void RecordGrammar::Match(const std::vector<std::string>& sigs, std::vector<GrammarPlacement>* placed,
                          std::vector<GrammarDiag>* diags) const {
    struct Frame {
        const std::vector<GrammarItem>* items;
        bool unordered;
        size_t pos = 0;
        std::vector<int> count;
        std::vector<bool> reported;
        std::string slotPrefix;
        std::string groupPath;
    };
    auto make = [](const std::vector<GrammarItem>* items, bool unordered, std::string slot, std::string group) {
        Frame f{items, unordered, 0, std::vector<int>(items->size(), 0), std::vector<bool>(items->size(), false),
                std::move(slot), std::move(group)};
        return f;
    };
    std::vector<Frame> stack;
    stack.push_back(make(&items_, false, "", ""));
    placed->assign(sigs.size(), GrammarPlacement{});
    auto missing = [&](Frame& f, size_t j, std::optional<size_t> at) {
        if (f.count[j] < (*f.items)[j].min && !f.reported[j]) {
            diags->push_back({"missing", ItemName((*f.items)[j]), f.groupPath, at});
            f.reported[j] = true;
        }
    };
    auto close = [&](std::optional<size_t> at) {
        Frame& f = stack.back();
        for (size_t j = 0; j < f.items->size(); ++j) missing(f, j, at);
        stack.pop_back();
    };
    for (size_t k = 0; k < sigs.size(); ++k) {
        const std::string& s = sigs[k];
        for (;;) {
            Frame& f = stack.back();
            const auto& items = *f.items;
            size_t found = items.size();
            for (size_t i = f.unordered ? 0 : f.pos; i < items.size(); ++i) {
                if ((items[i].many || f.count[i] < 1) && StartsWith(items[i], s)) {
                    found = i;
                    break;
                }
            }
            if (found < items.size()) {
                if (!f.unordered) {
                    for (size_t j = f.pos; j < found; ++j) missing(f, j, k);
                    f.pos = found;
                }
                int instance = f.count[found]++;
                const GrammarItem& it = items[found];
                std::string slot = f.slotPrefix + std::to_string(found);
                if (!it.isGroup) {
                    (*placed)[k] = {slot, f.groupPath};
                    break;
                }
                std::string group = (f.groupPath.empty() ? "" : f.groupPath + "/") + it.group + "[" + std::to_string(instance) + "]";
                stack.push_back(make(&it.fields, it.unordered, slot + "/", group));
                continue;
            }
            if (stack.size() > 1) {
                close(k);
                continue;
            }
            std::string code = !HasSig(s) ? "unknown_field" : k > 0 && sigs[k - 1] == s ? "too_many" : "out_of_order";
            diags->push_back({code, s, "", k});
            (*placed)[k] = {std::nullopt, ""};
            break;
        }
    }
    while (!stack.empty()) close(std::nullopt);
}

std::vector<GrammarCount> RecordGrammar::Counts(const std::vector<std::string>& sigs, const std::vector<const Value*>& values,
                                                const std::vector<size_t>& sizes,
                                                const std::vector<GrammarPlacement>& placed) const {
    std::vector<GrammarCount> out;
    for (const GrammarCountRule& r : counts_) {
        GrammarCount c;
        c.count = r.text;
        if (r.of == "occurrences") {
            for (const auto& s : sigs) c.actual += s == r.what;
        } else if (r.of == "elements") {
            for (size_t i = 0; i < sigs.size(); ++i) {
                if (sigs[i] != r.what) continue;
                const Value* v = values[i];
                c.actual += !v || v->IsNull() ? 0 : v->kind() == Value::Kind::List ? static_cast<int64_t>(v->Items().size()) : 1;
            }
        } else if (r.of == "bytes") {
            int64_t total = 0;
            for (size_t i = 0; i < sigs.size(); ++i)
                if (sigs[i] == r.what) total += static_cast<int64_t>(sizes[i]);
            c.actual = total / r.per;
        } else {  // groups: distinct instances named r.what
            std::set<std::string> instances;
            for (const auto& p : placed) {
                const std::string& g = p.group;
                for (size_t e = 0; e <= g.size();) {
                    size_t slash = g.find('/', e);
                    if (slash == std::string::npos) slash = g.size();
                    std::string seg = g.substr(e, slash - e);
                    if (!seg.empty() && seg.compare(0, r.what.size() + 1, r.what + "[") == 0) instances.insert(g.substr(0, slash));
                    e = slash + 1;
                }
            }
            c.actual = static_cast<int64_t>(instances.size());
        }
        for (const auto& path : r.paths) {
            std::string sig = path.substr(0, 4);
            std::string member = path.size() > 5 ? path.substr(5) : "";
            for (size_t i = 0; i < sigs.size(); ++i) {
                if (sigs[i] != sig) continue;
                if (values[i]) {
                    auto v = ValueAt(*values[i], member);
                    if (v) c.stored = (c.stored ? *c.stored : 0) + *v;
                }
                break;  // the first occurrence
            }
        }
        if (!c.stored) {
            c.ok = c.actual == 0;
            if (!c.ok) c.code = "count_missing";
        } else {
            c.ok = *c.stored == c.actual;
            if (!c.ok) c.code = "count_mismatch";
        }
        out.push_back(std::move(c));
    }
    return out;
}

}  // namespace skqe
