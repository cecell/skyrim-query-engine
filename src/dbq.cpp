// skqe dbq: see dbq.h.
#include "dbq.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <regex>
#include <set>
#include <sstream>

#include "data_files.h"
#include "pattern_value.h"
#include "sqlite3.h"

namespace skqe::dbq {
namespace {

// ---------------------------------------------------------------- strings

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
std::string Upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}
std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
    return s.substr(a, b - a);
}
bool StartsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool EndsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
std::vector<std::string> SplitWs(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (static_cast<unsigned char>(c) <= ' ') {
            if (!cur.empty()) out.push_back(cur), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
std::string Join(const std::vector<std::string>& v, const std::string& sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? sep : "") + v[i];
    return s;
}
std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}
std::string Grouped(int64_t n) {
    std::string d = std::to_string(n < 0 ? -n : n), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out += ',';
        out += d[i];
    }
    return (n < 0 ? "-" : "") + out;
}
bool IsHex(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}
int EditDistance(const std::string& a, const std::string& b) {  // Damerau (adjacent swaps)
    const size_t n = a.size(), m = b.size();
    std::vector<std::vector<int>> d(n + 1, std::vector<int>(m + 1));
    for (size_t i = 0; i <= n; ++i) d[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= m; ++j) d[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; ++i)
        for (size_t j = 1; j <= m; ++j) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
        }
    return d[n][m];
}
std::string Quote(const std::string& s) { return "\"" + s + "\""; }

// Normalized relative path as build-assets stores it.
std::string NormPath(std::string s) {
    s = Lower(Trim(s));
    for (char& c : s)
        if (c == '/') c = '\\';
    while (!s.empty() && (s.front() == '\\' || s.front() == '"')) s.erase(0, 1);
    while (!s.empty() && (s.back() == '"')) s.pop_back();
    if (StartsWith(s, "data\\")) s.erase(0, 5);
    return s;
}
std::string Ext(const std::string& s) {
    size_t dot = s.rfind('.');
    size_t slash = s.find_last_of("\\/");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return Lower(s.substr(dot + 1));
}
std::string BaseName(const std::string& s) {
    size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

const std::set<std::string> kFileExts = {"dds", "tga", "nif", "bsa", "esp", "esm", "esl", "hkx", "pex", "psc", "wav", "xwm",
                                          "fuz", "lip", "tri", "swf", "seq", "bik", "txt", "ini", "json", "xml", "png", "btr",
                                          "bto", "egm", "kf", "gid", "jpg", "dll", "obj", "lst", "csv"};
bool IsPluginExt(const std::string& e) { return e == "esp" || e == "esm" || e == "esl"; }

// ---------------------------------------------------------------- tokens

enum class Ent { None, File, FormId, LoFormId, Number, Folder, Quoted };

struct Tok {
    std::string raw;    // lower case, punctuation stripped
    std::string word;   // canonical form (words only)
    Ent ent = Ent::None;
    std::string text;   // entity text
    std::string ext;    // File: extension
    double num = 0;     // Number
    bool kSuffix = false;
    bool possessive = false;
    std::string orig;   // as typed (case kept)
};

struct PTok {  // pattern token
    std::string word;
    int slot = -1;  // index into family slots
};

struct OptionDef {
    std::string fragment, order, sql;
    std::vector<std::string> drop;
};
struct SlotDef {
    std::string name, kind;
    bool optional = false, multi = false;
};
struct Extra {
    std::string title, sql;
};
struct Test {
    std::string q;
    int64_t minRows = 0;
};
struct Family {
    std::string id, db, group, title, sql, order, count = "count(*)", unit, handler, note;
    std::string requiresSql, requiresMsg;
    bool detail = false;
    std::vector<SlotDef> slots;
    std::vector<std::pair<std::string, std::string>> defaults;
    std::map<std::string, OptionDef> options;
    std::vector<Extra> extra;
    std::vector<std::string> wordings;
    std::vector<std::vector<PTok>> patterns;  // compiled
    std::vector<std::vector<std::string>> implied;  // options each wording's own words imply
    std::vector<Test> tests;
    bool Supports(const std::string& opt) const { return opt == "count" || options.count(opt) != 0; }
};

struct Resolved {
    std::vector<std::pair<int64_t, std::string>> rows;  // (id or NULL=INT64_MIN, text)
    std::string summary;                                // "252 textures named x"
    bool ok = false;                                    // something to run with
    bool ambiguous = false;                             // ask
    std::vector<std::string> labels;                    // choices when ambiguous
    std::string why;                                    // when nothing matched
    double numberValue = 0;
    std::string param;                                  // bound :slot value
    std::vector<std::string> alternatives;              // picked one of these; the user can pick another
    int pickedIndex = -1;
};
constexpr int64_t kNullId = INT64_MIN;

struct Candidate {
    const Family* fam = nullptr;
    double score = 0;
    std::vector<std::string> options;
    std::vector<SlotValue> slots;
    std::vector<std::string> ignored;  // option phrases this family can't use
};

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string UriFor(const std::wstring& path) {
    std::string p = ToUtf8(path), out = "file:";
    for (char c : p) {
        unsigned char u = static_cast<unsigned char>(c);
        if (c == '\\') out += '/';
        else if (c == '%' || c == '#' || c == '?' || c == ' ' || c == '&' || c == '=' || u < 0x20) {
            char b[4];
            snprintf(b, sizeof b, "%%%02X", u);
            out += b;
        } else {
            out += c;
        }
    }
    return out + "?mode=ro&immutable=1";
}

}  // namespace

// ================================================================== Impl

struct Engine::Impl {
    // vocabulary
    std::map<std::string, std::string> canon;
    std::set<std::string> stop, vocab;
    std::map<std::string, std::string> types, formats, fileKinds;
    size_t maxTypeWords = 1;
    std::vector<std::pair<std::string, std::vector<std::vector<std::string>>>> options;  // id -> phrases
    struct Scope {
        std::string id, slotKind;
        std::set<std::string> words;
    };
    std::vector<Scope> scopes;
    std::vector<std::vector<std::string>> refineMarkers;
    std::vector<Family> families;
    std::set<std::string> tempTables;

    // databases
    sqlite3* db = nullptr;
    DbInfo pinfo, ainfo;
    bool hasPlugins = false, hasAssets = false;
    std::set<std::string> recordCodes;
    bool assetIndexes = false;
    int64_t deadline = 0;
    bool authorizing = false;

    ~Impl() {
        if (db) sqlite3_close(db);
    }

    // ------------------------------------------------------------ load
    bool Load(std::string* error) {
        std::string text;
        if (!ReadDataFile(L"dbq_families.json", &text)) {
            *error = "dbq_families.json not found (SKQE_DATA, next to skqe.exe, or built in)";
            return false;
        }
        Value root;
        if (!ParseJson(text, &root, error)) {
            *error = "dbq_families.json: " + *error;
            return false;
        }
        auto str = [](const Value* v) { return v && v->IsString() ? v->AsString() : std::string(); };
        if (const Value* syn = root.Find("synonyms"))
            for (const auto& [c, list] : syn->Members()) {
                for (const auto& w : list.Items()) canon[Lower(w.AsString())] = c;
                vocab.insert(c);
            }
        if (const Value* sw = root.Find("stopwords"))
            for (const auto& w : sw->Items()) stop.insert(Lower(w.AsString()));
        if (const Value* t = root.Find("types"))
            for (const auto& [k, v] : t->Members()) {
                types[Lower(k)] = v.AsString();
                maxTypeWords = std::max(maxTypeWords, SplitWs(k).size());
            }
        if (const Value* t = root.Find("texture_formats"))
            for (const auto& [k, v] : t->Members()) formats[Lower(k)] = v.AsString();
        if (const Value* t = root.Find("file_kinds"))
            for (const auto& [k, v] : t->Members()) fileKinds[k] = v.AsString();
        if (const Value* o = root.Find("options"))
            for (const auto& [id, def] : o->Members()) {
                std::vector<std::vector<std::string>> phrases;
                if (const Value* ph = def.Find("phrases"))
                    for (const auto& p : ph->Items()) phrases.push_back(SplitWs(CleanWords(p.AsString())));
                std::sort(phrases.begin(), phrases.end(), [](auto& a, auto& b) { return a.size() > b.size(); });
                options.push_back({id, phrases});
            }
        if (const Value* s = root.Find("scope_options"))
            for (const auto& [id, def] : s->Members()) {
                Scope sc;
                sc.id = id;
                sc.slotKind = str(def.Find("slot"));
                if (const Value* w = def.Find("words"))
                    for (const auto& x : w->Items()) sc.words.insert(x.AsString());
                scopes.push_back(sc);
            }
        if (const Value* r = root.Find("refine_markers"))
            for (const auto& m : r->Items()) refineMarkers.push_back(SplitWs(CleanWords(m.AsString())));

        const Value* fams = root.Find("families");
        if (!fams) {
            *error = "dbq_families.json has no families";
            return false;
        }
        std::regex tempRe("\\bs_[a-z0-9_]+");
        for (const auto& fv : fams->Items()) {
            Family f;
            f.id = str(fv.Find("id"));
            f.db = str(fv.Find("db"));
            f.group = str(fv.Find("group"));
            f.title = str(fv.Find("title"));
            f.sql = str(fv.Find("sql"));
            f.order = str(fv.Find("order"));
            if (fv.Find("count")) f.count = str(fv.Find("count"));
            f.unit = str(fv.Find("unit"));
            f.handler = str(fv.Find("handler"));
            f.note = str(fv.Find("note"));
            if (const Value* d = fv.Find("detail")) f.detail = d->AsBool();
            if (const Value* r = fv.Find("requires")) {
                f.requiresSql = str(r->Find("sql"));
                f.requiresMsg = str(r->Find("message"));
            }
            if (const Value* sl = fv.Find("slots"))
                for (const auto& s : sl->Items()) {
                    SlotDef d;
                    d.name = str(s.Find("name"));
                    d.kind = str(s.Find("kind"));
                    if (const Value* o = s.Find("optional")) d.optional = o->AsBool();
                    if (const Value* m = s.Find("multi")) d.multi = m->AsBool();
                    f.slots.push_back(d);
                }
            if (const Value* d = fv.Find("defaults"))
                for (const auto& [k, v] : d->Members()) f.defaults.push_back({k, v.AsString()});
            if (const Value* o = fv.Find("options"))
                for (const auto& [k, v] : o->Members()) {
                    OptionDef od;
                    if (v.IsString()) {
                        od.fragment = v.AsString();
                    } else {
                        od.order = str(v.Find("order"));
                        od.sql = str(v.Find("sql"));
                        if (const Value* dr = v.Find("drop"))
                            for (const auto& x : dr->Items()) od.drop.push_back(x.AsString());
                    }
                    f.options[k] = od;
                }
            if (const Value* e = fv.Find("extra"))
                for (const auto& x : e->Items()) f.extra.push_back({str(x.Find("title")), str(x.Find("sql"))});
            if (const Value* w = fv.Find("wordings"))
                for (const auto& x : w->Items()) f.wordings.push_back(x.AsString());
            if (const Value* t = fv.Find("tests"))
                for (const auto& x : t->Items()) {
                    Test tt;
                    tt.q = str(x.Find("q"));
                    if (const Value* m = x.Find("min_rows")) m->ToInt(&tt.minRows);
                    f.tests.push_back(tt);
                }
            std::string all = f.sql;
            for (const auto& [k, od] : f.options) all += " " + od.fragment + " " + od.sql;
            for (const auto& e : f.extra) all += " " + e.sql;
            for (std::sregex_iterator it(all.begin(), all.end(), tempRe), end; it != end; ++it) tempTables.insert(it->str());
            for (const auto& s : f.slots) tempTables.insert("s_" + s.name);
            families.push_back(std::move(f));
        }
        for (const char* t : {"s_plugin", "s_folder", "s_archive", "s_archive_files"}) tempTables.insert(t);
        // vocabulary words seen in wordings, for typo correction
        for (auto& f : families)
            for (const auto& w : f.wordings) {
                for (const auto& piece : SplitWs(CleanWords(w)))
                    if (piece.find('{') == std::string::npos) vocab.insert(CanonWord(piece, false));
            }
        for (auto& f : families)
            for (const auto& w : f.wordings) {
                std::vector<std::string> imp;
                f.patterns.push_back(CompilePattern(f, w, &imp));
                f.implied.push_back(imp);
            }
        return true;
    }

    // Lower case, quotes / brackets / trailing punctuation off, apostrophes out
    // (possessive 's handled by the tokenizer).
    static std::string CleanWords(const std::string& s) {
        std::string out;
        for (char c : Lower(s)) {
            if (c == '?' || c == '!' || c == ',' || c == ';' || c == '(' || c == ')' || c == '"') out += ' ';
            else if (c == '\'') continue;
            else out += c;
        }
        return out;
    }

