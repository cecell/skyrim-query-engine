#include "itm.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

#include <bsa/reader.hpp>

#include "inflate.h"

namespace spf {
namespace {

// ---- xEdit's rules (Core\wbDefinitionsTES5.pas / wbDefinitionsCommon.pas,
// TES5Edit fd1e3602) ---------------------------------------------------------

// Subrecords xEdit never compares (cpIgnore).
const std::set<std::string> kIgnoredSubrecords = {
    "TACT.PNAM", "TACT.FNAM",          // :3347, :3349
    "CELL.LNAM",                       // :4348 leftover flags
    "CSTY.CSMD",                       // :4611
    "DIAL.TIFC",                       // :4903 info count
    "MESG.INAM",                       // :6610 leftover icon
    "MATO.DNAM",                       // :7532
    "PACK.PFOR",                       // :8704
    "SOUN.FNAM", "SOUN.SNDD",          // :10046, :10047
    "WATR.MNAM",                       // :10392
    "WRLD.NNAM", "WRLD.OFST", "WRLD.CLSZ", "WRLD.VISI", "WRLD.RNAM",  // :10773, Common :9246-9593
    "*.XPTL", "*.XMBP",                // :9731, :9754
    "*.SCHR", "*.SCTX",                // :9919, :9920
    "*.PRKF",                          // :6069 end marker
};

// Subrecords xEdit treats as identical when only the override has them and no
// later master overrides them (cpBenignIfAdded).
const std::set<std::string> kBenignIfAdded = {"*.XLRL"};

// Repeated subrecords xEdit compares as a sorted set (wbRArrayS).
const std::set<std::string> kSortedRepeats = {
    "*.XLKR", "*.SPLO", "*.CNTO", "NPC_.SNAM", "NPC_.PRKR", "NPC_.PNAM", "RACE.PNAM",
    "LVLI.LVLO", "LVLN.LVLO", "LVSP.LVLO", "ARMA.MODL", "LTEX.GNAM", "DIAL.TNAM",
};

// Subrecords whose single value is an array xEdit compares sorted (wbArrayS).
const std::set<std::string> kSortedArrays = {
    "*.KWDA", "*.MODS", "*.MO2S", "*.MO3S", "*.MO4S", "*.MO5S", "*.DMDS",
    "CELL.XCLR", "CLMT.WLST", "DOBJ.DNAM", "OTFT.INAM", "COLL.CNAM", "RACE.HNAM", "RACE.ENAM",
    "NAVM.NVSI", "LCTN.ACPR", "LCTN.LCPR", "LCTN.RCPR", "LCTN.ACUN", "LCTN.LCUN", "LCTN.RCUN",
    "LCTN.ACSR", "LCTN.LCSR", "LCTN.RCSR", "LCTN.ACID", "LCTN.LCID", "LCTN.ACEP", "LCTN.LCEP",
    "REGN.RDGS", "REGN.RDWT",
};

// Leaves (map keys) xEdit ignores inside a compared subrecord, besides every
// "Unused" field (wbUnused, which is cpIgnore).
const std::set<std::string> kIgnoredLeaves = {
    "*.MODT.texture_count", "*.MODT.addon_count",  // model info counters, Common :8528
    "*.MO2T.texture_count", "*.MO2T.addon_count", "*.MO3T.texture_count", "*.MO3T.addon_count",
    "*.MO4T.texture_count", "*.MO4T.addon_count", "*.MO5T.texture_count", "*.MO5T.addon_count",
    "*.DMDT.texture_count", "*.DMDT.addon_count",
    "QUST.DNAM.form_version",                       // :8785
};

// Subrecords whose catalog pattern does not follow xEdit's field layout get
// an xEdit-shaped pattern here. "unused*" leaves and skipped bytes are not
// compared.
const std::pair<const char*, const char*> kItmPatterns[] = {
    // :10604 Damage, Unused(2), % Mult, On Death (u8), Unused(3|7), Effect, [Unused(4)]
    {"WEAP.CRDT", R"({"t":"union","on":"$_remaining","cases":{
        "16":[{"t":"uint16","id":"damage"},{"t":"skip","n":2},{"t":"float32","id":"mult"},
              {"t":"uint8","id":"on_death"},{"t":"skip","n":3},{"t":"formid","id":"effect"}],
        "24":[{"t":"uint16","id":"damage"},{"t":"skip","n":2},{"t":"float32","id":"mult"},
              {"t":"uint8","id":"on_death"},{"t":"skip","n":7},{"t":"formid","id":"effect"},{"t":"skip","n":4}]}})"},
    // :3065 Type, Location Value (union on Type; 2, 3, 7, 10-12 are ignored), Radius
    {"PACK.PLDT", R"([{"t":"int32","id":"type","as":"type"},
        {"t":"union","id":"location","on":"$type","default":"int32","cases":{
            "0":"formid","1":"formid","4":"formid","6":"formid","5":"uint32",
            "2":{"t":"skip","n":4},"3":{"t":"skip","n":4},"7":{"t":"skip","n":4},
            "10":{"t":"skip","n":4},"11":{"t":"skip","n":4},"12":{"t":"skip","n":4}}},
        {"t":"opt","id":"radius","do":"int32"}])"},
    {"PACK.PLVD", R"([{"t":"int32","id":"type","as":"type"},
        {"t":"union","id":"location","on":"$type","default":"int32","cases":{
            "0":"formid","1":"formid","4":"formid","6":"formid","5":"uint32",
            "2":{"t":"skip","n":4},"3":{"t":"skip","n":4},"7":{"t":"skip","n":4},
            "10":{"t":"skip","n":4},"11":{"t":"skip","n":4},"12":{"t":"skip","n":4}}},
        {"t":"opt","id":"radius","do":"int32"}])"},
};

