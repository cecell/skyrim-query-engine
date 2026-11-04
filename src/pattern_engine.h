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
#include <set>
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

// The shape of what a pattern decodes to, worked out from the pattern alone
// (the decoder's output rules applied statically). build-db generates its
// tables from it.
struct Shape {
    enum class Kind {
        Unknown,  // a union whose arms disagree: any value
        Int, Real, Text, Bool,
        FormId,   // "[XXXXXXXX]"
        LString,  // "[LSTRING:0x...]", or text when not localized
        Marker,   // empty_marker: present or not
        Flags,    // bitfield: list of set flag names
        Bytes,    // raw byte run (hex text in exports)
        List,     // repeat: elem[0] is the element shape
        Map,      // labeled seq: fields by name
        Tuple,    // unlabeled seq / colour: a list, fields by position
    };
    Kind kind = Kind::Unknown;
    std::vector<std::pair<std::string, Shape>> fields;
    std::vector<Shape> elem;
};
Shape ShapeOf(const Pattern& pattern);

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
    // Every "<RT>.<SUBRECORD>" key, sorted.
    std::vector<std::string> Keys() const;
    // From the optional "_multi" list: the subrecord can occur more than once
    // in one record.
    bool IsMulti(const std::string& rt, const std::string& fs) const;
    // From the optional "_display" object: xEdit's name for it, or empty.
    std::string Display(const std::string& rt, const std::string& fs) const;
    // From the optional "_names" object: names for the items of an unlabeled
    // tuple at `where` in the subrecord's value ("" the value itself, "[]"
    // each list element), for "RT.FS" or else "*.FS". nullptr = none.
    const std::vector<std::string>* Names(const std::string& rt, const std::string& fs, const std::string& where) const;

private:
    std::unordered_map<std::string, std::shared_ptr<const Pattern>> byKey_;
    std::unordered_map<std::string, std::string> lstringClass_;
    std::unordered_map<std::string, std::string> display_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> names_;
    std::set<std::string> multi_;
};

}  // namespace spf