    std::string CanonWord(const std::string& w, bool fuzzy) const {
        if (stop.count(w)) return w;
        auto it = canon.find(w);
        if (it != canon.end()) return it->second;
        if (w.size() > 3 && EndsWith(w, "ies")) {
            auto j = canon.find(w.substr(0, w.size() - 3) + "y");
            if (j != canon.end()) return j->second;
        }
        if (w.size() > 3 && EndsWith(w, "es")) {
            auto j = canon.find(w.substr(0, w.size() - 2));
            if (j != canon.end()) return j->second;
        }
        if (w.size() > 3 && EndsWith(w, "s")) {
            auto j = canon.find(w.substr(0, w.size() - 1));
            if (j != canon.end()) return j->second;
            if (vocab.count(w.substr(0, w.size() - 1))) return w.substr(0, w.size() - 1);
        }
        if (vocab.count(w)) return w;
        if (fuzzy && w.size() >= 5) {
            std::string best;
            int hits = 0;
            for (const auto& [k, c] : canon)
                if (k.size() >= 4 && std::abs(static_cast<int>(k.size()) - static_cast<int>(w.size())) <= 1 && EditDistance(k, w) == 1) {
                    if (best != c) ++hits;
                    best = c;
                }
            for (const auto& v : vocab)
                if (v.size() >= 4 && std::abs(static_cast<int>(v.size()) - static_cast<int>(w.size())) <= 1 && EditDistance(v, w) == 1) {
                    if (best != v && canon.find(v) == canon.end()) ++hits, best = v;
                }
            if (hits == 1) return best;
        }
        return w;
    }

    std::vector<PTok> CompilePattern(const Family& f, const std::string& wording, std::vector<std::string>* implied) const {
        // the same steps as a question: option phrases out (raw words), then
        // stop words out, then canonical words
        std::vector<std::string> raw = SplitWs(CleanWords(wording));
        for (const auto& [id, phrases] : options) {
            if (!f.Supports(id)) continue;
            for (const auto& ph : phrases)
                for (size_t i = 0; i + ph.size() <= raw.size();) {
                    bool m = !ph.empty();
                    for (size_t k = 0; k < ph.size() && m; ++k) m = raw[i + k] == ph[k];
                    if (m) {
                        raw.erase(raw.begin() + i, raw.begin() + i + ph.size());
                        if (std::find(implied->begin(), implied->end(), id) == implied->end()) implied->push_back(id);
                    } else {
                        ++i;
                    }
                }
        }
        std::vector<PTok> out;
        for (const auto& piece : raw) {
            if (piece.size() > 2 && piece.front() == '{') {
                std::string name = piece.substr(1, piece.find('}') - 1);
                for (size_t i = 0; i < f.slots.size(); ++i)
                    if (f.slots[i].name == name) out.push_back({"", static_cast<int>(i)});
                continue;
            }
            if (stop.count(piece)) continue;
            out.push_back({CanonWord(piece, false), -1});
        }
        return out;
    }

    // ------------------------------------------------------------ tokenize
    std::vector<Tok> Tokenize(const std::string& question) {
        std::vector<Tok> toks;
        std::string q = question;
        // quoted parts first
        std::vector<std::string> pieces;
        std::vector<bool> quoted;
        {
            std::string cur;
            bool inq = false;
            for (size_t i = 0; i < q.size(); ++i) {
                char c = q[i];
                bool open = c == '"' || (static_cast<unsigned char>(c) == 0xE2 && i + 2 < q.size() &&
                                         static_cast<unsigned char>(q[i + 1]) == 0x80 &&
                                         (static_cast<unsigned char>(q[i + 2]) == 0x9C || static_cast<unsigned char>(q[i + 2]) == 0x9D));
                if (open) {
                    if (static_cast<unsigned char>(c) == 0xE2) i += 2;
                    if (inq) {
                        pieces.push_back(cur), quoted.push_back(true), cur.clear();
                    } else {
                        for (auto& p : SplitWs(cur)) pieces.push_back(p), quoted.push_back(false);
                        cur.clear();
                    }
                    inq = !inq;
                    continue;
                }
                cur += c;
            }
            if (inq) pieces.push_back(cur), quoted.push_back(true);
            else
                for (auto& p : SplitWs(cur)) pieces.push_back(p), quoted.push_back(false);
        }
        for (size_t pi = 0; pi < pieces.size(); ++pi) {
            Tok t;
            if (quoted[pi]) {
                t.ent = Ent::Quoted;
                t.text = Trim(pieces[pi]);
                t.raw = Lower(t.text);
                if (t.text.empty()) continue;
                toks.push_back(t);
                continue;
            }
            std::string p = pieces[pi];
            // strip surrounding punctuation (keep dots inside names)
            while (!p.empty() && strchr("?!,;:()[]{}'`", p.front())) p.erase(0, 1);
            while (!p.empty() && strchr("?!,;()[]{}'`", p.back())) p.pop_back();
            while (!p.empty() && p.back() == '.' && Ext(p.substr(0, p.size() - 1)).empty() == false) p.pop_back();
            while (!p.empty() && p.back() == '.') p.pop_back();
            if (p.empty()) continue;
            std::string lp = Lower(p);
            if (EndsWith(lp, "'s") || EndsWith(lp, "\xE2\x80\x99s")) {
                t.possessive = true;
                lp = lp.substr(0, lp.size() - (lp[lp.size() - 2] == '\'' ? 2 : 4));
                p = p.substr(0, p.size() - (p[p.size() - 2] == '\'' ? 2 : 4));
            }
            t.raw = lp;
            t.orig = p;
            static const std::regex formRe("^(.+\\.(esm|esp|esl)):(?:0x)?([0-9a-f]{1,8})$");
            static const std::regex loRe("^(?:0x)?([0-9a-f]{8})$");
            static const std::regex numRe("^([0-9]+(?:\\.[0-9]+)?)(k|m|kb|mb|gb|px|x)?$");
            std::smatch m;
            if (std::regex_match(lp, m, formRe)) {
                t.ent = Ent::FormId;
                t.text = p;
            } else if (std::regex_match(lp, m, loRe) && std::any_of(lp.begin(), lp.end(), ::isdigit)) {
                t.ent = Ent::LoFormId;
                t.text = lp;
            } else if (std::regex_match(lp, m, numRe)) {
                t.ent = Ent::Number;
                t.text = lp;
                t.num = std::stod(m[1].str());
                std::string suf = m[2].str();
                if (suf == "k") t.kSuffix = true;
                if (suf == "m") t.num *= 1000000;
                if (suf == "kb") t.num *= 1024;
                if (suf == "mb") t.num *= 1024 * 1024;
                if (suf == "gb") t.num *= 1024.0 * 1024 * 1024;
            } else if (lp == "w/" || lp == "w/o" || lp == "-" || lp == "--" || lp == "&") {
                t.raw = lp == "w/o" ? "without" : lp == "w/" ? "with" : "and";
                t.word = t.raw;
            } else if (lp.find('\\') != std::string::npos || lp.find('/') != std::string::npos) {
                std::string e = Ext(lp);
                t.ent = (!e.empty() && kFileExts.count(e)) ? Ent::File : Ent::Folder;
                t.text = p;
                t.ext = e;
            } else if (kFileExts.count(Ext(lp)) && lp.size() > Ext(lp).size() + 1) {
                t.ent = Ent::File;
                t.text = p;
                t.ext = Ext(lp);
            } else {
                std::string w;
                for (char c : lp)
                    if (c != '\'') w += c;
                t.raw = w;
                t.word = CanonWord(w, true);
            }
            toks.push_back(t);
        }
        ExtendNames(&toks);
        return toks;
    }

    // "Immersive Jewelry.esp", "skyrim - textures0.bsa": a file name with
    // spaces arrives as several pieces. Join the words before it while the
    // longer name exists.
    void ExtendNames(std::vector<Tok>* toks) {
        for (size_t i = 0; i < toks->size(); ++i) {
            Tok& t = (*toks)[i];
            if (t.ent != Ent::File && t.ent != Ent::FormId) continue;
            const bool isPath = t.text.find('\\') != std::string::npos || t.text.find('/') != std::string::npos;
            std::string name = t.ent == Ent::FormId ? t.text.substr(0, t.text.rfind(':')) : t.text;
            std::string suffix = t.ent == Ent::FormId ? t.text.substr(t.text.rfind(':')) : "";
            if (NameExists(name, Ext(name))) continue;
            size_t bestStart = i;
            std::string best = name, joined = name;
            for (size_t k = 1; k <= 8 && k <= i; ++k) {
                const Tok& prev = (*toks)[i - k];
                // a path with spaces arrives as a folder piece, then words, then the file
                if (prev.ent != Ent::None && prev.ent != Ent::Number && !(isPath && prev.ent == Ent::Folder)) break;
                std::string pr = prev.ent == Ent::None ? prev.raw : prev.text;
                joined = pr + " " + joined;
                if (NameExists(joined, Ext(name))) {
                    best = joined;
                    bestStart = i - k;
                }
            }
            if (bestStart != i) {
                t.text = best + suffix;
                t.raw = Lower(t.text);
                toks->erase(toks->begin() + bestStart, toks->begin() + i);
                i = bestStart;
            }
        }
    }

    bool NameExists(const std::string& name, const std::string& ext) {
        if (!db) return false;
        auto it = existsCache.find(name);
        if (it != existsCache.end()) return it->second;
        bool e = NameExistsUncached(name, ext);
        existsCache[name] = e;
        return e;
    }
    bool NameExistsUncached(const std::string& name, const std::string& ext) {
        if (IsPluginExt(ext) && hasPlugins)
            return !Scalar("SELECT 1 FROM plugins WHERE name = ?1 COLLATE NOCASE", {name}).empty();
        if (name.find('\\') != std::string::npos || name.find('/') != std::string::npos) {
            std::string p = NormPath(name);
            if (hasAssets && !Scalar("SELECT 1 FROM paths WHERE path = ?1 LIMIT 1", {p}).empty()) return true;
            if (hasPlugins && !Scalar("SELECT 1 FROM asset_refs WHERE path = ?1 LIMIT 1", {p}).empty()) return true;
            return false;
        }
        if (hasAssets) return !Scalar("SELECT 1 FROM paths WHERE name = lower(?1) LIMIT 1", {name}).empty();
        return false;
    }

