#include "edit/ops.h"

#include <windows.h>

#include "plugin.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <thread>

namespace spf::edit {
namespace {

uint32_t U32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void PutU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Hex6(uint32_t v) {
    char b[16];
    snprintf(b, sizeof(b), "%06X", v & 0xFFFFFF);
    return b;
}

std::string JsonOf(const Value& v) {
    std::string s;
    WriteJson(v, &s);
    return s;
}

std::string JsonText(const std::string& s) { return JsonOf(Value::Str(s)); }

bool SigIs(const Sig& s, const char* t) { return std::memcmp(s.data(), t, 4) == 0; }

bool SigFrom(const Value* v, Sig* out) {
    if (!v || !v->IsString() || v->AsString().size() != 4) return false;
    std::memcpy(out->data(), v->AsString().data(), 4);
    return true;
}

const std::string& StrOr(const Value& op, const char* key, const std::string& dflt) {
    const Value* v = op.Find(key);
    return v && v->IsString() ? v->AsString() : dflt;
}
bool BoolOr(const Value& op, const char* key, bool dflt) {
    const Value* v = op.Find(key);
    return v && v->kind() == Value::Kind::Bool ? v->AsBool() : dflt;
}

bool IsPlacedType(const std::string& t) {
    return t == "REFR" || t == "ACHR" || t == "PMIS" || t == "PARW" || t == "PGRE" || t == "PBEA" || t == "PFLA" ||
           t == "PCON" || t == "PBAR" || t == "PHZD";
}
bool IsCellChildType(const std::string& t) { return IsPlacedType(t) || t == "LAND" || t == "NAVM"; }

// The order of top groups in a plugin, every Bethesda plugin agreeing
// (tools/check_edits.py checks this against the corpus).
const char* const kTopOrder[] = {
    "GMST", "KYWD", "LCRT", "AACT", "TXST", "GLOB", "CLAS", "FACT", "HDPT", "HAIR", "EYES", "RACE", "SOUN", "ASPC",
    "MGEF", "SCPT", "LTEX", "ENCH", "SPEL", "SCRL", "ACTI", "TACT", "ARMO", "BOOK", "CONT", "DOOR", "INGR", "LIGH",
    "MISC", "APPA", "STAT", "SCOL", "MSTT", "PWAT", "GRAS", "TREE", "CLDC", "FLOR", "FURN", "WEAP", "AMMO", "NPC_",
    "LVLN", "KEYM", "ALCH", "IDLM", "COBJ", "PROJ", "HAZD", "SLGM", "LVLI", "WTHR", "CLMT", "SPGD", "RFCT", "REGN",
    "NAVI", "CELL", "WRLD", "DIAL", "QUST", "IDLE", "PACK", "CSTY", "LSCR", "LVSP", "ANIO", "WATR", "EFSH", "EXPL",
    "DEBR", "IMGS", "IMAD", "FLST", "PERK", "BPTD", "ADDN", "AVIF", "CAMS", "CPTH", "VTYP", "MATT", "IPCT", "IPDS",
    "ARMA", "ECZN", "LCTN", "MESG", "RGDL", "DOBJ", "LGTM", "MUSC", "FSTP", "FSTS", "SMBN", "SMQN", "SMEN", "DLBR",
    "MUST", "DLVW", "WOOP", "SHOU", "EQUP", "RELA", "SCEN", "ASTP", "OTFT", "ARTO", "MATO", "MOVT", "SNDR", "DUAL",
    "SNCT", "SOPM", "COLL", "CLFM", "REVB"};

int TopRank(const Sig& s) {
    int i = 0;
    for (const char* t : kTopOrder) {
        if (std::memcmp(t, s.data(), 4) == 0) return i;
        ++i;
    }
    return i;  // unknown types go last
}

// Followers: subrecords that belong right after another one.
struct Follower {
    const char* leader;
    const char* follower;
};
const Follower kFollowers[] = {
    {"CNTO", "COED"}, {"LVLO", "COED"}, {"CTDA", "CIS1"}, {"CTDA", "CIS2"}, {"MODL", "MODT"}, {"MODL", "MODS"},
    {"MOD2", "MO2T"}, {"MOD2", "MO2S"}, {"MOD3", "MO3T"}, {"MOD3", "MO3S"}, {"MOD4", "MO4T"}, {"MOD4", "MO4S"},
    {"MOD5", "MO5T"}, {"MOD5", "MO5S"}, {"DMDL", "DMDT"}, {"DMDL", "DMDS"}, {"XAMT", "XAMC"}, {"XPSN", "XPSC"},
    {"XRGD", "XRGB"}, {"XAPD", "XAPR"}, {"XWCN", "XWCU"}, {"EFID", "EFIT"}, {"ATKD", "ATKE"}};

// A list and the subrecord holding its length.
struct CountedList {
    const char* list;
    const char* count;
    int width;
    bool bytes4;      // count = size / 4 of one subrecord, not occurrences
    bool required;    // the count must exist while the list does
};
const CountedList kCounted[] = {{"KWDA", "KSIZ", 4, true, true},
                                {"CNTO", "COCT", 4, false, true},
                                {"SPLO", "SPCT", 4, false, true},
                                {"LVLO", "LLCT", 1, false, true},
                                {"CTDA", "CITC", 4, false, false}};

struct OwnedBody {
    Bytes storage;  // backing for views parsed from the old body
    std::deque<Bytes> added;
    std::vector<SubView> subs;

    void Replace(size_t i, Bytes bytes) {
        added.push_back(std::move(bytes));
        subs[i].data = added.back().data();
        subs[i].size = added.back().size();
        if (subs[i].size > 0xFFFF) {
            subs[i].viaXxxx = true;
            subs[i].sizeField = 0;
        } else {
            subs[i].viaXxxx = false;
            subs[i].sizeField = static_cast<uint16_t>(subs[i].size);
        }
    }
    void Insert(size_t i, Sig sig, Bytes bytes) {
        added.push_back(std::move(bytes));
        SubView s;
        s.sig = sig;
        s.data = added.back().data();
        s.size = added.back().size();
        if (s.size > 0xFFFF) s.viaXxxx = true;
        else s.sizeField = static_cast<uint16_t>(s.size);
        subs.insert(subs.begin() + i, s);
    }
};

// One subrecord's decode.
struct SubTrace {
    bool hasPattern = false;
    bool decoded = false;
    std::string failure;
    Value value;
    std::vector<TraceLeaf> leaves;
    std::string json;
};

// The same running context Plugin::Fields keeps while it walks a record.
std::vector<SubTrace> TraceSubs(const std::string& rt, uint16_t formVersion, bool localized,
                                const std::vector<SubView>& subs, const PatternCatalog& cat) {
    std::vector<SubTrace> out(subs.size());
    DecodeContext ctx;
    ctx.rt = rt;
    ctx.formVersion = formVersion;
    ctx.localized = localized;
    int64_t lastEpft = 0;
    for (size_t i = 0; i < subs.size(); ++i) {
        const SubView& s = subs[i];
        std::string fs = SigText(s.sig);
        if (rt == "PERK") {
            if (fs == "PRKE") lastEpft = 0;
            else if (fs == "EPFT" && s.size) lastEpft = s.data[0];
            ctx.epft = lastEpft;
        }
        if (rt == "GMST" && fs == "EDID" && s.size) ctx.edidPrefix = std::string(1, static_cast<char>(tolower(s.data[0])));
        if (rt == "PACK" && fs == "ANAM") {
            ctx.lastAnam.assign(reinterpret_cast<const char*>(s.data), strnlen(reinterpret_cast<const char*>(s.data), s.size));
        }
        const Pattern* pat = cat.Find(rt, fs);
        if (!pat) continue;
        out[i].hasPattern = true;
        ctx.fsig = fs;
        try {
            out[i].value = TraceSubrecord(*pat, s.data, s.size, ctx, &out[i].leaves);
            out[i].decoded = true;
            out[i].json = JsonOf(out[i].value);
        } catch (const std::exception& e) {
            out[i].leaves.clear();
            out[i].failure = e.what();
        }
    }
    return out;
}

bool SameBytes(const SubView& a, const SubView& b) {
    return a.sig == b.sig && a.size == b.size && (a.size == 0 || std::memcmp(a.data, b.data, a.size) == 0);
}

// After an edit: every other subrecord has the same bytes and decodes the
// same (a change can reach them through $@EDID0, $@ANAM, $@EPFT), and inside
// the target only the target leaf changed.
bool OnlyTargetChanged(const std::vector<SubView>& beforeSubs, const std::vector<SubTrace>& before,
                       const std::vector<SubView>& afterSubs, const std::vector<SubTrace>& after, size_t target,
                       const std::string& leafPath, std::string* why) {
    if (beforeSubs.size() != afterSubs.size()) {
        *why = "the number of subrecords changed";
        return false;
    }
    for (size_t i = 0; i < beforeSubs.size(); ++i) {
        if (i == target) continue;
        if (!SameBytes(beforeSubs[i], afterSubs[i])) {
            *why = SigText(beforeSubs[i].sig) + " changed bytes";
            return false;
        }
        if (before[i].json != after[i].json) {
            *why = SigText(beforeSubs[i].sig) + " now decodes differently";
            return false;
        }
    }
    const auto& a = before[target].leaves;
    const auto& b = after[target].leaves;
    if (a.size() != b.size()) {
        *why = "the subrecord's layout changed";
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].path != b[i].path) {
            *why = "the subrecord's layout changed at " + a[i].path;
            return false;
        }
        if (a[i].path != leafPath && JsonOf(a[i].value) != JsonOf(b[i].value)) {
            *why = "value " + a[i].path + " changed too";
            return false;
        }
    }
    return true;
}

// A field path: "DATA.damage", "CTDA[2].func_idx", "KWDA[0]", "EDID".
struct FieldPath {
    Sig sig{};
    int occurrence = -1;  // -1 = not given
    std::string inner;
    std::string bracketInner;  // inner, if the bracket turns out to index inside the value
};

bool ParseFieldPath(const std::string& text, FieldPath* out) {
    if (text.size() < 4) return false;
    std::memcpy(out->sig.data(), text.data(), 4);
    size_t q = 4;
    if (q < text.size() && text[q] == '[') {
        size_t close = text.find(']', q);
        if (close == std::string::npos) return false;
        out->occurrence = atoi(text.c_str() + q + 1);
        out->bracketInner = text.substr(q);
        q = close + 1;
    }
    if (q < text.size() && text[q] == '.') ++q;
    out->inner = text.substr(q);
    return true;
}

}  // namespace

// ---- data files --------------------------------------------------------------

namespace {

std::vector<std::vector<std::string>> ParseCsv(const std::string& text) {
    std::vector<std::vector<std::string>> rows(1);
    std::string field;
    bool quoted = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(c);
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            rows.back().push_back(std::move(field));
            field.clear();
        } else if (c == '\n') {
            rows.back().push_back(std::move(field));
            field.clear();
            rows.emplace_back();
        } else if (c != '\r') {
            field.push_back(c);
        }
    }
    if (!field.empty() || !rows.back().empty()) rows.back().push_back(std::move(field));
    else rows.pop_back();
    return rows;
}

bool ReadWhole(const std::wstring& path, std::string* out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

}  // namespace

bool FieldOrder::Load(const std::string& csvText) {
    auto rows = ParseCsv(csvText);
    if (rows.empty()) return false;
    for (size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].size() < 4 || rows[i][2].size() != 4) continue;
        bool always = rows[i].size() > 4 && rows[i][4] == "1";
        byType_[rows[i][0]].push_back({MakeSig(rows[i][2].c_str()), atoi(rows[i][1].c_str()), rows[i][3] == "extra-list", always});
    }
    return !byType_.empty();
}

const std::vector<FieldOrder::Entry>* FieldOrder::Of(const std::string& t) const {
    auto it = byType_.find(t);
    return it == byType_.end() ? nullptr : &it->second;
}

bool CatalogOpen::Load(const std::string& csvText) {
    auto rows = ParseCsv(csvText);
    for (size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].size() < 5) continue;
        Row r;
        const std::string& off = rows[i][2];
        r.offset = !off.empty() && isdigit(static_cast<unsigned char>(off[0])) ? atoi(off.c_str()) : -1;
        const std::string& sz = rows[i][3];
        r.size = !sz.empty() && isdigit(static_cast<unsigned char>(sz[0])) ? atoi(sz.c_str()) : -1;
        r.correction = rows[i][4];
        byKey_[rows[i][0] + "." + rows[i][1]].push_back(std::move(r));
    }
    return true;
}

std::vector<CatalogOpen::Row> CatalogOpen::Overlapping(const std::string& rt, const std::string& fs, size_t offset,
                                                       size_t size) const {
    std::vector<Row> out;
    for (const std::string& key : {rt + "." + fs, "*." + fs}) {
        auto it = byKey_.find(key);
        if (it == byKey_.end()) continue;
        for (const Row& r : it->second) {
            if (r.offset < 0) {
                out.push_back(r);
                continue;
            }
            size_t a = static_cast<size_t>(r.offset);
            size_t b = r.size < 0 ? SIZE_MAX : a + static_cast<size_t>(std::max(r.size, 1));
            if (a < offset + std::max<size_t>(size, 1) && offset < b) out.push_back(r);
        }
    }
    return out;
}

bool LoadEditData(const std::wstring& folder, const PatternCatalog* catalog, EditData* out, std::string* error) {
    out->catalog = catalog;
    std::string text;
    if (!ReadWhole(folder + L"field_order.csv", &text) || !out->order.Load(text)) {
        *error = "cannot read field_order.csv";
        return false;
    }
    if (ReadWhole(folder + L"catalog_open.csv", &text)) out->open.Load(text);
    return true;
}

// ---- the editor --------------------------------------------------------------

struct Editor::Impl {
    Editor& e;
    Document& doc;
    const PatternCatalog& cat;
    OpResult res;

    explicit Impl(Editor& ed) : e(ed), doc(*ed.doc_), cat(*ed.data_->catalog) {}

    bool Fail(const char* code, std::string message) {
        res.ok = false;
        res.code = code;
        res.message = std::move(message);
        return false;
    }
    void Warn(std::string w) { res.warnings.push_back(std::move(w)); }
    void Changed(const std::string& form, const char* what, const std::string& field, std::string before = {},
                 std::string after = {}) {
        res.changes.push_back({form, what, field, std::move(before), std::move(after)});
    }