// Record flag bits xEdit ignores per record type (wbFlags(..., [bits])).
uint32_t IgnoredFlagBits(const std::string& rt) {
    auto bit = [](int b) { return 1u << b; };
    if (rt == "TACT") return bit(17);
    if (rt == "ANIO") return bit(9);
    if (rt == "CELL") return bit(14) | bit(18);
    if (rt == "DIAL" || rt == "WRLD") return bit(14);
    if (rt == "NAVM" || rt == "LAND" || rt == "NPC_") return bit(18);
    return 0;
}

bool InSet(const std::set<std::string>& s, const std::string& rt, const std::string& rest) {
    return s.count(rt + "." + rest) || s.count("*." + rest);
}

// ---- plugin files -------------------------------------------------------------

uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out->resize(static_cast<size_t>(size.QuadPart));
    size_t got = 0;
    while (got < out->size()) {
        DWORD n = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(out->size() - got, 1u << 30));
        if (!ReadFile(h, out->data() + got, want, &n, nullptr) || n == 0) break;
        got += n;
    }
    CloseHandle(h);
    return got == out->size();
}

// One file from a BSA ("strings\<name>"), compressed or not, through
// bsa-reader. The archive is opened for this one lookup and closed again:
// a mapping held inside explorer.exe would stop tools rewriting the archive.
// Any failure (no archive, not a BSA, corrupt, no such file) is a miss.
bool ReadFromBsa(const std::wstring& bsaPath, const std::string& path, std::vector<uint8_t>* out) {
    constexpr size_t kMaxStringsFile = size_t{64} << 20;
    bsa::Reader r;
    if (r.open(bsaPath) != bsa::Status::Ok) return false;
    bsa::Entry e;
    return r.find(path, e) == bsa::Status::Ok && r.read(e, *out, kMaxStringsFile) == bsa::Status::Ok;
}

// A read-only memory mapping of a whole file.
struct MappedView {
    HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr;
    const uint8_t* p = nullptr;
    size_t n = 0;

    MappedView() = default;
    MappedView(const MappedView&) = delete;
    MappedView& operator=(const MappedView&) = delete;
    ~MappedView() {
        if (p) UnmapViewOfFile(p);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    bool Open(const std::wstring& path) {
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart == 0) return false;
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) return false;
        p = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        n = static_cast<size_t>(size.QuadPart);
        return p != nullptr;
    }
    const uint8_t* data() const { return p; }
    size_t size() const { return n; }
};

struct PluginFile {
    std::string name;  // lower case
    std::wstring dir;  // folder, with trailing separator
    MappedView bytes;
    std::vector<std::string> masters;  // lower case
    bool localized = false;
    // Every record, sorted by stored FormID (first occurrence wins), with the
    // stored FormID of the WRLD / CELL / DIAL it sits under (0 = none).
    struct Entry {
        uint32_t fid, offset, parent;
    };
    std::vector<Entry> index;
    std::vector<size_t> order;  // record offsets in file order (the checked plugin only)
    const Entry* Find(uint32_t fid) const {
        auto it = std::lower_bound(index.begin(), index.end(), fid,
                                   [](const Entry& e, uint32_t f) { return e.fid < f; });
        return it == index.end() || it->fid != fid ? nullptr : &*it;
    }
    bool Find(uint32_t fid, size_t* offset) const {
        const Entry* e = Find(fid);
        if (e) *offset = e->offset;
        return e != nullptr;
    }
    // Records inside a world / cell / topic children group, by the stored
    // FormID of that WRLD / CELL / DIAL (the checked plugin only).
    std::unordered_map<uint32_t, std::vector<size_t>> children;

