// skqe's data files: patterns.json (the catalog), ut_reference.sql (the lookup
// tables build-db loads) and search_filters.txt (skqe search's rules).
//
// Where each is looked for, in order:
//   1. the folder the SKQE_DATA environment variable names (for development:
//      point it at the repository's data folder and an edit takes effect on
//      the next run, with no build);
//   2. next to the running exe;
//   3. the copy built into the exe, in builds configured with
//      SKQE_EMBED_DATA=ON (the releases), so the exe works on its own.
// An explicit path (skqe --patterns, BuildOptions::patterns) is the caller's
// business and comes before all of these.
#pragma once

#include <string>

namespace skqe {

// Reads one data file by name ("patterns.json"). `source` gets the path it
// came from, or "embedded". False when none of the places has it.
bool ReadDataFile(const std::wstring& name, std::string* text, std::wstring* source = nullptr);

// Where ReadDataFile would read `name` from, without reading it: a path,
// "embedded", or "" when it is nowhere.
std::wstring DataFileSource(const std::wstring& name);

// True when this build carries the data files inside it.
bool DataFilesEmbedded();

}  // namespace skqe