    // --- facts -------------------------------------------------------------

    uint32_t SelfIndex() const { return static_cast<uint32_t>(e.masters_.size()); }

    std::string FormKey(uint32_t stored) const {
        uint32_t h = stored >> 24;
        const std::string& owner = h < e.masters_.size() ? e.masters_[h] : doc.name();
        return owner + ":" + Hex6(stored);
    }

    bool IsLightSelf() const {
        const Node& t = *doc.tes4();
        std::string n = Lower(doc.name());
        return (t.flags() & 0x200) != 0 || (n.size() >= 4 && n.compare(n.size() - 4, 4, ".esl") == 0);
    }

    // "Owner.esm:012EB7" or a stored "00012EB7" -> stored FormID in this plugin.
    bool ResolveForm(const std::string& text, uint32_t* out, bool quiet = false) {
        size_t colon = text.rfind(':');
        if (colon == std::string::npos) {
            char* end = nullptr;
            unsigned long v = strtoul(text.c_str(), &end, 16);
            if (text.empty() || *end || text.size() > 8) {
                return quiet ? false : Fail("bad_form", "expected Owner.esm:XXXXXX or eight hex digits, got " + text);
            }
            *out = static_cast<uint32_t>(v);
            return true;
        }
        std::string owner = Lower(text.substr(0, colon));
        char* end = nullptr;
        unsigned long id = strtoul(text.c_str() + colon + 1, &end, 16);
        if (*end || id > 0xFFFFFF) return quiet ? false : Fail("bad_form", "bad object ID in " + text);
        for (size_t i = 0; i < e.masters_.size(); ++i) {
            if (Lower(e.masters_[i]) == owner) {
                *out = static_cast<uint32_t>(i << 24 | id);
                return true;
            }
        }
        if (owner == Lower(doc.name())) {
            *out = SelfIndex() << 24 | static_cast<uint32_t>(id);
            return true;
        }
        return quiet ? false
                     : Fail("master_missing", text.substr(0, colon) + " is not a master of " + doc.name() +
                                                  " (add it with add-master)");
    }

    // The rules on a reference this plugin is about to store.
    bool CheckReference(uint32_t stored) {
        if (stored == 0) return true;
        uint32_t h = stored >> 24, id = stored & 0xFFFFFF;
        const std::string& owner = h < e.masters_.size() ? e.masters_[h] : doc.name();
        std::string low = Lower(owner);
        if (id > 0xFFF && low.size() >= 4 && low.compare(low.size() - 4, 4, ".esl") == 0) {
            return Fail("light_master_range", FormKey(stored) + ": a light master's references are masked to 12 bits");
        }
        if (id < 0x800 && e.version_ < 1.705f && low != "skyrim.esm") {
            return Fail("low_id_needs_171", FormKey(stored) + ": object IDs below 0x800 read as Skyrim.esm in a plugin with HEDR below 1.71");
        }
        return true;
    }

    // --- bodies ------------------------------------------------------------

    bool LoadBody(const Node& n, OwnedBody* b) {
        const uint8_t* body;
        size_t size;
        Bytes scratch;
        if (!Document::PlainBody(n, &scratch, &body, &size)) {
            return Fail("opaque_record", FormKey(n.formId()) + ": the compressed body will not inflate; only raw record bytes can be kept");
        }
        b->storage.assign(body, body + size);
        ParsedBody parsed = ParseBody(b->storage.data(), b->storage.size());
        if (parsed.issue != BodyIssue::None) {
            return Fail("opaque_record", FormKey(n.formId()) + ": the body cannot be split exactly (" + BodyIssueName(parsed.issue) + ")");
        }
        b->subs = std::move(parsed.subs);
        return true;
    }

    NodePtr WithBody(const Node& n, const OwnedBody& b) {
        auto c = std::make_shared<Node>(n);
        c->span = nullptr;
        c->spanSize = 0;
        auto plain = std::make_shared<Bytes>();
        EmitBody(b.subs, plain.get());
        c->plain = plain;
        c->stored = nullptr;
        c->storedSize = 0;
        c->owned.reset();
        return c;
    }

    // Replace a record without moving anything: paths stay valid.
    void PutRecord(const Path& path, NodePtr node) {
        NodePtr root = doc.Replaced(path, std::move(node));
        doc.SetRootKeepIndex(std::move(root));
    }

    bool FindUnique(const std::string& text, const Value& op, Path* path, uint32_t* stored) {
        if (!ResolveForm(text, stored)) return false;
        const std::vector<Path>* found = doc.Find(*stored);
        if (!found) return Fail("not_found", text + " is not in " + doc.name());
        size_t nth = 0;
        if (found->size() > 1) {
            const Value* n = op.Find("nth");
            int64_t i = 0;
            if (!n || !n->ToInt(&i) || i < 0 || static_cast<size_t>(i) >= found->size()) {
                return Fail("duplicate_form", text + " occurs " + std::to_string(found->size()) + " times in this file; give nth");
            }
            nth = static_cast<size_t>(i);
        }
        *path = (*found)[nth];
        return true;
    }

    std::string TypeOf(const Node& n) const { return SigText(n.sig()); }

    // --- header facts ------------------------------------------------------

    bool EditTes4(const std::function<bool(OwnedBody&)>& fn) {
        OwnedBody b;
        if (!LoadBody(*doc.tes4(), &b)) return false;
        if (!fn(b)) return false;
        PutRecord(Path{0}, WithBody(*doc.tes4(), b));
        e.LoadHeaderFacts();
        return true;
    }

    size_t FindSub(const OwnedBody& b, const char* sig) const {
        for (size_t i = 0; i < b.subs.size(); ++i)
            if (SigIs(b.subs[i].sig, sig)) return i;
        return SIZE_MAX;
    }

    // HEDR after a structural edit: count recomputed, next object ID raised.
    bool RefreshHedr(std::optional<uint32_t> newNext = std::nullopt) {
        uint32_t count = doc.CountForHedr();
        return EditTes4([&](OwnedBody& b) {
            size_t i = FindSub(b, "HEDR");
            if (i == SIZE_MAX || b.subs[i].size < 12) return Fail("bad_header", "no usable HEDR");
            Bytes h(b.subs[i].data, b.subs[i].data + b.subs[i].size);
            PutU32(h.data() + 4, count);
            if (newNext) PutU32(h.data() + 8, *newNext);
            b.Replace(i, std::move(h));
            return true;
        });
    }

    uint32_t NextObjectId() {
        OwnedBody b;
        if (!LoadBody(*doc.tes4(), &b)) return 0x800;
        size_t i = FindSub(b, "HEDR");
        if (i == SIZE_MAX || b.subs[i].size < 12) return 0x800;
        return U32(b.subs[i].data + 8);
    }

    bool HasOnam() {
        OwnedBody b;
        return LoadBody(*doc.tes4(), &b) && FindSub(b, "ONAM") != SIZE_MAX;
    }

    // Keeps ONAM true for an override that came or went: only where the plugin
    // already keeps one or is ESM-flagged; the Creation Kit's rule for what
    // belongs in it (temporary placed references, LAND, NAVM); sorted.
    bool SyncOnam(const std::string& type, uint32_t recordFlags, uint32_t stored, bool add) {
        if (stored >> 24 >= SelfIndex()) return true;  // not an override
        bool persistent = (recordFlags & 0x400) != 0;
        bool belongs = type == "LAND" || type == "NAVM" || (IsPlacedType(type) && type != "ACHR" && !persistent);
        if (!belongs) return true;
        if (!HasOnam() && !(doc.tes4()->flags() & 1)) return true;
        return EditTes4([&](OwnedBody& b) {
            size_t i = FindSub(b, "ONAM");
            std::vector<uint32_t> ids;
            if (i != SIZE_MAX) {
                for (size_t k = 0; k + 4 <= b.subs[i].size; k += 4) ids.push_back(U32(b.subs[i].data + k));
            }
            auto it = std::lower_bound(ids.begin(), ids.end(), stored);
            if (add) {
                if (it != ids.end() && *it == stored) return true;
                ids.insert(it, stored);
            } else {
                if (it == ids.end() || *it != stored) return true;
                ids.erase(it);
            }
            Bytes raw(ids.size() * 4);
            for (size_t k = 0; k < ids.size(); ++k) PutU32(raw.data() + k * 4, ids[k]);
            if (i != SIZE_MAX) {
                if (raw.empty()) b.subs.erase(b.subs.begin() + i);
                else b.Replace(i, std::move(raw));
            } else {
                // After the masters, before INTV: the Creation Kit's order.
                size_t at = 0;
                for (size_t k = 0; k < b.subs.size(); ++k) {
                    if (SigIs(b.subs[k].sig, "HEDR") || SigIs(b.subs[k].sig, "CNAM") || SigIs(b.subs[k].sig, "SNAM") ||
                        SigIs(b.subs[k].sig, "MAST") || SigIs(b.subs[k].sig, "DATA")) {
                        at = k + 1;
                    }
                }
                b.Insert(at, MakeSig("ONAM"), std::move(raw));
            }
            return true;
        });
    }

    // --- subrecord placement ------------------------------------------------

    // Where a new subrecord goes: after its leader if it has one; then the
    // Creation Kit's written order; nothing already there moves.
    bool PlaceNew(const std::string& rt, const OwnedBody& b, const Sig& sig, const Value& op, size_t* at) {
        Sig anchor;
        if (SigFrom(op.Find("after"), &anchor)) {
            for (size_t i = b.subs.size(); i-- > 0;) {
                if (b.subs[i].sig == anchor) {
                    *at = i + 1;
                    return true;
                }
            }
            return Fail("not_found", "no " + SigText(anchor) + " to place it after");
        }
        if (SigFrom(op.Find("before"), &anchor)) {
            for (size_t i = 0; i < b.subs.size(); ++i) {
                if (b.subs[i].sig == anchor) {
                    *at = i;
                    return true;
                }
            }
            return Fail("not_found", "no " + SigText(anchor) + " to place it before");
        }
        for (const Follower& f : kFollowers) {
            if (std::memcmp(f.follower, sig.data(), 4) != 0) continue;
            for (size_t i = b.subs.size(); i-- > 0;) {
                if (SigIs(b.subs[i].sig, f.leader)) {
                    size_t j = i + 1;
                    while (j < b.subs.size() && IsFollowerOf(b.subs[j].sig, f.leader)) ++j;
                    *at = j;
                    return true;
                }
            }
        }
        const auto* order = e.data_->order.Of(rt);
        if (!order) return Fail("no_order", rt + " has no known subrecord order; give after or before");
        std::vector<int> mine;
        bool extra = false;
        for (const auto& en : *order) {
            if (en.sig == sig) {
                mine.push_back(en.position);
                extra = extra || en.extraList;
            }
        }
        if (mine.empty()) return Fail("no_order", SigText(sig) + " has no known position in " + rt + "; give after or before");
        if (extra) {
            // The extra-data block keeps input order: new ones go at its end,
            // before the fixed tail of a reference.
            size_t tail = b.subs.size();
            for (size_t i = 0; i < b.subs.size(); ++i) {
                if (rt != "CELL" && (SigIs(b.subs[i].sig, "XLRL") || SigIs(b.subs[i].sig, "XSCL") ||
                                     SigIs(b.subs[i].sig, "ONAM") || SigIs(b.subs[i].sig, "DATA"))) {
                    tail = i;
                    break;
                }
            }
            *at = tail;
            return true;
        }
        if (mine.size() > 1) {
            return Fail("ambiguous_position", SigText(sig) + " appears at several positions in " + rt + "; give after or before");
        }
        int rank = mine[0];
        auto minRank = [&](const Sig& s) {
            int best = INT_MAX;
            for (const auto& en : *order)
                if (en.sig == s) best = std::min(best, en.position);
            return best;
        };
        for (size_t i = b.subs.size(); i-- > 0;) {
            if (minRank(b.subs[i].sig) < rank) {
                *at = i + 1;
                return true;
            }
        }
        *at = 0;
        return true;
    }

    bool IsFollowerOf(const Sig& s, const char* leader) const {
        for (const Follower& f : kFollowers)
            if (std::memcmp(f.leader, leader, 4) == 0 && SigIs(s, f.follower)) return true;
        return false;
    }

    // Keep list lengths true after a list changed.
    bool SyncCounts(const std::string& rt, OwnedBody& b, const Sig& changed) {
        for (const CountedList& c : kCounted) {
            if (!SigIs(changed, c.list)) continue;
            uint64_t n = 0;
            size_t first = SIZE_MAX;
            for (size_t i = 0; i < b.subs.size(); ++i) {
                if (SigIs(b.subs[i].sig, c.list)) {
                    if (first == SIZE_MAX) first = i;
                    n += c.bytes4 ? b.subs[i].size / 4 : 1;
                }
            }
            size_t countAt = FindSub(b, c.count);
            int countSubs = 0;
            for (const SubView& s : b.subs) countSubs += SigIs(s.sig, c.count);
            if (countSubs > 1) {
                Warn(SigText(MakeSig(c.count)) + " occurs several times in this " + rt + ": not updated");
                return true;
            }
            if (n == 0) {
                if (countAt != SIZE_MAX) b.subs.erase(b.subs.begin() + countAt);
                return true;
            }
            Bytes raw(static_cast<size_t>(c.width));
            for (int i = 0; i < c.width; ++i) raw[i] = static_cast<uint8_t>(n >> (8 * i));
            if (countAt != SIZE_MAX) {
                b.Replace(countAt, std::move(raw));
            } else if (c.required) {
                b.Insert(first, MakeSig(c.count), std::move(raw));
            }
        }
        return true;
    }

    // --- set ---------------------------------------------------------------