    // Localized string tables, loaded on first use: strings, dlstrings, ilstrings.
    // Masters are shared between checks on several threads.
    mutable std::mutex stringsLock;
    mutable bool stringsLoaded = false;
    mutable std::unordered_map<uint32_t, std::string> strings[3];

    void LoadStrings() const {
        stringsLoaded = true;
        std::string base = name.substr(0, name.find_last_of('.'));
        static const char* kExt[3] = {"strings", "dlstrings", "ilstrings"};
        for (int c = 0; c < 3; ++c) {
            std::string fn = base + "_english." + kExt[c];
            std::wstring wfn(fn.begin(), fn.end());
            std::vector<uint8_t> data;
            if (!ReadWholeFile(dir + L"Strings\\" + wfn, &data)) {
                std::wstring wbase(base.begin(), base.end());
                if (!ReadFromBsa(dir + wbase + L".bsa", "strings\\" + fn, &data) &&
                    !ReadFromBsa(dir + L"Skyrim - Interface.bsa", "strings\\" + fn, &data)) {
                    continue;
                }
            }
            if (data.size() < 8) continue;
            uint32_t count = U32(data.data()), dataSize = U32(data.data() + 4);
            size_t dirEnd = 8 + static_cast<size_t>(count) * 8;
            if (dirEnd + dataSize > data.size()) continue;
            const uint8_t* blob = data.data() + dirEnd;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t id = U32(data.data() + 8 + i * 8), off = U32(data.data() + 12 + i * 8);
                if (off >= dataSize) continue;
                const uint8_t* s = blob + off;
                size_t len;
                if (c == 0) {
                    len = strnlen(reinterpret_cast<const char*>(s), dataSize - off);
                } else {
                    if (off + 4 > dataSize) continue;
                    len = U32(s);
                    s += 4;
                    if (off + 4 + len > dataSize) continue;
                    while (len && s[len - 1] == 0) --len;
                }
                strings[c][id] = Utf8Lossy(s, len);
            }
        }
    }
    // Text of a localized string ID, trying the field's own table first.
    const std::string* Text(uint32_t id, const std::string& cls) const {
        std::lock_guard<std::mutex> g(stringsLock);  // the tables never change once loaded
        if (!stringsLoaded) LoadStrings();
        int first = cls == "dlstrings" ? 1 : cls == "ilstrings" ? 2 : 0;
        for (int k = 0; k < 3; ++k) {
            int c = k == 0 ? first : (k <= first ? k - 1 : k);
            auto it = strings[c].find(id);
            if (it != strings[c].end()) return &it->second;
        }
        return nullptr;
    }

    // `tree`: also record order, parents and children (the checked plugin).
    bool Load(const std::wstring& path, bool tree, std::string* error) {
        dir = path.substr(0, path.find_last_of(L"\\/") + 1);
        if (!bytes.Open(path)) {
            *error = "cannot open";
            return false;
        }
        if (bytes.size() < 24 || memcmp(bytes.data(), "TES4", 4) != 0) {
            *error = "not a plugin";
            return false;
        }
        localized = (U32(bytes.data() + 8) & 0x80) != 0;
        size_t hdrSize = U32(bytes.data() + 4);
        if (24 + hdrSize > bytes.size()) {
            *error = "truncated header";
            return false;
        }
        for (size_t p = 24; p + 6 <= 24 + hdrSize;) {
            size_t n = U16(bytes.data() + p + 4);
            if (memcmp(bytes.data() + p, "MAST", 4) == 0 && p + 6 + n <= bytes.size()) {
                const char* s = reinterpret_cast<const char*>(bytes.data() + p + 6);
                masters.push_back(Lower(std::string(s, strnlen(s, n))));
            }
            p += 6 + n;
        }
        struct Group {
            size_t end;
            uint32_t type, label;
        };
        std::vector<Group> groups;
        for (size_t p = 24 + hdrSize; p + 24 <= bytes.size();) {
            while (!groups.empty() && p >= groups.back().end) groups.pop_back();
            const uint8_t* r = bytes.data() + p;
            if (memcmp(r, "GRUP", 4) == 0) {
                groups.push_back({p + U32(r + 4), U32(r + 12), U32(r + 8)});
                p += 24;
                continue;
            }
            size_t len = U32(r + 4);
            if (p + 24 + len > bytes.size()) {
                *error = "record runs past the end of the file";
                return false;
            }
            uint32_t par = 0;
            for (auto g = groups.rbegin(); g != groups.rend(); ++g) {
                if (g->type == 1 || g->type == 6 || g->type == 7) {  // world / cell / topic children
                    par = g->label;
                    break;
                }
            }
            index.push_back({U32(r + 12), static_cast<uint32_t>(p), par});
            if (tree) {
                order.push_back(p);
                if (par) children[par].push_back(p);
            }
            p += 24 + len;
        }
        std::stable_sort(index.begin(), index.end(), [](const Entry& a, const Entry& b) { return a.fid < b.fid; });
        index.erase(std::unique(index.begin(), index.end(),
                                [](const Entry& a, const Entry& b) { return a.fid == b.fid; }),
                    index.end());
        index.shrink_to_fit();
        return true;
    }

    // Owner (lower-case file name) and object ID of a FormID stored in this file.
    std::pair<std::string, uint32_t> Owner(uint32_t fid) const {
        size_t hb = fid >> 24;
        return {hb < masters.size() ? masters[hb] : name, fid & 0xFFFFFF};
    }
    // How this file stores a form owned by `owner`, or false if it cannot.
    bool StoredId(const std::string& owner, uint32_t local, uint32_t* out) const {
        if (owner == name) {
            *out = (static_cast<uint32_t>(masters.size()) << 24) | local;
            return true;
        }
        for (size_t i = 0; i < masters.size(); ++i) {
            if (masters[i] == owner) {
                *out = (static_cast<uint32_t>(i) << 24) | local;
                return true;
            }
        }
        return false;
    }
};

