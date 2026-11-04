// `spf build-assets`: catalogues every asset file of a Skyrim SE install (the
// Data folder, mod staging folders, MO2 instances, and the files packed in
// their BSAs) into its own SQLite database, assets.db. Design: ASSETS_DDL.md.
//
// One `files` row per copy of a file; header facts per type in detail tables
// keyed by file_id; `in_data` / `is_winner` say which copy the game sees.
// Folders are walked and files read in parallel; one thread writes.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace spf {

struct AssetOptions {
    std::wstring out;            // the .db to write (replaced when the build succeeds)
    std::wstring data;           // the game's Data folder (always scanned)
    std::wstring staging;        // staging root: each immediate subfolder is one mod
    std::wstring mo2;            // MO2 instance folder (ModOrganizer.ini, mods, profiles, overwrite)
    std::wstring profile;        // MO2 profile name
    std::wstring loadOrderFile;  // plugins.txt; empty = MO2 profile's, else every plugin in Data
    std::vector<std::string> skip;  // default extras not to build (nif_textures, indexes)
    bool hash = false;           // SHA-256 of every file (packed: of its unpacked bytes)
    int threads = 0;             // 0 = one per logical core, minus the writer
    bool quiet = false;
};

// Returns 0 on success; prints progress to stderr unless quiet.
int BuildAssets(const AssetOptions& opt, std::string* error);

// The extras (on by default, --skip to leave out) and what each is.
std::vector<std::pair<std::string, std::string>> AssetExtras();

namespace assets {

// Which archives the game loads, why, and in which order: those in the ini
// lists first (in list order), then those named after active plugins
// (<Plugin>.bsa, <Plugin> - Textures.bsa) in load order. An archive loads
// once, at its first claim. Names compare case-insensitively.
struct ArchiveLoad {
    std::string name;      // as in `archivesInData`
    std::string loadedBy;  // ini | plugin
    std::string plugin;    // the plugin that loads it ('' for ini)
    int rank = 0;          // 0 = loaded first; higher wins
};
std::vector<ArchiveLoad> PlanArchives(const std::vector<std::string>& archivesInData,
                                      const std::vector<std::string>& iniList,
                                      const std::vector<std::string>& loadOrder);

}  // namespace assets
}  // namespace spf
