#include "db_build.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "pattern_value.h"
#include "plugin.h"
#include "sqlite3.h"

namespace spf {
namespace {

constexpr int kSchemaVersion = 1;
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
    BY_HANDLE_FILE_INFORMATION fi{};
    GetFileInformationByHandle(h, &fi);
    p->size = (static_cast<int64_t>(fi.nFileSizeHigh) << 32) | fi.nFileSizeLow;
    int64_t ft = (static_cast<int64_t>(fi.ftLastWriteTime.dwHighDateTime) << 32) | fi.ftLastWriteTime.dwLowDateTime;
    p->mtime = ft / 10000000 - 11644473600LL;  // unix seconds
    uint8_t hdr[24];
    DWORD got = 0;
    bool ok = ReadFile(h, hdr, 24, &got, nullptr) && got == 24 && memcmp(hdr, "TES4", 4) == 0;
    std::vector<uint8_t> body;
    if (ok) {
        body.resize(U32(hdr + 4));
        ok = body.empty() || (ReadFile(h, body.data(), static_cast<DWORD>(body.size()), &got, nullptr) && got == body.size());
    }
    CloseHandle(h);
    if (!ok) return false;
    p->exists = true;
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

struct Batch {
    int pluginNo = 0;
    std::vector<FormRow> forms;
    std::vector<StringRow> strings;
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

// One plugin: every record -> a form row; its strings tables -> string rows.
void DecodePlugin(const PluginInfo& info, const std::vector<PluginInfo>& all,
                  const std::unordered_map<std::string, int>& noByName, const std::string& language,
                  BatchQueue* queue) {
    static const PatternCatalog kNoPatterns;
    Batch batch;
    batch.pluginNo = info.no;
    std::string err;
    auto plugin = Plugin::Open(info.path, kNoPatterns, &err, language);
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

    std::vector<uint8_t> scratch;
    const auto& recs = plugin->records();
    for (size_t i = 1; i < recs.size(); ++i) {
        const Record& r = recs[i];
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
        batch.forms.push_back(std::move(row));
        if (batch.forms.size() >= kBatchRows) {
            Batch next;
            next.pluginNo = info.no;
            std::swap(next, batch);
            queue->Push(std::move(next));
        }
    }
    for (int c = 0; c < 3; ++c) {
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
  form_count     INTEGER
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
)SQL";

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
       f.plugin_no <> o.plugin_no AS is_override,
       f.is_winner, f.parent_key, f.form_version, f.plugin_no,
       o.plugin_no AS owner_no
FROM forms f
JOIN plugins o ON o.plugin_no = f.form_key >> 24
JOIN plugins r ON r.plugin_no = f.plugin_no;

CREATE VIEW winning_forms AS SELECT * FROM forms_v WHERE is_winner;
)SQL";

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
        if (!all.back().exists) {
            *error = n + ": missing or not a plugin";
            return 1;
        }
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

    // The database, written beside the target and moved over it at the end.
    std::wstring tmp = opt.out + L".building";
    DeleteFileW(tmp.c_str());
    {
        Db db;
        if (!db.Open(tmp, error)) return 1;
        if (!db.Exec("PRAGMA page_size=8192; PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;"
                     "PRAGMA locking_mode=EXCLUSIVE; PRAGMA temp_store=MEMORY; PRAGMA cache_size=-262144;",
                     error) ||
            !db.Exec(kSchema, error) || !db.Exec("BEGIN", error))
            return 1;

        Stmt insPlugin{db.Prepare("INSERT INTO plugins VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,NULL,NULL)", error)};
        Stmt insMaster{db.Prepare("INSERT INTO plugin_masters VALUES(?,?,?)", error)};
        Stmt insForm{db.Prepare("INSERT INTO forms VALUES(?,?,?,?,?,?,?,?,0)", error)};
        Stmt insString{db.Prepare("INSERT OR REPLACE INTO strings VALUES(?,?,?,?)", error)};
        Stmt setFacts{db.Prepare("UPDATE plugins SET crc32=?, form_count=? WHERE plugin_no=?", error)};
        if (!insPlugin.st || !insMaster.st || !insForm.st || !insString.st || !setFacts.st) return 1;
        for (const auto& p : all) {
            insPlugin.Int(1, p.no).Text(2, p.name);
            if (p.inLoadOrder) insPlugin.Int(3, p.loadOrder).Int(4, p.modIndex);
            else insPlugin.Null(3).Null(4);
            insPlugin.Int(5, p.light).Int(6, p.master).Int(7, p.localized);
            if (p.exists) {
                insPlugin.Int(8, p.flags).Real(9, p.hedrVersion).Int(10, p.numRecords).Int(11, p.nextObjectId)
                    .Text(12, p.author).Text(13, p.description);
            } else {
                for (int c = 8; c <= 13; ++c) insPlugin.Null(c);
            }
            insPlugin.Int(14, p.exists).Text(15, Utf8(p.path));
            if (p.exists) insPlugin.Int(16, p.size).Int(17, p.mtime);
            else insPlugin.Null(16).Null(17);
            if (!insPlugin.Run()) {
                *error = db.Message();
                return 1;
            }
            for (size_t k = 0; k < p.masters.size(); ++k)
                insMaster.Int(1, p.no).Int(2, static_cast<int64_t>(k)).Int(3, noByName.at(Lower(p.masters[k]))).Run();
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
                for (size_t w; (w = nextWork++) < work.size();)
                    DecodePlugin(all[work[w]], all, noByName, opt.language, &queue);
                queue.ProducerDone();
            });
        }
        say("decoding with %d threads", threads);
        static const char* kKinds[3] = {"strings", "dlstrings", "ilstrings"};
        int64_t forms = 0, strings = 0;
        size_t done = 0;
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
            forms += static_cast<int64_t>(b.forms.size());
            strings += static_cast<int64_t>(b.strings.size());
            if (b.last) {
                ++done;
                if (!b.error.empty() && firstError.empty()) firstError = all[b.pluginNo].name + ": " + b.error;
                setFacts.Int(1, b.crc).Int(2, b.formCount).Int(3, b.pluginNo).Run();
            }
        }
        for (auto& t : pool) t.join();
        if (!firstError.empty()) {
            *error = firstError;
            return 1;
        }
        say("wrote %lld forms, %lld strings from %zu plugins", static_cast<long long>(forms),
            static_cast<long long>(strings), done);

        if (!db.Exec(kIndexes, error)) return 1;
        say("indexes");
        if (!db.Exec(kWinners, error)) return 1;
        say("winners");
        if (!db.Exec(kViews, error)) return 1;

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
        Stmt mod{db.Prepare("INSERT INTO spf_modules VALUES('core', ?, NULL)", error)};
        mod.Text(1, NowIso()).Run();
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

}  // namespace spf