    // ------------------------------------------------------------ sql helpers
    std::string Scalar(const std::string& sql, const std::vector<std::string>& args) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return "";
        for (size_t i = 0; i < args.size(); ++i)
            sqlite3_bind_text(st, static_cast<int>(i + 1), args[i].c_str(), -1, SQLITE_TRANSIENT);
        std::string out;
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            out = t ? t : "\x01";  // non-empty for a NULL row
        }
        sqlite3_finalize(st);
        return out;
    }
    std::vector<std::vector<std::string>> Rows(const std::string& sql, const std::vector<std::string>& args, int limit = 100000) {
        std::vector<std::vector<std::string>> out;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
        for (size_t i = 0; i < args.size(); ++i)
            sqlite3_bind_text(st, static_cast<int>(i + 1), args[i].c_str(), -1, SQLITE_TRANSIENT);
        while (static_cast<int>(out.size()) < limit && sqlite3_step(st) == SQLITE_ROW) {
            std::vector<std::string> r;
            for (int c = 0; c < sqlite3_column_count(st); ++c) {
                const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, c));
                r.push_back(t ? t : "");
            }
            out.push_back(r);
        }
        sqlite3_finalize(st);
        return out;
    }
    bool Exec(const std::string& sql) { return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK; }

    static int Progress(void* p) {
        auto* self = static_cast<Impl*>(p);
        return self->deadline && NowMs() > self->deadline ? 1 : 0;
    }
    static int Authorize(void* p, int action, const char* a, const char* b, const char*, const char*) {
        auto* self = static_cast<Impl*>(p);
        if (!self->authorizing) return SQLITE_OK;
        int rc = AuthorizeAction(action, a, b);
        if (rc != SQLITE_OK && GetEnvironmentVariableW(L"SKQE_DBQ_DEBUG", nullptr, 0))
            fprintf(stderr, "denied action %d (%s, %s)\n", action, a ? a : "", b ? b : "");
        return rc;
    }
    static int AuthorizeAction(int action, const char* a, const char* b) {
        switch (action) {
            case SQLITE_PRAGMA:
                return b == nullptr ? SQLITE_OK : SQLITE_DENY;  // reading a pragma only
            case SQLITE_SELECT:
            case SQLITE_READ:
            case SQLITE_RECURSIVE:
                return SQLITE_OK;
            case SQLITE_FUNCTION: {
                std::string f = a ? Lower(a) : "";
                if (f == "zeroblob" || f == "randomblob" || f == "load_extension" || f == "readfile" || f == "writefile")
                    return SQLITE_DENY;
                return SQLITE_OK;
            }
            default:
                return SQLITE_DENY;
        }
    }

    // ------------------------------------------------------------ open
    bool OpenDbs(const std::vector<std::wstring>& paths, std::string* error) {
        if (db) sqlite3_close(db), db = nullptr;
        pinfo = DbInfo{};
        ainfo = DbInfo{};
        hasPlugins = hasAssets = false;
        std::vector<DbInfo> infos;
        for (const auto& p : paths) {
            if (p.empty()) continue;
            DbInfo info;
            info.path = p;
            sqlite3* d = nullptr;
            if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
                *error = "no such database: " + ToUtf8(p);
                return false;
            }
            if (sqlite3_open_v2(UriFor(p).c_str(), &d, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
                *error = "cannot open " + ToUtf8(p) + ": " + (d ? sqlite3_errmsg(d) : "");
                if (d) sqlite3_close(d);
                return false;
            }
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(d, "SELECT key, value FROM skqe_meta", -1, &st, nullptr) == SQLITE_OK) {
                while (sqlite3_step(st) == SQLITE_ROW) {
                    std::string k = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                    const char* v = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                    std::string val = v ? v : "";
                    if (k == "built_by") info.builtBy = val;
                    if (k == "built_at") info.builtAt = val;
                    if (k == "data_dir") info.dataDir = val;
                    if (k == "schema_version") info.schema = atoi(val.c_str());
                }
                sqlite3_finalize(st);
            }
            sqlite3_close(d);
            if (info.builtBy.find("build-db") != std::string::npos) {
                info.ok = true;
                pinfo = info;
            } else if (info.builtBy.find("build-assets") != std::string::npos) {
                info.ok = true;
                ainfo = info;
            } else {
                *error = ToUtf8(p) + " is not a skqe database (no skqe_meta.built_by)";
                return false;
            }
        }
        hasPlugins = pinfo.ok;
        hasAssets = ainfo.ok;
        if (!hasPlugins && !hasAssets) {
            *error = "no database set";
            return false;
        }
        const std::wstring mainPath = hasPlugins ? pinfo.path : ainfo.path;
        if (sqlite3_open_v2(UriFor(mainPath).c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
            *error = "cannot open " + ToUtf8(mainPath);
            return false;
        }
        sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
        sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
        sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 64 * 1024 * 1024);
        if (hasPlugins && hasAssets) {
            sqlite3_stmt* st = nullptr;
            sqlite3_prepare_v2(db, "ATTACH DATABASE ?1 AS a", -1, &st, nullptr);
            std::string uri = UriFor(ainfo.path);
            sqlite3_bind_text(st, 1, uri.c_str(), -1, SQLITE_TRANSIENT);
            int rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) {
                *error = "cannot attach " + ToUtf8(ainfo.path) + ": " + sqlite3_errmsg(db);
                return false;
            }
        }
        Exec("PRAGMA temp_store = MEMORY; PRAGMA cache_size = -262144;");
        if (hasPlugins && hasAssets) Exec("PRAGMA a.cache_size = -262144;");
        resolveCache.clear();
        existsCache.clear();
        nameCache.clear();
        for (const auto& t : tempTables) Exec("CREATE TEMP TABLE IF NOT EXISTS " + t + "(id INTEGER, txt TEXT)");
        if (hasAssets)
            assetIndexes = !Scalar("SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'paths_name'", {}).empty() ||
                           !Scalar("SELECT 1 FROM a.sqlite_master WHERE type = 'index' AND name = 'paths_name'", {}).empty();
        recordCodes.clear();
        if (hasPlugins)
            for (const auto& r : Rows("SELECT DISTINCT record_type FROM st_tables WHERE record_type IS NOT NULL", {})) recordCodes.insert(r[0]);
        sqlite3_progress_handler(db, 10000, &Impl::Progress, this);
        sqlite3_set_authorizer(db, &Impl::Authorize, this);
        return true;
    }

    bool Available(const Family& f) const {
        if (f.db == "assets") return hasAssets;
        if (f.db == "plugins") return hasPlugins;
        if (f.db == "both") return hasAssets && hasPlugins;
        return false;
    }

    // ------------------------------------------------------------ matching

    struct Detected {
        std::vector<std::string> options;  // supported by this family
        std::vector<std::string> ignored;  // phrases of options it doesn't support
        std::vector<Tok> toks;             // what is left
    };

    static bool WordsAt(const std::vector<Tok>& toks, size_t i, const std::vector<std::string>& ph) {
        if (i + ph.size() > toks.size()) return false;
        for (size_t k = 0; k < ph.size(); ++k)
            if (toks[i + k].ent != Ent::None || toks[i + k].raw != ph[k]) return false;
        return true;
    }

    Detected DetectOptions(const Family& f, const std::vector<Tok>& in) const {
        Detected d;
        d.toks = in;
        for (const auto& [id, phrases] : options) {
            for (const auto& ph : phrases) {
                if (ph.empty()) continue;
                for (size_t i = 0; i < d.toks.size();) {
                    if (WordsAt(d.toks, i, ph)) {
                        if (f.Supports(id)) {
                            if (std::find(d.options.begin(), d.options.end(), id) == d.options.end()) d.options.push_back(id);
                            d.toks.erase(d.toks.begin() + i, d.toks.begin() + i + ph.size());
                            continue;
                        }
                        if (std::find(d.ignored.begin(), d.ignored.end(), Join(ph, " ")) == d.ignored.end())
                            d.ignored.push_back(Join(ph, " "));
                    }
                    ++i;
                }
            }
        }
        return d;
    }

    bool EntityFits(const Tok& t, const std::string& kind) const {
        const std::string& e = t.ext;
        switch (t.ent) {
            case Ent::Quoted:
                return kind != "number" && kind != "folder";
            case Ent::FormId:
            case Ent::LoFormId:
                return kind == "form";
            case Ent::Number:
                return kind == "number";
            case Ent::Folder:
                return kind == "folder";
            case Ent::File:
                if (kind == "file" || kind == "ref_file") return !IsPluginExt(e) || kind == "file";
                if (kind == "texture") return e == "dds" || e == "tga" || e == "png";
                if (kind == "mesh") return e == "nif" || e == "btr" || e == "bto";
                if (kind == "archive") return e == "bsa";
                if (kind == "sound") return e == "wav" || e == "xwm" || e == "fuz" || e == "lip";
                if (kind == "animation") return e == "hkx" || e == "kf";
                if (kind == "script" || kind == "script_name") return e == "pex" || e == "psc";
                if (kind == "tri") return e == "tri";
                if (kind == "plugin" || kind == "plugin_name") return IsPluginExt(e);
                return false;
            default:
                return false;
        }
    }

    // Can these free words fill a slot of this kind?
    bool FreeFits(const std::vector<Tok>& toks, size_t i, size_t n, const std::string& kind) const {
        std::vector<std::string> words;
        bool unknown = false;
        for (size_t k = i; k < i + n; ++k) {
            if (toks[k].ent != Ent::None) return false;
            words.push_back(toks[k].raw);
            if (!vocab.count(toks[k].word) && !canon.count(toks[k].raw) && !types.count(toks[k].raw) &&
                !recordCodes.count(Upper(toks[k].raw)))
                unknown = true;
        }
        std::string joined = Join(words, " ");
        if (kind == "type") {
            if (types.count(joined)) return true;
            return n == 1 && recordCodes.count(Upper(joined)) != 0;
        }
        if (kind == "format") return n == 1 && formats.count(joined) != 0;
        if (kind == "filekind") return n == 1 && fileKinds.count(toks[i].word) != 0;
        if (kind == "game") return n == 1 && (joined == "le" || joined == "se" || joined == "sse" || joined == "oldrim" ||
                                               joined == "legendary" || joined == "both");
        if (kind == "number" || kind == "folder") return false;
        if (kind == "field") return n <= 4;
        if (kind == "text" || kind == "script_name") return n <= 6;
        // names (forms, plugins, files without extension): at least one word
        // that isn't question vocabulary
        return unknown && n <= 6;
    }

    struct Align {
        double score = 0;
        std::vector<std::pair<int, std::string>> captures;  // slot index -> text
    };

    double WordWeight(const std::string& w) const {
        if (w == "what" || w == "who" || w == "how" || w == "show") return 0.35;
        return 1.0;
    }

    // A name fills a slot; the more specific the slot, the better the fit
    // (Dawnguard.esm is a plugin before it is a file).
    double SlotGain(const Tok& t, const std::string& kind) const {
        double g = (kind == "file" || kind == "ref_file") && t.ent == Ent::File ? 1.35 : 1.5;
        if (t.possessive && kind == "form") g += 0.25;
        return g;
    }

    // The same words in another order: "Dragonborn.esm masters" for
    // "masters of {plugin}". Only when every slot takes a name (an entity),
    // and a little below an in-order match.
    Align BagPattern(const Family& f, const std::vector<Tok>& q, const std::vector<PTok>& p) const {
        Align a;
        double total = 0, gain = 0;
        std::vector<bool> used(q.size(), false);
        for (const auto& t : p) {
            total += t.slot >= 0 ? 1.5 : WordWeight(t.word);
            if (t.slot >= 0) {
                bool got = false;
                for (size_t i = 0; i < q.size() && !got; ++i)
                    if (!used[i] && q[i].ent != Ent::None && EntityFits(q[i], f.slots[t.slot].kind)) {
                        used[i] = true;
                        gain += SlotGain(q[i], f.slots[t.slot].kind);
                        a.captures.push_back({t.slot, q[i].text});
                        got = true;
                    }
                if (!got && !f.slots[t.slot].optional) return {};
                if (!got) gain += 0.6;
            } else {
                for (size_t i = 0; i < q.size(); ++i)
                    if (!used[i] && q[i].ent == Ent::None && q[i].word == t.word) {
                        used[i] = true;
                        gain += WordWeight(t.word);
                        break;
                    }
            }
        }
        if (total <= 0) return {};
        double cost = 0;
        for (size_t i = 0; i < q.size(); ++i)
            if (!used[i]) cost += q[i].ent != Ent::None ? 1.5 : (stop.count(q[i].raw) ? 0.02 : (vocab.count(q[i].word) ? 0.5 : 0.3));
        a.score = std::max(0.0, std::min(1.0, (gain - cost) / total)) * 0.92;
        return a;
    }

    Align AlignPattern(const Family& f, const std::vector<Tok>& q, const std::vector<PTok>& p) const {
        const size_t n = q.size(), m = p.size();
        const double NEG = -1e9;
        struct Cell {
            double v = -1e9;
            int pi = -1, pj = -1;
            int slot = -1;
            size_t from = 0, len = 0;
        };
        std::vector<std::vector<Cell>> dp(n + 1, std::vector<Cell>(m + 1));
        dp[0][0].v = 0;
        double total = 0;
        for (const auto& t : p) total += t.slot >= 0 ? 1.5 : WordWeight(t.word);
        if (total <= 0) return {};
        auto relax = [&](size_t i, size_t j, double v, size_t fi, size_t fj, int slot, size_t from, size_t len) {
            if (v > dp[i][j].v) dp[i][j] = {v, static_cast<int>(fi), static_cast<int>(fj), slot, from, len};
        };
        for (size_t i = 0; i <= n; ++i)
            for (size_t j = 0; j <= m; ++j) {
                double v = dp[i][j].v;
                if (v <= NEG / 2) continue;
                if (i < n) {  // skip a question token
                    const Tok& t = q[i];
                    double cost = t.ent != Ent::None ? 1.5 : (stop.count(t.raw) ? 0.02 : (vocab.count(t.word) ? 0.5 : 0.3));
                    relax(i + 1, j, v - cost, i, j, -1, 0, 0);
                }
                if (j < m) {
                    const PTok& pt = p[j];
                    if (pt.slot < 0) {
                        relax(i, j + 1, v, i, j, -1, 0, 0);  // pattern word unmatched
                        if (i < n && q[i].ent == Ent::None && q[i].word == pt.word)
                            relax(i + 1, j + 1, v + WordWeight(pt.word), i, j, -1, 0, 0);
                    } else {
                        const SlotDef& sd = f.slots[pt.slot];
                        if (sd.optional) relax(i, j + 1, v + 0.6, i, j, -1, 0, 0);
                        if (i < n && EntityFits(q[i], sd.kind))
                            relax(i + 1, j + 1, v + SlotGain(q[i], sd.kind), i, j, pt.slot, i, 1);
                        for (size_t len = 1; len <= 6 && i + len <= n; ++len) {
                            if (!FreeFits(q, i, len, sd.kind)) continue;
                            double pen = 0.08 * static_cast<double>(len - 1);
                            if (q[i + len - 1].possessive) pen += sd.kind == "form" ? -0.25 : 0.4;
                            for (size_t k = i; k < i + len; ++k)
                                if (vocab.count(q[k].word) && sd.kind != "type" && sd.kind != "filekind" && sd.kind != "format" &&
                                    sd.kind != "game")
                                    pen += 0.35;
                            relax(i + len, j + 1, v + 1.2 - pen, i, j, pt.slot, i, len);
                        }
                    }
                }
            }
        Align a;
        if (dp[n][m].v <= NEG / 2) return a;
        a.score = std::max(0.0, std::min(1.0, dp[n][m].v / total));
        // captures
        size_t i = n, j = m;
        while (i || j) {
            const Cell& c = dp[i][j];
            if (c.pi < 0) break;
            if (c.slot >= 0) {
                std::vector<std::string> words;
                for (size_t k = c.from; k < c.from + c.len; ++k)
                    words.push_back(q[k].ent != Ent::None ? q[k].text : (q[k].orig.empty() ? q[k].raw : q[k].orig));
                a.captures.push_back({c.slot, Join(words, " ")});
            }
            i = static_cast<size_t>(c.pi);
            j = static_cast<size_t>(c.pj);
        }
        return a;
    }

    // Scope options: "in X.esp", "inside y.bsa", "under textures\armor".
    struct ScopeHit {
        std::string option, slotName, text;
        size_t at = 0;
    };
    std::vector<ScopeHit> FindScopes(const Family& f, const std::vector<Tok>& toks) const {
        std::vector<ScopeHit> hits;
        for (size_t i = 0; i + 1 < toks.size(); ++i) {
            if (toks[i].ent != Ent::None) continue;
            for (const auto& sc : scopes) {
                if (!f.options.count(sc.id) || !sc.words.count(toks[i].raw)) continue;
                const Tok& t = toks[i + 1];
                bool fits = (sc.slotKind == "plugin" && (EntityFits(t, "plugin"))) ||
                            (sc.slotKind == "archive" && EntityFits(t, "archive")) ||
                            (sc.slotKind == "folder" && t.ent == Ent::Folder);
                if (fits) hits.push_back({sc.id, sc.slotKind, t.text, i});
            }
        }
        return hits;
    }

    Candidate Score(const Family& f, const std::vector<Tok>& toks) const {
        Candidate best;
        best.fam = &f;
        Detected d = DetectOptions(f, toks);
        std::vector<Tok> base;
        for (const auto& t : d.toks)
            if (!(t.ent == Ent::None && stop.count(t.raw))) base.push_back(t);
        // with and without scope options taken out
        std::vector<std::pair<std::vector<Tok>, std::vector<ScopeHit>>> variants;
        variants.push_back({base, {}});
        auto hits = FindScopes(f, base);
        if (!hits.empty()) {
            std::vector<Tok> cut;
            std::set<size_t> drop;
            for (const auto& h : hits) drop.insert(h.at), drop.insert(h.at + 1);
            for (size_t i = 0; i < base.size(); ++i)
                if (!drop.count(i)) cut.push_back(base[i]);
            variants.push_back({cut, hits});
        }
        for (const auto& [q, sc] : variants) {
            for (size_t pi = 0; pi < f.patterns.size(); ++pi) {
                const auto& p = f.patterns[pi];
                Align a = AlignPattern(f, q, p);
                Align bag = BagPattern(f, q, p);
                if (bag.score > a.score) a = bag;
                // a wording that says "inactive plugins" only fits a question that says inactive too
                const auto& imp = f.implied[pi];
                if (!imp.empty()) {
                    double have = 0;
                    for (const auto& o : imp)
                        if (std::find(d.options.begin(), d.options.end(), o) != d.options.end()) have += 1;
                    a.score = a.score * (static_cast<double>(p.size()) + have) / (static_cast<double>(p.size()) + static_cast<double>(imp.size()));
                }
                double s = a.score + (sc.empty() ? 0 : 0.02);
                if (s > best.score) {
                    best.score = s;
                    best.options = d.options;
                    best.ignored = d.ignored;
                    best.slots.clear();
                    for (const auto& [slot, text] : a.captures)
                        best.slots.push_back({f.slots[slot].name, f.slots[slot].kind, text, -1});
                    for (const auto& h : sc) {
                        best.options.push_back(h.option);
                        best.slots.push_back({h.slotName, h.slotName, h.text, -1});
                    }
                }
            }
        }
        static const std::set<std::string> fileKinds2 = {"file", "texture", "mesh", "archive", "sound", "animation", "script", "tri", "ref_file"};
        for (const auto& sv : best.slots)
            if (sv.kind == "text") best.score *= 0.93;  // free text is the fallback reading of a name
        for (const auto& sv : best.slots)
            if (fileKinds2.count(sv.kind) && Ext(sv.text).empty() && sv.text.find('\\') == std::string::npos) best.score *= 0.9;
        // a required slot nobody filled
        for (const auto& sd : f.slots) {
            if (sd.optional) continue;
            bool have = std::any_of(best.slots.begin(), best.slots.end(), [&](const SlotValue& v) { return v.name == sd.name; });
            if (!have) best.score *= 0.3;
        }
        return best;
    }

    // ------------------------------------------------------------ resolve

    std::string KindFilter(const std::string& kind) const {
        if (kind == "texture") return " AND p.kind = 'texture'";
        if (kind == "mesh") return " AND p.kind = 'mesh'";
        if (kind == "archive") return " AND p.kind = 'archive'";
        if (kind == "sound") return " AND p.kind IN ('audio', 'voice')";
        if (kind == "animation") return " AND p.kind = 'animation'";
        if (kind == "script") return " AND p.kind = 'script'";
        if (kind == "tri") return " AND p.ext = 'tri'";
        return "";
    }
    std::string KindRoot(const std::string& kind, const std::string& ext) const {
        if (kind == "texture" || ext == "dds" || ext == "tga") return "textures\\";
        if (kind == "mesh" || ext == "nif" || ext == "tri" || ext == "btr") return "meshes\\";
        if (kind == "script" || ext == "pex") return "scripts\\";
        if (ext == "psc") return "scripts\\source\\";
        if (kind == "sound" || ext == "wav" || ext == "xwm" || ext == "fuz" || ext == "lip") return "sound\\";
        if (kind == "animation" || ext == "hkx") return "meshes\\";
        return "";
    }
    std::string KindNoun(const std::string& kind, bool plural) const {
        static const std::map<std::string, std::string> nouns = {
            {"texture", "texture"}, {"mesh", "mesh"},     {"archive", "archive"}, {"sound", "sound file"},
            {"animation", "animation"}, {"script", "script"}, {"tri", "TRI file"}, {"file", "file"}, {"ref_file", "file"}};
        auto it = nouns.find(kind);
        std::string n = it == nouns.end() ? "file" : it->second;
        if (plural) n += (n.back() == 'h' ? "es" : "s");
        return n;
    }

    Resolved ResolvePath(const std::string& kind, const std::string& typed) {
        Resolved r;
        std::string t = NormPath(typed);
        if (t.empty()) return r;
        if (!hasAssets) {
            r.why = "needs an asset database (skqe build-assets)";
            return r;
        }
        std::string ext = Ext(t);
        std::string kf = KindFilter(kind);
        auto take = [&](const std::string& sql, const std::vector<std::string>& args, int cap) {
            auto rows = Rows(sql, args, cap + 1);
            r.rows.clear();
            for (const auto& row : rows) r.rows.push_back({_atoi64(row[0].c_str()), row[1]});
            return rows.size();
        };
        const std::string sel = "SELECT p.path_id, p.path FROM paths p WHERE ";
        // 1. a path
        if (t.find('\\') != std::string::npos) {
            std::string root = KindRoot(kind, ext);
            if (take(sel + "p.path IN (?1, ?2)" + kf, {t, root + t}, 5)) {
                r.ok = true;
                r.summary = r.rows.size() == 1 ? "" : std::to_string(r.rows.size()) + " paths";
                return r;
            }
            size_t n = take(sel + "p.path LIKE '%\\' || ?1" + kf + " ORDER BY p.path", {t}, 50);
            if (n) return Finish(r, kind, typed, n > 50, "ending in");
        }
        // 2. a name
        std::string name = BaseName(t);
        if (!ext.empty()) {
            size_t n = take(sel + "p.name = ?1" + kf + " ORDER BY p.path", {name}, 5000);
            if (n) {
                r.ok = true;
                if (n > 1) r.summary = Grouped(static_cast<int64_t>(n)) + " " + KindNoun(kind, true) + " named " + name;
                return r;
            }
        } else {
            size_t n = take(sel + "p.name LIKE ?1 || '.%'" + kf + " ORDER BY p.path", {name}, 5000);
            if (n) {
                std::set<std::string> names;
                for (const auto& row : r.rows) names.insert(BaseName(row.second));
                if (names.size() == 1) {
                    r.ok = true;
                    if (n > 1) r.summary = Grouped(static_cast<int64_t>(n)) + " " + KindNoun(kind, true) + " named " + *names.begin();
                    return r;
                }
                return Finish(r, kind, typed, n > 50, "named");
            }
        }
        // 3. textures that exist nowhere but meshes still name them
        if (kind == "texture") {
            std::string full = t.find('\\') != std::string::npos ? (StartsWith(t, "textures\\") ? t : "textures\\" + t) : "";
            auto rows = full.empty()
                            ? Rows("SELECT DISTINCT texture FROM nif_textures WHERE path_id IS NULL AND texture LIKE '%\\' || ?1 LIMIT 51", {name})
                            : Rows("SELECT DISTINCT texture FROM nif_textures WHERE path_id IS NULL AND texture = ?1 LIMIT 51", {full});
            if (!rows.empty()) {
                r.rows.clear();
                for (const auto& row : rows) r.rows.push_back({kNullId, row[0]});
                r.ok = true;
                r.summary = rows.size() == 1 ? "no such file exists; meshes still name it" :
                                               std::to_string(rows.size()) + " texture paths that exist nowhere";
                return r;
            }
        }
        // 4. part of a name
        size_t n = take(sel + "p.name LIKE '%' || ?1 || '%'" + kf + " ORDER BY length(p.name), p.path", {name}, 50);
        if (n) return Finish(r, kind, typed, n > 50, "containing");
        r.why = "no " + KindNoun(kind, false) + " matches \"" + typed + "\"";
        return r;
    }

    Resolved& Finish(Resolved& r, const std::string& kind, const std::string& typed, bool more, const char* how) {
        if (r.rows.size() == 1) {
            r.ok = true;
            return r;
        }
        r.ambiguous = true;
        r.ok = true;
        r.summary = (more ? "more than 50" : std::to_string(r.rows.size())) + " " + KindNoun(kind, true) + " " + how + " \"" +
                    typed + "\"";
        if (r.rows.size() > 50) r.rows.resize(50);
        for (const auto& row : r.rows) r.labels.push_back(row.second);
        return r;
    }

    Resolved ResolveRefFile(const std::string& typed) {
        Resolved r;
        if (hasAssets) {
            r = ResolvePath("file", typed);
            for (auto& row : r.rows) row.first = kNullId;
            return r;
        }
        std::string t = NormPath(typed);
        std::string root = KindRoot("", Ext(t));
        auto rows = Rows("SELECT DISTINCT path FROM asset_refs WHERE path IN (?1, ?2) LIMIT 5", {t, root + t});
        if (rows.empty()) rows = Rows("SELECT DISTINCT path FROM asset_refs WHERE path LIKE '%\\' || ?1 LIMIT 51", {BaseName(t)});
        for (const auto& row : rows) r.rows.push_back({kNullId, row[0]});
        if (r.rows.empty()) {
            r.why = "no record names a file \"" + typed + "\"";
            return r;
        }
        return Finish(r, "file", typed, r.rows.size() > 50, "ending in");
    }

    Resolved ResolvePlugin(const std::string& typed, bool nameOnly) {
        Resolved r;
        std::string t = Trim(typed);
        if (!hasPlugins) {
            if (nameOnly) {
                std::string e = Ext(t);
                if (IsPluginExt(e)) {
                    r.rows.push_back({kNullId, t});
                    r.ok = true;
                    return r;
                }
                if (hasAssets) {
                    auto rows = Rows("SELECT name FROM paths WHERE kind = 'plugin' AND name LIKE lower(?1) || '.es_' LIMIT 20", {t});
                    for (const auto& row : rows) r.rows.push_back({kNullId, row[0]});
                    if (!r.rows.empty()) return Finish(r, "plugin", typed, false, "named");
                }
                r.rows.push_back({kNullId, t + ".esp"});
                r.ok = true;
                return r;
            }
            r.why = "needs a plugin database (skqe build-db)";
            return r;
        }
        auto q = [&](const std::string& where, const std::vector<std::string>& args) {
            auto rows = Rows("SELECT plugin_no, name FROM plugins WHERE " + where + " ORDER BY load_order IS NULL, load_order LIMIT 51", args);
            r.rows.clear();
            for (const auto& row : rows) r.rows.push_back({_atoi64(row[0].c_str()), row[1]});
            return rows.size();
        };
        size_t n = q("name = ?1 COLLATE NOCASE", {t});
        if (!n) n = q("name IN (?1 || '.esm', ?1 || '.esp', ?1 || '.esl') COLLATE NOCASE", {t});
        if (!n) n = q("name LIKE ?1 || '%'", {t});
        if (!n) n = q("name LIKE '%' || ?1 || '%'", {t});
        if (!n) {
            r.why = "no plugin matches \"" + typed + "\"";
            return r;
        }
        if (n == 1) {
            r.ok = true;
            return r;
        }
        r.ambiguous = true;
        r.ok = true;
        r.summary = std::to_string(n > 50 ? 50 : n) + (n > 50 ? "+" : "") + " plugins match \"" + typed + "\"";
        if (r.rows.size() > 50) r.rows.resize(50);
        for (const auto& row : r.rows) r.labels.push_back(row.second);
        return r;
    }

    std::string FormLabelSql() const {
        return "(SELECT o.name FROM plugins o WHERE o.plugin_no = f.form_key >> 24) || ':' || printf('%06X', f.form_key & 16777215)"
               " || '  ' || f.type || '  ' || coalesce(f.edid, '') || coalesce('  \"' || (SELECT n.name FROM form_names n WHERE "
               "n.form_key = f.form_key AND n.plugin_no = f.plugin_no AND n.name IS NOT NULL LIMIT 1) || '\"', '')";
    }

    int sv_pick_ = -1;
    Resolved ResolveForm(const std::string& typed, bool multi) {
        Resolved r;
        if (!hasPlugins) {
            r.why = "needs a plugin database (skqe build-db)";
            return r;
        }
        std::string t = Trim(typed);
        std::string lt = Lower(t);
        std::vector<std::string> typesOf;
        std::vector<int64_t> ownerOrder;
        auto fill = [&](const std::string& sql, const std::vector<std::string>& args) {
            auto rows = Rows(sql, args, 201);
            r.rows.clear();
            r.labels.clear();
            typesOf.clear();
            ownerOrder.clear();
            for (const auto& row : rows) {
                r.rows.push_back({_atoi64(row[0].c_str()), row[1]});
                r.labels.push_back(row[1]);
                typesOf.push_back(row[2]);
                ownerOrder.push_back(row[3].empty() ? 1 << 30 : _atoi64(row[3].c_str()));
            }
            return rows.size();
        };
        const std::string pick = "SELECT f.form_key, " + FormLabelSql() +
                                 ", f.type, (SELECT o.load_order FROM plugins o WHERE o.plugin_no = f.form_key >> 24) FROM forms f WHERE f.is_winner AND ";
        // Plugin.esm:XXXXXX
        static const std::regex formRe("^(.+\\.(?:esm|esp|esl)):(?:0x)?([0-9a-fA-F]{1,8})$", std::regex::icase);
        std::smatch m;
        if (std::regex_match(t, m, formRe)) {
            Resolved pl = ResolvePlugin(m[1].str(), false);
            if (!pl.ok || pl.ambiguous || pl.rows.empty()) {
                r.why = "no plugin " + m[1].str();
                return r;
            }
            int64_t no = pl.rows[0].first;
            int64_t id = std::stoll(m[2].str(), nullptr, 16);
            bool light = Scalar("SELECT is_light FROM plugins WHERE plugin_no = ?1", {std::to_string(no)}) == "1";
            int64_t key = (no << 24) + (id & (light ? 0xFFF : 0xFFFFFF));
            if (fill(pick + "f.form_key = ?1", {std::to_string(key)})) {
                r.ok = true;
                return r;
            }
            r.why = "no record " + t;
            return r;
        }
        // 8-digit load-order FormID
        if (lt.size() >= 8 && IsHex(lt.substr(lt.size() - 8)) && (lt.size() == 8 || StartsWith(lt, "0x"))) {
            uint32_t v = static_cast<uint32_t>(std::stoul(lt.substr(lt.size() - 8), nullptr, 16));
            std::string sql;
            if ((v >> 24) == 0xFE)
                sql = "f.form_key = (SELECT plugin_no FROM plugins WHERE is_light AND load_order IS NOT NULL AND mod_index = " +
                      std::to_string((v >> 12) & 0xFFF) + ") * 16777216 + " + std::to_string(v & 0xFFF);
            else
                sql = "f.form_key = (SELECT plugin_no FROM plugins WHERE NOT is_light AND load_order IS NOT NULL AND mod_index = " +
                      std::to_string(v >> 24) + ") * 16777216 + " + std::to_string(v & 0xFFFFFF);
            if (fill(pick + sql, {})) {
                r.ok = true;
                r.summary = "load-order FormID " + Upper(lt.substr(lt.size() - 8)) + " is " + r.labels[0];
                return r;
            }
            r.why = "no record has the load-order FormID " + Upper(lt.substr(lt.size() - 8));
            return r;
        }
        bool exact = true;
        // editor ID
        size_t n = fill(pick + "f.form_key IN (SELECT form_key FROM forms WHERE edid = ?1) ORDER BY f.type", {t});
        // display name, any copy
        if (!n)
            n = fill(pick + "f.form_key IN (SELECT n.form_key FROM fts_names s JOIN form_names n ON n.id = s.rowid WHERE "
                            "fts_names MATCH ?1 AND (n.name = ?2 COLLATE NOCASE OR n.edid = ?2 COLLATE NOCASE)) ORDER BY f.type",
                     {"\"" + ReplaceAll(t, "\"", "") + "\"", t});
        if (!n) exact = false;
        if (!n)  // words of the name
            n = fill(pick + "f.form_key IN (SELECT n.form_key FROM fts_names s JOIN form_names n ON n.id = s.rowid WHERE "
                            "fts_names MATCH ?1) ORDER BY f.type",
                     {"\"" + ReplaceAll(t, "\"", "") + "\""});
        if (!n)
            n = fill(pick + "f.form_key IN (SELECT n.form_key FROM fts_names_tri s JOIN form_names n ON n.id = s.rowid WHERE "
                            "fts_names_tri MATCH ?1) ORDER BY f.type",
                     {"\"" + ReplaceAll(t, "\"", "") + "\""});
        if (!n) {
            r.why = "no record is named \"" + typed + "\"";
            return r;
        }
        if (n == 1 || multi) {
            r.ok = true;
            if (n > 1) r.summary = (n > 200 ? "200+" : std::to_string(n)) + " records match \"" + typed + "\"";
            return r;
        }
        r.ok = true;
        r.summary = (n > 200 ? "200+" : std::to_string(n)) + " records match \"" + typed + "\"";
        if (r.rows.size() > 30) r.rows.resize(30), r.labels.resize(30), typesOf.resize(30), ownerOrder.resize(30);
        // Several records share the name. When exactly one is a main kind of
        // record (an NPC, not her hair part), or exactly one comes from the
        // base game, use it and offer the others.
        if (exact && sv_pick_ < 0) {
            static const std::set<std::string> mainTypes = {"NPC_", "WEAP", "ARMO", "BOOK", "ALCH", "MISC", "SPEL", "QUST", "FACT", "RACE",
                                                            "CONT", "KEYM", "AMMO", "SCRL", "INGR", "SHOU", "PERK", "LCTN", "CELL", "WRLD",
                                                            "FURN", "DOOR", "ACTI", "LVLI", "LVLN", "OTFT", "ENCH", "MGEF", "CLAS", "SLGM"};
            int pickIdx = -1, mains = 0;
            for (size_t i = 0; i < typesOf.size(); ++i)
                if (mainTypes.count(typesOf[i])) ++mains, pickIdx = static_cast<int>(i);
            if (mains != 1) {
                pickIdx = -1;
                int base = 0;
                for (size_t i = 0; i < ownerOrder.size(); ++i)
                    if (ownerOrder[i] < 5 && (mains == 0 || mainTypes.count(typesOf[i]))) ++base, pickIdx = static_cast<int>(i);
                if (base != 1) pickIdx = -1;
            }
            if (pickIdx >= 0) {
                r.alternatives = r.labels;
                r.pickedIndex = pickIdx;
                r.summary = std::to_string(r.labels.size()) + " records are named \"" + typed + "\"; using " + r.labels[pickIdx] +
                            ". Type a number to use another.";
                r.rows = {r.rows[pickIdx]};
                return r;
            }
        }
        r.ambiguous = true;
        return r;
    }

    std::map<std::string, Resolved> resolveCache;
    std::map<std::string, bool> existsCache;

    Resolved ResolveSlot(const Family& f, const SlotValue& sv) {
        bool multi = std::any_of(f.slots.begin(), f.slots.end(), [&](const SlotDef& d) { return d.name == sv.name && d.multi; });
        std::string key = sv.kind + "|" + sv.text + "|" + std::to_string(sv.pick) + "|" + (multi ? "m" : "") +
                          (sv.kind == "number" && f.id.find("texture") != std::string::npos ? "px" : "");
        auto it = resolveCache.find(key);
        if (it != resolveCache.end()) return it->second;
        Resolved r = ResolveSlotUncached(f, sv);
        resolveCache[key] = r;
        return r;
    }

    Resolved ResolveSlotUncached(const Family& f, const SlotValue& sv) {
        Resolved r;
        const std::string& k = sv.kind;
        std::string t = Trim(sv.text);
        if (k == "texture" || k == "mesh" || k == "archive" || k == "sound" || k == "animation" || k == "script" || k == "tri" ||
            k == "file")
            r = ResolvePath(k, t);
        else if (k == "ref_file")
            r = ResolveRefFile(t);
        else if (k == "plugin")
            r = ResolvePlugin(t, false);
        else if (k == "plugin_name") {
            r = ResolvePlugin(t, true);
            for (auto& row : r.rows) row.first = kNullId;
        } else if (k == "form") {
            sv_pick_ = sv.pick;
            r = ResolveForm(t, std::any_of(f.slots.begin(), f.slots.end(), [&](const SlotDef& d) { return d.name == sv.name && d.multi; }));
            sv_pick_ = -1;
        } else if (k == "folder") {
            std::string p = NormPath(t);
            if (!p.empty() && p.back() != '\\') p += '\\';
            r.rows.push_back({kNullId, p});
            r.ok = !p.empty();
        } else if (k == "script_name") {
            std::string s = BaseName(NormPath(t));
            if (EndsWith(s, ".pex") || EndsWith(s, ".psc")) s = s.substr(0, s.size() - 4);
            r.rows.push_back({kNullId, s});
            r.ok = !s.empty();
        } else if (k == "type") {
            std::string lt = Lower(t);
            std::string code = types.count(lt) ? types.at(lt) : Upper(lt);
            if (hasPlugins && Scalar("SELECT 1 FROM st_tables WHERE record_type = ?1 LIMIT 1", {code}).empty()) {
                r.why = "no record type " + t;
            } else {
                r.rows.push_back({kNullId, code});
                r.ok = true;
            }
        } else if (k == "filekind") {
            std::string w = CanonWord(Lower(t), true);
            if (fileKinds.count(w)) {
                r.rows.push_back({kNullId, fileKinds.at(w)});
                if (fileKinds.at(w) == "audio") r.rows.push_back({kNullId, "voice"});
                r.ok = true;
            }
        } else if (k == "format") {
            std::string lt = Lower(t);
            if (formats.count(lt)) {
                r.param = formats.at(lt);
                r.ok = true;
            } else {
                r.why = "unknown texture format " + t;
            }
        } else if (k == "game") {
            std::string lt = Lower(t);
            r.param = (lt == "le" || lt == "oldrim" || lt == "legendary") ? "le" : lt == "both" ? "both" : "se";
            r.ok = true;
        } else if (k == "number") {
            static const std::regex numRe("^([0-9]+(?:\\.[0-9]+)?)(k|m|kb|mb|gb|px|x)?$");
            std::smatch m;
            std::string lt = Lower(t);
            if (std::regex_match(lt, m, numRe)) {
                double v = std::stod(m[1].str());
                std::string suf = m[2].str();
                bool pixels = f.id.find("texture") != std::string::npos;
                if (suf == "k") v *= pixels ? 1024 : 1000;
                if (suf == "m") v *= 1000000;
                if (suf == "kb") v *= 1024;
                if (suf == "mb") v *= 1024 * 1024;
                if (suf == "gb") v *= 1024.0 * 1024 * 1024;
                r.numberValue = v;
                r.param = std::to_string(static_cast<int64_t>(v));
                r.ok = true;
            }
        } else {  // text, field
            r.param = t;
            r.rows.push_back({kNullId, t});
            r.ok = !t.empty();
        }
        if (!r.ok && r.why.empty()) r.why = "couldn't understand \"" + t + "\" as a " + k;
        if (sv.pick >= 0 && sv.pick < static_cast<int>(r.rows.size())) {
            auto row = r.rows[sv.pick];
            r.rows = {row};
            r.ambiguous = false;
            r.summary.clear();
        }
        if ((k == "text" || k == "field") && r.rows.empty()) r.rows.push_back({kNullId, t});
        if (r.param.empty() && !r.rows.empty()) r.param = r.rows[0].second;
        return r;
    }

    // ------------------------------------------------------------ run

    std::string Interpreted(const Family& f, const std::vector<SlotValue>& slots, const std::vector<std::string>& opts) const {
        std::string s = f.title;
        for (const auto& sd : f.slots) {
            std::string val;
            for (const auto& v : slots)
                if (v.name == sd.name) val = v.text;
            std::string key = "{" + sd.name + "}";
            if (val.empty()) {
                s = ReplaceAll(s, key + " ", "");
                s = ReplaceAll(s, " " + key, "");
                s = ReplaceAll(s, key, "");
            } else {
                s = ReplaceAll(s, key, sd.kind == "type" || sd.kind == "filekind" || sd.kind == "number" ? val : "\"" + val + "\"");
            }
        }
        std::vector<std::string> extra;
        for (const auto& o : opts) {
            std::string scopeText;
            for (const auto& v : slots)
                if ((o == "in_plugin" && v.name == "plugin" && !f.Supports("x_plugin_slot")) || (o == "in_folder" && v.name == "folder") ||
                    (o == "in_archive" && v.name == "archive"))
                    scopeText = v.text;
            if (o == "count" && f.detail) continue;
            if (o == "count") extra.push_back("how many");
            else if (o == "in_plugin" || o == "in_folder" || o == "in_archive") extra.push_back("in " + scopeText);
            else extra.push_back(ReplaceAll(o, "_", " "));
        }
        if (!extra.empty()) s += " (" + Join(extra, ", ") + ")";
        return s;
    }

    std::string BuildSql(const Family& f, const std::vector<std::string>& opts, std::string* order) const {
        std::string sql = f.sql;
        for (const auto& o : opts) {
            auto it = f.options.find(o);
            if (it != f.options.end() && !it->second.sql.empty()) sql = it->second.sql;
        }
        std::set<std::string> dropped;
        std::string optFrag;
        *order = f.order;
        for (const auto& o : opts) {
            auto it = f.options.find(o);
            if (it == f.options.end()) continue;
            for (const auto& d : it->second.drop) dropped.insert(d);
            if (!it->second.fragment.empty()) optFrag += " " + it->second.fragment;
            if (!it->second.order.empty()) *order = it->second.order;
        }
        std::string def;
        for (const auto& [k, v] : f.defaults)
            if (!dropped.count(k)) def += " " + v;
        sql = ReplaceAll(sql, "{defaults}", def);
        sql = ReplaceAll(sql, "{opts}", optFrag);
        sql = ReplaceAll(sql, "{order}", *order);
        return sql;
    }

    bool FillTemps(const Family& f, const std::vector<SlotValue>& slots, std::map<std::string, Resolved>* res, Answer* a) {
        authorizing = false;
        for (const auto& t : tempTables) Exec("DELETE FROM " + t);
        std::vector<std::string> summaries;
        for (const auto& sv : slots) {
            Resolved r = ResolveSlot(f, sv);
            if (!r.ok) {
                a->kind = Answer::Kind::Unknown;
                a->message = r.why;
                return false;
            }
            if (r.ambiguous) {
                a->kind = Answer::Kind::Ask;
                a->message = r.summary + ". Which one?";
                for (size_t i = 0; i < r.labels.size(); ++i) {
                    a->choices.push_back(r.labels[i]);
                    Interpretation in = a->interp;
                    for (auto& s : in.slots)
                        if (s.name == sv.name) s.pick = static_cast<int>(i), s.text = sv.text;
                    a->choiceInterps.push_back(in);
                }
                return false;
            }
            if (!r.summary.empty()) summaries.push_back(r.summary);
            if (!r.alternatives.empty() && a->choices.empty()) {
                for (size_t i = 0; i < r.alternatives.size(); ++i) {
                    a->choices.push_back(r.alternatives[i]);
                    Interpretation in = a->interp;
                    for (auto& s2 : in.slots)
                        if (s2.name == sv.name) s2.pick = static_cast<int>(i);
                    a->choiceInterps.push_back(in);
                }
            }
            std::string table = "s_" + sv.name;
            if (sv.name == "archive" ||
                std::any_of(a->interp.options.begin(), a->interp.options.end(), [](const std::string& o) { return o == "in_archive"; })) {
                // archives as files rows, for in_archive
                if (sv.kind == "archive" || sv.name == "archive")
                    for (const auto& row : r.rows)
                        if (row.first != kNullId)
                            Exec("INSERT INTO s_archive_files SELECT file_id, NULL FROM files WHERE path_id = " + std::to_string(row.first));
            }
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, ("INSERT INTO " + table + " VALUES(?1, ?2)").c_str(), -1, &st, nullptr) == SQLITE_OK) {
                for (const auto& row : r.rows) {
                    if (row.first == kNullId) sqlite3_bind_null(st, 1);
                    else sqlite3_bind_int64(st, 1, row.first);
                    sqlite3_bind_text(st, 2, row.second.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(st);
                    sqlite3_reset(st);
                }
                sqlite3_finalize(st);
            }
            (*res)[sv.name] = r;
        }
        a->resolved = Join(summaries, "; ");
        return true;
    }

    void BindParams(sqlite3_stmt* st, const std::map<std::string, Resolved>& res) {
        for (int i = 1; i <= sqlite3_bind_parameter_count(st); ++i) {
            const char* nm = sqlite3_bind_parameter_name(st, i);
            if (!nm) continue;
            std::string name = nm + 1;
            auto it = res.find(name);
            if (it == res.end()) {
                sqlite3_bind_null(st, i);
                continue;
            }
            const Resolved& r = it->second;
            if (r.numberValue != 0 || name == "number") sqlite3_bind_double(st, i, r.numberValue);
            else sqlite3_bind_text(st, i, r.param.c_str(), -1, SQLITE_TRANSIENT);
        }
    }

    // Runs sql, keeps rows [offset, offset+page), counts all.
    bool RunPaged(const std::string& sql, const std::map<std::string, Resolved>& res, int64_t offset, int64_t page, Answer* a) {
        sqlite3_stmt* st = nullptr;
        authorizing = true;
        deadline = NowMs() + 120000;
        int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
        if (rc != SQLITE_OK) {
            authorizing = false;
            a->kind = Answer::Kind::Error;
            a->message = std::string("query failed: ") + sqlite3_errmsg(db);
            return false;
        }
        BindParams(st, res);
        int cols = sqlite3_column_count(st);
        a->table.columns.clear();
        a->table.numeric.assign(cols, true);
        for (int c = 0; c < cols; ++c) a->table.columns.push_back(sqlite3_column_name(st, c));
        int64_t n = 0;
        std::vector<bool> sawText(cols, false);
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            if (n >= offset && (page <= 0 || n < offset + page)) {
                std::vector<std::string> row;
                for (int c = 0; c < cols; ++c) {
                    int type = sqlite3_column_type(st, c);
                    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, c));
                    if (type == SQLITE_TEXT || type == SQLITE_BLOB) sawText[c] = true;
                    if (type == SQLITE_FLOAT) {
                        double d = sqlite3_column_double(st, c);
                        char b[64];
                        snprintf(b, sizeof b, "%g", d);
                        row.push_back(b);
                    } else {
                        row.push_back(t ? t : "");
                    }
                }
                a->table.rows.push_back(row);
            }
            ++n;
        }
        authorizing = false;
        deadline = 0;
        sqlite3_finalize(st);
        for (int c = 0; c < cols; ++c) a->table.numeric[c] = !sawText[c];
        if (rc != SQLITE_DONE) {
            a->kind = Answer::Kind::Error;
            a->message = rc == SQLITE_INTERRUPT ? "the query took longer than 2 minutes and was stopped" : std::string("query failed: ") + sqlite3_errmsg(db);
            return false;
        }
        a->total = n;
        a->offset = offset;
        return true;
    }

    Answer RunInterp(const Interpretation& in, int64_t offset, int64_t page) {
        Answer a;
        a.interp = in;
        int64_t t0 = NowMs();
        const Family* f = Find(in.family);
        if (!f) {
            a.message = "unknown family " + in.family;
            return a;
        }
        if (!Available(*f)) {
            a.kind = Answer::Kind::Unknown;
            a.message = f->db == "assets"  ? "That needs an asset database: skqe build-assets, then skqe db use <assets.db>."
                        : f->db == "plugins" ? "That needs a plugin database: skqe build-db, then skqe db use <plugins.db>."
                                             : "That needs both databases: skqe db use <plugins.db> <assets.db>.";
            return a;
        }
        a.interpreted = Interpreted(*f, in.slots, in.options);
        a.note = f->note;
        a.unit = f->unit;
        if (!f->requiresSql.empty() && Scalar(f->requiresSql, {}).empty()) {
            a.kind = Answer::Kind::Info;
            a.message = f->requiresMsg;
            return a;
        }
        std::map<std::string, Resolved> res;
        if (!FillTemps(*f, in.slots, &res, &a)) return a;
        if (!f->handler.empty()) {
            RunHandler(*f, res, offset, page, &a);
            a.ms = static_cast<double>(NowMs() - t0);
            return a;
        }
        std::string order;
        std::string sql = BuildSql(*f, in.options, &order);
        bool count = std::find(in.options.begin(), in.options.end(), "count") != in.options.end() && !f->detail;
        if (count && !f->detail) {
            std::string csql = "SELECT " + f->count + " FROM (" + ReplaceAll(sql, order, "") + ")";
            Answer tmp;
            if (!RunPaged(csql, res, 0, 1, &tmp)) {
                a.kind = tmp.kind;
                a.message = tmp.message;
                return a;
            }
            a.kind = Answer::Kind::Info;
            a.countValue = tmp.table.rows.empty() ? 0 : _atoi64(tmp.table.rows[0][0].c_str());
            a.message = Grouped(a.countValue) + " " + (f->unit.empty() ? "rows" : f->unit);
            a.sql = csql;
            a.ms = static_cast<double>(NowMs() - t0);
            return a;
        }
        a.sql = sql;
        if (f->detail) {
            if (!RunPaged(sql, res, 0, 0, &a)) return a;
            a.kind = Answer::Kind::Detail;
            Table pane;
            pane.columns = {"Property", "Value"};
            if (!a.table.rows.empty())
                for (size_t c = 0; c < a.table.columns.size(); ++c)
                    pane.rows.push_back({a.table.columns[c], a.table.rows[0][c]});
            a.total = static_cast<int64_t>(a.table.rows.size());
            if (a.table.rows.empty()) {
                a.kind = Answer::Kind::Unknown;
                a.message = "nothing found (the game may not load any copy of it)";
                return a;
            }
            a.table = pane;
            a.table.numeric = {false, false};
            for (const auto& e : f->extra) {
                Answer x;
                if (!RunPaged(e.sql, res, 0, 0, &x)) continue;
                Table t;
                t.columns = {e.title, ""};
                for (auto& r : x.table.rows) t.rows.push_back({r.size() > 0 ? r[0] : "", r.size() > 1 ? r[1] : ""});
                t.numeric = {false, false};
                a.sections.push_back({e.title, t});
            }
            a.ms = static_cast<double>(NowMs() - t0);
            return a;
        }
        if (!RunPaged(sql, res, offset, page, &a)) return a;
        a.kind = Answer::Kind::Rows;
        a.ms = static_cast<double>(NowMs() - t0);
        return a;
    }

    const Family* Find(const std::string& id) const {
        for (const auto& f : families)
            if (f.id == id) return &f;
        return nullptr;
    }

    // ------------------------------------------------------------ handlers

    std::map<int64_t, std::string> nameCache;
    std::string FormName(int64_t key) {
        auto it = nameCache.find(key);
        if (it != nameCache.end()) return it->second;
        auto rows = Rows("SELECT (SELECT name FROM plugins WHERE plugin_no = ?1 >> 24) || ':' || printf('%06X', ?1 & 16777215), "
                         "(SELECT edid FROM forms WHERE form_key = ?1 AND is_winner) ",
                         {std::to_string(key)});
        std::string s = rows.empty() ? std::to_string(key) : rows[0][0] + (rows[0][1].empty() ? "" : " " + rows[0][1]);
        nameCache[key] = s;
        return s;
    }

    struct Col {
        std::string table, name, path, kind, desc;
    };
    std::vector<Col> ColumnsOf(const std::string& type, bool children) {
        std::vector<Col> out;
        auto rows = Rows("SELECT c.table_name, c.column_name, coalesce(c.path, ''), coalesce(c.kind, ''), coalesce(c.description, ''), t.kind "
                         "FROM st_tables t JOIN st_columns c ON c.table_name = t.table_name WHERE t.record_type = ?1 AND t.kind IN "
                         "('type', 'child') ORDER BY t.kind = 'child', t.table_name, c.position",
                         {type});
        for (const auto& r : rows) {
            if (!children && r[5] == "child") continue;
            out.push_back({r[0], r[1], r[2], r[3], r[4]});
        }
        return out;
    }

    std::string Display(const Col& c, sqlite3_stmt* st, int i) {
        int type = sqlite3_column_type(st, i);
        if (type == SQLITE_NULL) return "";
        if (c.kind == "formid" && type == SQLITE_INTEGER) {
            int64_t v = sqlite3_column_int64(st, i);
            return v ? FormName(v) : "NULL";
        }
        if (c.kind == "flags" && type == SQLITE_INTEGER) {
            char b[32];
            snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(sqlite3_column_int64(st, i)));
            return b;
        }
        if (type == SQLITE_BLOB) {
            const unsigned char* p = static_cast<const unsigned char*>(sqlite3_column_blob(st, i));
            int n = sqlite3_column_bytes(st, i);
            std::string s;
            char b[4];
            for (int k = 0; k < n && k < 32; ++k) snprintf(b, sizeof b, "%02X", p[k]), s += b;
            return s + (n > 32 ? "..." : "");
        }
        if (type == SQLITE_FLOAT) {
            char b[64];
            snprintf(b, sizeof b, "%g", sqlite3_column_double(st, i));
            return b;
        }
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, i));
        return t ? t : "";
    }

    // property -> value for one copy of a record (type table + child tables)
    std::vector<std::pair<std::string, std::string>> CopyFields(const std::string& type, int64_t key, int64_t plugin) {
        std::vector<std::pair<std::string, std::string>> out;
        auto cols = ColumnsOf(type, true);
        std::map<std::string, std::vector<Col>> byTable;
        std::vector<std::string> order;
        for (const auto& c : cols) {
            if (!byTable.count(c.table)) order.push_back(c.table);
            byTable[c.table].push_back(c);
        }
        bool first = true;
        for (const auto& t : order) {
            const auto& tc = byTable[t];
            std::string sql = "SELECT * FROM \"" + t + "\" WHERE form_key = ?1 AND plugin_no = ?2";
            bool child = !first;
            if (child) sql += " ORDER BY idx";
            first = false;
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) continue;
            sqlite3_bind_int64(st, 1, key);
            sqlite3_bind_int64(st, 2, plugin);
            std::map<std::string, Col> colByName;
            for (const auto& c : tc) colByName[c.name] = c;
            int row = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::vector<std::string> parts;
                for (int i = 0; i < sqlite3_column_count(st); ++i) {
                    std::string nm = sqlite3_column_name(st, i);
                    if (nm == "form_key" || nm == "plugin_no" || nm == "idx") continue;
                    Col c = colByName.count(nm) ? colByName[nm] : Col{t, nm, nm, "", ""};
                    std::string v = Display(c, st, i);
                    if (v.empty()) continue;
                    std::string label = c.path.empty() ? c.name : c.path;
                    if (!child) out.push_back({label, v});
                    else {
                        size_t dot = label.find('.');
                        parts.push_back((dot == std::string::npos ? label : label.substr(dot + 1)) + "=" + v);
                    }
                }
                if (child && !parts.empty()) {
                    std::string sig = tc.empty() ? t : tc.back().path.substr(0, tc.back().path.find_first_of(".["));
                    for (const auto& c : tc)
                        if (!c.path.empty()) {
                            sig = c.path.substr(0, c.path.find_first_of(".["));
                            break;
                        }
                    out.push_back({sig + "[" + std::to_string(row) + "]", Join(parts, ", ")});
                }
                ++row;
            }
            sqlite3_finalize(st);
        }
        return out;
    }

    void RunHandler(const Family& f, std::map<std::string, Resolved>& res, int64_t offset, int64_t page, Answer* a) {
        a->table = Table{};
        if (f.handler == "fields_of_type") {
            std::string type = res["type"].param;
            auto cols = ColumnsOf(type, true);
            a->table.columns = {"table", "column", "field (xEdit path)", "kind", "description"};
            int64_t n = 0;
            for (const auto& c : cols) {
                if (c.kind == "key") continue;
                if (n >= offset && n < offset + page) a->table.rows.push_back({c.table, c.name, c.path, c.kind, c.desc});
                ++n;
            }
            a->table.numeric.assign(5, false);
            a->total = n;
            a->offset = offset;
            a->kind = n ? Answer::Kind::Rows : Answer::Kind::Unknown;
            if (!n) a->message = "no fields known for " + type;
            return;
        }
        int64_t key = res["form"].rows.empty() ? 0 : res["form"].rows[0].first;
        std::string type = Scalar("SELECT type FROM forms WHERE form_key = ?1 LIMIT 1", {std::to_string(key)});
        auto copies = Rows("SELECT f.plugin_no, p.name, f.is_winner FROM forms f JOIN plugins p ON p.plugin_no = f.plugin_no WHERE "
                           "f.form_key = ?1 ORDER BY p.load_order IS NULL, p.load_order",
                           {std::to_string(key)});
        if (copies.empty()) {
            a->kind = Answer::Kind::Unknown;
            a->message = "no such record";
            return;
        }
        if (f.handler == "record_detail") {
            int64_t winner = _atoi64(copies.back()[0].c_str());
            std::string winnerName = copies.back()[1];
            for (const auto& c : copies)
                if (c[2] == "1") winner = _atoi64(c[0].c_str()), winnerName = c[1];
            a->kind = Answer::Kind::Detail;
            a->table.columns = {"Field", "Value"};
            a->table.rows.push_back({"Form", FormName(key)});
            a->table.rows.push_back({"Type", type});
            a->table.rows.push_back({"Winning copy", winnerName});
            a->table.rows.push_back({"Copies", std::to_string(copies.size())});
            for (auto& [k, v] : CopyFields(type, key, winner)) a->table.rows.push_back({k, v});
            a->table.numeric = {false, false};
            a->total = static_cast<int64_t>(a->table.rows.size());
            return;
        }
        if (f.handler == "record_compare") {
            if (copies.size() < 2) {
                a->kind = Answer::Kind::Info;
                a->message = "Only " + copies[0][1] + " has this record, so there is nothing to compare.";
                return;
            }
            std::vector<std::map<std::string, std::string>> vals;
            std::vector<std::string> props;
            std::set<std::string> seen;
            for (const auto& c : copies) {
                std::map<std::string, std::string> m;
                for (auto& [k, v] : CopyFields(type, key, _atoi64(c[0].c_str()))) {
                    m[k] = v;
                    if (seen.insert(k).second) props.push_back(k);
                }
                vals.push_back(m);
            }
            a->table.columns = {"field"};
            for (const auto& c : copies) a->table.columns.push_back(c[1] + (c[2] == "1" ? " (wins)" : ""));
            int64_t n = 0;
            for (const auto& p : props) {
                std::set<std::string> distinct;
                for (auto& m : vals) distinct.insert(m.count(p) ? m[p] : "");
                if (distinct.size() < 2) continue;
                if (n >= offset && n < offset + page) {
                    std::vector<std::string> row = {p};
                    for (auto& m : vals) row.push_back(m.count(p) ? m[p] : "");
                    a->table.rows.push_back(row);
                }
                ++n;
            }
            a->table.numeric.assign(a->table.columns.size(), false);
            a->total = n;
            a->offset = offset;
            a->kind = Answer::Kind::Rows;
            if (!n) {
                a->kind = Answer::Kind::Info;
                a->message = "All " + std::to_string(copies.size()) + " copies have the same values in every field skqe compares.";
            }
            return;
        }
        if (f.handler == "record_field") {
            std::string want = Lower(res["field"].param);
            std::vector<std::string> wantWords = SplitWs(ReplaceAll(ReplaceAll(want, "_", " "), ".", " "));
            auto cols = ColumnsOf(type, true);
            double bestScore = 0;
            std::vector<Col> best;
            for (const auto& c : cols) {
                if (c.kind == "key") continue;
                std::string hay = Lower(c.path + " " + c.name + " " + c.desc);
                for (char& ch : hay)
                    if (ch == '_' || ch == '.' || ch == '[' || ch == ']' || ch == '(' || ch == ')') ch = ' ';
                std::vector<std::string> hw = SplitWs(hay);
                double s = 0;
                for (const auto& w : wantWords) {
                    std::string cw = w;
                    if (cw.size() > 3 && cw.back() == 's') cw.pop_back();
                    for (const auto& h : hw) {
                        std::string ch = h;
                        if (ch.size() > 3 && ch.back() == 's') ch.pop_back();
                        if (ch == cw) {
                            s += 1;
                            break;
                        }
                    }
                }
                std::string tail = Lower(c.path.substr(c.path.find_last_of('.') == std::string::npos ? 0 : c.path.find_last_of('.') + 1));
                if (tail == want || Lower(c.path) == want || Lower(c.name) == want) s += 1.5;
                if (s > bestScore) bestScore = s, best = {c};
                else if (s == bestScore && s > 0) best.push_back(c);
            }
            if (bestScore <= 0) {
                a->kind = Answer::Kind::Unknown;
                a->message = "A " + type + " record has no field like \"" + want + "\". Try: fields of " + type;
                return;
            }
            if (best.size() > 4) best.resize(4);
            a->table.columns = {"plugin", "wins", "field", "value"};
            for (const auto& c : copies) {
                int64_t pno = _atoi64(c[0].c_str());
                for (const auto& col : best) {
                    std::string sql = "SELECT \"" + col.name + "\" FROM \"" + col.table + "\" WHERE form_key = ?1 AND plugin_no = ?2";
                    sqlite3_stmt* st = nullptr;
                    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) continue;
                    sqlite3_bind_int64(st, 1, key);
                    sqlite3_bind_int64(st, 2, pno);
                    std::vector<std::string> vals;
                    while (sqlite3_step(st) == SQLITE_ROW) vals.push_back(Display(col, st, 0));
                    sqlite3_finalize(st);
                    a->table.rows.push_back({c[1], c[2] == "1" ? "Yes" : "", col.path.empty() ? col.name : col.path, Join(vals, "; ")});
                }
            }
            a->table.numeric.assign(4, false);
            a->total = static_cast<int64_t>(a->table.rows.size());
            a->kind = Answer::Kind::Rows;
            return;
        }
    }

    // ------------------------------------------------------------ ask

    bool HasRefineMarker(const std::vector<Tok>& toks) const {
        for (const auto& m : refineMarkers)
            for (size_t i = 0; i < toks.size(); ++i)
                if (WordsAt(toks, i, m)) return true;
        return false;
    }

    Answer AskQ(const std::string& question, const Interpretation* last, int64_t offset, int64_t page) {
        int64_t t0 = NowMs();
        Answer a;
        std::string q = Trim(question);
        if (q.empty()) {
            a.kind = Answer::Kind::Unknown;
            a.message = "Ask a question, for example: which meshes use femalehead.dds";
            return a;
        }
        std::vector<Tok> toks = Tokenize(q);
        std::vector<Candidate> cands;
        for (const auto& f : families) {
            if (!Available(f)) continue;
            Candidate c = Score(f, toks);
            if (c.score > 0.05) cands.push_back(c);
        }
        std::sort(cands.begin(), cands.end(), [](const Candidate& x, const Candidate& y) { return x.score > y.score; });

        // follow-up: change the last question
        Candidate refine;
        double refineScore = 0;
        if (last && !last->family.empty()) {
            const Family* lf = Find(last->family);
            if (lf && Available(*lf)) {
                Detected d = DetectOptions(*lf, toks);
                std::vector<Tok> rest;
                for (const auto& t : d.toks)
                    if (!(t.ent == Ent::None && stop.count(t.raw))) rest.push_back(t);
                refine.fam = lf;
                refine.options = last->options;
                refine.slots = last->slots;
                for (auto& s : refine.slots) s.pick = -1;
                bool changed = false;
                for (const auto& o : d.options)
                    if (std::find(refine.options.begin(), refine.options.end(), o) == refine.options.end())
                        refine.options.push_back(o), changed = true;
                // "show all" / "all of them again": options off
                for (size_t i = 0; i < toks.size(); ++i)
                    if (WordsAt(toks, i, {"all", "of", "them"}) || WordsAt(toks, i, {"show", "all"}) || WordsAt(toks, i, {"no", "filter"})) {
                        refine.options.clear();
                        changed = true;
                    }
                // a new name of the same kind replaces the old one
                std::vector<size_t> used;
                for (auto& s : refine.slots)
                    for (size_t i = 0; i < rest.size(); ++i)
                        if (rest[i].ent != Ent::None && EntityFits(rest[i], s.kind) &&
                            std::find(used.begin(), used.end(), i) == used.end()) {
                            s.text = rest[i].text;
                            used.push_back(i);
                            changed = true;
                            break;
                        }
                // scope words ("in X.esp")
                for (const auto& h : FindScopes(*lf, rest)) {
                    if (std::find(refine.options.begin(), refine.options.end(), h.option) == refine.options.end()) refine.options.push_back(h.option);
                    refine.slots.push_back({h.slotName, h.slotName, h.text, -1});
                    changed = true;
                }
                size_t leftover = 0;
                for (size_t i = 0; i < rest.size(); ++i)
                    if (std::find(used.begin(), used.end(), i) == used.end() && !(rest[i].ent == Ent::None && canon.count(rest[i].raw) == 0 && vocab.count(rest[i].word) == 0 && rest[i].raw.size() <= 3))
                        ++leftover;
                bool marker = HasRefineMarker(toks);
                // "only missing" after a question that has no such option: say so
                size_t uncovered = 0;  // words that are neither option phrases nor refine words
                {
                    std::vector<bool> cov(rest.size(), false);
                    auto cover = [&](const std::vector<std::string>& ph) {
                        for (size_t i = 0; i < rest.size(); ++i)
                            if (!ph.empty() && WordsAt(rest, i, ph))
                                for (size_t k = 0; k < ph.size(); ++k) cov[i + k] = true;
                    };
                    for (const auto& [id, phrases] : options)
                        for (const auto& ph : phrases) cover(ph);
                    for (const auto& m : refineMarkers) cover(m);
                    for (bool c : cov) uncovered += c ? 0 : 1;
                }
                if (!changed && !d.ignored.empty() && uncovered == 0) {
                    std::vector<std::string> takes;
                    for (const auto& [o, def] : lf->options) takes.push_back(ReplaceAll(o, "_", " "));
                    a.kind = Answer::Kind::Info;
                    a.message = "\"" + Join(d.ignored, "\", \"") + "\" can't narrow the last question (" + Interpreted(*lf, last->slots, last->options) + ")." +
                                (takes.empty() ? "" : " It takes: " + Join(takes, ", ") + ".");
                    a.interp = *last;
                    a.ms = static_cast<double>(NowMs() - t0);
                    return a;
                }
                if (changed && (marker || leftover == 0)) refineScore = leftover == 0 ? 0.97 : 0.85;
                else if (changed && leftover <= 1) refineScore = 0.6;
            }
        }
        double top = cands.empty() ? 0 : cands[0].score;
        if (refineScore > 0 && refineScore >= top) {
            Interpretation in;
            in.family = refine.fam->id;
            in.options = refine.options;
            in.slots = refine.slots;
            in.score = refineScore;
            in.refined = true;
            a = RunInterp(in, offset, page);
            a.interp = in;
            a.ms = static_cast<double>(NowMs() - t0);
            return a;
        }

        // resolve the best few: a name that matches nothing sinks its family
        size_t check = std::min<size_t>(cands.size(), 6);
        std::vector<std::string> whyNot;
        for (size_t i = 0; i < check; ++i) {
            if (cands[i].score < 0.3) break;
            for (const auto& sv : cands[i].slots) {
                if (sv.kind == "text" || sv.kind == "field" || sv.kind == "number") continue;
                Resolved r = ResolveSlot(*cands[i].fam, sv);
                if (!r.ok) {
                    cands[i].score *= 0.45;
                    whyNot.push_back(r.why);
                } else if (r.ambiguous) {
                    cands[i].score *= 0.85;
                }
            }
        }
        std::sort(cands.begin(), cands.end(), [](const Candidate& x, const Candidate& y) { return x.score > y.score; });
        top = cands.empty() ? 0 : cands[0].score;
        auto interpOf = [](const Candidate& c) {
            Interpretation in;
            in.family = c.fam->id;
            in.options = c.options;
            in.slots = c.slots;
            in.score = c.score;
            return in;
        };
        if (top >= 0.6) {
            // a near tie between different readings: ask
            if (cands.size() > 1 && cands[1].fam->id != cands[0].fam->id && cands[1].score > top - 0.05 &&
                (top < 0.92 || cands[1].score >= top - 0.005)) {
                a.kind = Answer::Kind::Ask;
                a.message = "Two readings fit. Which did you mean?";
                for (size_t i = 0; i < cands.size() && i < 3 && cands[i].score > top - 0.08; ++i) {
                    a.choices.push_back(Interpreted(*cands[i].fam, cands[i].slots, cands[i].options));
                    a.choiceInterps.push_back(interpOf(cands[i]));
                }
                a.ms = static_cast<double>(NowMs() - t0);
                return a;
            }
            Interpretation in = interpOf(cands[0]);
            a = RunInterp(in, offset, page);
            if (!cands[0].ignored.empty() && a.kind != Answer::Kind::Error)
                a.note += std::string(a.note.empty() ? "" : " ") + "(Ignored: \"" + Join(cands[0].ignored, "\", \"") + "\" - not an option here.)";
            a.ms = static_cast<double>(NowMs() - t0);
            return a;
        }
        if (top >= 0.33) {
            a.kind = Answer::Kind::Ask;
            a.message = "Not sure what you're asking. Closest:";
            for (size_t i = 0; i < cands.size() && i < 3 && cands[i].score >= 0.25; ++i) {
                a.choices.push_back(Interpreted(*cands[i].fam, cands[i].slots, cands[i].options));
                a.choiceInterps.push_back(interpOf(cands[i]));
            }
            if (!whyNot.empty()) a.note = "(" + whyNot[0] + ")";
            a.ms = static_cast<double>(NowMs() - t0);
            return a;
        }
        a.kind = Answer::Kind::Unknown;
        a.message = "I don't know that one yet.";
        if (!whyNot.empty()) a.message += " (" + whyNot[0] + ")";
        for (size_t i = 0; i < cands.size() && i < 3; ++i) {
            a.choices.push_back(Interpreted(*cands[i].fam, cands[i].slots, cands[i].options));
            a.choiceInterps.push_back(interpOf(cands[i]));
        }
        a.ms = static_cast<double>(NowMs() - t0);
        return a;
    }
};

