// skqe dbq: plain-English questions over a skqe plugin database (build-db)
// and / or asset database (build-assets).
//
// A question is answered by tested SQL templates (data/dbq_families.json),
// never by generated SQL: entity lookup finds the names in the question,
// a deterministic matcher picks the question family and its options, the
// names are resolved against the database, and the family's template runs
// read-only. When the matcher or a name is ambiguous, the answer asks.
// See QUERY_SPEC.md (internal) for the design.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace skqe::dbq {

// One slot of an interpreted question, as typed and as resolved.
struct SlotValue {
    std::string name;   // slot name in the family ("texture", "plugin2")
    std::string kind;   // slot kind ("texture", "form", "number"...)
    std::string text;   // what the user typed for it
    int pick = -1;      // which of several matches the user chose (-1: all / one)
};

// What the engine understood: enough to run it again (paging, refining).
struct Interpretation {
    std::string family;
    std::vector<std::string> options;
    std::vector<SlotValue> slots;
    double score = 0;
    bool refined = false;  // built from the previous question
};

struct Table {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    std::vector<bool> numeric;  // per column: right-align
};

struct Answer {
    enum class Kind { Rows, Detail, Ask, Unknown, Error, Info };
    Kind kind = Kind::Error;
    std::string interpreted;          // "meshes using texture \"femalehead.dds\""
    std::string resolved;             // "252 textures named femalehead.dds"
    std::string note;                 // family note, ignored options, caveats
    std::string message;              // error / unknown / info text
    Table table;                      // the page of rows (or the detail pane)
    std::vector<std::pair<std::string, Table>> sections;  // detail panes: extra groups
    int64_t total = 0;                // rows in the whole answer
    int64_t countValue = -1;          // for "how many": the count
    std::string unit;                 // "meshes"
    int64_t offset = 0;               // first row of this page
    std::vector<std::string> choices; // Ask: numbered choices
    std::vector<Interpretation> choiceInterps;  // Ask: what each choice runs
    std::string sql;                  // what ran (for --sql)
    Interpretation interp;
    double ms = 0;
};

struct DbInfo {
    std::wstring path;
    std::string builtBy, builtAt, dataDir;
    int schema = 0;
    bool ok = false;
    std::string error;
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Loads data/dbq_families.json (SKQE_DATA, next to the exe, or built in).
    bool LoadFamilies(std::string* error);
    // Opens one or two databases read-only; which is which comes from
    // skqe_meta.built_by. Either may be empty.
    bool Open(const std::vector<std::wstring>& dbs, std::string* error);
    const DbInfo& plugins() const;
    const DbInfo& assets() const;

    // Answers a question. `last` (may be null) is the previous interpretation,
    // for follow-ups. pageSize rows from offset.
    Answer Ask(const std::string& question, const Interpretation* last, int64_t offset, int64_t pageSize);
    // A teaching line ("draw a connection between "person", "actor" -> NPC_",
    // "teach women = female"): learns the words, saves them (taught.json).
    // False when the line isn't one.
    bool Teach(const std::string& line, std::string* message);
    // Runs an interpretation again (paging, picking a choice).
    Answer Run(const Interpretation& in, int64_t offset, int64_t pageSize);
    // Every family: id, group, title, first wording, and whether the open dbs can answer it.
    struct FamilyInfo {
        std::string id, group, title, example, db;
        bool available = false;
    };
    std::vector<FamilyInfo> Families() const;
    // Runs every family's tests. Returns lines of results; *failures counts fails.
    std::vector<std::string> SelfTest(int* failures, int* run);
    // How the matcher scored a question: tokens, then the best families.
    std::vector<std::string> Explain(const std::string& question);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Local history of every question (%LOCALAPPDATA%\skqe\dbq\history.jsonl).
std::wstring StateDir();
void LogQuestion(const std::string& source, const std::string& question, const Answer& a,
                 const std::vector<std::wstring>& dbs);
void LogFeedback(const std::string& kind, const std::string& text);

// Remembered databases and the CLI's last question (state.json).
struct CliState {
    std::vector<std::wstring> dbs;
    bool hasLast = false;
    Interpretation last;
    std::string lastQuestion;
    int64_t offset = 0, pageSize = 10;
    std::string console;  // which console window it belongs to
    int64_t time = 0;
    std::vector<Interpretation> choices;  // the last answer's numbered choices
};
bool LoadState(CliState* s);
bool SaveState(const CliState& s);

std::string InterpToJson(const Interpretation& in);
bool InterpFromJson(const std::string& json, Interpretation* in);

}  // namespace skqe::dbq