    // Bytes for a whole new subrecord from an op's "raw" (hex) or "value".
    bool SubBytesFrom(const std::string& rt, const Sig& sig, const Value& op, Bytes* out) {
        if (const Value* raw = op.Find("raw")) {
            if (!raw->IsString() || raw->AsString().size() % 2) return Fail("bad_value", "raw needs hex digits");
            std::string err;
            const std::string& h = raw->AsString();
            for (size_t i = 0; i < h.size(); i += 2) {
                char buf[3] = {h[i], h[i + 1], 0};
                char* end = nullptr;
                long v = strtol(buf, &end, 16);
                if (*end) return Fail("bad_value", "raw needs hex digits");
                out->push_back(static_cast<uint8_t>(v));
            }
            return true;
        }
        const Value* val = op.Find("value");
        if (!val) {
            out->clear();  // a marker with no data
            return true;
        }
        const Pattern* pat = cat.Find(rt, SigText(sig));
        if (!pat) return Fail("no_pattern", rt + "." + SigText(sig) + " has no pattern: give raw bytes");
        Value input = *val;
        if (!ResolveInput(*pat, &input)) return false;
        std::string err;
        if (!PatternIsLeaf(*pat)) return Fail("not_a_leaf", rt + "." + SigText(sig) + " has a structure: give raw bytes");
        if (!EncodeLeaf(*pat, input, nullptr, 0, e.localized_, out, &err)) return Fail("bad_value", err);
        return true;
    }

    // Owner.esm:XXXXXX -> the stored number, for a FormID leaf.
    bool ResolveInput(const Pattern& leaf, Value* input) {
        if (!LeafIsFormId(leaf)) return true;
        if (input->IsString() && input->AsString().rfind("raw:", 0) != 0) {
            uint32_t stored;
            if (input->AsString().find(':') != std::string::npos) {
                if (!ResolveForm(input->AsString(), &stored)) return false;
            } else if (input->AsString().empty()) {
                stored = 0;
            } else if (!ResolveForm(input->AsString(), &stored)) {
                return false;
            }
            *input = Value::Int(stored);
        }
        int64_t v;
        if (input->ToInt(&v) && !CheckReference(static_cast<uint32_t>(v))) return false;
        return true;
    }

    bool OpSet(const Value& op) {
        const std::string& formText = StrOr(op, "form", "");
        const std::string& fieldText = StrOr(op, "field", "");
        bool force = BoolOr(op, "force", false);
        Path path;
        uint32_t stored;
        if (!FindUnique(formText, op, &path, &stored)) return false;
        const Node& node = *doc.At(path);
        std::string rt = TypeOf(node);
        OwnedBody b;
        if (!LoadBody(node, &b)) return false;
        FieldPath fp;
        if (!ParseFieldPath(fieldText, &fp)) return Fail("bad_field", "cannot read field path " + fieldText);
        std::vector<size_t> occ;
        for (size_t i = 0; i < b.subs.size(); ++i)
            if (b.subs[i].sig == fp.sig) occ.push_back(i);
        if (occ.empty()) return Fail("not_found", rt + " " + formText + " has no " + SigText(fp.sig) + " (add it with add-sub)");
        std::string sigText = SigText(fp.sig);
        size_t target;
        std::string inner = fp.inner;
        if (fp.occurrence >= 0 && (occ.size() > 1 || cat.IsMulti(rt, sigText))) {
            if (static_cast<size_t>(fp.occurrence) >= occ.size()) return Fail("not_found", fieldText + ": only " + std::to_string(occ.size()) + " of them");
            target = occ[static_cast<size_t>(fp.occurrence)];
        } else if (fp.occurrence >= 0) {
            target = occ[0];
            inner = fp.bracketInner;  // an index into the value
        } else if (occ.size() > 1) {
            return Fail("ambiguous_field", sigText + " occurs " + std::to_string(occ.size()) + " times: use " + sigText + "[n]");
        } else {
            target = occ[0];
        }

        const Value* val = op.Find("value");
        if (const Value* raw = op.Find("raw"); raw && inner.empty()) {
            // Whole subrecord, exact bytes: no catalog involved.
            Bytes bytes;
            if (!SubBytesFrom(rt, fp.sig, op, &bytes)) return false;
            std::string before = JsonText(HexOf(b.subs[target].data, b.subs[target].size));
            std::string after = JsonText(HexOf(bytes.data(), bytes.size()));
            std::vector<SubView> oldSubs = b.subs;
            b.Replace(target, std::move(bytes));
            auto beforeTrace = TraceSubs(rt, node.formVersion(), e.localized_, oldSubs, cat);
            auto afterTrace = TraceSubs(rt, node.formVersion(), e.localized_, b.subs, cat);
            for (size_t i = 0; i < b.subs.size(); ++i) {
                if (i != target && beforeTrace[i].json != afterTrace[i].json) {
                    if (!force) return Fail("layout_dependency", SigText(b.subs[i].sig) + " would decode differently; force to accept");
                    Warn(SigText(b.subs[i].sig) + " now decodes differently");
                }
            }
            if (!afterTrace[target].decoded && afterTrace[target].hasPattern) {
                Warn(fieldText + ": the new bytes do not fit the catalog's pattern");
            }
            PutRecord(path, WithBody(node, b));
            Changed(formText, "set", fieldText, before, after);
            return true;
        }
        if (!val) return Fail("bad_op", "set needs value (or raw for a whole subrecord)");

        auto beforeTrace = TraceSubs(rt, node.formVersion(), e.localized_, b.subs, cat);
        const SubTrace& st = beforeTrace[target];
        if (!st.hasPattern) return Fail("no_pattern", rt + "." + sigText + " has no pattern: use raw");
        if (!st.decoded) return Fail("not_decodable", fieldText + " does not fit its pattern (" + st.failure + "): use raw");
        const TraceLeaf* leaf = nullptr;
        for (const TraceLeaf& l : st.leaves) {
            if (l.path == inner) {
                if (leaf) return Fail("ambiguous_field", fieldText + " matches several values");
                leaf = &l;
            }
        }
        if (!leaf) {
            std::string names;
            size_t shown = 0;
            for (const TraceLeaf& l : st.leaves) {
                if (!l.pattern) continue;
                if (shown++ < 16) names += (names.empty() ? "" : ", ") + (l.path.empty() ? std::string("(whole)") : l.path);
            }
            return Fail("not_found", "no value at " + fieldText + "; values here: " + names);
        }
        if (!leaf->pattern) return Fail("layout_dependency", fieldText + " is a list's length");
        if (leaf->bound && !force) {
            return Fail("layout_dependency", fieldText + ": later values in this subrecord depend on it; use raw on the whole subrecord, or force");
        }
        for (const CatalogOpen::Row& r : e.data_->open.Overlapping(rt, sigText, leaf->offset, leaf->size)) {
            bool layout = r.correction == "SIZE_MISMATCH" || r.correction == "FIELD_SPLIT" ||
                          r.correction == "MISSING_FIELD" || r.correction == "REPEAT_WRONG";
            if (layout && !force) {
                return Fail("catalog_open", fieldText + ": the catalog's layout here is in question (" + r.correction + "); use raw, or force");
            }
            Warn(fieldText + ": open catalog row (" + r.correction + ")");
        }
        Value input = *val;
        if (!ResolveInput(*leaf->pattern, &input)) return false;
        const uint8_t* curBytes = b.subs[target].data + leaf->offset;

        // Asking for what it already shows changes nothing, even where the
        // display rounds (degrees, colours): those bytes stay exact.
        if (ValuesEqual(leaf->value, input)) {
            Changed(formText, "unchanged", fieldText, JsonOf(leaf->value), JsonOf(leaf->value));
            return true;
        }
        Bytes encoded;
        std::string err;
        if (!EncodeLeaf(*leaf->pattern, input, curBytes, leaf->size, e.localized_, &encoded, &err)) {
            return Fail("bad_value", fieldText + ": " + err);
        }
        if (encoded.size() == leaf->size && (encoded.empty() || std::memcmp(encoded.data(), curBytes, encoded.size()) == 0)) {
            Changed(formText, "unchanged", fieldText, JsonOf(leaf->value), JsonOf(leaf->value));
            return true;
        }
        std::string beforeJson = JsonOf(leaf->value);
        std::string leafPath = leaf->path;
        size_t leafOffset = leaf->offset, leafSize = leaf->size;

        Bytes newSub;
        const SubView oldSub = b.subs[target];
        newSub.insert(newSub.end(), oldSub.data, oldSub.data + leafOffset);
        newSub.insert(newSub.end(), encoded.begin(), encoded.end());
        newSub.insert(newSub.end(), oldSub.data + leafOffset + leafSize, oldSub.data + oldSub.size);
        std::vector<SubView> oldSubs = b.subs;
        b.Replace(target, std::move(newSub));

        // The self-check: re-decode everything and compare.
        auto afterTrace = TraceSubs(rt, node.formVersion(), e.localized_, b.subs, cat);
        std::string why;
        if (!afterTrace[target].decoded) return Fail("readback", fieldText + ": the subrecord no longer decodes");
        if (!OnlyTargetChanged(oldSubs, beforeTrace, b.subs, afterTrace, target, leafPath, &why)) {
            if (!force) return Fail("layout_dependency", fieldText + ": " + why + "; use raw on the whole subrecord, or force");
            Warn(fieldText + ": " + why);
        }
        std::string afterJson;
        for (const TraceLeaf& l : afterTrace[target].leaves) {
            if (l.path != leafPath) continue;
            afterJson = JsonOf(l.value);
            if (!ReadsBackAs(l.value, input, *val)) {
                return Fail("readback", fieldText + ": wrote " + JsonOf(*val) + " but it reads back as " + afterJson);
            }
        }
        PutRecord(path, WithBody(node, b));
        Changed(formText, "set", fieldText, beforeJson, afterJson);
        return true;
    }

    static std::string HexOf(const uint8_t* p, size_t n) {
        static const char k[] = "0123456789ABCDEF";
        std::string s;
        for (size_t i = 0; i < n; ++i) {
            s.push_back(k[p[i] >> 4]);
            s.push_back(k[p[i] & 15]);
        }
        return s;
    }

    static bool IsNum(const Value& v) { return v.kind() == Value::Kind::Int || v.kind() == Value::Kind::Float; }
    static double Num(const Value& v) { return v.kind() == Value::Kind::Int ? static_cast<double>(v.AsInt()) : v.AsFloat(); }

    static bool ValuesEqual(const Value& shown, const Value& in) {
        if (IsNum(shown) && IsNum(in)) return Num(shown) == Num(in);
        return shown.kind() == in.kind() && JsonOf(shown) == JsonOf(in);
    }