// ================================================================== Engine

Engine::Engine() : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;
bool Engine::LoadFamilies(std::string* error) { return impl_->Load(error); }
bool Engine::Open(const std::vector<std::wstring>& dbs, std::string* error) { return impl_->OpenDbs(dbs, error); }
const DbInfo& Engine::plugins() const { return impl_->pinfo; }
const DbInfo& Engine::assets() const { return impl_->ainfo; }
Answer Engine::Ask(const std::string& q, const Interpretation* last, int64_t offset, int64_t page) {
    return impl_->AskQ(q, last, offset, page);
}
Answer Engine::Run(const Interpretation& in, int64_t offset, int64_t page) { return impl_->RunInterp(in, offset, page); }

std::vector<Engine::FamilyInfo> Engine::Families() const {
    std::vector<FamilyInfo> out;
    for (const auto& f : impl_->families) {
        std::string ex = f.wordings.empty() ? "" : f.wordings[0];
        out.push_back({f.id, f.group, f.title, ex, f.db, impl_->Available(f)});
    }
    return out;
}

std::vector<std::string> Engine::Explain(const std::string& question) {
    std::vector<std::string> out;
    auto toks = impl_->Tokenize(question);
    std::string line = "tokens:";
    for (const auto& t : toks) {
        static const char* names[] = {"", "file", "formid", "lo-formid", "number", "folder", "quoted"};
        line += " [" + (t.ent == Ent::None ? t.raw + (t.word != t.raw ? "=" + t.word : "") : std::string(names[static_cast<int>(t.ent)]) + ":" + t.text) + "]";
    }
    out.push_back(line);
    std::vector<Candidate> cands;
    for (const auto& f : impl_->families)
        if (impl_->Available(f)) cands.push_back(impl_->Score(f, toks));
    std::sort(cands.begin(), cands.end(), [](const Candidate& x, const Candidate& y) { return x.score > y.score; });
    for (size_t i = 0; i < cands.size() && i < 8; ++i) {
        char b[32];
        snprintf(b, sizeof b, "%.3f", cands[i].score);
        std::string l = std::string(b) + "  " + cands[i].fam->id;
        for (const auto& sv : cands[i].slots) l += "  " + sv.name + "=\"" + sv.text + "\"";
        if (!cands[i].options.empty()) l += "  options: " + Join(cands[i].options, ",");
        out.push_back(l);
    }
    return out;
}

