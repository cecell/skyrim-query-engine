// `skqe search`: finds files by name in a Data folder, optionally with a mod
// staging folder, including the files packed in BSAs.
//
// A result is one path. Its copies are the distinct files holding that path: a
// file hardlinked between Data and a mod is one copy found in two places, and an
// archive hardlinked between them is read once. Copies that merely have the same
// contents stay separate, since telling them apart would mean reading every file.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace skqe {

struct AssetSearchRoot {
    std::wstring path;  // scanned recursively; .bsa files directly in it are read
    std::string name;   // shown in results: "Data", or the mod's folder name
};

struct AssetSearchOptions {
    std::vector<AssetSearchRoot> roots;
    std::string pattern;  // case-insensitive. Text anywhere in the path, or with * and ?
                          // a wildcard over the whole path (the file name if it has no \ or /)
    int threads = 0;      // 0 = one per logical core
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
    std::vector<std::string> warnings;   // archives and folders that couldn't be read
};

bool SearchAssets(const AssetSearchOptions& opt, AssetSearchResult* result, std::string* error);

// Whether a lowercase path with backslashes matches a lowercase pattern.
bool AssetPathMatches(const std::string& path, const std::string& pattern);

// The immediate subfolders of a staging folder, one root per mod, sorted.
std::vector<AssetSearchRoot> StagingRoots(const std::wstring& staging);

}  // namespace skqe
