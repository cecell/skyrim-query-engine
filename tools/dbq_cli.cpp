// skqe dbq / skqe db use: the command line and the interactive console over
// skqe::dbq::Engine (src/dbq.h).
#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "dbq.h"
#include "pattern_value.h"

namespace {

using skqe::dbq::Answer;
using skqe::dbq::CliState;
using skqe::dbq::Engine;
using skqe::dbq::Interpretation;

std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
    return n ? std::wstring(buf, n) : p;
}

// ---------------------------------------------------------------- output

struct Style {
    bool color = false;  // VT sequences
    bool unicode = true; // box drawing
    int width = 120;
};
Style g;

const char* C(const char* code) { return g.color ? code : ""; }
#define RESET C("\x1b[0m")
#define BOLD C("\x1b[1m")
#define DIM C("\x1b[2m")
#define ACCENT C("\x1b[38;5;173m")
#define BLUE C("\x1b[38;5;110m")
#define GREEN C("\x1b[38;5;108m")
#define RED C("\x1b[38;5;167m")
#define GREY C("\x1b[38;5;245m")

void Out(const std::string& s) { fwrite(s.data(), 1, s.size(), stdout); }

void SetupConsole() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) g.color = true;
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(h, &info)) g.width = std::max(60, info.srWindow.Right - info.srWindow.Left + 1);
    } else {
        _setmode(_fileno(stdout), _O_BINARY);
        g.width = 200;
    }
}

// Display width: UTF-8 code points (no wide-char tables; good enough for paths and names).
size_t Cols(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}
std::string Fit(const std::string& s, size_t w) {
    if (Cols(s) <= w) return s;
    std::string out;
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) {
            if (n + 1 >= w) break;
            ++n;
        }
        out += s[i];
    }
    return out + (g.unicode ? "\xE2\x80\xA6" : "~");
}
std::string Pad(const std::string& s, size_t w, bool right) {
    std::string f = Fit(s, w);
    std::string sp(w - std::min(w, Cols(f)), ' ');
    return right ? sp + f : f + sp;
}
std::string Rep(const char* unit, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) s += unit;
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
std::string Bytes(const std::string& v) {
    char* end = nullptr;
    double d = strtod(v.c_str(), &end);
    if (v.empty() || *end) return v;
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (d >= 1024 && u < 4) d /= 1024, ++u;
    char b[32];
    snprintf(b, sizeof b, u ? "%.1f %s" : "%.0f %s", d, units[u]);
    return b;
}
bool IsSizeColumn(const std::string& c) {
    std::string l = c;
    for (auto& ch : l) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    return (l.find("size") != std::string::npos && l.find("(px)") == std::string::npos && l != "array size") || l == "unpacked size";
}
std::string Cell(const std::string& col, const std::string& v, bool numeric) {
    if (IsSizeColumn(col)) return Bytes(v);
    if (numeric && !v.empty() && v.find_first_not_of("-0123456789") == std::string::npos && v.size() > 4 &&
        col.find("order") == std::string::npos && col.find("rank") == std::string::npos && col.find("id") == std::string::npos)
        return Grouped(_atoi64(v.c_str()));
    return v;
}

struct Box {
    const char *tl, *tr, *bl, *br, *h, *v, *tj, *bj, *lj, *rj, *x;
};
Box Lines() {
    if (g.unicode)
        return {"\xE2\x95\xAD", "\xE2\x95\xAE", "\xE2\x95\xB0", "\xE2\x95\xAF", "\xE2\x94\x80", "\xE2\x94\x82",
                "\xE2\x94\xAC", "\xE2\x94\xB4", "\xE2\x94\x9C", "\xE2\x94\xA4", "\xE2\x94\xBC"};
    return {"+", "+", "+", "+", "-", "|", "+", "+", "+", "+", "+"};
}

// A table with box lines, fitted to the console width.
void PrintTable(const skqe::dbq::Table& t, int indent = 2) {
    const size_t nc = t.columns.size();
    if (!nc) return;
    std::vector<std::vector<std::string>> cells;
    for (const auto& r : t.rows) {
        std::vector<std::string> row;
        for (size_t c = 0; c < nc; ++c) row.push_back(Cell(t.columns[c], c < r.size() ? r[c] : "", c < t.numeric.size() && t.numeric[c]));
        cells.push_back(row);
    }
    std::vector<size_t> w(nc);
    for (size_t c = 0; c < nc; ++c) {
        w[c] = Cols(t.columns[c]);
        for (const auto& r : cells) w[c] = std::max(w[c], Cols(r[c]));
        w[c] = std::max<size_t>(w[c], 1);
    }
    // shrink the widest columns until it fits
    size_t avail = static_cast<size_t>(std::max(40, g.width - indent - 1));
    auto total = [&] {
        size_t s = 1;
        for (auto x : w) s += x + 3;
        return s;
    };
    while (total() > avail) {
        size_t big = 0;
        for (size_t c = 1; c < nc; ++c)
            if (w[c] > w[big]) big = c;
        if (w[big] <= 8) break;
        --w[big];
    }
    Box b = Lines();
    std::string pad(indent, ' ');
    auto rule = [&](const char* l, const char* m, const char* r) {
        std::string s = pad + GREY + l;
        for (size_t c = 0; c < nc; ++c) s += Rep(b.h, w[c] + 2) + (c + 1 < nc ? m : r);
        return s + RESET + "\n";
    };
    std::string out = rule(b.tl, b.tj, b.tr);
    out += pad + GREY + b.v + RESET;
    for (size_t c = 0; c < nc; ++c) out += std::string(" ") + BOLD + Pad(t.columns[c], w[c], false) + RESET + " " + GREY + b.v + RESET;
    out += "\n" + rule(b.lj, b.x, b.rj);
    for (const auto& r : cells) {
        out += pad + GREY + b.v + RESET;
        for (size_t c = 0; c < nc; ++c) {
            bool right = c < t.numeric.size() && t.numeric[c];
            out += " " + Pad(r[c], w[c], right) + " " + GREY + b.v + RESET;
        }
        out += "\n";
    }
    out += rule(b.bl, b.bj, b.br);
    Out(out);
}