std::vector<std::string> Engine::SelfTest(int* failures, int* run) {
    std::vector<std::string> out;
    *failures = 0;
    *run = 0;
    // wordings that read the same in two families (after stop words and synonyms)
    std::map<std::string, std::string> seenPat;
    for (const auto& f : impl_->families)
        for (size_t i = 0; i < f.patterns.size(); ++i) {
            std::string key;
            for (const auto& t : f.patterns[i]) key += (t.slot >= 0 ? "{" + f.slots[t.slot].kind + "}" : t.word) + " ";
            for (const auto& o : f.implied[i]) key += "+" + o;
            auto it = seenPat.find(key);
            if (it != seenPat.end() && it->second != f.id) {
                out.push_back("DUP   \"" + f.wordings[i] + "\" (" + f.id + ") reads the same as a wording of " + it->second + ": " + key);
                ++*failures;
            } else {
                seenPat[key] = f.id;
            }
        }
    for (const auto& f : impl_->families) {
        if (!impl_->Available(f)) continue;
        for (const auto& t : f.tests) {
            ++*run;
            Answer a = impl_->AskQ(t.q, nullptr, 0, 10);
            bool famOk = a.interp.family == f.id;
            bool kindOk = a.kind == Answer::Kind::Rows || a.kind == Answer::Kind::Detail ||
                          (a.kind == Answer::Kind::Info && (a.countValue >= 0 || !a.message.empty()));
            bool rowsOk = a.total >= t.minRows || a.countValue >= t.minRows || (a.kind == Answer::Kind::Info && t.minRows == 0);
            bool ok = famOk && kindOk && rowsOk;
            char buf[64];
            snprintf(buf, sizeof buf, "%6.0f ms", a.ms);
            std::string line = std::string(ok ? "ok    " : "FAIL  ") + buf + "  " + f.id + "  \"" + t.q + "\"";
            if (!famOk) line += "  -> got " + (a.interp.family.empty() ? std::string("(none)") : a.interp.family);
            if (!kindOk || !rowsOk) line += "  -> " + (a.message.empty() ? std::to_string(a.total) + " rows" : a.message);
            if (!ok) ++*failures;
            out.push_back(line);
        }
    }
    return out;
}