// ---- normalising one record version into comparable strings -------------------

struct Field {
    std::vector<std::string> values;  // one per occurrence, comparison keys
    bool allZero = true;              // every occurrence's bytes are zero
};

struct Version {
    const PluginFile* file = nullptr;
    uint32_t flags = 0;
    bool deleted = false;
    std::map<std::string, Field> fields;  // by subrecord signature
    std::string error;
};

std::string FloatKey(double v) {
    // xEdit TwbFloatDef.ToSortKey: zero / denormal -> 0, sign + fixed digits.
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "+inf" : "-inf";
    if (v == 0 || std::fabs(v) < 1.17549435e-38) v = 0;
    char buf[400];
    snprintf(buf, sizeof(buf), "%c%.6f", v < 0 ? '-' : '+', std::fabs(v));
    return buf;
}

bool IsFormidText(const std::string& s, uint32_t* out) {
    if (s.size() != 10 || s[0] != '[' || s[9] != ']') return false;
    uint32_t v = 0;
    for (int i = 1; i < 9; ++i) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return false;
    }
    *out = v;
    return true;
}

bool IgnoredKey(const std::string& rt, const std::string& fs, const std::string& key) {
    if (Lower(key).rfind("unused", 0) == 0) return true;
    return InSet(kIgnoredLeaves, rt, fs + "." + key);
}

std::string FormidText(uint32_t raw) {
    char buf[16];
    snprintf(buf, sizeof(buf), "[%08X]", raw);
    return buf;
}

// Fields whose meaning depends on other fields in ways the pattern cannot
// express; returns the value to compare.
struct CtdaParams {
    int index;
    char p1, p2;
};
const CtdaParams kCtdaParams[] = {
#include "itm_ctda.inc"
};

Value Adjust(const std::string& rt, const std::string& fs, const Value& v) {
    if (fs == "CTDA" && v.kind() == Value::Kind::Map) {
        // Condition parameters take their type from the function (xEdit
        // wbConditionParameters): FormIDs compare by target, unused and
        // String parameters are not compared.
        static const std::unordered_map<int, std::pair<char, char>> table = [] {
            std::unordered_map<int, std::pair<char, char>> t;
            for (const auto& c : kCtdaParams) t[c.index] = {c.p1, c.p2};
            return t;
        }();
        const Value* fn = v.Find("func_idx");
        auto it = fn && fn->kind() == Value::Kind::Int ? table.find(static_cast<int>(fn->AsInt())) : table.end();
        if (it == table.end()) return v;
        Value out = Value::Map();
        for (const auto& m : v.Members()) {
            char k = m.first == "param1" ? it->second.first : m.first == "param2" ? it->second.second : 0;
            if (k == 'I') continue;
            if (k == 'F' && m.second.kind() == Value::Kind::Int) {
                out.Set(m.first, Value::Str(FormidText(static_cast<uint32_t>(m.second.AsInt()))));
                continue;
            }
            out.Set(m.first, m.second);
        }
        return out;
    }
    if (rt == "BOOK" && fs == "DATA" && v.kind() == Value::Kind::Map) {
        // :4247 Teaches: Skill if flag 0x1, Spell if flag 0x4, else unused.
        const Value* flags = v.Find("flags");
        int64_t fl = flags && flags->kind() == Value::Kind::Int ? flags->AsInt() : 0;
        Value out = Value::Map();
        for (const auto& m : v.Members()) {
            if (m.first == "teaches") {
                if (fl & 1) {
                    out.Set(m.first, m.second);
                } else if (fl & 4) {
                    out.Set(m.first, m.second.kind() == Value::Kind::Int
                                         ? Value::Str(FormidText(static_cast<uint32_t>(m.second.AsInt())))
                                         : m.second);
                }
                continue;
            }
            out.Set(m.first, m.second);
        }
        return out;
    }
    return v;
}