// Property / value pane; values with line breaks take several lines.
void PrintPane(const skqe::dbq::Table& t, const std::string& title) {
    size_t kw = 0;
    for (const auto& r : t.rows) kw = std::max(kw, Cols(r[0]));
    kw = std::min<size_t>(kw, 32);
    size_t vw = static_cast<size_t>(std::max(20, g.width - static_cast<int>(kw) - 9));
    Box b = Lines();
    std::string out = "  " + std::string(GREY) + b.tl + Rep(b.h, 1) + RESET + " " + BOLD + title + RESET + " " + GREY +
                      Rep(b.h, std::max<size_t>(1, kw + vw + 3 - std::min(kw + vw + 3, Cols(title) + 3))) + b.tr + RESET + "\n";
    for (const auto& r : t.rows) {
        std::string v = r.size() > 1 ? r[1] : "";
        std::vector<std::string> lines;
        size_t s = 0;
        while (true) {
            size_t e = v.find('\n', s);
            lines.push_back(v.substr(s, e == std::string::npos ? std::string::npos : e - s));
            if (e == std::string::npos) break;
            s = e + 1;
        }
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string val = Cell(r[0], lines[i], false);
            out += "  " + std::string(GREY) + b.v + RESET + " " + (i ? std::string(kw, ' ') : std::string(BLUE) + Pad(r[0], kw, false) + RESET) +
                   "  " + Pad(val, vw, false) + " " + GREY + b.v + RESET + "\n";
        }
    }
    out += "  " + std::string(GREY) + b.bl + Rep(b.h, kw + vw + 4) + b.br + RESET + "\n";
    Out(out);
}

void PrintAnswer(const Answer& a, bool showSql) {
    std::string bullet = g.unicode ? "\xE2\x97\x8F " : "* ";
    if (!a.interpreted.empty())
        Out(std::string(ACCENT) + bullet + RESET + BOLD + a.interpreted + RESET + (a.interp.refined ? std::string(DIM) + "  (refining the last question)" + RESET : "") + "\n");
    if (!a.resolved.empty()) Out(std::string("  ") + DIM + a.resolved + RESET + "\n");
    switch (a.kind) {
        case Answer::Kind::Rows: {
            if (a.total == 0) {
                Out(std::string("  ") + GREEN + "Nothing found." + RESET + "\n");
                break;
            }
            int64_t last = a.offset + static_cast<int64_t>(a.table.rows.size());
            Out("  " + Grouped(a.total) + " " + (a.unit.empty() ? "rows" : a.unit) + (a.total > static_cast<int64_t>(a.table.rows.size()) ? ", showing " + Grouped(a.offset + 1) + "-" + Grouped(last) : "") + "\n");
            PrintTable(a.table);
            break;
        }
        case Answer::Kind::Detail:
            PrintPane(a.table, "Details");
            for (const auto& [title, t] : a.sections)
                if (!t.rows.empty()) PrintPane(t, title);
            break;
        case Answer::Kind::Info:
            Out(std::string("  ") + BOLD + a.message + RESET + "\n");
            break;
        case Answer::Kind::Ask:
            Out(std::string("  ") + ACCENT + a.message + RESET + "\n");
            for (size_t i = 0; i < a.choices.size(); ++i)
                Out(std::string("   ") + BOLD + std::to_string(i + 1) + RESET + "  " + a.choices[i] + "\n");
            break;
        case Answer::Kind::Unknown:
            Out(std::string("  ") + RED + a.message + RESET + "\n");
            if (!a.choices.empty()) {
                Out(std::string("  ") + DIM + "Closest:" + RESET + "\n");
                for (size_t i = 0; i < a.choices.size(); ++i)
                    Out(std::string("   ") + BOLD + std::to_string(i + 1) + RESET + "  " + a.choices[i] + "\n");
            }
            break;
        case Answer::Kind::Error:
            Out(std::string("  ") + RED + a.message + RESET + "\n");
            break;
    }
    if ((a.kind == Answer::Kind::Rows || a.kind == Answer::Kind::Detail || a.kind == Answer::Kind::Info) && !a.choices.empty()) {
        Out(std::string("  ") + DIM + "Other matches (type the number):" + RESET + "\n");
        for (size_t i = 0; i < a.choices.size() && i < 12; ++i)
            Out(std::string("   ") + DIM + std::to_string(i + 1) + "  " + a.choices[i] + RESET + "\n");
    }
    if (!a.note.empty()) Out(std::string("  ") + DIM + a.note + RESET + "\n");
    if (showSql && !a.sql.empty()) Out(std::string("  ") + DIM + "SQL: " + a.sql + RESET + "\n");
}