// ================================================================== state & log

std::wstring StateDir() {
    // SKQE_DBQ_STATE: another folder, so tests don't write into the real history
    wchar_t env[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"SKQE_DBQ_STATE", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        CreateDirectoryW(env, nullptr);
        return env;
    }
    wchar_t* p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    dir += L"\\skqe";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\dbq";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

namespace {
std::string IsoNow() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &tm);
    return b;
}
void AppendLine(const std::wstring& file, const std::string& line) {
    std::ofstream f(file, std::ios::binary | std::ios::app);
    f << line << "\n";
}
Value SlotsToValue(const std::vector<SlotValue>& slots) {
    Value list = Value::List();
    for (const auto& s : slots) {
        Value m = Value::Map();
        m.Set("name", Value::Str(s.name));
        m.Set("kind", Value::Str(s.kind));
        m.Set("text", Value::Str(s.text));
        if (s.pick >= 0) m.Set("pick", Value::Int(s.pick));
        list.Items().push_back(m);
    }
    return list;
}
Value InterpToValue(const Interpretation& in) {
    Value m = Value::Map();
    m.Set("family", Value::Str(in.family));
    Value opts = Value::List();
    for (const auto& o : in.options) opts.Items().push_back(Value::Str(o));
    m.Set("options", opts);
    m.Set("slots", SlotsToValue(in.slots));
    if (in.refined) m.Set("refined", Value::Bool(true));
    return m;
}
bool InterpFromValue(const Value& v, Interpretation* in) {
    const Value* f = v.Find("family");
    if (!f || !f->IsString()) return false;
    in->family = f->AsString();
    in->options.clear();
    in->slots.clear();
    if (const Value* o = v.Find("options"))
        for (const auto& x : o->Items()) in->options.push_back(x.AsString());
    if (const Value* s = v.Find("slots"))
        for (const auto& x : s->Items()) {
            SlotValue sv;
            if (const Value* a = x.Find("name")) sv.name = a->AsString();
            if (const Value* a = x.Find("kind")) sv.kind = a->AsString();
            if (const Value* a = x.Find("text")) sv.text = a->AsString();
            if (const Value* a = x.Find("pick")) sv.pick = static_cast<int>(a->AsInt());
            in->slots.push_back(sv);
        }
    return true;
}
const char* KindName(Answer::Kind k) {
    switch (k) {
        case Answer::Kind::Rows: return "rows";
        case Answer::Kind::Detail: return "detail";
        case Answer::Kind::Ask: return "ask";
        case Answer::Kind::Unknown: return "unknown";
        case Answer::Kind::Error: return "error";
        case Answer::Kind::Info: return "info";
    }
    return "";
}
}  // namespace