    // Numbers come back within the stored type's precision; a FormID, enum or
    // flag input is written as asked and shows as its own spelling.
    static bool ReadsBackAs(const Value& shown, const Value& input, const Value& asked) {
        if (input.IsString() && input.AsString().rfind("raw:", 0) == 0) return true;
        if (IsNum(shown) && IsNum(asked)) {
            double a = Num(shown), b = Num(asked);
            double tolerance = std::max(1e-5, std::abs(b) * 1e-5);
            // A *_div100 field holds two decimals, *_div65535 about five.
            return std::abs(a - b) <= std::max(tolerance, 0.0051);
        }
        if (shown.IsString() && asked.IsString()) {
            const std::string& t = asked.AsString();
            bool ascii = std::all_of(t.begin(), t.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
            if (ascii && shown.AsString().rfind("[", 0) != 0 && asked.AsString().find(':') == std::string::npos &&
                t.rfind("0x", 0) != 0) {
                // Text and enum names read back as written, unless a number was given for an enum.
                char* end = nullptr;
                strtod(t.c_str(), &end);
                if (!t.empty() && *end == 0) return true;
                return shown.AsString() == t;
            }
        }
        return true;
    }

    // --- add-sub / remove-sub ----------------------------------------------

    bool OpAddSub(const Value& op) {
        const std::string& formText = StrOr(op, "form", "");
        Sig sig;
        if (!SigFrom(op.Find("sig"), &sig)) return Fail("bad_op", "add-sub needs sig, four characters");
        Path path;
        uint32_t stored;
        if (!FindUnique(formText, op, &path, &stored)) return false;
        const Node& node = *doc.At(path);
        std::string rt = TypeOf(node);
        OwnedBody b;
        if (!LoadBody(node, &b)) return false;
        Bytes bytes;
        if (!SubBytesFrom(rt, sig, op, &bytes)) return false;
        size_t at = 0;
        if (!PlaceNew(rt, b, sig, op, &at)) return false;
        std::vector<SubView> oldSubs = b.subs;
        std::string after = JsonText(HexOf(bytes.data(), bytes.size()));
        b.Insert(at, sig, std::move(bytes));
        if (!CheckNeighbours(rt, node, oldSubs, b, BoolOr(op, "force", false))) return false;
        if (!SyncCounts(rt, b, sig)) return false;
        PutRecord(path, WithBody(node, b));
        Changed(formText, "add-sub", SigText(sig), "", after);
        return true;
    }

    // The other subrecords must decode as before: a new ANAM or EDID can change
    // how later ones read. The new subrecord should fit its pattern.
    bool CheckNeighbours(const std::string& rt, const Node& node, const std::vector<SubView>& oldSubs,
                         const OwnedBody& b, bool force) {
        auto before = TraceSubs(rt, node.formVersion(), e.localized_, oldSubs, cat);
        auto after = TraceSubs(rt, node.formVersion(), e.localized_, b.subs, cat);
        size_t i = 0, j = 0;
        while (i < oldSubs.size() && j < b.subs.size()) {
            if (SameBytes(oldSubs[i], b.subs[j])) {
                if (before[i].json != after[j].json) {
                    if (!force) return Fail("layout_dependency", SigText(oldSubs[i].sig) + " would decode differently; force to accept");
                    Warn(SigText(oldSubs[i].sig) + " now decodes differently");
                }
                ++i;
                ++j;
            } else {
                if (after[j].hasPattern && !after[j].decoded) Warn(SigText(b.subs[j].sig) + ": the new bytes do not fit the catalog's pattern");
                ++j;
            }
        }
        return true;
    }

    bool OpRemoveSub(const Value& op) {
        const std::string& formText = StrOr(op, "form", "");
        Sig sig;
        if (!SigFrom(op.Find("sig"), &sig)) return Fail("bad_op", "remove-sub needs sig, four characters");
        bool force = BoolOr(op, "force", false);
        Path path;
        uint32_t stored;
        if (!FindUnique(formText, op, &path, &stored)) return false;
        const Node& node = *doc.At(path);
        std::string rt = TypeOf(node);
        OwnedBody b;
        if (!LoadBody(node, &b)) return false;
        std::vector<size_t> occ;
        for (size_t i = 0; i < b.subs.size(); ++i)
            if (b.subs[i].sig == sig) occ.push_back(i);
        if (occ.empty()) return Fail("not_found", formText + " has no " + SigText(sig));
        size_t which = 0;
        if (const Value* n = op.Find("occurrence")) {
            int64_t i;
            if (!n->ToInt(&i) || i < 0 || static_cast<size_t>(i) >= occ.size()) return Fail("not_found", "no such occurrence of " + SigText(sig));
            which = static_cast<size_t>(i);
        } else if (occ.size() > 1) {
            return Fail("ambiguous_field", SigText(sig) + " occurs " + std::to_string(occ.size()) + " times: give occurrence");
        }
        for (const CountedList& c : kCounted) {
            if (SigIs(sig, c.count) && FindSub(b, c.list) != SIZE_MAX && !force) {
                return Fail("linked_subrecord", SigText(sig) + " holds the length of " + c.list + ": remove the list");
            }
        }
        std::vector<SubView> oldSubs = b.subs;
        size_t first = occ[which], last = first + 1;
        // A leader takes its followers along.
        const std::string sigT = SigText(sig);
        while (last < b.subs.size() && IsFollowerOf(b.subs[last].sig, sigT.c_str())) ++last;
        std::string removed;
        for (size_t i = first; i < last; ++i) removed += (removed.empty() ? "" : " ") + SigText(b.subs[i].sig);
        std::string before = JsonText(HexOf(b.subs[first].data, b.subs[first].size));
        b.subs.erase(b.subs.begin() + first, b.subs.begin() + last);
        if (!SyncCounts(rt, b, sig)) return false;
        PutRecord(path, WithBody(node, b));
        Changed(formText, "remove-sub", removed, before, "");
        return true;
    }

    // A FormID into / out of a pure FormID array, like KWDA.
    bool OpFormList(const Value& op, bool add) {
        const std::string& formText = StrOr(op, "form", "");
        Sig sig;
        if (!SigFrom(op.Find("sig"), &sig)) return Fail("bad_op", "needs sig, four characters");
        Path path;
        uint32_t stored;
        if (!FindUnique(formText, op, &path, &stored)) return false;
        uint32_t item;
        if (!ResolveForm(StrOr(op, "item", ""), &item)) return false;
        if (add && !CheckReference(item)) return false;
        const Node& node = *doc.At(path);
        std::string rt = TypeOf(node);
        OwnedBody b;
        if (!LoadBody(node, &b)) return false;
        size_t at = SIZE_MAX;
        for (size_t i = 0; i < b.subs.size(); ++i)
            if (b.subs[i].sig == sig) at = i;
        Bytes list;
        if (at != SIZE_MAX) {
            if (b.subs[at].size % 4) return Fail("not_a_list", SigText(sig) + " is not a list of FormIDs here");
            list.assign(b.subs[at].data, b.subs[at].data + b.subs[at].size);
        }
        size_t found = SIZE_MAX;
        for (size_t k = 0; k + 4 <= list.size(); k += 4)
            if (U32(list.data() + k) == item) found = k;
        if (add) {
            if (found != SIZE_MAX) {
                Changed(formText, "unchanged", SigText(sig), "", StrOr(op, "item", ""));
                return true;
            }
            list.resize(list.size() + 4);
            PutU32(list.data() + list.size() - 4, item);
            if (at != SIZE_MAX) {
                b.Replace(at, std::move(list));
            } else {
                size_t pos = 0;
                Sig countSig{};
                bool hasCount = false;
                for (const CountedList& c : kCounted) {
                    if (SigIs(sig, c.list)) {
                        countSig = MakeSig(c.count);
                        hasCount = true;
                    }
                }
                // The count's position, when it has one in the order map, comes first.
                if (!PlaceNew(rt, b, hasCount ? countSig : sig, op, &pos)) return false;
                b.Insert(pos, sig, std::move(list));
            }
        } else {
            if (found == SIZE_MAX) return Fail("not_found", StrOr(op, "item", "") + " is not in " + SigText(sig));
            list.erase(list.begin() + found, list.begin() + found + 4);
            if (list.empty()) b.subs.erase(b.subs.begin() + at);
            else b.Replace(at, std::move(list));
        }
        if (!SyncCounts(rt, b, sig)) return false;
        PutRecord(path, WithBody(node, b));
        Changed(formText, add ? "add-item" : "remove-item", SigText(sig), add ? "" : StrOr(op, "item", ""), add ? StrOr(op, "item", "") : "");
        return true;
    }

    // --- groups ------------------------------------------------------------

    static std::shared_ptr<Node> NewGroup(uint32_t label, uint32_t type) {
        auto g = std::make_shared<Node>();
        g->isGroup = true;
        std::memcpy(g->header.data(), "GRUP", 4);
        SetU32(g.get(), 8, label);
        SetU32(g.get(), 12, type);
        return g;
    }

    static uint32_t SigLabel(const Sig& s) { return U32(reinterpret_cast<const uint8_t*>(s.data())); }

    // A child group of `parent` by type and label, created at `insertAt` when
    // missing (nullopt: keep group order within the parent).
    Path ChildGroup(const Path& parent, uint32_t type, uint32_t label, std::optional<size_t> insertAt = std::nullopt) {
        const Node& p = *doc.At(parent);
        for (uint32_t i = 0; i < p.children.size(); ++i) {
            const Node& c = *p.children[i];
            if (c.isGroup && c.groupType() == type && c.groupLabel() == label) {
                Path out = parent;
                out.push_back(i);
                return out;
            }
        }
        size_t at = insertAt ? *insertAt : p.children.size();
        if (!insertAt && type == 8) {
            // Persistent children go before temporary ones.
            for (size_t i = 0; i < p.children.size(); ++i) {
                const Node& c = *p.children[i];
                if (c.isGroup && (c.groupType() == 9 || c.groupType() == 10)) {
                    at = i;
                    break;
                }
            }
        }
        doc.SetRoot(doc.Inserted(parent, at, NewGroup(label, type)));
        Path out = parent;
        out.push_back(static_cast<uint32_t>(at));
        return out;
    }

    Path TopGroup(const Sig& type) {
        const Node& root = doc.root();
        int rank = TopRank(type);
        size_t at = root.children.size();
        for (uint32_t i = 1; i < root.children.size(); ++i) {
            const Node& c = *root.children[i];
            if (!c.isGroup) continue;
            if (c.groupType() == 0 && c.groupLabel() == SigLabel(type)) return Path{i};
            Sig other;
            std::memcpy(other.data(), c.header.data() + 8, 4);
            if (c.groupType() == 0 && TopRank(other) > rank && at == root.children.size()) at = i;
        }
        return ChildGroup(Path{}, 0, SigLabel(type), at);
    }

    // The group right after a parent record (WRLD -> 1, CELL -> 6, DIAL -> 7).
    Path OwnedGroup(const Path& parentRecord, uint32_t type, uint32_t label) {
        Path up(parentRecord.begin(), parentRecord.end() - 1);
        return ChildGroup(up, type, label, parentRecord.back() + 1);
    }

    bool AppendRecord(const Path& group, NodePtr rec) {
        const Node& g = *doc.At(group);
        size_t at = g.children.size();
        doc.SetRoot(doc.Inserted(group, at, std::move(rec)));
        return true;
    }

    // Where a record of this type lives: for a new record.
    bool GroupForNew(const Sig& type, uint32_t formId, uint32_t flags, const Value& op, Path* group) {
        std::string t = SigText(type);
        std::string parentText = StrOr(op, "parent", "");
        if (t == "CELL") {
            if (parentText.empty()) {  // interior
                Path top = TopGroup(type);
                uint32_t obj = formId & 0xFFFFFF;
                Path block = ChildGroup(top, 2, obj % 10);
                *group = ChildGroup(block, 3, (obj % 100) / 10);
                return true;
            }
            uint32_t wrld;
            Path wp;
            if (!FindUnique(parentText, Value::Map(), &wp, &wrld)) return false;
            if (TypeOf(*doc.At(wp)) != "WRLD") return Fail("bad_parent", parentText + " is not a WRLD");
            Path kids = OwnedGroup(wp, 1, wrld);
            if (flags & 0x400 && BoolOr(op, "persistentCell", false)) {
                *group = kids;  // a world's persistent cell sits directly under it
                return true;
            }
            const Value* grid = op.Find("grid");
            if (!grid || grid->kind() != Value::Kind::List || grid->Items().size() != 2) {
                return Fail("bad_op", "an exterior CELL needs grid: [x, y]");
            }
            int64_t x = 0, y = 0;
            grid->Items()[0].ToInt(&x);
            grid->Items()[1].ToInt(&y);
            auto packed = [](int64_t a, int64_t b, int shift) {
                return static_cast<uint32_t>((static_cast<uint32_t>(a >> shift) << 16) | (static_cast<uint32_t>(b >> shift) & 0xFFFF));
            };
            Path block = ChildGroup(kids, 4, packed(x, y, 5));
            *group = ChildGroup(block, 5, packed(x, y, 3));
            return true;
        }
        if (IsCellChildType(t)) {
            uint32_t cell;
            Path cp;
            if (parentText.empty()) return Fail("bad_op", t + " needs parent, its CELL");
            if (!FindUnique(parentText, Value::Map(), &cp, &cell)) return false;
            if (TypeOf(*doc.At(cp)) != "CELL") return Fail("bad_parent", parentText + " is not a CELL");
            Path kids = OwnedGroup(cp, 6, cell);
            uint32_t kind = (IsPlacedType(t) && (flags & 0x400)) ? 8 : 9;
            *group = ChildGroup(kids, kind, cell);
            return true;
        }
        if (t == "INFO") {
            uint32_t dial;
            Path dp;
            if (parentText.empty()) return Fail("bad_op", "INFO needs parent, its DIAL");
            if (!FindUnique(parentText, Value::Map(), &dp, &dial)) return false;
            if (TypeOf(*doc.At(dp)) != "DIAL") return Fail("bad_parent", parentText + " is not a DIAL");
            *group = OwnedGroup(dp, 7, dial);
            return true;
        }
        *group = TopGroup(type);
        return true;
    }

    // --- add-record --------------------------------------------------------

    bool OpAddRecord(const Value& op) {
        Sig type;
        if (!SigFrom(op.Find("type"), &type)) return Fail("bad_op", "add-record needs type, four characters");
        std::string t = SigText(type);
        if (t == "TES4" || t == "GRUP") return Fail("bad_op", "not a record type you can add");
        uint32_t flags = 0;
        if (const Value* f = op.Find("flags")) {
            int64_t v;
            if (!f->ToInt(&v)) return Fail("bad_op", "flags must be a number");
            flags = static_cast<uint32_t>(v);
        }
        if (flags & (kFlagCompressed | kFlagDeleted)) return Fail("bad_op", "a new record is neither compressed nor deleted");
        // An ID: the one asked for, or the next free one at or above 0x800.
        const auto& index = doc.Index();
        std::set<uint32_t> used;
        uint32_t self = SelfIndex();
        for (const auto& en : index)
            if (en.first >> 24 >= self) used.insert(en.first & 0xFFFFFF);
        uint32_t id;
        uint32_t next = NextObjectId() & 0xFFFFFF;
        std::optional<uint32_t> newNext;
        if (const Value* want = op.Find("objectId")) {
            int64_t v = 0;
            if (want->IsString()) v = strtoll(want->AsString().c_str(), nullptr, 16);
            else want->ToInt(&v);
            if (v <= 0 || v > 0xFFFFFF) return Fail("bad_op", "objectId out of range");
            id = static_cast<uint32_t>(v);
            if (used.count(id)) return Fail("id_in_use", "object ID " + Hex6(id) + " is already used here");
            if (id < 0x800 && e.version_ < 1.705f) return Fail("low_id_needs_171", "object IDs below 0x800 need HEDR 1.71");
            if (id < 0x800 && e.masters_.empty()) return Fail("low_id_needs_master", "a plugin with no masters reads IDs below 0x800 as the engine's own");
            if (id >= next) newNext = id + 1;
        } else {
            id = std::max<uint32_t>(next, 0x800);
            while (used.count(id)) ++id;
            newNext = id + 1;
        }
        if (id > 0xFFFFFF) return Fail("id_exhausted", "no object IDs left");
        if (IsLightSelf() && id > 0xFFF) {
            return Fail("light_full", doc.name() + " is light: object ID " + Hex6(id) + " is past 0xFFF");
        }
        uint32_t formId = self << 24 | id;

        // Subrecords in order; add-sub's placing rules apply.
        OwnedBody b;
        auto addSub = [&](const Value& spec) {
            Sig sig;
            if (!SigFrom(spec.Find("sig"), &sig)) return Fail("bad_op", "each sub needs sig, four characters");
            Bytes bytes;
            if (!SubBytesFrom(t, sig, spec, &bytes)) return false;
            size_t at = b.subs.size();
            if (e.data_->order.Of(t) && !PlaceNew(t, b, sig, spec, &at)) return false;
            b.Insert(at, sig, std::move(bytes));
            return SyncCounts(t, b, sig);
        };
        if (const Value* edid = op.Find("edid")) {
            Value spec = Value::Map();
            spec.Set("sig", Value::Str("EDID"));
            spec.Set("value", *edid);
            if (!addSub(spec)) return false;
        }
        if (const Value* subs = op.Find("subs")) {
            if (subs->kind() != Value::Kind::List) return Fail("bad_op", "subs must be a list");
            for (const auto& spec : subs->Items())
                if (!addSub(spec)) return false;
        }

        auto rec = std::make_shared<Node>();
        std::memcpy(rec->header.data(), type.data(), 4);
        SetU32(rec.get(), 8, flags);
        SetU32(rec.get(), 12, formId);
        rec->header[20] = 44;  // form version, what the Creation Kit writes
        auto plain = std::make_shared<Bytes>();
        EmitBody(b.subs, plain.get());
        rec->plain = plain;

        Path group;
        if (!GroupForNew(type, formId, flags, op, &group)) return false;
        AppendRecord(group, rec);
        if (t == "CELL" && StrOr(op, "parent", "").empty()) BumpIncc(+1);
        if (!RefreshHedr(newNext ? std::optional<uint32_t>(*newNext) : std::nullopt)) return false;
        res.newForm = doc.name() + ":" + Hex6(formId);
        Changed(res.newForm, "add-record", t, "", "");
        MissingFieldsWarning(t, b);
        return true;
    }

    void BumpIncc(int delta) {
        EditTes4([&](OwnedBody& b) {
            size_t i = FindSub(b, "INCC");
            if (i == SIZE_MAX || b.subs[i].size != 4) return true;
            uint32_t v = U32(b.subs[i].data);
            Bytes raw(4);
            PutU32(raw.data(), v + static_cast<uint32_t>(delta));
            b.Replace(i, std::move(raw));
            return true;
        });
    }

    // What a Creation Kit save always writes for this type, and the record lacks.
    // Only a warning: values for them are the caller's to choose.
    void MissingFieldsWarning(const std::string& t, const OwnedBody& b) {
        const auto* order = e.data_->order.Of(t);
        if (!order) return;
        std::string missing;
        for (const auto& en : *order) {
            if (!en.always) continue;
            bool there = false;
            for (const SubView& s : b.subs) there = there || s.sig == en.sig;
            if (!there) missing += (missing.empty() ? "" : " ") + SigText(en.sig);
        }
        if (!missing.empty()) Warn(t + ": the Creation Kit always writes these and this record lacks them: " + missing);
    }

    // --- FormID remapping --------------------------------------------------

    using ByteMap = std::function<uint32_t(uint32_t)>;

    struct RemapStats {
        bool undecodable = false;
        int unscanned = 0;  // subrecords with no decode, so no FormIDs found in them
        std::vector<std::string> possible;
    };

    // A record's FormIDs rewritten through `map`. Returns null when nothing
    // changed: the node, and its compressed bytes, stay as they are.
    NodePtr RemapRecord(const Node& n, const std::string& rt, const ByteMap& map, bool localized,
                        const std::vector<uint32_t>* selfIds, uint32_t selfByte, RemapStats* stats) const {
        Bytes scratch;
        const uint8_t* body;
        size_t size;
        uint32_t oldId = n.formId();
        uint32_t newId = map(oldId);
        bool headerChanged = newId != oldId;
        if (!Document::PlainBody(n, &scratch, &body, &size)) {
            stats->undecodable = true;
            stats->possible.push_back(rt + " " + Hex6(oldId) + ": compressed body will not inflate");
            if (!headerChanged) return nullptr;
            auto c = std::make_shared<Node>(n);
            c->span = nullptr;
            c->spanSize = 0;
            SetU32(c.get(), 12, newId);
            return c;
        }
        ParsedBody parsed = ParseBody(body, size);
        if (parsed.issue != BodyIssue::None) {
            stats->undecodable = true;
            stats->possible.push_back(rt + " " + Hex6(oldId) + ": body cannot be split");
            if (!headerChanged) return nullptr;
            auto c = std::make_shared<Node>(n);
            c->span = nullptr;
            c->spanSize = 0;
            SetU32(c.get(), 12, newId);
            return c;
        }
        auto traces = TraceSubs(rt, n.formVersion(), localized, parsed.subs, cat);
        std::deque<Bytes> changed;
        bool any = false;
        for (size_t i = 0; i < parsed.subs.size(); ++i) {
            SubView& s = parsed.subs[i];
            const SubTrace& st = traces[i];
            if (!st.decoded && s.size >= 4) ++stats->unscanned;
            std::set<size_t> formOffsets;
            Bytes copy;
            for (const TraceLeaf& l : st.leaves) {
                if (!l.pattern || !LeafIsFormId(*l.pattern) || l.size != 4) continue;
                formOffsets.insert(l.offset);
                uint32_t v = U32(s.data + l.offset);
                uint32_t m = map(v);
                if (m == v) continue;
                if (copy.empty()) copy.assign(s.data, s.data + s.size);
                PutU32(copy.data() + l.offset, m);
            }
            if (!copy.empty()) {
                changed.push_back(std::move(copy));
                s.data = changed.back().data();
                any = true;
            }
            // The audit: a 4-byte value the catalog doesn't call a FormID, whose
            // high byte is the moving one and whose low bits name a form here.
            if (selfIds) {
                auto looksLikeSelf = [&](uint32_t v) {
                    return v >> 24 == selfByte && std::binary_search(selfIds->begin(), selfIds->end(), v & 0xFFFFFF);
                };
                auto note = [&](size_t off, uint32_t v, const char* kind) {
                    if (stats->possible.size() < 200) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%08X", v);
                        stats->possible.push_back(rt + " " + Hex6(oldId) + " " + SigText(s.sig) + " +" + std::to_string(off) + " = " + buf + " (" + kind + ")");
                    }
                };
                if (!st.decoded) {
                    if (s.size >= 4) {
                        for (size_t off = 0; off + 4 <= s.size; off += 4) {
                            uint32_t v = U32(s.data + off);
                            if (looksLikeSelf(v)) note(off, v, st.hasPattern ? "undecodable" : "no pattern");
                        }
                    }
                } else {
                    for (const TraceLeaf& l : st.leaves) {
                        if (!l.pattern || formOffsets.count(l.offset)) continue;
                        if (l.size == 4 && rt != "TES4" && LeafTypeName(*l.pattern) != "lstring") {  // a string ID is no FormID
                            uint32_t v = U32(s.data + l.offset);
                            if (looksLikeSelf(v)) note(l.offset, v, "not marked as a FormID");
                        } else if (l.size > 4 && LeafIsByteRun(*l.pattern) && !GeometryRun(rt, s.sig)) {
                            for (size_t off = 0; off + 4 <= l.size; off += 4) {
                                uint32_t v = U32(s.data + l.offset + off);
                                if (looksLikeSelf(v)) note(l.offset + off, v, "raw bytes");
                            }
                        }
                    }
                }
            }
        }
        if (!any && !headerChanged) return nullptr;
        auto c = std::make_shared<Node>(n);
        c->span = nullptr;
        c->spanSize = 0;
        SetU32(c.get(), 12, newId);
        if (any) {
            auto plain = std::make_shared<Bytes>();
            EmitBody(parsed.subs, plain.get());
            c->plain = plain;
            c->stored = nullptr;
            c->storedSize = 0;
            c->owned.reset();
        }
        return c;
    }

