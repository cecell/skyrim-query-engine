// `skqe search`: finds files by name in a Data folder, optionally with a mod
// staging folder, including the files packed in BSAs.
//
// A result is one path. Its copies are the distinct files holding that path: a
// file hardlinked between Data and a mod is one copy found in two places, and an
// archive hardlinked between them is read once. Copies that merely have the same
// contents stay separate, since telling them apart would mean reading every file.
#pragma once

#include <cstdint>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace skqe {

struct AssetSearchRoot {
    std::wstring path;  // scanned recursively; .bsa files directly in it are read
    std::string name;   // shown in results: "Data", or the mod's folder name
};

// The folders each kind of file is looked for in, from search_filters.txt.
class SearchFilters {
public:
    bool LoadFile(const std::wstring& path, std::string* error);
    bool Parse(const std::string& text, std::string* error);
    // Whether a file (path relative to its root) passes the rules.
    bool Allows(const std::string& path) const;
    // If the search text names one kind of file, the only places it can be:
    // *folders (lowercase top-level names) and *dataRoot. False = can't say.
    bool Narrow(const std::string& pattern, std::vector<std::string>* folders, bool* dataRoot) const;
    size_t RuleCount() const { return rules_.size(); }
    size_t ExcludeCount() const { return excludes_.size(); }

private:
    struct Rule {
        enum Kind { Extension, Wildcard, Regex } kind = Extension;
        std::string text;  // lowercase: ".dds" or a wildcard
        std::regex regex;
        std::vector<std::string> folders;  // lowercase, each ending in a backslash
        bool dataRoot = false;
    };
    const Rule* RuleFor(const std::string& name, const std::string& lowerName) const;
    std::vector<Rule> rules_;
    std::vector<std::string> excludes_;  // lowercase names or wildcards
};

struct AssetSearchOptions {
    std::vector<AssetSearchRoot> roots;
    std::string pattern;  // case-insensitive. Text anywhere in the path; or with * and ?, a
                          // wildcard on the file name, or if it has a \ or / on the path
                          // from any folder down; or a regex if `regex` is set
    bool regex = false;   // the pattern is a regular expression, searched for in the whole path
    int threads = 0;      // 0 = one per logical core
    const SearchFilters* filters = nullptr;  // null = no rules
    bool bsaOnly = false;                    // only the files packed in BSAs
    // Set for winners: each path's one copy the game loads. Keys are lowercase
    // archive names, values their load rank, higher wins. Other archives aren't read.
    const std::unordered_map<std::string, int>* winnerRanks = nullptr;
};

struct AssetSearchCopy {
    std::string archive;          // the .bsa's file name; empty = a loose file
    std::vector<std::string> in;  // roots holding this file or archive
    int64_t size = -1;            // loose files only
};

struct AssetSearchFile {
    std::string path;
    std::vector<AssetSearchCopy> copies;
};

struct AssetSearchResult {
    std::vector<AssetSearchFile> files;  // sorted by path
    int64_t looseScanned = 0, packedScanned = 0, archivesRead = 0;
    int64_t hardlinksMerged = 0;         // places folded into a copy already found
    std::vector<std::string> narrowedTo; // the folders searched, if the text named a file type
    bool narrowedToDataRoot = false;
    std::vector<std::string> warnings;   // archives and folders that couldn't be read
};

bool SearchAssets(const AssetSearchOptions& opt, AssetSearchResult* result, std::string* error);

// Whether a lowercase path with backslashes matches a lowercase pattern.
bool AssetPathMatches(const std::string& path, const std::string& pattern);

// The immediate subfolders of a staging folder, one root per mod, sorted.
std::vector<AssetSearchRoot> StagingRoots(const std::wstring& staging);

}  // namespace skqe