std::string InterpToJson(const Interpretation& in) {
    std::string s;
    WriteJson(InterpToValue(in), &s);
    return s;
}
bool InterpFromJson(const std::string& json, Interpretation* in) {
    Value v;
    std::string err;
    return ParseJson(json, &v, &err) && InterpFromValue(v, in);
}

void LogQuestion(const std::string& source, const std::string& question, const Answer& a, const std::vector<std::wstring>& dbs) {
    Value m = Value::Map();
    m.Set("time", Value::Str(IsoNow()));
    m.Set("source", Value::Str(source));
    m.Set("q", Value::Str(question));
    m.Set("result", Value::Str(KindName(a.kind)));
    m.Set("interpreted", Value::Str(a.interpreted));
    m.Set("interp", InterpToValue(a.interp));
    m.Set("total", Value::Int(a.total));
    if (a.countValue >= 0) m.Set("count", Value::Int(a.countValue));
    if (!a.message.empty()) m.Set("message", Value::Str(a.message));
    if (!a.choices.empty()) {
        Value c = Value::List();
        for (const auto& x : a.choices) c.Items().push_back(Value::Str(x));
        m.Set("choices", c);
    }
    m.Set("ms", Value::Int(static_cast<int64_t>(a.ms)));
    Value d = Value::List();
    for (const auto& p : dbs) d.Items().push_back(Value::Str(ToUtf8(p)));
    m.Set("dbs", d);
    std::string line;
    WriteJson(m, &line);
    AppendLine(StateDir() + L"\\history.jsonl", line);
}

