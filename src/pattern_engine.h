// Decodes Skyrim SE subrecords into named fields using the pattern DSL of
// working_patterns.json. A C++ port of the Rust pattern-engine crate the
// reference database is built with; its output values match that engine's
// ParsedValue exactly (formids as "[XXXXXXXX]", localized strings as
// "[LSTRING:0xXXXXXXXX]", byte runs as upper-case hex).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "pattern_value.h"

namespace spf {

class PatternError : public std::runtime_error {
public:
    // Overread is the one error an open-ended repeat treats as its end.
    PatternError(bool overread, const std::string& what) : std::runtime_error(what), overread_(overread) {}
    bool overread() const { return overread_; }

private:
    bool overread_;
};

struct Pattern;

// One decode's context, bound at the root scope as $@RT, $@FSIG, ...
struct DecodeContext {
    std::string rt;       // record signature
    std::string fsig;     // subrecord signature, or "Record Header"
    int64_t formVersion = 0;
    bool localized = true;
    std::optional<int64_t> epft;  // PERK only: first byte of the last EPFT
};

// Compiles a pattern from its JSON form. Throws PatternError on a bad shape.
std::shared_ptr<const Pattern> CompilePattern(const Value& json);

// Decodes one subrecord payload. Throws PatternError when the bytes do not
// fit the pattern.
Value DecodeSubrecord(const Pattern& pattern, const uint8_t* data, size_t size, const DecodeContext& ctx);

// The pattern catalog: "<RT>.<SUBRECORD>" -> compiled pattern, plus the
// "<RT>.Record Header" pseudo-keys.
class PatternCatalog {
public:
    // Loads working_patterns.json: an object of key -> string, each string
    // itself JSON (or a bare primitive name). Keys starting with "_" are
    // skipped.
    bool Load(const std::string& jsonText, std::string* error);
    const Pattern* Find(const std::string& rt, const std::string& fs) const;
    size_t size() const { return byKey_.size(); }
    // Replaces (or adds) one pattern; `json` as in the file.
    bool Override(const std::string& key, const std::string& json, std::string* error);
    // "strings", "dlstrings" or "ilstrings" for a localized field (from the
    // optional "_lstrings" object), or empty.
    std::string LstringClass(const std::string& rt, const std::string& fs) const;

private:
    std::unordered_map<std::string, std::shared_ptr<const Pattern>> byKey_;
    std::unordered_map<std::string, std::string> lstringClass_;
};

}  // namespace spf
