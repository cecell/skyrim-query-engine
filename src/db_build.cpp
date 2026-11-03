#include "db_build.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include "pattern_value.h"
#include "db_schema.h"
#include "plugin.h"
#include "sqlite3.h"

namespace spf {
namespace {

constexpr int kSchemaVersion = 4;
constexpr size_t kBatchRows = 50000;
constexpr int64_t kKeyShift = 1 << 24;

uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring WithSlash(std::wstring d) {
    if (!d.empty() && d.back() != L'\\' && d.back() != L'/') d += L'\\';
    return d;
}

bool IsPluginName(const std::string& name) {
    std::string l = Lower(name);
    return l.size() > 4 && (l.compare(l.size() - 4, 4, ".esm") == 0 || l.compare(l.size() - 4, 4, ".esp") == 0 ||
                            l.compare(l.size() - 4, 4, ".esl") == 0);
}

bool HasExt(const std::string& name, const char* ext) {
    std::string l = Lower(name);
    return l.size() > 4 && l.compare(l.size() - 4, 4, ext) == 0;
}

uint32_t Crc32(const uint8_t* p, size_t n) {
    static uint32_t table[8][256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int t = 1; t < 8; ++t) table[t][i] = (table[t - 1][i] >> 8) ^ table[0][table[t - 1][i] & 0xFF];
    });
    uint32_t c = 0xFFFFFFFFu;
    while (n >= 8) {  // slicing by 8
        uint32_t a = c ^ U32(p), b = U32(p + 4);
        c = table[7][a & 0xFF] ^ table[6][(a >> 8) & 0xFF] ^ table[5][(a >> 16) & 0xFF] ^ table[4][a >> 24] ^
            table[3][b & 0xFF] ^ table[2][(b >> 8) & 0xFF] ^ table[1][(b >> 16) & 0xFF] ^ table[0][b >> 24];
        p += 8;
        n -= 8;
    }
    while (n--) c = table[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

// ---- Plugin headers (phase 1: cheap, decides keys and load-order slots) ----

struct PluginInfo {
    int no = 0;
    std::string name;
    bool inLoadOrder = false;
    int loadOrder = -1, modIndex = -1;
    bool exists = false, light = false, master = false, localized = false;
    uint32_t flags = 0;
    float hedrVersion = 0;
    uint32_t numRecords = 0, nextObjectId = 0;
    std::string author, description;
    std::vector<std::string> masters;
    std::wstring path;
    int64_t size = 0, mtime = 0;
};

bool ReadHeader(PluginInfo* p) {
    HANDLE h = CreateFileW(p->path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    p->exists = true;  // on disk; whether its header reads is the return value
    BY_HANDLE_FILE_INFORMATION fi{};
    GetFileInformationByHandle(h, &fi);
    p->size = (static_cast<int64_t>(fi.nFileSizeHigh) << 32) | fi.nFileSizeLow;
    int64_t ft = (static_cast<int64_t>(fi.ftLastWriteTime.dwHighDateTime) << 32) | fi.ftLastWriteTime.dwLowDateTime;
    p->mtime = ft / 10000000 - 11644473600LL;  // unix seconds
    uint8_t hdr[24];
    DWORD got = 0;
    bool ok = ReadFile(h, hdr, 24, &got, nullptr) && got == 24 && memcmp(hdr, "TES4", 4) == 0;
    std::vector<uint8_t> body;
    // The header's size comes from the file: no larger than the file itself,
    // and far below 64 MB (big override lists make it a few hundred KB).
    ok = ok && U32(hdr + 4) <= std::min<int64_t>(p->size - 24, int64_t{64} << 20);
    if (ok) {
        body.resize(U32(hdr + 4));
        ok = body.empty() || (ReadFile(h, body.data(), static_cast<DWORD>(body.size()), &got, nullptr) && got == body.size());
    }
    CloseHandle(h);
    if (!ok) return false;
    p->flags = U32(hdr + 8);
    p->master = (p->flags & 1) || HasExt(p->name, ".esm") || HasExt(p->name, ".esl");
    p->light = (p->flags & 0x200) || HasExt(p->name, ".esl");
    p->localized = (p->flags & 0x80) != 0;
    for (size_t q = 0; q + 6 <= body.size();) {
        size_t n = U16(body.data() + q + 4);
        const uint8_t* d = body.data() + q + 6;
        if (q + 6 + n > body.size()) break;
        auto text = [&] { return Utf8Lossy(d, strnlen(reinterpret_cast<const char*>(d), n)); };
        if (memcmp(body.data() + q, "HEDR", 4) == 0 && n >= 12) {
            memcpy(&p->hedrVersion, d, 4);
            p->numRecords = U32(d + 4);
            p->nextObjectId = U32(d + 8);
        } else if (memcmp(body.data() + q, "CNAM", 4) == 0) {
            p->author = text();
        } else if (memcmp(body.data() + q, "SNAM", 4) == 0) {
            p->description = text();
        } else if (memcmp(body.data() + q, "MAST", 4) == 0) {
            p->masters.push_back(text());
        }
        q += 6 + n;
    }
    return true;
}

// The load order: names only, in order.
std::vector<std::string> ResolveLoadOrder(const BuildOptions& opt, const std::wstring& data, std::string* error) {
    if (!opt.plugins.empty()) return opt.plugins;

    std::vector<std::string> inData;  // every plugin file in the Data folder
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((data + L"*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) {
        *error = "cannot list the Data folder";
        return {};
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsPluginName(Utf8(fd.cFileName)))
            inData.push_back(Utf8(fd.cFileName));
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::unordered_map<std::string, std::string> byLower;  // lower -> name as on disk
    for (const auto& n : inData) byLower[Lower(n)] = n;

    std::vector<std::string> order;
    std::unordered_map<std::string, bool> seen;
    auto add = [&](const std::string& name) {
        auto it = byLower.find(Lower(name));
        if (it == byLower.end() || seen[it->first]) return;
        seen[it->first] = true;
        order.push_back(it->second);
    };
    // The game loads these first, without being listed: the base masters,
    // then Creation Club content in Skyrim.ccc order.
    for (const char* n : {"Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"}) add(n);
    auto readLines = [](const std::wstring& path, std::vector<std::string>* lines) {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return false;
        char buf[1024];
        while (fgets(buf, sizeof(buf), fp)) {
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (s.size() >= 3 && static_cast<uint8_t>(s[0]) == 0xEF) s.erase(0, 3);  // BOM
            lines->push_back(s);
        }
        fclose(fp);
        return true;
    };
    std::vector<std::string> ccc;
    readLines(data + L"..\\Skyrim.ccc", &ccc);
    for (const auto& n : ccc) add(n);

    if (!opt.loadOrderFile.empty()) {
        std::vector<std::string> lines;
        if (!readLines(opt.loadOrderFile, &lines)) {
            *error = "cannot read " + Utf8(opt.loadOrderFile);
            return {};
        }
        for (const auto& l : lines)
            if (!l.empty() && l[0] == '*') add(l.substr(1));  // '*' = active
    } else {
        std::vector<std::string> rest = inData;
        std::sort(rest.begin(), rest.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
        for (const auto& n : rest) add(n);
    }
    return order;
}

// Forms the game engine defines itself (no plugin has them), owned by
// Skyrim.esm: references to them, PlayerRef above all, would dangle without
// rows. From an xEdit dump of SkyrimSE.exe (plugin_sql_bundle.zip,
// dml/skyrimse_exe_records.sql).
struct EngineForm {
    uint32_t objectId;
    const char* type;
    const char* edid;
};
const EngineForm kEngineForms[] = {
    {0x000014, "PLYR", "PlayerRef"},
    {0x00001A, "EYES", "eyeReanimate"},
    {0x00001C, "BPTD", "PlayerBodyPartData"},
    {0x000028, "TXST", "NullTextureSet"},
    {0x000063, "GLOB", "PlayCredits"},
    {0x000164, "IMAD", "ImageSpaceConcussion"},
    {0x000166, "IMAD", "ExplosionInFace"},
    {0x000276, "IPDS", "DefaultImpactDataSet"},
    {0x0003F5, "AVIF", "AVVoicePoints"},
    {0x0005E0, "AVIF", "AVWaterBreathing"},
    {0x0005E1, "AVIF", "AVWaterWalking"},
    {0x0005E6, "AVIF", "AVWardPower"},
    {0x0005EA, "AVIF", "AVWardDeflection"},
    {0x0005EE, "AVIF", "AVVariable04"},
    {0x0005EF, "AVIF", "AVVariable05"},
    {0x0005FC, "AVIF", "AVWeaponSpeedMult"},
    {0x00060B, "AVIF", "AVSmithingMod"},
    {0x00062F, "AVIF", "AVTwoHandedPowerMod"},
    {0x00063C, "AVIF", "AVDestructionPowerMod"},
    {0x000644, "AVIF", "AVStaminaRateMult"},
    {0x000647, "AVIF", "AVGrabActorOffset"},
    {0x000648, "AVIF", "AVGrabbed"},
    {0x000649, "AVIF", "AVDEPRECATED05"},
};
constexpr char kEngineName[] = "SkyrimSE.exe";

// ---- Rows flowing from the decode workers to the writer ----

struct FormRow {
    int64_t key;
    uint32_t storedId, flags;
    uint16_t formVersion;
    char type[4];
    int64_t parent;  // -1 = none
    std::string edid;
};

struct StringRow {
    int kind;
    uint32_t id;
    std::string text;
};

// One SQL value.
struct Cell {
    enum class T : uint8_t { Null, Int, Real, Text } t = T::Null;
    int64_t i = 0;
    double d = 0;
    std::string s;
    static Cell Int(int64_t v) {
        Cell c;
        c.t = T::Int;
        c.i = v;
        return c;
    }
    static Cell Real(double v) {
        Cell c;
        c.t = T::Real;
        c.d = v;
        return c;
    }
    static Cell Text(std::string v) {
        Cell c;
        c.t = T::Text;
        c.s = std::move(v);
        return c;
    }
};

// Table ids besides the schema's (which are >= 0).
constexpr int kOverflowTable = -1, kRawTable = -2, kRefsTable = -3;

struct TableRow {
    int table;
    std::vector<Cell> cells;  // every column, keys first
};

struct Batch {
    int pluginNo = 0;
    std::vector<FormRow> forms;
    std::vector<StringRow> strings;
    std::vector<TableRow> rows;
    bool last = false;  // the plugin is done: facts below are set
    uint32_t crc = 0;
    int64_t formCount = 0;
    std::string error;
};

class BatchQueue {
public:
    void Push(Batch b) {
        std::unique_lock<std::mutex> lk(m_);
        notFull_.wait(lk, [this] { return q_.size() < 32; });
        q_.push_back(std::move(b));
        notEmpty_.notify_one();
    }
    bool Pop(Batch* b) {
        std::unique_lock<std::mutex> lk(m_);
        notEmpty_.wait(lk, [this] { return !q_.empty() || producers_ == 0; });
        if (q_.empty()) return false;
        *b = std::move(q_.front());
        q_.pop_front();
        notFull_.notify_one();
        return true;
    }
    void SetProducers(int n) { producers_ = n; }
    void ProducerDone() {
        std::lock_guard<std::mutex> lk(m_);
        --producers_;
        notEmpty_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable notEmpty_, notFull_;
    std::deque<Batch> q_;
    int producers_ = 0;
};

// A decoded value as the cell of a column of that kind.
Cell ToCell(const Value& v, const ColumnDef& col) {
    switch (v.kind()) {
        case Value::Kind::Null: return Cell();
        case Value::Kind::Bool: return Cell::Int(v.AsBool());
        case Value::Kind::Int: return Cell::Int(v.AsInt());
        case Value::Kind::Float: return std::isfinite(v.AsFloat()) ? Cell::Real(v.AsFloat()) : Cell();
        case Value::Kind::String: return Cell::Text(v.AsString());
        default: break;
    }
    (void)col;
    std::string json;  // a list or object: JSON text
    WriteJson(v, &json);
    return Cell::Text(std::move(json));
}

const Value* Walk(const Value* v, const std::vector<PathStep>& steps) {
    for (const auto& s : steps) {
        if (s.index) {
            if (v->kind() != Value::Kind::List || s.pos >= v->Items().size()) return nullptr;
            v = &v->Items()[s.pos];
        } else {
            if (v->kind() != Value::Kind::Map) return nullptr;
            v = v->Find(s.key);
            if (!v) return nullptr;
        }
    }
    return v;
}

// Fills `cells[first..]` with the columns of one subrecord value.
void FillColumns(const Value& value, const std::vector<ColumnDef>& cols, size_t firstCol, size_t count,
                 std::vector<Cell>* cells, size_t firstCell) {
    for (size_t k = 0; k < count; ++k) {
        const ColumnDef& c = cols[firstCol + k];
        if (c.kind == "marker") {
            (*cells)[firstCell + k] = Cell::Int(1);
            continue;
        }
        if (const Value* v = Walk(&value, c.steps)) (*cells)[firstCell + k] = ToCell(*v, c);
    }
}

// One plugin: every record -> a form row and its type / child table rows;
// its strings tables -> string rows.
// `modules`: the optional modules to decode too. `core` false = only those
// modules (spf db add): no forms rows, type table rows, header or strings.
void DecodePlugin(const PluginInfo& info, const std::vector<PluginInfo>& all,
                  const std::unordered_map<std::string, int>& noByName, const std::string& language,
                  const PatternCatalog& catalog, const Schema& schema, const std::set<std::string>& modules,
                  bool core, BatchQueue* queue) {
    Batch batch;
    batch.pluginNo = info.no;
    std::string err;
    auto plugin = Plugin::Open(info.path, catalog, &err, language);
    if (!plugin) {
        batch.last = true;
        batch.error = err;
        queue->Push(std::move(batch));
        return;
    }
    // Stored FormID -> key: the high byte picks a master (or this plugin).
    std::vector<int64_t> ownerBase;
    std::vector<uint32_t> objMask;
    for (const auto& m : plugin->masters()) {
        int no = noByName.at(Lower(m));
        ownerBase.push_back(no * kKeyShift);
        objMask.push_back(all[no].light ? 0xFFF : 0xFFFFFF);
    }
    ownerBase.push_back(info.no * kKeyShift);
    objMask.push_back(info.light ? 0xFFF : 0xFFFFFF);
    const size_t self = ownerBase.size() - 1;
    auto keyOf = [&](uint32_t stored) {
        size_t i = std::min<size_t>(stored >> 24, self);
        return ownerBase[i] + (stored & objMask[i]);
    };

    std::string rt;      // the record being decoded
    int64_t curKey = 0;  // and its form_key
    bool refsOn = true;  // off for the TES4 header
    // Every FormID becomes a key, and a form_refs row (the xref).
    const Plugin::FormIdMap formIds = [&](uint32_t stored, const std::string& path) {
        const int64_t ref = keyOf(stored);
        if (refsOn) {
            batch.rows.push_back(
                {kRefsTable, {Cell::Int(curKey), Cell::Int(info.no), Cell::Int(ref), Cell::Text(rt), Cell::Text(path)}});
        }
        return Value::Int(ref);
    };
    const Plugin::SigFilter wanted = [&](const std::string& fs) {
        const SubrecordDef* d = schema.Find(rt, fs);
        if (!d || d->module.empty()) return core;
        return modules.count(d->module) != 0;
    };
    auto flush = [&] {
        if (batch.forms.size() + batch.rows.size() < kBatchRows) return;
        Batch next;
        next.pluginNo = info.no;
        std::swap(next, batch);
        queue->Push(std::move(next));
    };

    std::vector<uint8_t> scratch;
    std::vector<const SubrecordDef*> seen;
    std::unordered_map<int, int64_t> childIdx;
    const auto& tables = schema.tables();
    const auto& recs = plugin->records();

    // A record's fields -> its type table row, child rows, and rows for what
    // does not fit (overflow, raw).
    auto decodeFields = [&](const Record& r, int64_t key) {
        rt = r.type;
        curKey = key;
        std::vector<Field> fields = plugin->Fields(r, &formIds, &wanted);
        int tt = core ? schema.TypeTable(rt) : -1;
        TableRow main;
        if (tt >= 0) {
            main.table = tt;
            main.cells.resize(2 + tables[tt].columns.size());
            main.cells[0] = Cell::Int(key);
            main.cells[1] = Cell::Int(info.no);
        }
        seen.clear();
        childIdx.clear();
        for (size_t k = 0; k < fields.size(); ++k) {
            const Field& f = fields[k];
            const SubrecordDef* def = schema.Find(rt, f.sig);
            if (!def || !f.decoded) {
                batch.rows.push_back({kRawTable,
                                      {Cell::Int(key), Cell::Int(info.no), Cell::Text(rt), Cell::Text(f.sig),
                                       Cell::Int(static_cast<int64_t>(k)), Cell::Text(f.value.AsString().substr(1)),
                                       Cell::Text(def ? "did not decode" : "no pattern")}});
                continue;
            }
            const TableDef& t = tables[def->table];
            if (def->child) {
                const bool list = def->elementRows && f.value.kind() == Value::Kind::List;
                const size_t n = list ? f.value.Items().size() : 1;
                for (size_t e = 0; e < n; ++e) {
                    TableRow child;
                    child.table = def->table;
                    child.cells.resize(3 + t.columns.size());
                    child.cells[0] = Cell::Int(key);
                    child.cells[1] = Cell::Int(info.no);
                    child.cells[2] = Cell::Int(childIdx[def->table]++);
                    FillColumns(list ? f.value.Items()[e] : f.value, t.columns, 0, t.columns.size(), &child.cells, 3);
                    batch.rows.push_back(std::move(child));
                }
            } else if (std::find(seen.begin(), seen.end(), def) != seen.end()) {
                std::string json;
                WriteJson(f.value, &json);
                batch.rows.push_back({kOverflowTable,
                                      {Cell::Int(key), Cell::Int(info.no), Cell::Text(rt), Cell::Text(f.sig),
                                       Cell::Text(std::move(json))}});
            } else {
                seen.push_back(def);
                FillColumns(f.value, t.columns, def->firstColumn, def->columnCount, &main.cells, 2 + def->firstColumn);
            }
        }
        if (tt >= 0) batch.rows.push_back(std::move(main));
    };

    // The TES4 header: one tes4 row per plugin (form_key 0), with its masters
    // and overridden-forms list (ONAM) in tes4_mast / tes4_onam. It is not a
    // form, so no forms row and no xref rows.
    if (core) {
        refsOn = false;
        decodeFields(recs.front(), 0);
        refsOn = true;
    }

    for (size_t i = 1; i < recs.size(); ++i) {
        const Record& r = recs[i];
        if (!core) {  // module subrecords only
            if (r.flags & 0x20) continue;  // deleted: no fields
            decodeFields(r, keyOf(r.formId));
            flush();
            continue;
        }
        FormRow row;
        row.key = keyOf(r.formId);
        row.storedId = r.formId;
        row.flags = r.flags;
        row.formVersion = r.formVersion;
        memcpy(row.type, r.type.data(), 4);
        row.parent = r.parent ? keyOf(r.parent) : -1;
        const uint8_t* body;
        size_t size;
        if (plugin->Body(r, &scratch, &body, &size)) {
            // EDID, when present, is the first subrecord.
            if (size >= 6 && memcmp(body, "EDID", 4) == 0) {
                size_t n = std::min<size_t>(U16(body + 4), size - 6);
                row.edid = Utf8Lossy(body + 6, strnlen(reinterpret_cast<const char*>(body + 6), n));
            }
        }
        const int64_t key = row.key;
        batch.forms.push_back(std::move(row));
        decodeFields(r, key);
        flush();
    }
    for (int c = 0; core && c < 3; ++c) {
        for (const auto& s : plugin->strings().table(c)) {
            batch.strings.push_back({c, s.first, s.second});
            if (batch.strings.size() >= kBatchRows) {
                Batch next;
                next.pluginNo = info.no;
                std::swap(next, batch);
                queue->Push(std::move(next));
            }
        }
    }
    batch.last = true;
    batch.formCount = static_cast<int64_t>(recs.size()) - 1;
    batch.crc = Crc32(plugin->bytes(), plugin->size());
    queue->Push(std::move(batch));
}

// ---- SQLite ----

class Db {
public:
    ~Db() {
        if (db_) sqlite3_close(db_);
    }
    bool Open(const std::wstring& path, std::string* error) {
        if (sqlite3_open16(path.c_str(), &db_) != SQLITE_OK) {
            *error = db_ ? sqlite3_errmsg(db_) : "cannot create the database";
            return false;
        }
        return true;
    }
    bool Exec(const std::string& sql, std::string* error) {
        char* msg = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
            *error = std::string(msg ? msg : "sqlite error") + " in: " + sql.substr(0, 200);
            sqlite3_free(msg);
            return false;
        }
        return true;
    }
    sqlite3_stmt* Prepare(const char* sql, std::string* error) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) *error = sqlite3_errmsg(db_);
        return st;
    }
    const char* Message() const { return sqlite3_errmsg(db_); }
    sqlite3* raw() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

struct Stmt {
    sqlite3_stmt* st = nullptr;
    ~Stmt() {
        if (st) sqlite3_finalize(st);
    }
    Stmt& Int(int i, int64_t v) {
        sqlite3_bind_int64(st, i, v);
        return *this;
    }
    Stmt& Real(int i, double v) {
        sqlite3_bind_double(st, i, v);
        return *this;
    }
    Stmt& Text(int i, const std::string& v) {
        sqlite3_bind_text(st, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& Null(int i) {
        sqlite3_bind_null(st, i);
        return *this;
    }
    bool Run() {
        int rc = sqlite3_step(st);
        sqlite3_reset(st);
        return rc == SQLITE_DONE;
    }
};

const char kSchema[] = R"SQL(
CREATE TABLE spf_meta(
  key   TEXT PRIMARY KEY,
  value TEXT
) WITHOUT ROWID;

CREATE TABLE spf_modules(
  name     TEXT PRIMARY KEY,
  built_at TEXT NOT NULL,
  settings TEXT
) WITHOUT ROWID;

-- Every plugin in the load order, plus masters they need that are not in it
-- (load_order NULL). plugin_no is the plugin's stable number in this DB.
CREATE TABLE plugins(
  plugin_no      INTEGER PRIMARY KEY,
  name           TEXT NOT NULL UNIQUE COLLATE NOCASE,
  load_order     INTEGER,
  mod_index      INTEGER,
  is_light       INTEGER NOT NULL,
  is_master      INTEGER NOT NULL,
  is_localized   INTEGER NOT NULL,
  header_flags   INTEGER,
  hedr_version   REAL,
  num_records    INTEGER,
  next_object_id INTEGER,
  author         TEXT,
  description    TEXT,
  file_exists    INTEGER NOT NULL,
  path           TEXT,
  size           INTEGER,
  mtime          INTEGER,
  crc32          INTEGER,
  form_count     INTEGER,
  load_error     TEXT
);

CREATE TABLE plugin_masters(
  plugin_no INTEGER NOT NULL REFERENCES plugins,
  idx       INTEGER NOT NULL,
  master_no INTEGER NOT NULL REFERENCES plugins,
  PRIMARY KEY(plugin_no, idx)
) WITHOUT ROWID;

-- One row per occurrence of a form: the plugin that owns it and every plugin
-- that overrides it. form_key = owner plugin_no * 16777216 + object ID.
CREATE TABLE forms(
  form_key      INTEGER NOT NULL,
  plugin_no     INTEGER NOT NULL REFERENCES plugins,
  type          TEXT NOT NULL,
  edid          TEXT COLLATE NOCASE,
  flags         INTEGER NOT NULL,
  form_version  INTEGER,
  parent_key    INTEGER,
  stored_formid INTEGER NOT NULL,
  is_winner     INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE strings(
  plugin_no INTEGER NOT NULL REFERENCES plugins,
  kind      TEXT NOT NULL,
  id        INTEGER NOT NULL,
  text      TEXT NOT NULL,
  PRIMARY KEY(plugin_no, kind, id)
) WITHOUT ROWID;

-- A subrecord the catalog says occurs once per record, occurring again: the
-- extra occurrences (the first is in the type table), as JSON.
CREATE TABLE subrecord_overflow(
  form_key  INTEGER NOT NULL,
  plugin_no INTEGER NOT NULL,
  type      TEXT NOT NULL,
  sig       TEXT NOT NULL,
  value     TEXT
);

-- Subrecords with no pattern, or whose bytes did not fit it: the raw bytes.
CREATE TABLE subrecord_raw(
  form_key  INTEGER NOT NULL,
  plugin_no INTEGER NOT NULL,
  type      TEXT NOT NULL,
  sig       TEXT NOT NULL,
  idx       INTEGER NOT NULL,
  bytes_hex TEXT NOT NULL,
  reason    TEXT NOT NULL
);

-- The xref: one row per FormID inside a record, "this occurrence of
-- form_key references ref_key", wherever the FormID sits (columns, child
-- tables, JSON). path_id says which field (ref_paths).
CREATE TABLE ref_paths(
  path_id INTEGER PRIMARY KEY,
  type    TEXT NOT NULL,
  path    TEXT NOT NULL,
  UNIQUE(type, path)
);

CREATE TABLE form_refs(
  form_key  INTEGER NOT NULL,
  plugin_no INTEGER NOT NULL REFERENCES plugins,
  ref_key   INTEGER NOT NULL,
  path_id   INTEGER NOT NULL REFERENCES ref_paths
);

-- The data dictionary: every table and column of this database.
CREATE TABLE st_tables(
  table_name  TEXT PRIMARY KEY,
  kind        TEXT NOT NULL,
  record_type TEXT,
  subrecord   TEXT,
  module      TEXT,
  description TEXT
) WITHOUT ROWID;

CREATE TABLE st_columns(
  table_name  TEXT NOT NULL REFERENCES st_tables,
  column_name TEXT NOT NULL,
  position    INTEGER NOT NULL,
  sql_type    TEXT,
  kind        TEXT,
  subrecord   TEXT,
  path        TEXT,
  refers_to   TEXT,
  description TEXT,
  PRIMARY KEY(table_name, column_name)
) WITHOUT ROWID;
)SQL";

const char* kCoreTables[][2] = {
    {"plugins", "Every plugin in the load order, plus masters it lacks (load_order NULL) and SkyrimSE.exe (load_order -1, the engine's own forms). load_error: why a plugin could not be read (it then has no rows)."},
    {"plugin_masters", "Each plugin's masters, in order."},
    {"forms", "One row per occurrence of a form: the plugin that owns it and every plugin that overrides it."},
    {"strings", "Every localized string of every plugin."},
    {"subrecord_overflow", "Extra occurrences of subrecords the catalog says occur once per record."},
    {"subrecord_raw", "Subrecords with no pattern, or whose bytes did not fit it."},
    {"form_refs", "The xref: one row per FormID inside a record (this occurrence of form_key references ref_key). Index on ref_key: what references a form."},
    {"ref_paths", "The field a form_refs row comes from: record type and path in the subrecord (\"SNAM.Faction\", \"KWDA\")."},
    {"spf_meta", "Build settings and versions."},
    {"spf_modules", "What is built: core and the optional modules."},
    {"st_tables", "The data dictionary: tables."},
    {"st_columns", "The data dictionary: columns."},
};

std::string Quote(const std::string& name) { return "\"" + name + "\""; }

// Whether a name works unquoted as a column (keywords like "order" do not).
bool UsableName(const std::string& name) {
    static sqlite3* mem = [] {
        sqlite3* d = nullptr;
        sqlite3_open(":memory:", &d);
        return d;
    }();
    std::string sql = "SELECT " + name + " FROM (SELECT 1 AS " + Quote(name) + ")";
    sqlite3_stmt* st = nullptr;
    bool ok = sqlite3_prepare_v2(mem, sql.c_str(), -1, &st, nullptr) == SQLITE_OK;
    sqlite3_finalize(st);
    return ok;
}

std::string TableDdl(const TableDef& t) {
    std::string sql = "CREATE TABLE " + Quote(t.name) + "(\n  form_key INTEGER NOT NULL,\n  plugin_no INTEGER NOT NULL";
    if (!t.fs.empty()) sql += ",\n  idx INTEGER NOT NULL";
    for (const auto& c : t.columns) sql += ",\n  " + Quote(c.name) + (c.sqlType.empty() ? "" : " " + c.sqlType);
    return sql + "\n);\n";
}

std::string InsertSql(const std::string& table, size_t columns) {
    std::string sql = "INSERT INTO " + Quote(table) + " VALUES(";
    for (size_t i = 0; i < columns; ++i) sql += i ? ",?" : "?";
    return sql + ")";
}

bool BindRun(sqlite3_stmt* st, const std::vector<Cell>& cells) {
    for (size_t i = 0; i < cells.size(); ++i) {
        const Cell& c = cells[i];
        int k = static_cast<int>(i) + 1;
        switch (c.t) {
            case Cell::T::Null: sqlite3_bind_null(st, k); break;
            case Cell::T::Int: sqlite3_bind_int64(st, k, c.i); break;
            case Cell::T::Real: sqlite3_bind_double(st, k, c.d); break;
            case Cell::T::Text: sqlite3_bind_text(st, k, c.s.data(), static_cast<int>(c.s.size()), SQLITE_STATIC); break;
        }
    }
    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    return rc == SQLITE_DONE;
}

const char kIndexes[] = R"SQL(
CREATE INDEX forms_key    ON forms(form_key, plugin_no);
CREATE INDEX forms_plugin ON forms(plugin_no, type);
CREATE INDEX forms_type   ON forms(type);
CREATE INDEX forms_edid   ON forms(edid);
CREATE INDEX forms_parent ON forms(parent_key) WHERE parent_key IS NOT NULL;
CREATE INDEX forms_deleted    ON forms(plugin_no) WHERE flags & 0x20;
CREATE INDEX forms_persistent ON forms(plugin_no) WHERE flags & 0x400;
CREATE INDEX plugin_masters_master ON plugin_masters(master_no);
)SQL";

// The winner of each form: its occurrence in the plugin loaded last.
const char kWinners[] = R"SQL(
UPDATE forms SET is_winner = 1 WHERE rowid IN (
  SELECT rid FROM (
    SELECT f.rowid AS rid,
           ROW_NUMBER() OVER (PARTITION BY f.form_key ORDER BY p.load_order DESC, f.rowid DESC) AS rn
    FROM forms f JOIN plugins p USING (plugin_no)
    WHERE p.load_order IS NOT NULL)
  WHERE rn = 1);
)SQL";

const char kViews[] = R"SQL(
-- Readable keys: "Skyrim.esm:012EB7" and the in-game (load-order) FormID.
CREATE VIEW forms_v AS
SELECT f.form_key,
       o.name || ':' || printf('%06X', f.form_key & 16777215) AS form,
       printf('%08X', CASE WHEN o.is_light
                           THEN 4261412864 + (o.mod_index << 12) + (f.form_key & 4095)
                           ELSE (o.mod_index << 24) + (f.form_key & 16777215) END) AS load_order_formid,
       r.name AS recorded_in,
       f.type, f.edid, f.flags,
       f.flags & 32 <> 0 AS is_deleted,
       f.plugin_no <> o.plugin_no AND r.load_order >= 0 AS is_override,
       f.is_winner, f.parent_key, f.form_version, f.plugin_no,
       o.plugin_no AS owner_no
FROM forms f
JOIN plugins o ON o.plugin_no = f.form_key >> 24
JOIN plugins r ON r.plugin_no = f.plugin_no;

CREATE VIEW winning_forms AS SELECT * FROM forms_v WHERE is_winner;

-- The xref, readable: who references what, from which field.
CREATE VIEW form_refs_v AS
SELECT s.name || ':' || printf('%06X', r.form_key & 16777215) AS form,
       c.name AS recorded_in,
       p.type, p.path,
       t.name || ':' || printf('%06X', r.ref_key & 16777215) AS ref,
       r.form_key, r.plugin_no, r.ref_key
FROM form_refs r
JOIN ref_paths p USING (path_id)
JOIN plugins s ON s.plugin_no = r.form_key >> 24
JOIN plugins t ON t.plugin_no = r.ref_key >> 24
JOIN plugins c ON c.plugin_no = r.plugin_no;

-- References made by the winning copy of each form: the load order's result.
CREATE VIEW winning_refs AS
SELECT v.* FROM form_refs_v v
JOIN forms f ON f.form_key = v.form_key AND f.plugin_no = v.plugin_no
WHERE f.is_winner;

-- References to forms that exist in no plugin of this database.
CREATE VIEW dangling_refs AS
SELECT v.* FROM form_refs_v v
WHERE NOT EXISTS (SELECT 1 FROM forms f WHERE f.form_key = v.ref_key);
)SQL";

// Inserts the table rows decode workers produce: type / child / module
// tables, overflow, raw, and xref rows (interning their paths). Shared by
// build-db and db add.
struct RowSink {
    std::vector<Stmt> insTable;
    Stmt insOverflow, insRaw, insRef, insPath;
    std::unordered_map<std::string, int64_t> pathIds;  // "TYPE PATH" -> path_id
    int64_t nextPathId = 1;
    int64_t rows = 0, refs = 0;

    bool Prepare(Db& db, const Schema& schema, std::string* error) {
        insTable.resize(schema.tables().size());
        for (size_t t = 0; t < schema.tables().size(); ++t) {
            const TableDef& td = schema.tables()[t];
            insTable[t].st = db.Prepare(InsertSql(td.name, (td.fs.empty() ? 2 : 3) + td.columns.size()).c_str(), error);
            if (!insTable[t].st) return false;
        }
        insOverflow.st = db.Prepare("INSERT INTO subrecord_overflow VALUES(?,?,?,?,?)", error);
        insRaw.st = db.Prepare("INSERT INTO subrecord_raw VALUES(?,?,?,?,?,?,?)", error);
        insRef.st = db.Prepare("INSERT INTO form_refs VALUES(?,?,?,?)", error);
        insPath.st = db.Prepare("INSERT INTO ref_paths VALUES(?,?,?)", error);
        return insOverflow.st && insRaw.st && insRef.st && insPath.st;
    }

    // Continues the path ids of an existing database (db add).
    bool LoadPaths(Db& db, std::string* error) {
        Stmt q{db.Prepare("SELECT path_id, type, path FROM ref_paths", error)};
        if (!q.st) return false;
        while (sqlite3_step(q.st) == SQLITE_ROW) {
            std::string key = reinterpret_cast<const char*>(sqlite3_column_text(q.st, 1));
            key += ' ';
            key += reinterpret_cast<const char*>(sqlite3_column_text(q.st, 2));
            pathIds[key] = sqlite3_column_int64(q.st, 0);
            nextPathId = std::max(nextPathId, sqlite3_column_int64(q.st, 0) + 1);
        }
        return true;
    }

    int64_t PathId(const std::string& type, const std::string& path) {
        std::string key = type + ' ' + path;
        auto it = pathIds.find(key);
        if (it == pathIds.end()) {
            int64_t id = nextPathId++;
            it = pathIds.emplace(std::move(key), id).first;
            insPath.Int(1, id).Text(2, type).Text(3, path).Run();
        }
        return it->second;
    }

    void Write(Db& db, const std::vector<TableRow>& batchRows, std::string* firstError) {
        for (const auto& r : batchRows) {
            if (r.table == kRefsTable) {  // form_key, plugin_no, ref_key, type, path
                insRef.Int(1, r.cells[0].i).Int(2, r.cells[1].i).Int(3, r.cells[2].i)
                    .Int(4, PathId(r.cells[3].s, r.cells[4].s));
                if (!insRef.Run() && firstError->empty()) *firstError = db.Message();
                ++refs;
                continue;
            }
            sqlite3_stmt* st = r.table == kOverflowTable ? insOverflow.st
                             : r.table == kRawTable      ? insRaw.st
                                                         : insTable[r.table].st;
            if (!BindRun(st, r.cells) && firstError->empty()) *firstError = db.Message();
            ++rows;
        }
    }
};

bool LoadCatalog(const std::wstring& path, PatternCatalog* catalog, std::string* error) {
    FILE* fp = nullptr;
    std::string text;
    if (_wfopen_s(&fp, path.c_str(), L"rb") == 0 && fp) {
        char buf[65536];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), fp)) > 0;) text.append(buf, n);
        fclose(fp);
    }
    std::string perr;
    if (text.empty() || !catalog->Load(text, &perr)) {
        *error = "patterns: " + (perr.empty() ? "cannot read " + Utf8(path) : perr);
        return false;
    }
    return true;
}

