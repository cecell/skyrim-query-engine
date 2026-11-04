// `spf build-db`: reads a load order and writes one SQLite database that any
// app opens with plain SQLite. Design: BACKLOG.md ("spf build-db").
//
// Every form is keyed by its natural key, computed from the bytes while
// reading: form_key = owner plugin_no * 2^24 + object ID (12-bit when the
// owner is light). Keys never depend on insert order, so plugins decode in
// parallel and one writer thread inserts whatever arrives.
#pragma once

#include <string>
#include <vector>

namespace spf {

struct BuildOptions {
    std::wstring out;                  // the .db to write (replaced when the build succeeds)
    std::wstring data;                 // the Data folder
    std::wstring loadOrderFile;        // plugins.txt; empty = see `plugins`
    std::vector<std::string> plugins;  // explicit load order; both empty = every plugin in `data`
    std::wstring patterns;             // the pattern catalog (patterns.json)
    std::vector<std::string> modules;  // optional modules (and off-by-default extras) to build too (--with)
    std::vector<std::string> skip;     // default extras not to build (--skip), e.g. fts_names
    std::string language = "english";
    int threads = 0;                   // 0 = one per logical core, minus the writer
    bool quiet = false;
};

// Returns 0 on success; prints progress to stderr unless quiet.
int BuildDb(const BuildOptions& opt, std::string* error);

struct AddOptions {
    std::wstring db;                   // a database build-db wrote
    std::vector<std::string> modules;  // to add
    std::wstring patterns;
    int threads = 0;
    bool quiet = false;
};

// spf db add: decodes only the modules' subrecords from the plugins the
// database was built from and fills their tables, in one transaction.
// Refuses when a plugin changed since the build (rebuild, or sync later).
int AddModules(const AddOptions& opt, std::string* error);

// The optional modules and what each holds.
std::vector<std::pair<std::string, std::string>> AvailableModules();

}  // namespace spf