void Canon(const Value& v, const PluginFile& f, const std::string& rt, const std::string& fs, bool top,
           std::string* out, const PatternCatalog* cat = nullptr) {
    switch (v.kind()) {
        case Value::Kind::Null: *out += "~"; break;
        case Value::Kind::Bool: *out += v.AsBool() ? "T" : "F"; break;
        case Value::Kind::Int: *out += std::to_string(v.AsInt()); break;
        case Value::Kind::Float: *out += FloatKey(v.AsFloat()); break;
        case Value::Kind::String: {
            uint32_t fid;
            const std::string& s = v.AsString();
            if (s.size() == 20 && s.rfind("[LSTRING:0x", 0) == 0) {
                // Localized: xEdit compares the text, so an ID in a master
                // equals the same text inline in the plugin.
                uint32_t id = static_cast<uint32_t>(strtoul(s.c_str() + 11, nullptr, 16));
                const std::string* text = id ? f.Text(id, cat ? cat->LstringClass(rt, fs) : "") : nullptr;
                if (id == 0) *out += "\"\"";
                else if (text) *out += '"' + *text + '"';
                else *out += "LS#" + f.name + s;  // unknown text: compares unequal to anything else
            } else if (IsFormidText(s, &fid)) {
                if (fid == 0) {
                    *out += "@NULL";
                } else {
                    auto o = f.Owner(fid);
                    char buf[32];
                    snprintf(buf, sizeof(buf), ":%06X", o.second);
                    *out += "@" + o.first + buf;
                }
            } else {
                *out += '"';
                *out += v.AsString();
                *out += '"';
            }
            break;
        }
        case Value::Kind::List: {
            std::vector<std::string> items;
            for (const auto& it : v.Items()) {
                std::string s;
                Canon(it, f, rt, fs, false, &s, cat);
                items.push_back(std::move(s));
            }
            if (top && InSet(kSortedArrays, rt, fs)) std::sort(items.begin(), items.end());
            *out += '[';
            for (const auto& s : items) *out += s + ',';
            *out += ']';
            break;
        }
        case Value::Kind::Map: {
            *out += '{';
            for (const auto& m : v.Members()) {
                if (IgnoredKey(rt, fs, m.first)) continue;
                *out += m.first + '=';
                // A map holding the subrecord's array (seq with one id) keeps
                // the sorted-array rule.
                Canon(m.second, f, rt, fs, top && v.Members().size() == 1, out, cat);
                *out += ';';
            }
            *out += '}';
            break;
        }
    }
}