std::string CsvField(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) o += c == '"' ? "\"\"" : std::string(1, c);
    return o + "\"";
}
void PrintCsv(const Answer& a) {
    std::string out;
    for (size_t c = 0; c < a.table.columns.size(); ++c) out += (c ? "," : "") + CsvField(a.table.columns[c]);
    out += "\n";
    for (const auto& r : a.table.rows) {
        for (size_t c = 0; c < r.size(); ++c) out += (c ? "," : "") + CsvField(r[c]);
        out += "\n";
    }
    Out(out);
}
void PrintJson(const Answer& a) {
    using skqe::Value;
    Value m = Value::Map();
    m.Set("interpreted", Value::Str(a.interpreted));
    m.Set("interpretation", Value::Str(skqe::dbq::InterpToJson(a.interp)));
    m.Set("result", Value::Str(a.kind == Answer::Kind::Rows ? "rows" : a.kind == Answer::Kind::Detail ? "detail" : a.kind == Answer::Kind::Ask ? "ask" : a.kind == Answer::Kind::Info ? "info" : a.kind == Answer::Kind::Unknown ? "unknown" : "error"));
    if (!a.message.empty()) m.Set("message", Value::Str(a.message));
    if (!a.resolved.empty()) m.Set("resolved", Value::Str(a.resolved));
    if (a.countValue >= 0) m.Set("count", Value::Int(a.countValue));
    m.Set("total", Value::Int(a.total));
    m.Set("offset", Value::Int(a.offset));
    Value cols = Value::List();
    for (const auto& c : a.table.columns) cols.Items().push_back(Value::Str(c));
    m.Set("columns", cols);
    Value rows = Value::List();
    for (const auto& r : a.table.rows) {
        Value row = Value::List();
        for (const auto& c : r) row.Items().push_back(Value::Str(c));
        rows.Items().push_back(row);
    }
    m.Set("rows", rows);
    if (!a.choices.empty()) {
        Value ch = Value::List();
        for (const auto& c : a.choices) ch.Items().push_back(Value::Str(c));
        m.Set("choices", ch);
    }
    for (const auto& [title, t] : a.sections) {
        Value sec = Value::List();
        for (const auto& r : t.rows) {
            Value row = Value::List();
            for (const auto& c : r) row.Items().push_back(Value::Str(c));
            sec.Items().push_back(row);
        }
        m.Set(title, sec);
    }
    std::string s;
    skqe::WriteJson(m, &s);
    Out(s + "\n");
}

std::string ConsoleKey() {
    char b[32];
    snprintf(b, sizeof b, "%p", static_cast<void*>(GetConsoleWindow()));
    return b;
}
int64_t NowSec() { return static_cast<int64_t>(time(nullptr)); }