    // Raw runs known to hold heights, normals, vertices or offsets, never a FormID.
    // (A navmesh's FormIDs are marked as such: only its vertex and triangle
    // arrays are raw.)
    static bool GeometryRun(const std::string& rt, const Sig& sig) {
        return rt == "LAND" || rt == "NAVM" || SigIs(sig, "MHDT") || SigIs(sig, "TVDT") || SigIs(sig, "OFST");
    }

    static bool GroupLabelIsForm(uint32_t type) { return type == 1 || type == 6 || type == 7 || type == 8 || type == 9 || type == 10; }

    // The whole tree through `map`, on all cores. Untouched nodes stay shared.
    NodePtr RemapTree(const ByteMap& map, const std::vector<uint32_t>& selfIds, uint32_t selfByte, RemapStats* stats) {
        std::vector<const Node*> records;
        std::function<void(const Node&)> walk = [&](const Node& n) {
            for (const auto& c : n.children) {
                if (c->isGroup) walk(*c);
                else records.push_back(c.get());
            }
        };
        walk(doc.root());
        std::vector<NodePtr> results(records.size());
        std::vector<RemapStats> perThread(std::max(1u, std::thread::hardware_concurrency()));
        std::atomic<size_t> next{0};
        bool localized = e.localized_;
        auto work = [&](size_t slot) {
            for (size_t i; (i = next++) < records.size();) {
                results[i] = RemapRecord(*records[i], SigText(records[i]->sig()), map, localized, &selfIds, selfByte, &perThread[slot]);
            }
        };
        std::vector<std::thread> pool;
        for (size_t t = 1; t < perThread.size(); ++t) pool.emplace_back(work, t);
        work(0);
        for (auto& t : pool) t.join();
        for (const auto& s : perThread) {
            stats->undecodable = stats->undecodable || s.undecodable;
            stats->possible.insert(stats->possible.end(), s.possible.begin(), s.possible.end());
        }
        std::unordered_map<const Node*, NodePtr> replaced;
        for (size_t i = 0; i < records.size(); ++i)
            if (results[i]) replaced[records[i]] = results[i];
        std::function<NodePtr(const NodePtr&)> rebuild = [&](const NodePtr& n) -> NodePtr {
            if (!n->isGroup) {
                auto it = replaced.find(n.get());
                return it == replaced.end() ? n : it->second;
            }
            bool changed = false;
            std::vector<NodePtr> kids;
            kids.reserve(n->children.size());
            for (const auto& c : n->children) {
                kids.push_back(rebuild(c));
                changed = changed || kids.back() != c;
            }
            uint32_t label = n->groupLabel();
            uint32_t newLabel = GroupLabelIsForm(n->groupType()) ? map(label) : label;
            if (!changed && newLabel == label) return n;
            auto g = std::make_shared<Node>(*n);
            g->span = nullptr;
            g->spanSize = 0;
            g->children = std::move(kids);
            SetU32(g.get(), 8, newLabel);
            return g;
        };
        auto root = std::make_shared<Node>(doc.root());
        root->span = nullptr;
        for (auto& c : root->children) c = rebuild(c);
        return root;
    }