void LogFeedback(const std::string& kind, const std::string& text) {
    Value m = Value::Map();
    m.Set("time", Value::Str(IsoNow()));
    m.Set("feedback", Value::Str(kind));
    m.Set("text", Value::Str(text));
    std::string line;
    WriteJson(m, &line);
    AppendLine(StateDir() + L"\\history.jsonl", line);
}

bool LoadState(CliState* s) {
    std::ifstream f(StateDir() + L"\\state.json", std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    Value v;
    std::string err;
    if (!ParseJson(ss.str(), &v, &err)) return false;
    if (const Value* d = v.Find("dbs"))
        for (const auto& x : d->Items()) s->dbs.push_back(ToWide(x.AsString()));
    if (const Value* l = v.Find("last")) s->hasLast = InterpFromValue(*l, &s->last);
    if (const Value* x = v.Find("question")) s->lastQuestion = x->AsString();
    if (const Value* x = v.Find("offset")) s->offset = x->AsInt();
    if (const Value* x = v.Find("page_size")) s->pageSize = x->AsInt();
    if (const Value* x = v.Find("console")) s->console = x->AsString();
    if (const Value* x = v.Find("time")) s->time = x->AsInt();
    if (const Value* c = v.Find("choices"))
        for (const auto& x : c->Items()) {
            Interpretation in;
            if (InterpFromValue(x, &in)) s->choices.push_back(in);
        }
    return true;
}

bool SaveState(const CliState& s) {
    Value m = Value::Map();
    Value d = Value::List();
    for (const auto& p : s.dbs) d.Items().push_back(Value::Str(ToUtf8(p)));
    m.Set("dbs", d);
    if (s.hasLast) {
        m.Set("last", InterpToValue(s.last));
        m.Set("question", Value::Str(s.lastQuestion));
        m.Set("offset", Value::Int(s.offset));
        m.Set("page_size", Value::Int(s.pageSize));
        m.Set("console", Value::Str(s.console));
        m.Set("time", Value::Int(s.time));
    }
    Value c = Value::List();
    for (const auto& in : s.choices) c.Items().push_back(InterpToValue(in));
    m.Set("choices", c);
    std::string text;
    WriteJson(m, &text);
    std::ofstream f(StateDir() + L"\\state.json", std::ios::binary | std::ios::trunc);
    f << text;
    return static_cast<bool>(f);
}

}  // namespace skqe::dbq