bool OpenEngine(Engine& e, const std::vector<std::wstring>& dbs) {
    std::string err;
    if (!e.LoadFamilies(&err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    if (dbs.empty()) {
        fprintf(stderr, "No database set. Run: skqe db use <plugins.db> [<assets.db>]   (or pass --db <file.db>)\n");
        return false;
    }
    if (!e.Open(dbs, &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    return true;
}

std::string DbLine(const skqe::dbq::DbInfo& d, const char* label) {
    if (!d.ok) return "";
    std::string name = U8(d.path);
    size_t s = name.find_last_of("\\/");
    std::string at = d.builtAt.substr(0, 16);
    for (auto& c : at)
        if (c == 'T') c = ' ';
    return std::string(label) + (s == std::string::npos ? name : name.substr(s + 1)) + "  (built " + at + ")";
}

void PrintFamilies(Engine& e, const std::string& filter) {
    std::string group;
    for (const auto& f : e.Families()) {
        if (!filter.empty() && f.group.find(filter) == std::string::npos && f.id.find(filter) == std::string::npos) continue;
        if (f.group != group) {
            group = f.group;
            Out(std::string("\n  ") + BOLD + group + RESET + "\n");
        }
        Out(std::string("   ") + (f.available ? "" : DIM) + f.example + RESET + (f.available ? "" : std::string(DIM) + "  (needs the " + f.db + " db)" + RESET) + "\n");
    }
    Out("\n");
}

// ---------------------------------------------------------------- console

const char kConsoleHelp[] = R"(  Ask in plain English, for example:
    which meshes use femalehead.dds          what does Dawnguard.esm override
    textures of femalehead.nif               who changes Lydia
    which textures are missing               details of Skyrim - Textures0.bsa
  Then narrow it: "only missing", "in bsas", "how many", "in textures\armor\", "same for malehead.dds"

  Enter        next page              /prev, /page N, /all   paging
  1, 2, 3      pick a numbered choice /list [word]           every question it knows
  /sql         show the SQL (toggle)  /csv [file], /json     the whole answer as CSV / JSON
  /good, /bad [why], /note text       mark the last answer (saved with your questions)
  Teach it words: draw a connection between "person", "actor" -> NPC_   (or: teach women = female)
  /db [a.db] [b.db]                   show or change the databases
  /history     your recent questions  /clear   /exit
)";

std::string ReadLineConsole(bool* eof) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(in, &mode)) {
        std::string line;
        int c;
        while ((c = getchar()) != EOF && c != '\n') line += static_cast<char>(c);
        *eof = c == EOF && line.empty();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return line;
    }
    std::wstring buf(4096, L'\0');
    DWORD n = 0;
    if (!ReadConsoleW(in, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) || n == 0) {
        *eof = true;
        return "";
    }
    buf.resize(n);
    while (!buf.empty() && (buf.back() == L'\n' || buf.back() == L'\r')) buf.pop_back();
    if (buf.size() == 1 && buf[0] == 26) *eof = true;  // Ctrl+Z
    return U8(buf);
}

// Earlier questions, oldest first, for Up / Down.
std::vector<std::wstring> LoadInputHistory() {
    std::vector<std::wstring> h;
    std::ifstream f(skqe::dbq::StateDir() + L"\\history.jsonl", std::ios::binary);
    std::string l;
    while (std::getline(f, l)) {
        skqe::Value v;
        std::string err;
        if (!skqe::ParseJson(l, &v, &err)) continue;
        const skqe::Value* q = v.Find("q");
        if (!q || !q->IsString() || q->AsString().empty()) continue;
        std::wstring w = W(q->AsString());
        if (h.empty() || h.back() != w) h.push_back(w);
    }
    if (h.size() > 1000) h.erase(h.begin(), h.end() - 1000);
    return h;
}

// The input box: a bordered box that grows a line at a time as the text
// wraps, with the cursor anywhere in it. Up / Down move between wrapped
// lines, and from the first / last line through earlier questions; Down
// past the newest returns what was being typed. Esc clears, Ctrl+C clears
// (or leaves on an empty box), Ctrl+Left / Right jump words, Home / End.
class InputBox {
public:
    explicit InputBox(std::vector<std::wstring>* history) : hist_(history) {}

    std::string Read(bool* eof) {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        DWORD oldMode = 0;
        if (!g.color || !GetConsoleMode(in, &oldMode)) return Plain(eof);
        SetConsoleMode(in, (oldMode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)) | ENABLE_WINDOW_INPUT);
        buf_.clear();
        draft_.clear();
        cur_ = 0;
        hi_ = hist_->size();
        drawnRows_ = 0;
        cursorRow_ = 0;
        Render();
        INPUT_RECORD recs[256];
        bool done = false;
        while (!done) {
            DWORD n = 0;
            if (!ReadConsoleInputW(in, recs, 256, &n) || n == 0) {
                *eof = true;
                break;
            }
            for (DWORD i = 0; i < n && !done; ++i) {
                const INPUT_RECORD& r = recs[i];
                if (r.EventType == WINDOW_BUFFER_SIZE_EVENT) continue;  // Render reads the width
                if (r.EventType != KEY_EVENT || !r.Event.KeyEvent.bKeyDown) continue;
                const KEY_EVENT_RECORD& k = r.Event.KeyEvent;
                const bool ctrl = (k.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                const wchar_t ch = k.uChar.UnicodeChar;
                for (WORD rep = 0; rep < std::max<WORD>(1, k.wRepeatCount) && !done; ++rep) {
                    switch (k.wVirtualKeyCode) {
                        case VK_RETURN: {
                            // more keys already waiting: a pasted line break, not Enter
                            DWORD pending = 0;
                            GetNumberOfConsoleInputEvents(in, &pending);
                            if (i + 1 < n || pending > 0) Insert(L' ');
                            else done = true;
                            break;
                        }
                        case VK_BACK:
                            if (cur_ > 0) {
                                size_t from = ctrl ? WordLeft(cur_) : cur_ - 1;
                                buf_.erase(from, cur_ - from);
                                cur_ = from;
                            }
                            break;
                        case VK_DELETE:
                            if (cur_ < buf_.size()) buf_.erase(cur_, ctrl ? WordRight(cur_) - cur_ : 1);
                            break;
                        case VK_LEFT: cur_ = ctrl ? WordLeft(cur_) : (cur_ ? cur_ - 1 : 0); break;
                        case VK_RIGHT: cur_ = ctrl ? WordRight(cur_) : std::min(buf_.size(), cur_ + 1); break;
                        case VK_HOME: cur_ = 0; break;
                        case VK_END: cur_ = buf_.size(); break;
                        case VK_UP: Up(); break;
                        case VK_DOWN: Down(); break;
                        case VK_ESCAPE:
                            buf_.clear();
                            cur_ = 0;
                            hi_ = hist_->size();
                            break;
                        default:
                            if (ch == 3) {  // Ctrl+C
                                if (buf_.empty()) *eof = done = true;
                                buf_.clear();
                                cur_ = 0;
                            } else if ((ch == 4 || ch == 26) && buf_.empty()) {  // Ctrl+D / Ctrl+Z
                                *eof = done = true;
                            } else if (ch == 1) {
                                cur_ = 0;  // Ctrl+A
                            } else if (ch == 5) {
                                cur_ = buf_.size();  // Ctrl+E
                            } else if (ch == 21) {  // Ctrl+U
                                buf_.erase(0, cur_);
                                cur_ = 0;
                            } else if (ch == 11) {  // Ctrl+K
                                buf_.erase(cur_);
                            } else if (ch == L'\t') {
                                Insert(L' ');
                            } else if (ch >= 32 && (!ctrl || (k.dwControlKeyState & RIGHT_ALT_PRESSED))) {  // AltGr reports Ctrl too
                                Insert(ch);
                            }
                    }
                }
            }
            if (!done) Render();
        }
        // leave the box drawn with the question in it, cursor below it
        cur_ = buf_.size();
        Render();
        Out("\x1b[" + std::to_string(std::max(1, drawnRows_ - 1 - cursorRow_)) + "B\r\n");
        fflush(stdout);
        SetConsoleMode(in, oldMode);
        std::string line = U8(buf_);
        if (!line.empty() && (hist_->empty() || hist_->back() != buf_)) hist_->push_back(buf_);
        return line;
    }

private:
    std::vector<std::wstring>* hist_;
    std::wstring buf_, draft_;
    size_t cur_ = 0, hi_ = 0;
    int drawnRows_ = 0, cursorRow_ = 0;

    static std::string Plain(bool* eof) {
        Out("> ");
        fflush(stdout);
        return ReadLineConsole(eof);
    }
    void Insert(wchar_t c) {
        buf_.insert(buf_.begin() + static_cast<std::ptrdiff_t>(cur_), c);
        ++cur_;
    }
    size_t WordLeft(size_t p) const {
        while (p > 0 && buf_[p - 1] == L' ') --p;
        while (p > 0 && buf_[p - 1] != L' ') --p;
        return p;
    }
    size_t WordRight(size_t p) const {
        while (p < buf_.size() && buf_[p] != L' ') ++p;
        while (p < buf_.size() && buf_[p] == L' ') ++p;
        return p;
    }
    size_t TextWidth() const { return static_cast<size_t>(std::max(20, ConsoleWidth() - 7)); }
    static int ConsoleWidth() {
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) return info.srWindow.Right - info.srWindow.Left + 1;
        return g.width;
    }
    // where each wrapped line starts (breaking after a space when one is in reach)
    std::vector<size_t> Starts(size_t cw) const {
        std::vector<size_t> s = {0};
        size_t pos = 0;
        while (buf_.size() - pos > cw) {
            size_t brk = pos + cw;
            for (size_t j = pos + cw; j > pos + cw / 2; --j)
                if (buf_[j - 1] == L' ') {
                    brk = j;
                    break;
                }
            s.push_back(brk);
            pos = brk;
        }
        return s;
    }
    void Where(const std::vector<size_t>& s, size_t cw, size_t* line, size_t* col) const {
        size_t l = 0;
        while (l + 1 < s.size() && s[l + 1] <= cur_) ++l;
        size_t c = cur_ - s[l];
        if (c >= cw) {  // after a full last line: the start of a new one
            ++l;
            c = 0;
        }
        *line = l;
        *col = c;
    }
    void Up() {
        size_t cw = TextWidth(), line, col;
        auto s = Starts(cw);
        Where(s, cw, &line, &col);
        if (line > 0) {
            size_t prevLen = s[line] - s[line - 1];
            cur_ = s[line - 1] + std::min(col, prevLen ? prevLen - 1 : 0);
            return;
        }
        if (hi_ == 0) return;
        if (hi_ == hist_->size()) draft_ = buf_;
        --hi_;
        buf_ = (*hist_)[hi_];
        cur_ = buf_.size();
    }
    void Down() {
        size_t cw = TextWidth(), line, col;
        auto s = Starts(cw);
        Where(s, cw, &line, &col);
        if (line + 1 < s.size()) {
            size_t end = line + 2 < s.size() ? s[line + 2] : buf_.size();
            cur_ = std::min(s[line + 1] + col, end);
            return;
        }
        if (hi_ >= hist_->size()) return;
        ++hi_;
        buf_ = hi_ == hist_->size() ? draft_ : (*hist_)[hi_];
        cur_ = buf_.size();
    }
    void Render() {
        const size_t cw = TextWidth();
        auto s = Starts(cw);
        size_t line, col;
        Where(s, cw, &line, &col);
        size_t lines = std::max(s.size(), line + 1);
        Box b = Lines();
        std::string out;
        if (drawnRows_ > 0) {
            if (cursorRow_ > 0) out += "\x1b[" + std::to_string(cursorRow_) + "A";
            out += "\r\x1b[J";
        }
        out += std::string(GREY) + b.tl + Rep(b.h, cw + 4) + b.tr + RESET + "\n";
        for (size_t i = 0; i < lines; ++i) {
            std::wstring text;
            if (i < s.size()) text = buf_.substr(s[i], (i + 1 < s.size() ? s[i + 1] : buf_.size()) - s[i]);
            std::string t = U8(text);
            out += std::string(GREY) + b.v + RESET + " " + (i == 0 ? std::string(ACCENT) + "> " + RESET : std::string("  ")) + t +
                   std::string(cw - std::min(cw, text.size()), ' ') + " " + GREY + b.v + RESET + "\n";
        }
        out += std::string(GREY) + b.bl + Rep(b.h, cw + 4) + b.br + RESET;
        const int rows = static_cast<int>(lines) + 2;
        const int target = static_cast<int>(line) + 1;
        out += "\x1b[" + std::to_string(rows - 1 - target) + "A";
        if (rows - 1 - target == 0) out.erase(out.size() - 4);  // ESC[0A moves one line on some consoles
        out += "\x1b[" + std::to_string(col + 5) + "G";
        Out(out);
        fflush(stdout);
        drawnRows_ = rows;
        cursorRow_ = target;
    }
};