    std::vector<uint32_t> SelfObjectIds() {
        std::vector<uint32_t> ids;
        uint32_t self = SelfIndex();
        for (const auto& en : doc.Index())
            if (en.first >> 24 >= self) ids.push_back(en.first & 0xFFFFFF);
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    bool AddMaster(const std::string& name, bool force) {
        if (name.empty()) return Fail("bad_op", "add-master needs name");
        for (const auto& m : e.masters_) {
            if (Lower(m) == Lower(name)) {
                Changed(doc.name(), "unchanged", "master", "", name);
                return true;
            }
        }
        if (Lower(name) == Lower(doc.name())) return Fail("bad_op", "a plugin cannot be its own master");
        uint32_t oldSelf = SelfIndex();
        if (oldSelf >= 0xFD) return Fail("too_many_masters", "no room for another master");
        uint32_t newSelf = oldSelf + 1;
        // Everything this plugin says about itself moves up one. A value with a
        // higher byte still means self. In a plugin with no masters, IDs below
        // 0x800 under byte 00 are the engine's own forms and stay.
        ByteMap map = [=](uint32_t v) {
            if (v >> 24 != oldSelf) return v;
            uint32_t id = v & 0xFFFFFF;
            if (oldSelf == 0 && id < 0x800) return v;
            return newSelf << 24 | id;
        };
        std::vector<uint32_t> selfIds = SelfObjectIds();
        RemapStats stats;
        NodePtr root = RemapTree(map, selfIds, oldSelf, &stats);
        if (!stats.possible.empty()) {
            res.possibleMisses = stats.possible;
            if (!force) {
                return Fail("possible_formids", std::to_string(stats.possible.size()) +
                                                    " values may be FormIDs the catalog does not mark: they would keep pointing at the old index. See possibleMisses; force to accept.");
            }
            Warn(std::to_string(stats.possible.size()) + " possible FormIDs left as they were");
        }
        doc.SetRoot(root);
        e.LoadHeaderFacts();
        if (!EditTes4([&](OwnedBody& b) {
                size_t at = 0;
                for (size_t k = 0; k < b.subs.size(); ++k) {
                    if (SigIs(b.subs[k].sig, "HEDR") || SigIs(b.subs[k].sig, "CNAM") || SigIs(b.subs[k].sig, "SNAM") ||
                        SigIs(b.subs[k].sig, "MAST") || SigIs(b.subs[k].sig, "DATA")) {
                        at = k + 1;
                    }
                }
                Bytes nameBytes(name.begin(), name.end());
                nameBytes.push_back(0);
                b.Insert(at, MakeSig("MAST"), std::move(nameBytes));
                b.Insert(at + 1, MakeSig("DATA"), Bytes(8, 0));  // what shipped files hold
                return true;
            })) {
            return false;
        }
        Changed(doc.name(), "add-master", name, "", std::to_string(oldSelf));
        return true;
    }

    // --- copy-as-override --------------------------------------------------

    Document* Source(const std::string& utf8Path) {
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, nullptr, 0);
        std::wstring w(static_cast<size_t>(std::max(n - 1, 0)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, w.data(), n);
        auto it = e.sources_.find(w);
        if (it != e.sources_.end()) return it->second.get();
        std::string err;
        auto d = Document::Open(w, &err);
        if (!d) {
            Fail("bad_source", utf8Path + ": " + err);
            return nullptr;
        }
        Document* out = d.get();
        e.sources_[w] = std::move(d);
        return out;
    }

    static std::vector<std::string> MastersOf(const Document& d) {
        std::vector<std::string> out;
        Bytes scratch;
        const uint8_t* body;
        size_t size;
        if (!Document::PlainBody(*d.tes4(), &scratch, &body, &size)) return out;
        ParsedBody p = ParseBody(body, size);
        for (const SubView& s : p.subs) {
            if (SigIs(s.sig, "MAST")) out.emplace_back(reinterpret_cast<const char*>(s.data), strnlen(reinterpret_cast<const char*>(s.data), s.size));
        }
        return out;
    }

    // Copies a source record here, group chain and parents included. `added`
    // reports what was copied.
    bool CopyOne(const Document& src, const Path& srcPath, const std::vector<std::string>& srcMasters, bool localizedSrc,
                 const std::string& asked, bool replace, bool force, bool parentOnly) {
        std::vector<const Node*> chain;
        const Node* rec = src.At(srcPath, &chain);
        std::string rt = TypeOf(*rec);
        auto mapped = [&](uint32_t v, uint32_t* out) {
            uint32_t h = v >> 24;
            std::string owner = h < srcMasters.size() ? srcMasters[h] : src.name();
            if (v == 0 || (srcMasters.empty() && (v & 0xFFFFFF) < 0x800)) {
                *out = v;
                return true;
            }
            std::string low = Lower(owner);
            for (size_t i = 0; i < e.masters_.size(); ++i) {
                if (Lower(e.masters_[i]) == low) {
                    *out = static_cast<uint32_t>(i) << 24 | (v & 0xFFFFFF);
                    return true;
                }
            }
            if (low == Lower(doc.name())) {
                *out = SelfIndex() << 24 | (v & 0xFFFFFF);
                return true;
            }
            return false;
        };
        // Every master this record's references need.
        std::set<std::string> needed;
        auto needOwner = [&](uint32_t v) {
            uint32_t ignored;
            if (mapped(v, &ignored)) return;
            uint32_t h = v >> 24;
            needed.insert(h < srcMasters.size() ? srcMasters[h] : src.name());
        };
        needOwner(rec->formId());
        {
            Bytes scratch;
            const uint8_t* body;
            size_t size;
            if (!Document::PlainBody(*rec, &scratch, &body, &size)) return Fail("opaque_record", asked + ": will not inflate");
            ParsedBody parsed = ParseBody(body, size);
            auto traces = TraceSubs(rt, rec->formVersion(), localizedSrc, parsed.subs, cat);
            for (size_t i = 0; i < parsed.subs.size(); ++i) {
                for (const TraceLeaf& l : traces[i].leaves) {
                    if (l.pattern && LeafIsFormId(*l.pattern) && l.size == 4) needOwner(U32(parsed.subs[i].data + l.offset));
                }
            }
        }
        for (const std::string& m : needed) {
            if (!AddMaster(m, force)) return false;
        }

        uint32_t targetId;
        if (!mapped(rec->formId(), &targetId)) return Fail("master_missing", "cannot map " + asked);
        if (const auto* there = doc.Find(targetId)) {
            if (parentOnly) return true;  // the parent is already here
            if (!replace) return Fail("already_here", asked + " is already in " + doc.name() + "; give replace to overwrite it");
            if (there->size() != 1) return Fail("duplicate_form", asked + " occurs several times here");
            NodePtr copy = BuildCopy(*rec, rt, srcMasters, mapped, localizedSrc);
            if (!copy) return false;
            PutRecord((*there)[0], copy);
            Changed(asked, "replace-record", rt);
            return true;
        }

        // The group chain, parents first.
        Path group{};
        for (size_t depth = 1; depth + 1 < chain.size(); ++depth) {
            const Node& g = *chain[depth];
            uint32_t type = g.groupType();
            uint32_t label = g.groupLabel();
            if (GroupLabelIsForm(type) && !mapped(label, &label)) {
                return Fail("master_missing", "cannot map a group label");
            }
            if (depth == 1 && type == 0) {
                Sig s;
                std::memcpy(s.data(), g.header.data() + 8, 4);
                group = TopGroup(s);
                continue;
            }
            if (type == 1 || type == 6 || type == 7) {
                // Its owner record sits just before it in the source.
                const Node& parent = *chain[depth - 1];
                size_t idx = srcPath[depth - 1];
                if (idx == 0 || parent.children[idx - 1]->isGroup || parent.children[idx - 1]->formId() != g.groupLabel()) {
                    return Fail("bad_source", "group " + Hex6(g.groupLabel()) + " has no parent record before it");
                }
                Path ownerPath(srcPath.begin(), srcPath.begin() + static_cast<std::ptrdiff_t>(depth - 1));
                ownerPath.push_back(static_cast<uint32_t>(idx - 1));
                if (!CopyOne(src, ownerPath, srcMasters, localizedSrc, FormKeyIn(src, srcMasters, g.groupLabel()), false, force, true)) {
                    return false;
                }
                // Masters may have been added: labels and paths move.
                if (!mapped(g.groupLabel(), &label)) return Fail("master_missing", "cannot map a group label");
                const std::vector<Path>* owners = doc.Find(label);
                if (!owners || owners->empty()) return Fail("bad_source", "parent record did not copy");
                group = OwnedGroup((*owners)[0], type, label);
                continue;
            }
            group = ChildGroup(group, type, label);
        }
        NodePtr copy = BuildCopy(*rec, rt, srcMasters, mapped, localizedSrc);
        if (!copy) return false;
        AppendRecord(group, copy);
        if (rt == "CELL" && chain.size() >= 2 && chain[chain.size() - 2]->isGroup && chain[chain.size() - 2]->groupType() == 3) BumpIncc(+1);
        if (!RefreshHedr()) return false;
        if (!SyncOnam(rt, rec->flags(), targetId, true)) return false;
        Changed(asked, "copy-as-override", rt);
        return true;
    }

    std::string FormKeyIn(const Document& d, const std::vector<std::string>& masters, uint32_t v) const {
        uint32_t h = v >> 24;
        return (h < masters.size() ? masters[h] : d.name()) + ":" + Hex6(v);
    }

    template <typename Mapped>
    NodePtr BuildCopy(const Node& rec, const std::string& rt, const std::vector<std::string>&, Mapped& mapped,
                      bool localizedSrc) {
        ByteMap map = [&](uint32_t v) {
            uint32_t out = v;
            mapped(v, &out);
            return out;
        };
        RemapStats stats;
        NodePtr changed = RemapRecord(rec, rt, map, localizedSrc, nullptr, 0, &stats);
        if (stats.unscanned && IdentityBroken(map)) {
            Warn(rt + ": " + std::to_string(stats.unscanned) + " subrecords have no decode, so FormIDs in them were not remapped");
        }
        // Either way it no longer views the source file: the copy owns its bytes
        // (the source's stored form, so a compressed record stays identical).
        auto c = std::make_shared<Node>(changed ? *changed : rec);
        c->span = nullptr;
        c->spanSize = 0;
        if (!c->plain) {
            auto own = std::make_shared<Bytes>(rec.stored, rec.stored + rec.storedSize);
            c->owned = own;
            c->stored = own->data();
            c->storedSize = own->size();
        }
        if (inlineFrom && !InlineStrings(c.get(), rt)) return nullptr;
        return c;
    }

    // A localized source into a plugin that isn't: each text field's string ID
    // becomes its text, read from the source's strings. Without this the field
    // would read as garbage, which is the damage this avoids.
    const StringTables* inlineFrom = nullptr;
    bool forceMissingText = false;

    bool InlineStrings(Node* c, const std::string& rt) {
        Bytes scratch;
        const uint8_t* body;
        size_t size;
        if (!Document::PlainBody(*c, &scratch, &body, &size)) return Fail("opaque_record", rt + ": will not inflate");
        ParsedBody parsed = ParseBody(body, size);
        if (parsed.issue != BodyIssue::None) return Fail("opaque_record", rt + ": cannot split the body to convert its text");
        auto traces = TraceSubs(rt, c->formVersion(), true, parsed.subs, cat);
        std::deque<Bytes> converted;
        bool any = false;
        for (size_t i = 0; i < parsed.subs.size(); ++i) {
            SubView& s = parsed.subs[i];
            std::vector<const TraceLeaf*> texts;
            for (const TraceLeaf& l : traces[i].leaves) {
                if (l.pattern && l.size == 4 && LeafTypeName(*l.pattern) == "lstring") texts.push_back(&l);
            }
            if (texts.empty()) continue;
            Bytes out;
            size_t at = 0;
            for (const TraceLeaf* l : texts) {
                out.insert(out.end(), s.data + at, s.data + l->offset);
                uint32_t id = U32(s.data + l->offset);
                std::string text;
                if (id) {
                    const std::string* found = inlineFrom->Find(id, cat.LstringClass(rt, SigText(s.sig)));
                    if (found) {
                        text = *found;
                    } else if (!forceMissingText) {
                        return Fail("string_missing", rt + "." + SigText(s.sig) + ": no text for string ID " + std::to_string(id) + " in the source's strings; force to write it empty");
                    } else {
                        Warn(rt + "." + SigText(s.sig) + ": no text for string ID " + std::to_string(id) + ": written empty");
                    }
                }
                Bytes enc;
                std::string err;
                if (!EncodeLeaf(*l->pattern, Value::Str(text), nullptr, 0, false, &enc, &err)) return Fail("bad_value", err);
                out.insert(out.end(), enc.begin(), enc.end());
                at = l->offset + l->size;
            }
            out.insert(out.end(), s.data + at, s.data + s.size);
            converted.push_back(std::move(out));
            s.data = converted.back().data();
            s.size = converted.back().size();
            s.viaXxxx = s.size > 0xFFFF;
            s.sizeField = s.viaXxxx ? 0 : static_cast<uint16_t>(s.size);
            any = true;
        }
        if (!any) return true;
        auto plain = std::make_shared<Bytes>();
        EmitBody(parsed.subs, plain.get());
        c->plain = plain;
        c->stored = nullptr;
        c->storedSize = 0;
        c->owned.reset();
        return true;
    }

    // Whether the two plugins' master lists disagree anywhere.
    bool IdentityBroken(const ByteMap& map) const {
        for (uint32_t h = 0; h <= SelfIndex(); ++h)
            if (map(h << 24 | 0x800) != (h << 24 | 0x800)) return true;
        return false;
    }

    bool OpCopy(const Value& op) {
        const std::string& fromPath = StrOr(op, "from", "");
        const std::string& formText = StrOr(op, "form", "");
        if (fromPath.empty()) return Fail("bad_op", "copy-as-override needs from, a plugin path");
        Document* src = Source(fromPath);
        if (!src) return false;
        std::vector<std::string> srcMasters = MastersOf(*src);
        bool localizedSrc = (src->tes4()->flags() & 0x80) != 0;
        if (localizedSrc && !e.localized_) {
            // Text comes along as text.
            std::string language = StrOr(op, "language", "english");
            std::string base = src->name().substr(0, src->name().find_last_of('.'));
            auto& tables = e.sourceStrings_[Lower(src->name()) + "|" + language];
            if (!tables) {
                tables = std::make_unique<StringTables>();
                const std::wstring& srcPath = src->path();
                size_t slash = srcPath.find_last_of(L"\\/");
                tables->Load(slash == std::wstring::npos ? std::wstring(L".\\") : srcPath.substr(0, slash + 1), base, language);
            }
            forceMissingText = BoolOr(op, "force", false);
            if (tables->empty() && !forceMissingText) {
                return Fail("strings_missing", src->name() + " is localized and " + doc.name() + " is not, and no " + language + " strings were found beside the source; force to write its text empty");
            }
            inlineFrom = tables.get();
        } else if (!localizedSrc && e.localized_) {
            if (!BoolOr(op, "force", false)) {
                return Fail("localized_mismatch", doc.name() + " is localized and the source is not: its text cannot become string IDs here; force to keep inline text under the flag");
            }
            Warn("inline text under the localized flag: the record's text fields will not read correctly");
        }
        // The form is named as the source names it.
        uint32_t stored = 0;
        size_t colon = formText.rfind(':');
        if (colon == std::string::npos) {
            stored = static_cast<uint32_t>(strtoul(formText.c_str(), nullptr, 16));
        } else {
            std::string owner = Lower(formText.substr(0, colon));
            uint32_t id = static_cast<uint32_t>(strtoul(formText.c_str() + colon + 1, nullptr, 16));
            bool found = false;
            for (size_t i = 0; i < srcMasters.size(); ++i) {
                if (Lower(srcMasters[i]) == owner) {
                    stored = static_cast<uint32_t>(i) << 24 | id;
                    found = true;
                }
            }
            if (!found && owner == Lower(src->name())) {
                stored = static_cast<uint32_t>(srcMasters.size()) << 24 | id;
                found = true;
            }
            if (!found) return Fail("not_found", formText + ": not in " + src->name() + "'s masters");
        }
        const auto* paths = src->Find(stored);
        if (!paths || paths->empty()) return Fail("not_found", formText + " is not in " + src->name());
        return CopyOne(*src, (*paths)[0], srcMasters, localizedSrc, formText, BoolOr(op, "replace", false),
                       BoolOr(op, "force", false), false);
    }

    // --- delete ------------------------------------------------------------

    void PruneEmpty(Path group) {
        while (!group.empty()) {
            const Node* g = doc.At(group);
            if (!g || !g->isGroup || !g->children.empty()) return;
            doc.SetRoot(doc.Replaced(group, nullptr));
            group.pop_back();
        }
    }

    bool OpDelete(const Value& op) {
        const std::string& formText = StrOr(op, "form", "");
        Path path;
        uint32_t stored;
        if (!FindUnique(formText, op, &path, &stored)) return false;
        std::vector<const Node*> chain;
        const Node& node = *doc.At(path, &chain);
        NodePtr keepAlive = doc.rootPtr();  // chain views the current tree
        std::string rt = TypeOf(node);
        bool own = stored >> 24 >= SelfIndex();
        const Node& parent = *chain[chain.size() - 2];
        size_t idx = path.back();
        bool hasKids = idx + 1 < parent.children.size() && parent.children[idx + 1]->isGroup &&
                       parent.children[idx + 1]->groupLabel() == stored &&
                       (parent.children[idx + 1]->groupType() == 1 || parent.children[idx + 1]->groupType() == 6 ||
                        parent.children[idx + 1]->groupType() == 7);
        bool interior = rt == "CELL" && chain.size() >= 2 && chain[chain.size() - 2]->groupType() == 3;
        if (own) {
            bool withChildren = BoolOr(op, "withChildren", false);
            if (hasKids && !withChildren) {
                return Fail("has_children", formText + " has a child group; give withChildren to remove it too");
            }
            Path up(path.begin(), path.end() - 1);
            if (hasKids) {
                Path kids = up;
                kids.push_back(static_cast<uint32_t>(idx + 1));
                doc.SetRoot(doc.Replaced(kids, nullptr));
            }
            doc.SetRoot(doc.Replaced(path, nullptr));
            PruneEmpty(up);
            if (interior) BumpIncc(-1);
            if (!RefreshHedr()) return false;
            Changed(formText, "remove-record", rt);
            Warn("references to " + formText + " elsewhere are not touched");
            return true;
        }
        // An override: it stays as a deleted record. What the Creation Kit
        // writes: placed references keep EDID, VMAD, NAME; CELL, WRLD and DIAL
        // keep their bodies; anything else is a header (doc 30).
        OwnedBody b;
        if (!LoadBody(node, &b)) return false;
        auto c = std::make_shared<Node>(node);
        c->span = nullptr;
        c->spanSize = 0;
        uint32_t flags = (node.flags() | kFlagDeleted);
        if (rt == "CELL" || rt == "WRLD" || rt == "DIAL") {
            auto plain = std::make_shared<Bytes>();
            EmitBody(b.subs, plain.get());
            c->plain = plain;
        } else {
            std::vector<SubView> keep;
            if (IsPlacedType(rt)) {
                for (const SubView& s : b.subs) {
                    if (SigIs(s.sig, "EDID") || SigIs(s.sig, "VMAD") || SigIs(s.sig, "NAME")) keep.push_back(s);
                }
            }
            auto plain = std::make_shared<Bytes>();
            EmitBody(keep, plain.get());
            c->plain = plain;
            flags &= ~kFlagCompressed;
        }
        c->stored = nullptr;
        c->storedSize = 0;
        c->owned.reset();
        SetU32(c.get(), 8, flags);
        PutRecord(path, c);
        if (!SyncOnam(rt, node.flags(), stored, false)) return false;
        Changed(formText, "delete-override", rt);
        if (IsPlacedType(rt)) Warn(formText + ": deleting a placed reference breaks anything that reaches it at runtime; marking it Initially Disabled is the safe form");
        return true;
    }

    // --- record flags, header ----------------------------------------------

    bool OpSetFlags(const Value& op) {
        const std::string& formText = StrOr(op, "form", "");
        Path path;
        uint32_t stored;
        if (!FindUnique(formText, op, &path, &stored)) return false;
        const Node& node = *doc.At(path);
        int64_t set = 0, clear = 0;
        if (const Value* v = op.Find("set")) {
            if (!(v->IsString() ? (set = strtoll(v->AsString().c_str(), nullptr, 0), true) : v->ToInt(&set))) return Fail("bad_op", "set must be a number");
        }
        if (const Value* v = op.Find("clear")) {
            if (!(v->IsString() ? (clear = strtoll(v->AsString().c_str(), nullptr, 0), true) : v->ToInt(&clear))) return Fail("bad_op", "clear must be a number");
        }
        uint32_t bits = static_cast<uint32_t>(set | clear);
        if (bits & kFlagDeleted) return Fail("bad_op", "use delete-record for the deleted flag");
        uint32_t before = node.flags();
        uint32_t after = (before | static_cast<uint32_t>(set)) & ~static_cast<uint32_t>(clear);
        if (before == after) {
            Changed(formText, "unchanged", "flags");
            return true;
        }
        auto c = std::make_shared<Node>(node);
        c->span = nullptr;
        c->spanSize = 0;
        if ((before ^ after) & kFlagCompressed) {
            // Switching compression needs the plain body.
            Bytes scratch;
            const uint8_t* body;
            size_t size;
            if (!Document::PlainBody(node, &scratch, &body, &size)) return Fail("opaque_record", formText + ": will not inflate");
            c->plain = std::make_shared<Bytes>(body, body + size);
            c->stored = nullptr;
            c->storedSize = 0;
            c->owned.reset();
        }
        SetU32(c.get(), 8, after);
        PutRecord(path, c);
        char a[16], b[16];
        snprintf(a, sizeof(a), "%08X", before);
        snprintf(b, sizeof(b), "%08X", after);
        Changed(formText, "set-flags", "flags", a, b);
        return true;
    }

    bool OpSetHeader(const Value& op) {
        std::string self = doc.name();
        auto setText = [&](const char* key, const char* sig) {
            const Value* v = op.Find(key);
            if (!v) return true;
            if (!v->IsString()) return Fail("bad_op", std::string(key) + " must be text");
            std::string t = v->AsString();
            return EditTes4([&](OwnedBody& b) {
                size_t i = FindSub(b, sig);
                Bytes raw(t.begin(), t.end());
                raw.push_back(0);
                if (t.empty()) {
                    if (i != SIZE_MAX) b.subs.erase(b.subs.begin() + i);
                } else if (i != SIZE_MAX) {
                    b.Replace(i, std::move(raw));
                } else {
                    size_t at = FindSub(b, "HEDR") + 1;
                    if (std::strcmp(sig, "SNAM") == 0 && FindSub(b, "CNAM") != SIZE_MAX) at = FindSub(b, "CNAM") + 1;
                    b.Insert(at, MakeSig(sig), std::move(raw));
                }
                Changed(self, "set-header", sig, "", JsonText(t));
                return true;
            });
        };
        if (!setText("author", "CNAM") || !setText("description", "SNAM")) return false;
        if (const Value* v = op.Find("version")) {
            double d;
            if (v->kind() == Value::Kind::Float) d = v->AsFloat();
            else if (v->kind() == Value::Kind::Int) d = static_cast<double>(v->AsInt());
            else return Fail("bad_op", "version must be a number");
            float f = static_cast<float>(d);
            if (!EditTes4([&](OwnedBody& b) {
                    size_t i = FindSub(b, "HEDR");
                    if (i == SIZE_MAX || b.subs[i].size < 12) return Fail("bad_header", "no usable HEDR");
                    Bytes h(b.subs[i].data, b.subs[i].data + b.subs[i].size);
                    std::memcpy(h.data(), &f, 4);
                    b.Replace(i, std::move(h));
                    return true;
                })) {
                return false;
            }
            Changed(self, "set-header", "version", "", std::to_string(d));
        }
        auto setFlag = [&](const char* key, uint32_t bit) {
            const Value* v = op.Find(key);
            if (!v) return true;
            if (v->kind() != Value::Kind::Bool) return Fail("bad_op", std::string(key) + " must be true or false");
            auto c = std::make_shared<Node>(*doc.tes4());
            c->span = nullptr;
            c->spanSize = 0;
            uint32_t f = v->AsBool() ? c->flags() | bit : c->flags() & ~bit;
            SetU32(c.get(), 8, f);
            PutRecord(Path{0}, c);
            Changed(self, "set-header", key, "", v->AsBool() ? "true" : "false");
            e.LoadHeaderFacts();
            return true;
        };
        if (const Value* v = op.Find("esl"); v && v->kind() == Value::Kind::Bool && v->AsBool()) {
            std::vector<uint32_t> ids = SelfObjectIds();
            if (!ids.empty() && ids.back() > 0xFFF) {
                return Fail("light_full", "object ID " + Hex6(ids.back()) + " is past 0xFFF: compact the IDs before flagging it light");
            }
        }
        if (op.Find("localized")) return Fail("bad_op", "changing the localized flag changes how every text field reads; not supported");
        if (!setFlag("esm", 0x1) || !setFlag("esl", 0x200)) return false;
        return true;
    }

    bool Dispatch(const Value& op) {
        if (op.kind() != Value::Kind::Map) return Fail("bad_op", "an op is a JSON object");
        const std::string& name = StrOr(op, "op", "");
        if (name == "set") return OpSet(op);
        if (name == "add-sub") return OpAddSub(op);
        if (name == "remove-sub") return OpRemoveSub(op);
        if (name == "add-item") return OpFormList(op, true);
        if (name == "remove-item") return OpFormList(op, false);
        if (name == "add-record") return OpAddRecord(op);
        if (name == "copy-as-override") return OpCopy(op);
        if (name == "delete-record") return OpDelete(op);
        if (name == "add-master") return AddMaster(StrOr(op, "name", ""), BoolOr(op, "force", false));
        if (name == "set-flags") return OpSetFlags(op);
        if (name == "set-header") return OpSetHeader(op);
        return Fail("unknown_op", "no op named " + name);
    }
};

Editor::Editor(Document* doc, const EditData* data) : doc_(doc), data_(data), original_(doc->rootPtr()) { LoadHeaderFacts(); }

void Editor::LoadHeaderFacts() {
    masters_ = Impl::MastersOf(*doc_);
    localized_ = (doc_->tes4()->flags() & 0x80) != 0;
    version_ = 0;
    Bytes scratch;
    const uint8_t* body;
    size_t size;
    if (Document::PlainBody(*doc_->tes4(), &scratch, &body, &size)) {
        ParsedBody p = ParseBody(body, size);
        for (const SubView& s : p.subs) {
            if (SigIs(s.sig, "HEDR") && s.size >= 4) {
                std::memcpy(&version_, s.data, 4);
                break;
            }
        }
    }
}

OpResult Editor::Apply(const Value& op) {
    NodePtr before = doc_->rootPtr();
    Impl impl(*this);
    bool ok = false;
    try {
        ok = impl.Dispatch(op);
    } catch (const std::exception& ex) {
        impl.Fail("internal", ex.what());
    }
    impl.res.ok = ok;
    if (!ok) {
        doc_->SetRoot(before);  // nothing happened
        LoadHeaderFacts();
        impl.res.changes.clear();
        return impl.res;
    }
    if (doc_->rootPtr() != before) {
        undo_.push_back(before);
        redo_.clear();
    }
    return impl.res;
}

bool Editor::Undo() {
    if (undo_.empty()) return false;
    redo_.push_back(doc_->rootPtr());
    doc_->SetRoot(undo_.back());
    undo_.pop_back();
    LoadHeaderFacts();
    return true;
}

bool Editor::Redo() {
    if (redo_.empty()) return false;
    undo_.push_back(doc_->rootPtr());
    doc_->SetRoot(redo_.back());
    redo_.pop_back();
    LoadHeaderFacts();
    return true;
}

OpResult Editor::Describe(const std::string& form, std::string* json) {
    Impl impl(*this);
    Path path;
    uint32_t stored = 0;
    bool isHeader = form == "TES4";
    Value none = Value::Map();
    if (isHeader) path = Path{0};
    else if (!impl.FindUnique(form, none, &path, &stored)) return impl.res;
    const Node& node = *doc_->At(path);
    OwnedBody b;
    if (!impl.LoadBody(node, &b)) return impl.res;
    std::string rt = SigText(node.sig());
    auto traces = TraceSubs(rt, node.formVersion(), localized_, b.subs, *data_->catalog);
    Value subs = Value::List();
    for (size_t i = 0; i < b.subs.size(); ++i) {
        Value s = Value::Map();
        s.Set("sig", Value::Str(SigText(b.subs[i].sig)));
        s.Set("size", Value::Int(static_cast<int64_t>(b.subs[i].size)));
        if (!traces[i].hasPattern) s.Set("pattern", Value::Bool(false));
        else if (!traces[i].decoded) s.Set("error", Value::Str(traces[i].failure));
        Value leaves = Value::List();
        for (const TraceLeaf& l : traces[i].leaves) {
            Value lv = Value::Map();
            lv.Set("path", Value::Str(l.path));
            lv.Set("offset", Value::Int(static_cast<int64_t>(l.offset)));
            lv.Set("size", Value::Int(static_cast<int64_t>(l.size)));
            lv.Set("value", l.value);
            if (l.bound) lv.Set("bound", Value::Bool(true));
            if (l.pattern && LeafIsFormId(*l.pattern)) lv.Set("formid", Value::Bool(true));
            leaves.Items().push_back(std::move(lv));
        }
        s.Set("values", std::move(leaves));
        subs.Items().push_back(std::move(s));
    }
    Value out = Value::Map();
    out.Set("type", Value::Str(rt));
    char flags[16];
    snprintf(flags, sizeof(flags), "%08X", node.flags());
    out.Set("flags", Value::Str(flags));
    out.Set("form_version", Value::Int(node.formVersion()));
    out.Set("subs", std::move(subs));
    WriteJson(out, json);
    impl.res.ok = true;
    return impl.res;
}

namespace {

std::wstring FullPath(const std::wstring& p) {
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, out.data(), nullptr);
    out.resize(n);
    return out;
}

// Where a folder really is, past junctions and symlinks.
std::wstring FinalPath(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return p;
    wchar_t buf[4096];
    DWORD n = GetFinalPathNameByHandleW(h, buf, 4096, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (n == 0 || n >= 4096) return p;
    return std::wstring(buf, n);
}

bool FileIdOf(const std::wstring& p, BY_HANDLE_FILE_INFORMATION* info) {
    HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BOOL ok = GetFileInformationByHandle(h, info);
    CloseHandle(h);
    return ok != 0;
}

bool InGameData(std::wstring folder) {
    for (auto& c : folder) c = towlower(c);
    for (auto& c : folder)
        if (c == L'/') c = L'\\';
    folder += L"\\";
    return folder.find(L"\\skyrim special edition\\data\\") != std::wstring::npos;
}

}  // namespace

bool Editor::Save(const std::wstring& path, const SaveOptions& opt, std::string* error) {
    std::wstring full = FullPath(path);
    size_t slash = full.find_last_of(L"\\/");
    std::wstring folder = slash == std::wstring::npos ? L"." : full.substr(0, slash);
    if (InGameData(folder) || InGameData(FinalPath(folder))) {
        *error = "refused: never writes into a Skyrim Special Edition\\Data folder";
        return false;
    }
    BY_HANDLE_FILE_INFORMATION out{};
    if (FileIdOf(full, &out)) {
        if (!opt.overwrite) {
            *error = "refused: the output exists (overwrite to replace it)";
            return false;
        }
    }
    // The file this document maps, or a hard link of it, must not be replaced,
    // whatever overwrite says. Also a source copied from.
    std::vector<std::wstring> inputs{doc_->path()};
    for (const auto& s : sources_) inputs.push_back(s.first);
    if (out.nNumberOfLinks) {
        for (const auto& src : inputs) {
            BY_HANDLE_FILE_INFORMATION in{};
            if (!src.empty() && FileIdOf(src, &in) && in.dwVolumeSerialNumber == out.dwVolumeSerialNumber &&
                in.nFileIndexHigh == out.nFileIndexHigh && in.nFileIndexLow == out.nFileIndexLow) {
                *error = "refused: the output is an input file (or a hard link of it)";
                return false;
            }
        }
    }
    Bytes bytes;
    if (!doc_->Serialize(&bytes, {}, error)) return false;
    // It must read back before it replaces anything.
    std::string reread;
    if (!Document::FromMemory(bytes.data(), bytes.size(), doc_->name(), &reread)) {
        *error = "internal: the result does not read back: " + reread;
        return false;
    }
    std::wstring temp = full + L".spfedit.tmp";
    {
        std::ofstream f(temp.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) {
            *error = "cannot create the output file";
            return false;
        }
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        f.flush();
        if (!f) {
            *error = "write failed";
            return false;
        }
    }
    // A rename, never a write into the old file: a hard link (Vortex) is
    // replaced here rather than written through into Data.
    if (!MoveFileExW(temp.c_str(), full.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        *error = "cannot rename the output into place (error " + std::to_string(GetLastError()) + ")";
        DeleteFileW(temp.c_str());
        return false;
    }
    original_ = doc_->rootPtr();
    return true;
}

namespace {

int TopRankPublic(const Sig& s) { return TopRank(s); }

void CheckPlacement(const Node& parent, const std::vector<const Node*>& stack, uint32_t wrldLabel, SelfCheck* c,
                    const PatternCatalog& cat, bool localized) {
    for (size_t i = 0; i < parent.children.size(); ++i) {
        const Node& n = *parent.children[i];
        auto problem = [&](uint64_t* counter, const std::string& what) {
            ++*counter;
            if (c->firstPlacement.empty()) c->firstPlacement = what;
        };
        if (n.isGroup) {
            uint32_t t = n.groupType();
            // A group owned by a record comes right after it.
            if (t == 1 || t == 6 || t == 7) {
                const Node* before = i ? parent.children[i - 1].get() : nullptr;
                const char* want = t == 1 ? "WRLD" : t == 6 ? "CELL" : "DIAL";
                if (!before || before->isGroup || !SigIs(before->sig(), want) || before->formId() != n.groupLabel()) {
                    problem(&c->orphanGroups, "group type " + std::to_string(t) + " label " + Hex6(n.groupLabel()) + " does not follow its record");
                }
            }
            std::vector<const Node*> next = stack;
            next.push_back(&n);
            CheckPlacement(n, next, t == 1 ? n.groupLabel() : wrldLabel, c, cat, localized);
            continue;
        }
        std::string rt = SigText(n.sig());
        if (rt != "CELL") {
            if (IsCellChildType(rt) && !stack.empty()) {
                uint32_t t = stack.back()->groupType();
                bool persistent = (n.flags() & 0x400) != 0;
                bool ok = rt == "LAND" || rt == "NAVM" ? t == 9 : (t == 8 && persistent) || (t == 9 && !persistent) || t == 10;
                if (!ok) problem(&c->childGroupWrong, rt + " " + Hex6(n.formId()) + " in group type " + std::to_string(t) + " flags " + std::to_string(n.flags()));
            }
            continue;
        }
        if (stack.empty()) continue;
        ++c->cellsChecked;
        const Node& g = *stack.back();
        uint32_t id = n.formId() & 0xFFFFFF;
        if (g.groupType() == 3) {
            const Node* block = stack.size() >= 2 ? stack[stack.size() - 2] : nullptr;
            if (g.groupLabel() != (id % 100) / 10 || !block || block->groupType() != 2 || block->groupLabel() != id % 10) {
                problem(&c->cellGroupWrong, "interior CELL " + Hex6(id) + " in sub-block " + std::to_string(g.groupLabel()));
            }
        } else if (g.groupType() == 5) {
            Bytes scratch;
            const uint8_t* body;
            size_t size;
            if (!Document::PlainBody(n, &scratch, &body, &size)) continue;
            ParsedBody p = ParseBody(body, size);
            for (const SubView& s : p.subs) {
                if (!SigIs(s.sig, "XCLC") || s.size < 8) continue;
                int32_t x = static_cast<int32_t>(U32(s.data)), y = static_cast<int32_t>(U32(s.data + 4));
                auto packed = [](int32_t a, int32_t b, int shift) {
                    return (static_cast<uint32_t>(a >> shift) << 16) | (static_cast<uint32_t>(b >> shift) & 0xFFFF);
                };
                const Node* block = stack.size() >= 2 ? stack[stack.size() - 2] : nullptr;
                // The label's two halves in either order count: which half is X is what this finds out.
                if (g.groupLabel() != packed(x, y, 3) || !block || block->groupLabel() != packed(x, y, 5)) {
                    problem(&c->cellGroupWrong, "exterior CELL " + Hex6(id) + " at " + std::to_string(x) + "," + std::to_string(y));
                }
            }
        }
    }
}

}  // namespace

SelfCheck RunSelfCheck(const std::wstring& path, const PatternCatalog& cat) {
    SelfCheck c;
    std::string err;
    auto doc = Document::Open(path, &err);
    if (!doc) {
        c.refused = err;
        return c;
    }
    bool localized = (doc->tes4()->flags() & 0x80) != 0;
    int lastRank = -1;
    for (size_t i = 1; i < doc->root().children.size(); ++i) {
        const Node& g = *doc->root().children[i];
        if (!g.isGroup || g.groupType() != 0) continue;
        Sig s;
        std::memcpy(s.data(), g.header.data() + 8, 4);
        int rank = TopRankPublic(s);
        if (rank < lastRank) {
            ++c.topOutOfOrder;
            if (c.firstPlacement.empty()) c.firstPlacement = "top group " + SigText(s) + " out of order";
        }
        lastRank = std::max(lastRank, rank);
    }
    CheckPlacement(doc->root(), {}, 0, &c, cat, localized);

    std::function<void(const Node&)> walk = [&](const Node& n) {
        for (const auto& child : n.children) {
            if (child->isGroup) {
                walk(*child);
                continue;
            }
            ++c.records;
            Bytes scratch;
            const uint8_t* body;
            size_t size;
            if (!Document::PlainBody(*child, &scratch, &body, &size)) continue;
            ParsedBody parsed = ParseBody(body, size);
            std::string rt = SigText(child->sig());
            auto traces = TraceSubs(rt, child->formVersion(), localized, parsed.subs, cat);
            DecodeContext ctx;
            ctx.rt = rt;
            ctx.formVersion = child->formVersion();
            ctx.localized = localized;
            int64_t lastEpft = 0;
            for (size_t i = 0; i < parsed.subs.size(); ++i) {
                const SubView& s = parsed.subs[i];
                const SubTrace& t = traces[i];
                ++c.subs;
                std::string fs = SigText(s.sig);
                if (rt == "PERK") {
                    if (fs == "PRKE") lastEpft = 0;
                    else if (fs == "EPFT" && s.size) lastEpft = s.data[0];
                    ctx.epft = lastEpft;
                }
                if (rt == "GMST" && fs == "EDID" && s.size) ctx.edidPrefix = std::string(1, static_cast<char>(tolower(s.data[0])));
                if (rt == "PACK" && fs == "ANAM") ctx.lastAnam.assign(reinterpret_cast<const char*>(s.data), strnlen(reinterpret_cast<const char*>(s.data), s.size));
                if (!t.hasPattern) continue;
                ctx.fsig = fs;
                std::string plain;
                bool decoded = false;
                try {
                    Value v = DecodeSubrecord(*cat.Find(rt, fs), s.data, s.size, ctx);
                    plain = JsonOf(v);
                    decoded = true;
                } catch (const std::exception&) {
                }
                auto report = [&](uint64_t* counter, const std::string& leafPath) {
                    ++*counter;
                    if (c.firstProblem.empty()) c.firstProblem = rt + "." + fs + " " + Hex6(child->formId()) + " " + leafPath;
                };
                if (decoded != t.decoded || (decoded && plain != t.json)) report(&c.traceDiffers, "");
                if (!t.decoded) {
                    ++c.undecoded;
                    continue;
                }
                size_t leafIndex = 0;
                const size_t leafCount = t.leaves.size();
                for (const TraceLeaf& l : t.leaves) {
                    ++c.leaves;
                    size_t thisLeaf = leafIndex++;
                    if (l.offset + l.size > s.size) {
                        report(&c.leafOutOfRange, l.path);
                        continue;
                    }
                    if (!l.pattern) continue;
                    std::string want = JsonOf(l.value);
                    // A leaf's path leads to its value in the decoded tree. Select
                    // copies the tree, so a huge subrecord (navmesh, landscape) is
                    // checked at its first and last values only, and is encoded
                    // back below in full either way.
                    if (leafCount <= 256 || thisLeaf < 32 || thisLeaf + 32 >= leafCount) {
                        auto found = Select(t.value, l.path);
                        bool pathOk = false;
                        for (const Value& f : found) pathOk = pathOk || JsonOf(f) == want;
                        if (!pathOk) report(&c.leafPathWrong, l.path);
                    }
                    // Encoding what it shows.
                    auto& e = c.encode[LeafTypeName(*l.pattern)];
                    ++e.total;
                    Bytes enc;
                    std::string encErr;
                    const uint8_t* cur = s.data + l.offset;
                    if (!EncodeLeaf(*l.pattern, l.value, cur, l.size, localized, &enc, &encErr)) {
                        ++e.refused;
                        if (e.example.empty()) e.example = rt + "." + fs + " " + l.path + ": " + encErr;
                        continue;
                    }
                    if (enc.size() == l.size && std::memcmp(enc.data(), cur, l.size) == 0) {
                        ++e.identical;
                        continue;
                    }
                    // Bytes differ (a display that rounds, lossy text). Does it
                    // still show the same? Only re-decoded when it comes to this,
                    // which is rare, and the same size for most types.
                    Bytes spliced(s.data, s.data + l.offset);
                    spliced.insert(spliced.end(), enc.begin(), enc.end());
                    spliced.insert(spliced.end(), s.data + l.offset + l.size, s.data + s.size);
                    bool same = false;
                    try {
                        same = JsonOf(DecodeSubrecord(*cat.Find(rt, fs), spliced.data(), spliced.size(), ctx)) == plain;
                    } catch (const std::exception&) {
                    }
                    if (same) {
                        ++e.sameDisplay;
                    } else {
                        ++e.differs;
                        if (e.example.empty()) e.example = rt + "." + fs + " " + Hex6(child->formId()) + " " + l.path + " shows " + want;
                    }
                }
            }
        }
    };
    walk(doc->root());
    return c;
}

const char* OpHelp() {
    return R"(Ops (JSON objects; `spfedit apply` takes a list of them):

  {"op":"set","form":F,"field":"DATA.damage","value":9}
      One value. Field paths are spf's: DATA.damage, CTDA[2].func_idx, EDID, KWDA[0]
      ("spfedit show" lists every path of a record with its bytes). A FormID value
      is Owner.esm:XXXXXX. Flags: a list of names, or {"add":[..],"remove":[..]}.
      Any value: "raw:<hex>" for exact bytes. Asking for the value it already
      shows changes nothing. Refused: a value later data depends on, a catalog
      row whose layout is in question; force accepts these.
  {"op":"set","form":F,"field":"DATA","raw":"0A0B..."}
      A whole subrecord's bytes; the catalog is not involved.
  {"op":"add-sub","form":F,"sig":"FULL","value":"Name"}   or "raw":"hex"
      At its place in the Creation Kit's order, or "after"/"before":"SIG". Nothing
      else moves. A list's count (KSIZ, COCT, SPCT, LLCT, CITC) follows.
  {"op":"remove-sub","form":F,"sig":"KWDA"[,"occurrence":n]}
      Takes its followers along (CNTO+COED, CTDA+CIS1/2, MODL+MODT/MODS).
  {"op":"add-item","form":F,"sig":"KWDA","item":"Skyrim.esm:01E718"}, {"op":"remove-item",...}
      A FormID in or out of a list held in one subrecord; KSIZ follows.
  {"op":"add-record","type":"WEAP","edid":"MySword","subs":[{"sig":"FULL","value":"x"},..]
                     [,"parent":CELL|DIAL|WRLD][,"flags":n][,"objectId":"000900"]
                     [,"grid":[x,y]][,"persistentCell":true]}
      A new form at the next free object ID. Records under a CELL, DIAL or WRLD
      name it in parent. Result: newForm.
  {"op":"copy-as-override","from":"path\\Source.esm","form":"Skyrim.esm:012EB7"[,"replace":true]}
      A record from another plugin, with its group chain and parent records
      (a REFR brings its CELL, a CELL its WRLD). Unchanged bytes where the two
      plugins' master lists agree; FormIDs remapped where they don't; masters
      added as needed.
  {"op":"delete-record","form":F[,"withChildren":true]}
      A form this plugin owns goes away. An override stays, marked deleted, in
      the shape the Creation Kit writes.
  {"op":"add-master","name":"Other.esm"}
      Appends it. The plugin's own index moves, so every FormID of its own is
      rewritten; values that look like FormIDs the catalog doesn't mark are listed
      in possibleMisses and refuse the op unless force.
  {"op":"set-flags","form":F,"set":"0x400","clear":"0x0"}
  {"op":"set-header","author":"..","description":"..","version":1.71,"esm":true,"esl":true}

F is Owner.esm:XXXXXX, or a stored FormID as eight hex digits. "nth" picks among
duplicates of one FormID in a file. HEDR's count and next object ID, ONAM and
INCC follow structural ops; everything else in the header stays as it was.)";
}

std::string ResultJson(const OpResult& r) {
    Value v = Value::Map();
    v.Set("ok", Value::Bool(r.ok));
    if (!r.ok) {
        v.Set("code", Value::Str(r.code));
        v.Set("message", Value::Str(r.message));
    }
    if (!r.newForm.empty()) v.Set("newForm", Value::Str(r.newForm));
    if (!r.changes.empty()) {
        Value list = Value::List();
        for (const Change& c : r.changes) {
            Value m = Value::Map();
            m.Set("form", Value::Str(c.form));
            m.Set("what", Value::Str(c.what));
            if (!c.field.empty()) m.Set("field", Value::Str(c.field));
            if (!c.before.empty()) m.Set("before", Value::Str(c.before));
            if (!c.after.empty()) m.Set("after", Value::Str(c.after));
            list.Items().push_back(std::move(m));
        }
        v.Set("changes", std::move(list));
    }
    auto strings = [&](const char* key, const std::vector<std::string>& items) {
        if (items.empty()) return;
        Value list = Value::List();
        for (const auto& s : items) list.Items().push_back(Value::Str(s));
        v.Set(key, std::move(list));
    };
    strings("warnings", r.warnings);
    strings("possibleMisses", r.possibleMisses);
    return JsonOf(v);
}

}  // namespace spf::edit
