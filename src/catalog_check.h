// skqe catalog check: what a pattern catalogue does to a folder of plugins,
// and a baseline to compare a later catalogue or engine against.
//
// Every field of every record of every plugin is decoded (and every record
// header that has a pattern). Per plugin and key ("RT.FS"): occurrences, raw
// fallbacks (a pattern that does not fit), occurrences with bytes left after
// the pattern, occurrences with no pattern, and a SHA-256 over the decoded
// JSON of every occurrence in file order.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "pattern_engine.h"
#include "pattern_value.h"

namespace skqe {

struct KeyStats {
    uint64_t n = 0;       // occurrences
    uint64_t raw = 0;     // the pattern did not fit: the field stays raw
    uint64_t left = 0;    // decoded, with bytes after the pattern
    uint64_t nopat = 0;   // no pattern for the key
    std::string sha;      // hex SHA-256 over every occurrence's decoded JSON, in file order
};

struct PluginStats {
    std::string error;                     // the plugin could not be read
    uint64_t records = 0;
    std::map<std::string, KeyStats> keys;  // "RT.FS" (or "RT.Record Header")
};

struct CheckRun {
    std::map<std::string, PluginStats> plugins;  // by file name
};

// Decodes every plugin (*.esm, *.esp, *.esl) in `folder`.
CheckRun CheckPlugins(const PatternCatalog& cat, const std::wstring& folder, int threads);

// The run as a baseline document, and back.
Value BaselineJson(const CheckRun& run);
bool BaselineFromJson(const Value& doc, CheckRun* run, std::string* error);

// Differences of `now` from `base` that are errors: a key's raw-fallback
// count rose, or its hash changed in a plugin (unless the key is in
// `expectChanged`); a key in `expectChanged` whose hash changed nowhere.
std::vector<std::string> CompareBaseline(const CheckRun& base, const CheckRun& now,
                                         const std::vector<std::string>& expectChanged);

}  // namespace skqe