void Banner(Engine& e) {
    Box b = Lines();
    std::vector<std::string> lines = {std::string(ACCENT) + (g.unicode ? "\xE2\x9C\xBB " : "* ") + RESET + BOLD + "skqe dbq" + RESET + "  ask your load order in plain English",
                                      ""};
    std::string p = DbLine(e.plugins(), "plugins  "), a = DbLine(e.assets(), "assets   ");
    if (!p.empty()) lines.push_back(std::string(DIM) + p + RESET);
    if (!a.empty()) lines.push_back(std::string(DIM) + a + RESET);
    lines.push_back("");
    lines.push_back(std::string(DIM) + "/help for help, /list for every question, /exit to leave" + RESET);
    size_t w = 0;
    auto vis = [](const std::string& s) {  // width without VT codes
        size_t n = 0;
        bool esc = false;
        for (unsigned char c : s) {
            if (c == 0x1b) esc = true;
            else if (esc && c == 'm') esc = false;
            else if (!esc && (c & 0xC0) != 0x80) ++n;
        }
        return n;
    };
    for (const auto& l : lines) w = std::max(w, vis(l));
    w += 2;
    std::string out = std::string(GREY) + b.tl + Rep(b.h, w + 2) + b.tr + RESET + "\n";
    for (const auto& l : lines) out += std::string(GREY) + b.v + RESET + "  " + l + std::string(w - vis(l), ' ') + GREY + b.v + RESET + "\n";
    out += std::string(GREY) + b.bl + Rep(b.h, w + 2) + b.br + RESET + "\n\n";
    Out(out);
}