bool ReadVersion(const PluginFile& f, size_t offset, const PatternCatalog& cat, Version* out) {
    const uint8_t* rec = f.bytes.data() + offset;
    std::string rt(reinterpret_cast<const char*>(rec), 4);
    out->file = &f;
    out->flags = U32(rec + 8);
    out->deleted = (out->flags & 0x20) != 0;
    size_t size = U32(rec + 4);
    const uint8_t* body = rec + 24;
    std::vector<uint8_t> inflated;
    if (out->flags & 0x00040000) {
        if (size < 4 || !ZlibInflate(body + 4, size - 4, U32(body), &inflated)) {
            out->error = "cannot decompress";
            return false;
        }
        body = inflated.data();
        size = inflated.size();
    }
    out->flags &= ~(IgnoredFlagBits(rt) | 0x00040000u);  // compression is a storage detail
    DecodeContext ctx;
    ctx.rt = rt;
    ctx.formVersion = U16(rec + 20);
    ctx.localized = f.localized;
    int64_t lastEpft = 0;
    uint32_t big = 0;
    bool haveBig = false;
    for (size_t p = 0; p + 6 <= size;) {
        std::string fs(reinterpret_cast<const char*>(body + p), 4);
        size_t len = U16(body + p + 4);
        p += 6;
        if (haveBig) {
            len = big;
            haveBig = false;
        }
        if (p + len > size) {
            out->error = fs + " runs past the record end";
            return false;
        }
        const uint8_t* data = body + p;
        p += len;
        if (fs == "XXXX" && len >= 4) {
            big = U32(data);
            haveBig = true;
            continue;
        }
        if (rt == "PERK") {
            if (fs == "PRKE") lastEpft = 0;
            else if (fs == "EPFT" && len) lastEpft = data[0];
            ctx.epft = lastEpft;
        }
        if (InSet(kIgnoredSubrecords, rt, fs)) continue;
        std::string key;
        if (const Pattern* pat = cat.Find(rt, fs)) {
            ctx.fsig = fs;
            try {
                Canon(Adjust(rt, fs, DecodeSubrecord(*pat, data, len, ctx)), f, rt, fs, true, &key, &cat);
            } catch (const PatternError&) {
                key.clear();  // fall back to the raw bytes below
            }
        }
        if (key.empty()) {
            // No pattern (or it does not fit): compare the bytes exactly.
            static const char kHex[] = "0123456789ABCDEF";
            key = "#";
            for (size_t i = 0; i < len; ++i) {
                key.push_back(kHex[data[i] >> 4]);
                key.push_back(kHex[data[i] & 15]);
            }
        }
        Field& fld = out->fields[fs];
        fld.values.push_back(std::move(key));
        // An empty marker subrecord is not "all zero": its presence is its value.
        if (len == 0) fld.allZero = false;
        for (size_t i = 0; i < len && fld.allZero; ++i) fld.allZero = data[i] == 0;
    }
    for (auto& [fs, fld] : out->fields) {
        if (InSet(kSortedRepeats, rt, fs)) std::sort(fld.values.begin(), fld.values.end());
    }
    // The cell a reference sits in (the topic of an INFO, the world of a
    // CELL) is part of what xEdit compares.
    const PluginFile::Entry* entry = f.Find(U32(rec + 12));
    if (entry && entry->offset == offset && entry->parent) {
        auto o = f.Owner(entry->parent);
        char buf[32];
        snprintf(buf, sizeof(buf), ":%06X", o.second);
        Field& fld = out->fields["<parent>"];
        fld.values.push_back(o.first + buf);
        fld.allZero = false;
    }
    return true;
}

// Masters stay loaded between checks (Explorer checks every plugin in a
// folder against the same few masters). Keyed by path, size and write time.
std::shared_ptr<const PluginFile> LoadMaster(const std::wstring& path, const std::string& name) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return nullptr;
    std::wstring key = path + L"|" + std::to_wstring(fa.nFileSizeLow) + L"|" + std::to_wstring(fa.nFileSizeHigh) +
                       L"|" + std::to_wstring(fa.ftLastWriteTime.dwLowDateTime) + L"|" +
                       std::to_wstring(fa.ftLastWriteTime.dwHighDateTime);
    for (auto& c : key) c = static_cast<wchar_t>(towlower(c));
    static std::mutex lock;
    static std::vector<std::pair<std::wstring, std::shared_ptr<const PluginFile>>> cache;  // most recent last
    constexpr size_t kCacheSize = 8;
    {
        std::lock_guard<std::mutex> g(lock);
        for (size_t i = 0; i < cache.size(); ++i) {
            if (cache[i].first == key) {
                auto hit = cache[i];
                cache.erase(cache.begin() + i);
                cache.push_back(hit);
                return hit.second;
            }
        }
    }
    // One load at a time: several checks starting together would otherwise
    // all index Skyrim.esm at once. A waiting check then finds it cached.
    static std::mutex loading;
    std::lock_guard<std::mutex> l(loading);
    {
        std::lock_guard<std::mutex> g(lock);
        for (const auto& c : cache) {
            if (c.first == key) return c.second;
        }
    }
    auto mf = std::make_shared<PluginFile>();
    mf->name = name;
    std::string e;
    if (!mf->Load(path, false, &e)) return nullptr;
    std::lock_guard<std::mutex> g(lock);
    cache.emplace_back(key, mf);
    if (cache.size() > kCacheSize) cache.erase(cache.begin());
    return mf;
}

// ---- the comparison --------------------------------------------------------------