std::string NowIso() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    return buf;
}

}  // namespace

int BuildDb(const BuildOptions& opt, std::string* error) {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    auto say = [&](const char* fmt, ...) {
        if (opt.quiet) return;
        double s = std::chrono::duration<double>(Clock::now() - t0).count();
        fprintf(stderr, "[%6.2fs] ", s);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    };
    const std::wstring data = WithSlash(opt.data);

    // The pattern catalog decodes the fields; the schema is generated from it.
    PatternCatalog catalog;
    if (!LoadCatalog(opt.patterns, &catalog, error)) return 2;
    Schema schema;
    schema.Build(catalog, UsableName);
    std::set<std::string> modules;  // optional modules built too (--with)
    for (const auto& m : opt.modules) {
        bool known = false;
        for (const auto& km : Schema::Modules()) known = known || km.first == m;
        if (!known) {
            *error = "unknown module '" + m + "' (spf build-db --help lists them)";
            return 2;
        }
        modules.insert(m);
    }

    // Phase 1: load order and headers.
    std::vector<std::string> order = ResolveLoadOrder(opt, data, error);
    if (order.empty()) {
        if (error->empty()) *error = "the load order is empty";
        return 1;
    }
    std::vector<PluginInfo> all;
    std::unordered_map<std::string, int> noByName;
    auto addPlugin = [&](const std::string& name, bool inLoadOrder) {
        PluginInfo p;
        p.no = static_cast<int>(all.size());
        p.name = name;
        p.inLoadOrder = inLoadOrder;
        p.path = data + Widen(name);
        ReadHeader(&p);
        noByName[Lower(name)] = p.no;
        all.push_back(std::move(p));
    };
    for (const auto& n : order) {
        if (noByName.count(Lower(n))) continue;
        addPlugin(n, true);
        // A plugin whose header does not read stays in the load order and is
        // reported (plugins.load_error) when its decode fails.
        if (!all.back().exists) say("warning: %s is listed but not in the Data folder", n.c_str());
    }
    // The game loads masters (ESM flag) before everything else.
    std::stable_partition(all.begin(), all.end(), [](const PluginInfo& p) { return p.master; });
    int full = 0, light = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        all[i].no = static_cast<int>(i);
        all[i].loadOrder = static_cast<int>(i);
        all[i].modIndex = all[i].light ? light++ : full++;
        noByName[Lower(all[i].name)] = all[i].no;
    }
    // Past the game's limits the DB is still useful; only in-game FormIDs
    // (forms_v.load_order_formid) stop meaning anything for the extra plugins.
    if (full > 254 || light > 4096)
        say("warning: more plugins than the game loads (%d full, max 254; %d light, max 4096)", full, light);
    const size_t loaded = all.size();
    for (size_t i = 0; i < loaded; ++i)  // masters the load order lacks still get a number
        for (const auto& m : all[i].masters)
            if (!noByName.count(Lower(m))) addPlugin(m, false);
    say("load order: %zu plugins (%d full, %d light), %zu missing masters", loaded, full, light, all.size() - loaded);
    // The engine's own forms: a pseudo plugin loaded before Skyrim.esm, so a
    // plugin that does define one of them wins.
    int engineNo = -1;
    auto skyrim = noByName.find("skyrim.esm");
    if (skyrim != noByName.end() && all[skyrim->second].inLoadOrder) {
        PluginInfo e;
        e.no = engineNo = static_cast<int>(all.size());
        e.name = kEngineName;
        e.inLoadOrder = true;
        e.loadOrder = -1;
        e.master = true;
        e.description = "forms defined by the game engine";
        noByName[Lower(e.name)] = e.no;
        all.push_back(std::move(e));
    }

    // The database, written beside the target and moved over it at the end.
    std::wstring tmp = opt.out + L".building";
    if (!DeleteFileW(tmp.c_str()) && GetLastError() == ERROR_SHARING_VIOLATION) {
        *error = Utf8(tmp) + " is in use: another build-db is writing this database";
        return 1;
    }
    {
        Db db;
        if (!db.Open(tmp, error)) return 1;
        if (!db.Exec("PRAGMA page_size=8192; PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;"
                     "PRAGMA locking_mode=EXCLUSIVE; PRAGMA temp_store=MEMORY; PRAGMA cache_size=-262144;",
                     error) ||
            !db.Exec(kSchema, error) || !db.Exec("BEGIN", error))
            return 1;
        for (const auto& td : schema.tables())
            if (!db.Exec(TableDdl(td), error)) return 1;
        RowSink sink;
        if (!sink.Prepare(db, schema, error)) return 1;

        Stmt insPlugin{db.Prepare("INSERT INTO plugins VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,NULL,NULL,NULL)", error)};
        Stmt insMaster{db.Prepare("INSERT INTO plugin_masters VALUES(?,?,?)", error)};
        Stmt insForm{db.Prepare("INSERT INTO forms VALUES(?,?,?,?,?,?,?,?,0)", error)};
        Stmt insString{db.Prepare("INSERT OR REPLACE INTO strings VALUES(?,?,?,?)", error)};
        Stmt setFacts{db.Prepare("UPDATE plugins SET crc32=?, form_count=? WHERE plugin_no=?", error)};
        Stmt setError{db.Prepare("UPDATE plugins SET load_error=? WHERE plugin_no=?", error)};
        if (!insPlugin.st || !insMaster.st || !insForm.st || !insString.st || !setFacts.st || !setError.st) return 1;
        for (const auto& p : all) {
            insPlugin.Int(1, p.no).Text(2, p.name);
            if (p.inLoadOrder) insPlugin.Int(3, p.loadOrder);
            else insPlugin.Null(3);
            if (p.modIndex >= 0) insPlugin.Int(4, p.modIndex);
            else insPlugin.Null(4);
            insPlugin.Int(5, p.light).Int(6, p.master).Int(7, p.localized);
            if (p.exists) {
                insPlugin.Int(8, p.flags).Real(9, p.hedrVersion).Int(10, p.numRecords).Int(11, p.nextObjectId)
                    .Text(12, p.author);
            } else {
                for (int c = 8; c <= 12; ++c) insPlugin.Null(c);
            }
            if (p.exists || !p.description.empty()) insPlugin.Text(13, p.description);
            else insPlugin.Null(13);
            insPlugin.Int(14, p.exists);
            if (p.path.empty()) insPlugin.Null(15);
            else insPlugin.Text(15, Utf8(p.path));
            if (p.exists) insPlugin.Int(16, p.size).Int(17, p.mtime);
            else insPlugin.Null(16).Null(17);
            if (!insPlugin.Run()) {
                *error = db.Message();
                return 1;
            }
            for (size_t k = 0; k < p.masters.size(); ++k)
                insMaster.Int(1, p.no).Int(2, static_cast<int64_t>(k)).Int(3, noByName.at(Lower(p.masters[k]))).Run();
        }

        if (engineNo >= 0) {
            const int64_t base = noByName.at("skyrim.esm") * kKeyShift;
            for (const auto& e : kEngineForms) {
                insForm.Int(1, base + e.objectId).Int(2, engineNo).Text(3, e.type).Text(4, e.edid).Int(5, 0)
                    .Null(6).Null(7).Int(8, e.objectId);
                if (!insForm.Run()) {
                    *error = db.Message();
                    return 1;
                }
            }
            setFacts.Null(1).Int(2, static_cast<int64_t>(std::size(kEngineForms))).Int(3, engineNo).Run();
            // Their type table rows: the EDID, and PlayerRef's link to the
            // Player NPC_ (Skyrim.esm 000007), also as an xref row.
            for (const auto& e : kEngineForms) {
                int tt = schema.TypeTable(e.type);
                if (tt < 0) continue;
                const TableDef& t = schema.tables()[tt];
                std::vector<Cell> cells(2 + t.columns.size());
                cells[0] = Cell::Int(base + e.objectId);
                cells[1] = Cell::Int(engineNo);
                for (size_t c = 0; c < t.columns.size(); ++c) {
                    if (t.columns[c].name == "edid") cells[2 + c] = Cell::Text(e.edid);
                    if (t.columns[c].name == "plyr" && std::string(e.type) == "PLYR") cells[2 + c] = Cell::Int(base + 7);
                }
                if (!BindRun(sink.insTable[tt].st, cells)) {
                    *error = db.Message();
                    return 1;
                }
            }
            sink.insRef.Int(1, base + 0x14).Int(2, engineNo).Int(3, base + 7).Int(4, sink.PathId("PLYR", "PLYR")).Run();
            ++sink.refs;
        }

        // Phase 2: decode workers (biggest plugin first) -> this thread writes.
        std::vector<int> work;
        for (size_t i = 0; i < loaded; ++i) work.push_back(static_cast<int>(i));
        std::sort(work.begin(), work.end(), [&](int a, int b) { return all[a].size > all[b].size; });
        int threads = opt.threads > 0 ? opt.threads
                                      : std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
        threads = std::min<int>(threads, static_cast<int>(work.size()));
        BatchQueue queue;
        queue.SetProducers(threads);
        std::atomic<size_t> nextWork{0};
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t) {
            pool.emplace_back([&] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                for (size_t w; (w = nextWork++) < work.size();) {
                    // A plugin that cannot be read is reported and skipped;
                    // it never takes the build (or the process) down.
                    std::string failure;
                    try {
                        DecodePlugin(all[work[w]], all, noByName, opt.language, catalog, schema, modules, true,
                                     &queue);
                    } catch (const std::exception& e) {
                        failure = e.what();
                    } catch (...) {
                        failure = "unexpected error";
                    }
                    if (!failure.empty()) {
                        Batch fail;
                        fail.pluginNo = all[work[w]].no;
                        fail.last = true;
                        fail.error = "failed while decoding: " + failure;
                        queue.Push(std::move(fail));
                    }
                }
                queue.ProducerDone();
            });
        }
        say("decoding with %d threads", threads);
        static const char* kKinds[3] = {"strings", "dlstrings", "ilstrings"};
        int64_t forms = 0, strings = 0;
        size_t done = 0;
        std::vector<int> failed;  // plugin_no of plugins that could not be read
        std::string firstError;
        Batch b;
        while (queue.Pop(&b)) {
            for (const auto& f : b.forms) {
                insForm.Int(1, f.key).Int(2, b.pluginNo).Text(3, std::string(f.type, 4));
                if (f.edid.empty()) insForm.Null(4);
                else insForm.Text(4, f.edid);
                insForm.Int(5, f.flags).Int(6, f.formVersion);
                if (f.parent < 0) insForm.Null(7);
                else insForm.Int(7, f.parent);
                insForm.Int(8, f.storedId);
                if (!insForm.Run() && firstError.empty()) firstError = db.Message();
            }
            for (const auto& s : b.strings)
                insString.Int(1, b.pluginNo).Text(2, kKinds[s.kind]).Int(3, s.id).Text(4, s.text).Run();
            sink.Write(db, b.rows, &firstError);
            forms += static_cast<int64_t>(b.forms.size());
            strings += static_cast<int64_t>(b.strings.size());
            if (b.last) {
                ++done;
                if (!b.error.empty()) {
                    failed.push_back(b.pluginNo);
                    setError.Text(1, b.error).Int(2, b.pluginNo).Run();
                    say("warning: %s skipped: %s", all[b.pluginNo].name.c_str(), b.error.c_str());
                } else {
                    setFacts.Int(1, b.crc).Int(2, b.formCount).Int(3, b.pluginNo).Run();
                }
            }
        }
        for (auto& t : pool) t.join();
        if (!firstError.empty()) {  // the database itself failed
            *error = firstError;
            return 1;
        }
        // A plugin that failed part-way may have rows already: remove them, so
        // the database holds each plugin completely or not at all.
        for (int no : failed) {
            std::string del = "DELETE FROM forms WHERE plugin_no=" + std::to_string(no) + ";" +
                              "DELETE FROM strings WHERE plugin_no=" + std::to_string(no) + ";" +
                              "DELETE FROM form_refs WHERE plugin_no=" + std::to_string(no) + ";" +
                              "DELETE FROM subrecord_overflow WHERE plugin_no=" + std::to_string(no) + ";" +
                              "DELETE FROM subrecord_raw WHERE plugin_no=" + std::to_string(no) + ";";
            for (const auto& t : schema.tables())
                del += "DELETE FROM " + Quote(t.name) + " WHERE plugin_no=" + std::to_string(no) + ";";
            if (!db.Exec(del, error)) return 1;
        }
        say("wrote %lld forms, %lld field rows, %lld references, %lld strings from %zu plugins",
            static_cast<long long>(forms), static_cast<long long>(sink.rows), static_cast<long long>(sink.refs),
            static_cast<long long>(strings), done);

        std::string sql = kIndexes;
        sql += "CREATE INDEX subrecord_overflow_key ON subrecord_overflow(form_key, plugin_no);\n"
               "CREATE INDEX subrecord_raw_key ON subrecord_raw(form_key, plugin_no);\n"
               "CREATE INDEX form_refs_ref ON form_refs(ref_key);\n"
               "CREATE INDEX form_refs_key ON form_refs(form_key, plugin_no);\n";
        for (const auto& t : schema.tables())
            sql += "CREATE INDEX " + Quote(t.name + "_key") + " ON " + Quote(t.name) + "(form_key, plugin_no);\n";
        if (!db.Exec(sql, error)) return 1;
        say("indexes");
        if (!db.Exec(kWinners, error)) return 1;
        say("winners");
        sql = kViews;
        for (const auto& t : schema.tables()) {
            if (!t.fs.empty()) continue;
            sql += "CREATE VIEW " + Quote("winning_" + t.name) + " AS SELECT t.* FROM " + Quote(t.name) +
                   " t JOIN forms f ON f.form_key = t.form_key AND f.plugin_no = t.plugin_no WHERE f.is_winner;\n";
        }
        if (!db.Exec(sql, error)) return 1;

        // The data dictionary.
        Stmt insT{db.Prepare("INSERT INTO st_tables VALUES(?,?,?,?,?,?)", error)};
        Stmt insC{db.Prepare("INSERT INTO st_columns VALUES(?,?,?,?,?,?,?,?,?)", error)};
        if (!insT.st || !insC.st) return 1;
        for (const auto& ct : kCoreTables)
            insT.Text(1, ct[0]).Text(2, "core").Null(3).Null(4).Null(5).Text(6, ct[1]).Run();
        if (!db.Exec("INSERT INTO st_columns SELECT m.table_name, p.name, p.cid, p.type, NULL, NULL, NULL, NULL, NULL "
                     "FROM st_tables m, pragma_table_info(m.table_name) p",
                     error))
            return 1;
        for (const auto& t : schema.tables()) {
            insT.Text(1, t.name).Text(2, t.fs.empty() ? "type" : "child").Text(3, t.rt);
            if (t.fs.empty()) insT.Null(4);
            else insT.Text(4, t.fs);
            if (t.module.empty()) insT.Null(5);
            else insT.Text(5, t.module);
            insT.Text(6, t.description).Run();
            int pos = 0;
            auto key = [&](const char* name, const char* refers, const char* desc) {
                insC.Text(1, t.name).Text(2, name).Int(3, pos++).Text(4, "INTEGER").Text(5, "key").Null(6).Null(7);
                if (refers) insC.Text(8, refers);
                else insC.Null(8);
                insC.Text(9, desc).Run();
            };
            key("form_key", "forms.form_key", "the form (owner plugin_no * 16777216 + object ID)");
            key("plugin_no", "plugins.plugin_no", "the plugin this occurrence is recorded in");
            if (!t.fs.empty()) key("idx", nullptr, "position within the record");
            for (const auto& c : t.columns) {
                std::string fs = c.path.substr(0, c.path.find_first_of(".["));
                std::string display = catalog.Display(t.rt, fs);
                insC.Text(1, t.name).Text(2, c.name).Int(3, pos++);
                if (c.sqlType.empty()) insC.Null(4);
                else insC.Text(4, c.sqlType);
                insC.Text(5, c.kind).Text(6, fs).Text(7, c.path);
                if (c.kind == "formid") insC.Text(8, "forms.form_key");
                else insC.Null(8);
                if (display.empty()) insC.Null(9);
                else insC.Text(9, display);
                insC.Run();
            }
        }

        Stmt meta{db.Prepare("INSERT INTO spf_meta VALUES(?,?)", error)};
        auto putMeta = [&](const char* k, const std::string& v) { meta.Text(1, k).Text(2, v).Run(); };
        putMeta("schema_version", std::to_string(kSchemaVersion));
        putMeta("built_by", "spf build-db");
        putMeta("built_at", NowIso());
        putMeta("sqlite_version", sqlite3_libversion());
        putMeta("data_dir", Utf8(data));
        putMeta("load_order_source", !opt.plugins.empty()       ? "--plugins"
                                     : !opt.loadOrderFile.empty() ? Utf8(opt.loadOrderFile)
                                                                  : "every plugin in data_dir");
        putMeta("language", opt.language);
        putMeta("key_scheme", "form_key = owner plugin_no * 16777216 + object id (12-bit when the owner is light)");
        std::string skipped;
        for (const auto& m : Schema::Modules())
            if (!modules.count(m.first)) skipped += (skipped.empty() ? "" : ",") + m.first;
        putMeta("modules_not_built", skipped);
        putMeta("plugins_failed", std::to_string(failed.size()));
        Stmt mod{db.Prepare("INSERT INTO spf_modules VALUES(?, ?, NULL)", error)};
        mod.Text(1, "core").Text(2, NowIso()).Run();
        for (const auto& m : modules) mod.Text(1, m).Text(2, NowIso()).Run();
        if (!db.Exec("COMMIT; ANALYZE;", error)) return 1;
        say("committed");
    }
    if (!MoveFileExW(tmp.c_str(), opt.out.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *error = "cannot replace " + Utf8(opt.out) + " (open in another program?)";
        return 1;
    }
    say("done: %s", Utf8(opt.out).c_str());
    return 0;
}