int RunConsole(Engine& e, std::vector<std::wstring> dbs, int64_t pageSize) {
    std::vector<std::wstring> inputHistory = LoadInputHistory();
    InputBox input(&inputHistory);
    Banner(e);
    Interpretation last;
    bool hasLast = false;
    std::string lastQ;
    Answer lastA;
    int64_t offset = 0;
    bool showSql = false;
    std::vector<Interpretation> choices;
    while (true) {
        bool eof = false;
        std::string line = input.Read(&eof);
        if (eof) break;
        std::string cmd = line;
        while (!cmd.empty() && cmd.back() == ' ') cmd.pop_back();
        while (!cmd.empty() && cmd.front() == ' ') cmd.erase(0, 1);
        Answer a;
        bool show = true;
        bool logIt = false;
        if (cmd.empty() || cmd == "/next" || cmd == "/more") {
            if (!hasLast) continue;
            offset += pageSize;
            if (offset >= lastA.total && lastA.kind == Answer::Kind::Rows) {
                Out(std::string("  ") + DIM + "That was the last page." + RESET + "\n\n");
                offset -= pageSize;
                continue;
            }
            a = e.Run(last, offset, pageSize);
        } else if (cmd == "/prev") {
            if (!hasLast) continue;
            offset = std::max<int64_t>(0, offset - pageSize);
            a = e.Run(last, offset, pageSize);
        } else if (cmd.rfind("/page ", 0) == 0) {
            if (!hasLast) continue;
            offset = std::max<int64_t>(0, (_atoi64(cmd.c_str() + 6) - 1) * pageSize);
            a = e.Run(last, offset, pageSize);
        } else if (cmd == "/all") {
            if (!hasLast) continue;
            offset = 0;
            a = e.Run(last, 0, 0);
        } else if (cmd == "/exit" || cmd == "/quit" || cmd == "exit" || cmd == "quit") {
            break;
        } else if (cmd == "/help" || cmd == "help" || cmd == "?") {
            Out(kConsoleHelp);
            Out("\n");
            continue;
        } else if (cmd.rfind("/list", 0) == 0) {
            PrintFamilies(e, cmd.size() > 6 ? cmd.substr(6) : "");
            continue;
        } else if (cmd == "/sql") {
            showSql = !showSql;
            Out(std::string("  ") + DIM + (showSql ? "SQL on" : "SQL off") + RESET + "\n\n");
            continue;
        } else if (cmd == "/clear") {
            Out("\x1b[2J\x1b[H");
            Banner(e);
            continue;
        } else if (cmd == "/good" || cmd.rfind("/bad", 0) == 0 || cmd.rfind("/note", 0) == 0) {
            std::string kind = cmd.substr(1, cmd.find(' ') == std::string::npos ? std::string::npos : cmd.find(' ') - 1);
            std::string why = cmd.find(' ') == std::string::npos ? "" : cmd.substr(cmd.find(' ') + 1);
            skqe::dbq::LogFeedback(kind, "q: " + lastQ + " | interpreted: " + lastA.interpreted + (why.empty() ? "" : " | " + why));
            Out(std::string("  ") + GREEN + "Saved." + RESET + DIM + " (it goes with your questions in " + U8(skqe::dbq::StateDir()) + "\\history.jsonl)" + RESET + "\n\n");
            continue;
        } else if (cmd == "/history") {
            std::ifstream f(skqe::dbq::StateDir() + L"\\history.jsonl", std::ios::binary);
            std::vector<std::string> qs;
            std::string l;
            while (std::getline(f, l)) {
                skqe::Value v;
                std::string err;
                if (skqe::ParseJson(l, &v, &err))
                    if (const skqe::Value* q = v.Find("q")) qs.push_back(q->AsString());
            }
            for (size_t i = qs.size() > 20 ? qs.size() - 20 : 0; i < qs.size(); ++i) Out("  " + std::string(DIM) + qs[i] + RESET + "\n");
            Out("\n");
            continue;
        } else if (cmd.rfind("/db", 0) == 0) {
            if (cmd.size() > 4) {
                std::vector<std::wstring> nd;
                std::string rest = cmd.substr(4);
                std::string cur;
                bool q = false;
                for (char c : rest) {
                    if (c == '"') q = !q;
                    else if (c == ' ' && !q) {
                        if (!cur.empty()) nd.push_back(FullPath(W(cur))), cur.clear();
                    } else cur += c;
                }
                if (!cur.empty()) nd.push_back(FullPath(W(cur)));
                std::string err;
                if (!e.Open(nd, &err)) {
                    Out(std::string("  ") + RED + err + RESET + "\n\n");
                    e.Open(dbs, &err);
                    continue;
                }
                dbs = nd;
                CliState s;
                skqe::dbq::LoadState(&s);
                s.dbs = dbs;
                s.hasLast = false;
                skqe::dbq::SaveState(s);
                hasLast = false;
            }
            std::string p = DbLine(e.plugins(), "plugins  "), a2 = DbLine(e.assets(), "assets   ");
            if (!p.empty()) Out("  " + p + "\n");
            if (!a2.empty()) Out("  " + a2 + "\n");
            Out("\n");
            continue;
        } else if (cmd == "/csv" || cmd.rfind("/csv ", 0) == 0 || cmd == "/json") {
            if (!hasLast) continue;
            Answer all = e.Run(last, 0, 0);
            if (cmd.size() > 5) {
                FILE* f = nullptr;
                if (_wfopen_s(&f, W(cmd.substr(5)).c_str(), L"wb") == 0 && f) {
                    std::string csv;
                    for (size_t c = 0; c < all.table.columns.size(); ++c) csv += (c ? "," : "") + CsvField(all.table.columns[c]);
                    csv += "\n";
                    for (const auto& r : all.table.rows) {
                        for (size_t c = 0; c < r.size(); ++c) csv += (c ? "," : "") + CsvField(r[c]);
                        csv += "\n";
                    }
                    fwrite(csv.data(), 1, csv.size(), f);
                    fclose(f);
                    Out(std::string("  ") + GREEN + "Wrote " + Grouped(static_cast<int64_t>(all.table.rows.size())) + " rows to " + cmd.substr(5) + RESET + "\n\n");
                }
            } else if (cmd == "/json") {
                PrintJson(all);
            } else {
                PrintCsv(all);
            }
            continue;
        } else if (!cmd.empty() && cmd.find_first_not_of("0123456789") == std::string::npos && !choices.empty()) {
            size_t i = static_cast<size_t>(atoi(cmd.c_str()));
            if (i < 1 || i > choices.size()) {
                Out(std::string("  ") + DIM + "Pick 1 to " + std::to_string(choices.size()) + "." + RESET + "\n\n");
                continue;
            }
            offset = 0;
            a = e.Run(choices[i - 1], 0, pageSize);
            logIt = true;
        } else {
            std::string taughtMsg;
            if (e.Teach(cmd, &taughtMsg)) {
                Answer t;
                t.kind = Answer::Kind::Info;
                t.message = taughtMsg;
                skqe::dbq::LogQuestion("console", cmd, t, dbs);
                Out(std::string("  ") + GREEN + taughtMsg + RESET + "\n\n");
                continue;
            }
            offset = 0;
            a = e.Ask(cmd, hasLast ? &last : nullptr, 0, pageSize);
            logIt = true;
        }
        if (logIt) skqe::dbq::LogQuestion("console", cmd, a, dbs);
        if (show) {
            PrintAnswer(a, showSql);
            if (a.kind == Answer::Kind::Rows && a.offset + static_cast<int64_t>(a.table.rows.size()) < a.total)
                Out(std::string("  ") + DIM + "Enter for more" + (g.unicode ? " \xC2\xB7 " : " - ") + "/bad if this is wrong" + RESET + "\n");
            char ms[32];
            snprintf(ms, sizeof ms, "%.0f ms", a.ms);
            Out(std::string("  ") + DIM + ms + RESET + "\n\n");
        }
        if (a.kind == Answer::Kind::Ask || a.kind == Answer::Kind::Unknown) {
            choices = a.choiceInterps;
        } else if (a.kind != Answer::Kind::Error) {
            choices = a.choiceInterps;
            last = a.interp;
            hasLast = !last.family.empty();
            lastA = a;
            if (logIt) lastQ = cmd;
        }
    }
    return 0;
}

}  // namespace