std::wstring g_mastersFolder;  // test hook, see SetItmMastersFolder

struct Session {
    PatternCatalog cat;  // the catalog with the xEdit-shaped patterns applied
    PluginFile plugin;
    std::vector<std::shared_ptr<const PluginFile>> masters;  // in load order (the plugin's master list)
    std::vector<std::string> missing;

    explicit Session(const PatternCatalog& c) : cat(c) {
        for (const auto& [key, json] : kItmPatterns) cat.Override(key, json, nullptr);
    }

    bool Open(const std::wstring& path, std::string* error) {
        std::wstring dir = path.substr(0, path.find_last_of(L"\\/") + 1);
        std::wstring masterDir = g_mastersFolder.empty() ? dir : g_mastersFolder;
        // Master names in a plugin are Windows-1252 text.
        auto narrow = [](const std::wstring& w) {
            int n = WideCharToMultiByte(1252, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(n, '\0');
            WideCharToMultiByte(1252, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
            return s;
        };
        auto widen = [](const std::string& s) {
            int n = MultiByteToWideChar(1252, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(n, L'\0');
            MultiByteToWideChar(1252, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
            return w;
        };
        plugin.name = Lower(narrow(path.substr(dir.size())));
        if (!plugin.Load(path, true, error)) return false;
        for (const auto& m : plugin.masters) {
            auto mf = LoadMaster(masterDir + widen(m), m);
            if (mf) masters.push_back(std::move(mf));
            else missing.push_back(m);
        }
        return true;
    }

    // Returns true for ITM; `why` collects the reasons it is not.
    bool Compare(size_t offset, std::vector<std::string>* why) {
        const uint8_t* rec = plugin.bytes.data() + offset;
        std::string rt(reinterpret_cast<const char*>(rec), 4);
        uint32_t fid = U32(rec + 12);
        auto [owner, local] = plugin.Owner(fid);
        auto note = [why](std::string s) {
            if (why) why->push_back(std::move(s));
            return false;
        };
        if (owner == plugin.name) return note("new record, not an override");

        // Every loaded version, master first, the plugin last.
        std::vector<Version> versions;
        bool definingFound = false;
        for (const auto& m : masters) {
            uint32_t stored;
            if (!m->StoredId(owner, local, &stored)) continue;
            size_t at;
            if (!m->Find(stored, &at)) continue;
            if (m->name == owner) definingFound = true;
            versions.emplace_back();
            if (!ReadVersion(*m, at, cat, &versions.back())) return note(m->name + ": " + versions.back().error);
        }
        if (!definingFound) return note("injected or the defining master is missing");
        versions.emplace_back();
        Version& mine = versions.back();
        if (!ReadVersion(plugin, offset, cat, &mine)) return note(mine.error);
        if (mine.deleted) return note("deleted");
        const Version& first = versions.front();

        bool itm = true;
        if (mine.flags != first.flags) {
            char buf[80];
            snprintf(buf, sizeof(buf), "record flags %08X vs master %08X", mine.flags, first.flags);
            itm = note(buf);
        }
        std::set<std::string> sigs;
        for (const auto& v : versions)
            for (const auto& f : v.fields) sigs.insert(f.first);
        for (const auto& fs : sigs) {
            std::vector<const Field*> per;
            bool anyMissing = false, allZero = true;
            for (const auto& v : versions) {
                auto it = v.fields.find(fs);
                per.push_back(it == v.fields.end() ? nullptr : &it->second);
                if (!per.back()) anyMissing = true;
                else if (!it->second.allZero) allZero = false;
            }
            const Field* m0 = per.front();
            const Field* me = per.back();
            if (!m0 && me && InSet(kBenignIfAdded, rt, fs)) {
                bool onlyMine = true;
                for (size_t i = 0; i + 1 < per.size(); ++i) onlyMine &= per[i] == nullptr;
                if (onlyMine) continue;
            }
            // xEdit's "missing equals all-zero" rule (xeMainForm.pas :2745) only
            // covers array / struct / value elements, never a whole subrecord,
            // so an added subrecord always differs.
            (void)anyMissing;
            (void)allZero;
            auto same = [](const Field* a, const Field* b) {
                if (!a || !b) return a == b;
                return a->values == b->values;
            };
            if (!same(me, m0)) {
                if (why) {
                    // Show the first occurrence that differs.
                    size_t na = me ? me->values.size() : 0, nb = m0 ? m0->values.size() : 0;
                    size_t k = 0;
                    while (k < na && k < nb && me->values[k] == m0->values[k]) ++k;
                    std::string a = k < na ? me->values[k] : "(absent)";
                    std::string b = k < nb ? m0->values[k] : "(absent)";
                    note(fs + " differs (#" + std::to_string(k) + " of " + std::to_string(na) + "/" +
                         std::to_string(nb) + "): " + a.substr(0, 200) + " | master: " + b.substr(0, 200));
                }
                itm = false;
                continue;
            }
            // Same as the master; a different value in between makes it
            // "identical to master, wins conflict", which cleaning keeps.
            for (size_t i = 1; i + 1 < per.size(); ++i) {
                if (!same(per[i], m0)) {
                    itm = note(fs + ": restores the master's value over " + versions[i].file->name);
                    break;
                }
            }
        }
        if (mine.flags == first.flags) {
            for (size_t i = 1; i + 1 < versions.size(); ++i) {
                if (versions[i].flags != first.flags) {
                    itm = note("record flags: restores the master's value over " + versions[i].file->name);
                    break;
                }
            }
        }
        return itm;
    }
};

}  // namespace

namespace {

ItmResult FindItmsImpl(const std::wstring& pluginPath, const PatternCatalog& catalog) {
    ItmResult r;
    Session s(catalog);
    if (!s.Open(pluginPath, &r.error)) return r;
    r.missingMasters = s.missing;
    if (!s.missing.empty()) {
        r.ok = true;  // without every master there is nothing to compare against
        return r;
    }
    std::unordered_map<size_t, bool> itm;
    for (size_t off : s.plugin.order) {
        uint32_t fid = U32(s.plugin.bytes.data() + off + 12);
        if (fid == 0 || (fid >> 24) >= s.plugin.masters.size()) continue;  // header / new records
        ++r.overrides;
        itm[off] = s.Compare(off, nullptr);
    }
    // Cleaning removes an ITM record only once nothing is left under it: a
    // CELL / WRLD / DIAL whose children group still holds records stays.
    std::unordered_map<size_t, bool> removed;
    std::function<bool(size_t)> isRemoved = [&](size_t off) -> bool {
        auto memo = removed.find(off);
        if (memo != removed.end()) return memo->second;
        removed[off] = false;  // guards against cycles in malformed files
        auto it = itm.find(off);
        bool res = it != itm.end() && it->second;
        if (res) {
            auto kids = s.plugin.children.find(U32(s.plugin.bytes.data() + off + 12));
            if (kids != s.plugin.children.end()) {
                for (size_t k : kids->second) {
                    if (!isRemoved(k)) {
                        res = false;
                        break;
                    }
                }
            }
        }
        removed[off] = res;
        return res;
    };
    for (size_t off : s.plugin.order) {
        if (itm.count(off) && isRemoved(off)) {
            const uint8_t* rec = s.plugin.bytes.data() + off;
            r.itms.push_back({U32(rec + 12), std::string(reinterpret_cast<const char*>(rec), 4)});
        }
    }
    r.ok = true;
    return r;
}

void FindItmsGuarded(const std::wstring& pluginPath, const PatternCatalog& catalog, ItmResult* out) {
    try {
        *out = FindItmsImpl(pluginPath, catalog);
    } catch (const std::exception& e) {
        *out = ItmResult();
        out->error = e.what();
    }
}

// Files are memory-mapped: a file shrinking under us faults on access.
bool FindItmsSeh(const std::wstring* pluginPath, const PatternCatalog* catalog, ItmResult* out) {
    __try {
        FindItmsGuarded(*pluginPath, *catalog, out);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

}  // namespace

void SetItmMastersFolder(const std::wstring& folder) {
    g_mastersFolder = folder;
    if (!g_mastersFolder.empty() && g_mastersFolder.back() != L'\\' && g_mastersFolder.back() != L'/')
        g_mastersFolder += L'\\';
}

ItmResult FindItms(const std::wstring& pluginPath, const PatternCatalog& catalog) {
    ItmResult r;
    if (!FindItmsSeh(&pluginPath, &catalog, &r)) {
        r = ItmResult();
        r.error = "a file changed while it was read";
    }
    return r;
}

std::vector<std::string> ExplainItm(const std::wstring& pluginPath, const PatternCatalog& catalog, uint32_t formId) {
    std::vector<std::string> why;
    Session s(catalog);
    std::string err;
    if (!s.Open(pluginPath, &err)) return {err};
    size_t at;
    if (!s.plugin.Find(formId, &at)) return {"no such record"};
    if (s.Compare(at, &why)) why.push_back("ITM");
    return why;
}

}  // namespace spf