std::vector<std::pair<std::string, std::string>> AvailableModules() { return Schema::Modules(); }

int AddModules(const AddOptions& opt, std::string* error) {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    auto say = [&](const char* fmt, ...) {
        if (opt.quiet) return;
        double s = std::chrono::duration<double>(Clock::now() - t0).count();
        fprintf(stderr, "[%6.2fs] ", s);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    };

    if (GetFileAttributesW(opt.db.c_str()) == INVALID_FILE_ATTRIBUTES) {
        *error = "no database at " + Utf8(opt.db);
        return 1;
    }
    Db db;
    if (!db.Open(opt.db, error)) return 1;
    auto metaValue = [&](const char* key) {
        std::string v;
        std::string e;
        Stmt q{db.Prepare("SELECT value FROM spf_meta WHERE key=?", &e)};
        if (!q.st) return v;
        q.Text(1, key);
        if (sqlite3_step(q.st) == SQLITE_ROW && sqlite3_column_text(q.st, 0))
            v = reinterpret_cast<const char*>(sqlite3_column_text(q.st, 0));
        return v;
    };
    std::string version = metaValue("schema_version");
    if (version != std::to_string(kSchemaVersion)) {
        *error = "the database has schema version " + (version.empty() ? std::string("?") : version) +
                 ", this spf writes " + std::to_string(kSchemaVersion) + ": rebuild it with build-db";
        return 1;
    }

    PatternCatalog catalog;
    if (!LoadCatalog(opt.patterns, &catalog, error)) return 2;
    Schema schema;
    schema.Build(catalog, UsableName);

    // The modules asked for: known, and not built yet.
    std::set<std::string> modules;
    for (const auto& m : opt.modules) {
        bool known = false;
        for (const auto& km : Schema::Modules()) known = known || km.first == m;
        if (!known) {
            *error = "unknown module '" + m + "' (spf db add --help lists them)";
            return 2;
        }
        Stmt q{db.Prepare("SELECT 1 FROM spf_modules WHERE name=?", error)};
        if (!q.st) return 1;
        q.Text(1, m);
        if (sqlite3_step(q.st) == SQLITE_ROW) {
            *error = "module " + m + " is already built";
            return 1;
        }
        modules.insert(m);
    }
    if (modules.empty()) {
        *error = "no module given";
        return 2;
    }

    // The plugins as the build recorded them; any that changed since refuse
    // the add (their core rows would no longer match the new module rows).
    std::vector<PluginInfo> all;
    std::unordered_map<std::string, int> noByName;
    std::vector<int> work;
    std::vector<std::string> changed;
    {
        Stmt q{db.Prepare("SELECT plugin_no, name, load_order, is_light, file_exists, path, size, mtime, load_error "
                          "FROM plugins ORDER BY plugin_no",
                          error)};
        if (!q.st) return 1;
        while (sqlite3_step(q.st) == SQLITE_ROW) {
            PluginInfo p;
            p.no = sqlite3_column_int(q.st, 0);
            p.name = reinterpret_cast<const char*>(sqlite3_column_text(q.st, 1));
            p.inLoadOrder = sqlite3_column_type(q.st, 2) != SQLITE_NULL && sqlite3_column_int(q.st, 2) >= 0;
            p.light = sqlite3_column_int(q.st, 3) != 0;
            p.exists = sqlite3_column_int(q.st, 4) != 0;
            if (sqlite3_column_text(q.st, 5)) p.path = Widen(reinterpret_cast<const char*>(sqlite3_column_text(q.st, 5)));
            p.size = sqlite3_column_int64(q.st, 6);
            p.mtime = sqlite3_column_int64(q.st, 7);
            bool failed = sqlite3_column_type(q.st, 8) != SQLITE_NULL;
            if (p.no != static_cast<int>(all.size())) {
                *error = "the plugins table is not numbered 0..n-1";
                return 1;
            }
            noByName[Lower(p.name)] = p.no;
            if (p.inLoadOrder && p.exists && !failed) {
                PluginInfo now = p;
                ReadHeader(&now);
                if (now.size != p.size || now.mtime != p.mtime) changed.push_back(p.name);
                work.push_back(p.no);
            }
            all.push_back(std::move(p));
        }
    }
    if (!changed.empty()) {
        *error = std::to_string(changed.size()) + " plugin(s) changed since the build (first: " + changed[0] +
                 "); rebuild the database";
        return 1;
    }
    std::sort(work.begin(), work.end(), [&](int a, int b) { return all[a].size > all[b].size; });
    const std::string language = metaValue("language").empty() ? "english" : metaValue("language");

    // One transaction: a failure leaves the database as it was.
    if (!db.Exec("PRAGMA cache_size=-262144; PRAGMA temp_store=MEMORY; BEGIN", error)) return 1;
    RowSink sink;
    if (!sink.Prepare(db, schema, error) || !sink.LoadPaths(db, error)) return 1;

    int threads = opt.threads > 0 ? opt.threads : std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
    threads = std::min<int>(threads, static_cast<int>(work.size()));
    BatchQueue queue;
    queue.SetProducers(threads);
    std::atomic<size_t> nextWork{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (size_t w; (w = nextWork++) < work.size();) {
                std::string failure;
                try {
                    DecodePlugin(all[work[w]], all, noByName, language, catalog, schema, modules, false, &queue);
                } catch (const std::exception& e) {
                    failure = e.what();
                } catch (...) {
                    failure = "unexpected error";
                }
                if (!failure.empty()) {
                    Batch fail;
                    fail.pluginNo = all[work[w]].no;
                    fail.last = true;
                    fail.error = "failed while decoding: " + failure;
                    queue.Push(std::move(fail));
                }
            }
            queue.ProducerDone();
        });
    }
    std::string list;
    for (const auto& m : modules) list += (list.empty() ? "" : ",") + m;
    say("adding %s from %zu plugins with %d threads", list.c_str(), work.size(), threads);
    std::string firstError;
    Batch b;
    while (queue.Pop(&b)) {
        sink.Write(db, b.rows, &firstError);
        if (b.last && !b.error.empty() && firstError.empty()) firstError = all[b.pluginNo].name + ": " + b.error;
    }
    for (auto& t : pool) t.join();
    if (!firstError.empty()) {
        db.Exec("ROLLBACK", error);
        *error = firstError + " (nothing was added)";
        return 1;
    }
    say("wrote %lld rows, %lld references", static_cast<long long>(sink.rows), static_cast<long long>(sink.refs));

    Stmt mod{db.Prepare("INSERT INTO spf_modules VALUES(?, ?, NULL)", error)};
    if (!mod.st) return 1;
    for (const auto& m : modules) mod.Text(1, m).Text(2, NowIso()).Run();
    std::string notBuilt;
    {
        Stmt q{db.Prepare("SELECT name FROM spf_modules", error)};
        if (!q.st) return 1;
        std::set<std::string> built;
        while (sqlite3_step(q.st) == SQLITE_ROW) built.insert(reinterpret_cast<const char*>(sqlite3_column_text(q.st, 0)));
        for (const auto& m : Schema::Modules())
            if (!built.count(m.first)) notBuilt += (notBuilt.empty() ? "" : ",") + m.first;
    }
    Stmt meta{db.Prepare("UPDATE spf_meta SET value=? WHERE key='modules_not_built'", error)};
    if (!meta.st) return 1;
    meta.Text(1, notBuilt).Run();
    if (!db.Exec("COMMIT; ANALYZE;", error)) return 1;
    say("done: %s", Utf8(opt.db).c_str());
    return 0;
}

}  // namespace spf