extern const char kHelpDbq[];
const char kHelpDbq[] = R"HELP(skqe dbq ["question"] [options]     ask a database a question in plain English
skqe db use <file.db> [<file.db>]  remember the database(s) dbq asks (plugins and/or assets)

  skqe dbq                       the interactive console (paging, follow-ups, /help)
  skqe dbq "which meshes use femalehead.dds"
  skqe dbq --next | --prev | --page N | --all     page through the last answer
  skqe dbq "only missing"        narrow the last question (follow-ups work here too)
  skqe dbq 2                     pick choice 2 of the last answer
  skqe dbq --list [word]         every question it knows, with an example of each

  --db <file.db>                 use this database for this question (repeat for both)
  --limit N                      rows per page (default 10)
  --json | --csv                 the whole answer, for scripts (no paging state)
  --sql                          also print the SQL that ran
  --new                          don't treat this as a follow-up
  --ascii                        plain + - | lines instead of box drawing
  --selftest                     run every family's tests against the database(s)

The answers come from tested SQL templates (dbq_families.json next to skqe.exe),
never from generated SQL. When a question or a name is ambiguous, it asks.
Every question you ask is saved in %LOCALAPPDATA%\skqe\dbq\history.jsonl so
the wording can be tuned; nothing leaves your PC.
)HELP";

int DbUseCommand(int argc, wchar_t** argv);
int DbqCommand(int argc, wchar_t** argv);

int DbUseCommand(int argc, wchar_t** argv) {
    CliState s;
    skqe::dbq::LoadState(&s);
    std::vector<std::wstring> dbs;
    bool clear = false;
    for (int i = 3; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--clear") clear = true;
        else dbs.push_back(FullPath(a));
    }
    if (clear) {
        s.dbs.clear();
        s.hasLast = false;
        skqe::dbq::SaveState(s);
        printf("cleared\n");
        return 0;
    }
    if (dbs.empty()) dbs = s.dbs;
    if (dbs.empty()) {
        printf("No database set. skqe db use <plugins.db> [<assets.db>]\n");
        return 0;
    }
    Engine e;
    if (!OpenEngine(e, dbs)) return 1;
    s.dbs = dbs;
    s.hasLast = false;
    s.choices.clear();
    skqe::dbq::SaveState(s);
    SetupConsole();
    std::string p = DbLine(e.plugins(), "plugins  "), a = DbLine(e.assets(), "assets   ");
    if (!p.empty()) printf("%s\n", p.c_str());
    if (!a.empty()) printf("%s\n", a.c_str());
    return 0;
}

