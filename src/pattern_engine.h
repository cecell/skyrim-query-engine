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

namespace skqe {

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
    std::string fsig;     // field signature, or "Record Header"
    int64_t formVersion = 0;
    bool localized = true;
    // The record's context variables as the fields before this one left
    // them, each bound as $@name. A RecordContext fills them.
    std::vector<std::pair<std::string, Value>> vars;
    // $@Prev, the signature of the field before this one ("" for the first).
    // Format 2 only: unset, $@Prev does not exist.
    std::optional<std::string> prev;
};

// A context variable: its value before the record's first field, and what
// the fields with a given signature do to it once they have been processed.
struct ContextAction {
    enum class Kind : uint8_t { Set, Read } kind = Kind::Set;
    Value set;           // Set: the new value
    std::string read;    // Read: uint8 uint16 uint32 int8 int16 int32 char zstring
    size_t at = 0;       // Read: offset in the field
    std::optional<int64_t> mask;
    bool lower = false;  // char / zstring: lower-case A-Z
};
struct ContextVar {
    std::string name;
    Value init;
    std::vector<std::pair<std::string, ContextAction>> on;  // field signature -> action
};

// Compiles a pattern from its JSON form. Throws PatternError on a bad shape.
// `format2`: with the features of catalogue format 2 (range case keys,
// read-ahead at an offset, ...); without it, as format 1.
std::shared_ptr<const Pattern> CompilePattern(const Value& json);
std::shared_ptr<const Pattern> CompilePattern(const Value& json, bool format2);
// `unnamedShow`: the catalogue's "_options": {"unnamed_bits": "show"}.
std::shared_ptr<const Pattern> CompilePattern(const Value& json, bool format2, bool unnamedShow);
// Format 2 syntax helpers, shared by the compiler and the catalogue check.
bool ParseCaseRange(const std::string& key, std::optional<int64_t>* lo, std::optional<int64_t>* hi);
bool ParsePeek(const std::string& name, int* width, size_t* offset);

// Decodes one subrecord payload. Throws PatternError when the bytes do not
// fit the pattern. `consumed`, when given, gets where the pattern stopped:
// bytes from there to `size` are not in the value.
Value DecodeSubrecord(const Pattern& pattern, const uint8_t* data, size_t size, const DecodeContext& ctx,
                      size_t* consumed = nullptr);

// One decoded value and where its bytes are. `path` follows the decoded value
// the way Select does ("damage", "[2].level", "" for the value itself).
struct TraceLeaf {
    std::string path;
    size_t offset = 0;
    size_t size = 0;
    const Pattern* pattern = nullptr;  // null for a list's count prefix ("#count")
    Value value;
    bool bound = false;  // kept under a name later nodes can read: changing it can change their layout
};

// DecodeSubrecord, also reporting every value's byte range. The same code does
// both, so the two cannot disagree. Bytes a `skip` passes and bytes past the
// end of the pattern belong to no leaf.
Value TraceSubrecord(const Pattern& pattern, const uint8_t* data, size_t size, const DecodeContext& ctx,
                     std::vector<TraceLeaf>* leaves);

// The bytes for a new value of one leaf. `cur` is what the leaf holds now
// (flags keep the bits nobody named). Inputs, by type: numbers (also in a
// string, decimal or 0x...), text as UTF-8, enum names, flag names as a list or
// {"add":[...],"remove":[...]}, hex for byte runs; any type takes "raw:<hex>"
// for exact bytes. False with *err when the value does not fit. A FormID is the
// stored number: resolving Owner.esm:XXXXXX is the caller's.
bool EncodeLeaf(const Pattern& leaf, const Value& input, const uint8_t* cur, size_t curSize, bool localized,
                std::vector<uint8_t>* out, std::string* err);
bool LeafIsFormId(const Pattern& leaf);
bool LeafIsByteRun(const Pattern& leaf);
// The leaf's type, as the catalog names it: "uint32", "formid", "flags", "bytes"...
std::string LeafTypeName(const Pattern& leaf);
// The pattern is one value, not a structure: a whole subrecord can be encoded from it.
bool PatternIsLeaf(const Pattern& pattern);

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

class PatternCatalog;

// Walks one record's fields in order and keeps its context variables. For
// every field, pattern or not, wanted or not (never for XXXX): Before() gives
// the context to decode or encode it with, then After() with the bytes as
// they are (or will be) in the file updates the variables from them.
class RecordContext {
public:
    RecordContext(const PatternCatalog& cat, std::string rt, int64_t formVersion, bool localized);
    const DecodeContext& Before(const std::string& fsig);
    void After(const std::string& fsig, const uint8_t* data, size_t size);
    // The signature of the field before the next one ("" before the first).
    const std::string& Prev() const { return prev_; }

private:
    const std::vector<ContextVar>* decl_;
    DecodeContext ctx_;
    std::string prev_;
    bool format2_ = false;
};

// True for a primitive type name ("uint8", "formid", ...); "bytes" is not one.
bool IsPrimitiveName(const std::string& name);

// An error or warning about the catalogue: a code (E0xx, W1xx), the key or
// section, a path into the pattern ("[5].do[1]"), and a message.
struct CatalogFinding {
    std::string code, key, path, message;
    std::string Text() const;  // "KEY: PATH: CODE message"
};

// The pattern catalog: "<RT>.<SUBRECORD>" -> compiled pattern, plus the
// "<RT>.Record Header" pseudo-keys.
//
// Format 1 (no "$format" member): an object of key -> string, each string
// itself JSON (or a bare primitive name); "_multi", "_lstrings", "_display"
// and "_names" are read, other keys starting with "_" and values that are
// not strings are skipped, a repeated key takes the last value.
// Format 2 ("$format": "spf-patterns/2"): every member is checked, patterns
// may be JSON values, and the first error stops the load ("KEY: PATH: E0xx
// message"); warnings are collected.
class PatternCatalog {
public:
    struct LoadOptions {
        bool strict = false;  // check a format 1 file as if it had the format 2 marker
    };
    bool Load(const std::string& jsonText, std::string* error);
    bool Load(const std::string& jsonText, std::string* error, const LoadOptions& options);
    int format() const { return format_; }
    const std::vector<CatalogFinding>& warnings() const { return warnings_; }
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
    // The context variables of a record type.
    const std::vector<ContextVar>& ContextVars(const std::string& rt) const;

    // Format 2 sections, as loaded (null when absent).
    const Value& Shared() const { return shared_; }
    const Value& Records() const { return records_; }
    const Value& Options() const { return options_; }
    const Value& Meta() const { return meta_; }
    // The variables "_context" declares for a record type (format 2).
    const std::vector<ContextVar>& DeclaredContext(const std::string& rt) const;

private:
    void Clear();
    bool LoadFormat1(const Value& root, std::string* error);
    bool LoadFormat2(const Value& root, const std::string& duplicate, std::string* error);
    bool LoadContext(const Value& section, std::string* error);

    int format_ = 1;
    std::vector<CatalogFinding> warnings_;
    std::unordered_map<std::string, std::shared_ptr<const Pattern>> byKey_;
    std::unordered_map<std::string, std::string> lstringClass_;
    std::unordered_map<std::string, std::string> display_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> names_;
    std::set<std::string> multi_;
    Value shared_, records_, options_, meta_;
    std::map<std::string, std::vector<ContextVar>> context_;
};

}  // namespace skqe