int DbqCommand(int argc, wchar_t** argv) {
    std::vector<std::wstring> dbs;
    std::string question, listFilter;
    bool next = false, prev = false, all = false, json = false, csv = false, sql = false, fresh = false, list = false, selftest = false,
         explain = false;
    int64_t page = -1, limit = 10;
    for (int i = 2; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--db" && i + 1 < argc) dbs.push_back(FullPath(argv[++i]));
        else if (a == L"--next") next = true;
        else if (a == L"--prev") prev = true;
        else if (a == L"--page" && i + 1 < argc) page = _wtoi64(argv[++i]);
        else if (a == L"--all") all = true;
        else if (a == L"--json") json = true;
        else if (a == L"--csv") csv = true;
        else if (a == L"--sql") sql = true;
        else if (a == L"--new") fresh = true;
        else if (a == L"--ascii") g.unicode = false;
        else if (a == L"--limit" && i + 1 < argc) limit = std::max<int64_t>(1, _wtoi64(argv[++i]));
        else if (a == L"--list") {
            list = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') listFilter = U8(argv[++i]);
        } else if (a == L"--selftest") selftest = true;
        else if (a == L"--explain") explain = true;
        else question += (question.empty() ? "" : " ") + U8(a);
    }
    CliState st;
    skqe::dbq::LoadState(&st);
    bool stateless = !dbs.empty() || json || csv;
    if (dbs.empty()) dbs = st.dbs;
    SetupConsole();
    Engine e;
    if (!OpenEngine(e, dbs)) return 2;
    if (list) {
        PrintFamilies(e, listFilter);
        return 0;
    }
    if (selftest) {
        int fails = 0, run = 0;
        for (const auto& l : e.SelfTest(&fails, &run)) Out(l + "\n");
        Out(std::to_string(run - fails) + " of " + std::to_string(run) + " tests passed\n");
        return fails ? 1 : 0;
    }
    if (explain) {
        for (const auto& l : e.Explain(question)) Out(l + "\n");
        return 0;
    }
    bool interactive = question.empty() && !next && !prev && !all && page < 0;
    if (interactive) return RunConsole(e, dbs, limit);

    const bool sameConsole = st.hasLast && st.console == ConsoleKey() && NowSec() - st.time < 30 * 60;
    Answer a;
    std::string logged = question;
    int64_t pageSize = (json || csv || all) ? 0 : limit;
    if (next || prev || page >= 0 || (all && question.empty())) {
        if (!sameConsole) {
            fprintf(stderr, "There is no recent question in this window to page through. Ask one first.\n");
            return 1;
        }
        int64_t off = st.offset;
        if (next) off += st.pageSize;
        if (prev) off = std::max<int64_t>(0, off - st.pageSize);
        if (page >= 1) off = (page - 1) * limit;
        if (all) off = 0;
        a = e.Run(st.last, off, pageSize);
        if (a.kind == Answer::Kind::Rows && off >= a.total && a.total > 0) {
            printf("That was the last page.\n");
            return 0;
        }
        st.offset = off;
        logged = "";
    } else if (!question.empty() && question.find_first_not_of("0123456789") == std::string::npos && sameConsole && !st.choices.empty()) {
        size_t i = static_cast<size_t>(atoi(question.c_str()));
        if (i < 1 || i > st.choices.size()) {
            fprintf(stderr, "Pick 1 to %zu.\n", st.choices.size());
            return 1;
        }
        a = e.Run(st.choices[i - 1], 0, pageSize);
        st.offset = 0;
    } else {
        std::string taughtMsg;
        if (e.Teach(question, &taughtMsg)) {
            Answer t;
            t.kind = Answer::Kind::Info;
            t.message = taughtMsg;
            skqe::dbq::LogQuestion("cli", question, t, dbs);
            printf("%s\n", taughtMsg.c_str());
            return 0;
        }
        a = e.Ask(question, (sameConsole && !fresh && !stateless) ? &st.last : nullptr, 0, pageSize);
        st.offset = 0;
    }
    if (!logged.empty()) skqe::dbq::LogQuestion("cli", logged, a, dbs);
    if (json) PrintJson(a);
    else if (csv) PrintCsv(a);
    else {
        PrintAnswer(a, sql);
        if (a.kind == Answer::Kind::Rows && a.offset + static_cast<int64_t>(a.table.rows.size()) < a.total)
            Out(std::string("  ") + DIM + "skqe dbq --next for more" + RESET + "\n");
        if (a.kind == Answer::Kind::Ask || (a.kind == Answer::Kind::Unknown && !a.choices.empty()))
            Out(std::string("  ") + DIM + "skqe dbq <number> to pick one" + RESET + "\n");
    }
    if (!stateless) {
        if (a.kind == Answer::Kind::Ask || a.kind == Answer::Kind::Unknown) {
            st.choices = a.choiceInterps;
            st.console = ConsoleKey();
            st.time = NowSec();
        } else if (a.kind != Answer::Kind::Error) {
            st.hasLast = !a.interp.family.empty();
            st.last = a.interp;
            if (!logged.empty()) st.lastQuestion = logged;
            st.pageSize = limit;
            st.console = ConsoleKey();
            st.time = NowSec();
            st.choices = a.choiceInterps;
        }
        skqe::dbq::SaveState(st);
    }
    return a.kind == Answer::Kind::Error ? 1 : 0;
}
