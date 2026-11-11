// The record grammar of a format 2 catalogue ("_records"): which fields a
// record type has, in which order, which repeat, which form groups (an
// instance opened by its leader field), and which fields count others.
//
// Decoding never depends on it. It places each field of a record (a slot in
// the grammar and the group instance it belongs to), reports what does not
// fit, and checks the count fields. One pass, no backtracking, never fails.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pattern_value.h"

namespace skqe {

struct Shape;

struct GrammarItem {
    bool isGroup = false;
    std::vector<std::string> sigs;  // a field item: the signatures that share this slot
    std::string group;              // a group item: its name
    int min = 0;                    // 0 or 1
    bool many = false;              // max "*" (else 1)
    bool unordered = false;         // a group whose members may come in any order
    std::string label;              // free text (field_order.csv's group column)
    std::vector<GrammarItem> fields;  // a group's items
};

struct GrammarCountRule {
    std::string text;                // as written: "CITC.condition_count", or several joined by "+"
    std::vector<std::string> paths;  // "SIG" or "SIG.member.path"
    std::string of;                  // occurrences, groups, elements, bytes
    std::string what;                // the signature or group name counted
    int64_t per = 1;                 // bytes: per N bytes
    bool create = false;             // a writer adds the count field when it is missing
};

struct GrammarDiag {
    std::string code;  // missing, too_many, out_of_order, unknown_field
    std::string what;  // the field signature (or the missing item: signature or group name)
    std::string in;    // missing: the group instance path
    std::optional<size_t> at;  // the index of the field being placed; none for what the record's end found
};

struct GrammarPlacement {
    std::optional<std::string> slot;  // index path into the grammar ("10/1"); none when unplaced
    std::string group;                // instance path ("quests[1]"), "" at record level
};

struct GrammarCount {
    std::string count;
    std::optional<int64_t> stored;  // none when the count field is absent
    int64_t actual = 0;
    bool ok = true;
    std::string code;  // count_mismatch, count_missing
};

class RecordGrammar {
public:
    // Reads one record type's grammar. `shapeOf(sig)` gives the shape of that
    // field's pattern (nullptr: no pattern), for checking count paths. False
    // with *error (an E030 message) when it is not a valid grammar.
    using ShapeLookup = std::function<const Shape*(const std::string& sig)>;
    bool Parse(const Value& json, const ShapeLookup& shapeOf, std::string* error);

    // Places each field (by signature, in file order).
    void Match(const std::vector<std::string>& sigs, std::vector<GrammarPlacement>* placed,
               std::vector<GrammarDiag>* diags) const;
    // The count checks. `values[i]` is field i's decoded value (nullptr when
    // it did not decode), `sizes[i]` its size in bytes.
    std::vector<GrammarCount> Counts(const std::vector<std::string>& sigs, const std::vector<const Value*>& values,
                                     const std::vector<size_t>& sizes, const std::vector<GrammarPlacement>& placed) const;

    bool HasSig(const std::string& sig) const { return allSigs_.count(sig) != 0; }
    // Signatures that may occur more than once in a record: an item with max
    // "*", inside a group with max "*", or in more than one item.
    const std::set<std::string>& MultiSigs() const { return multi_; }
    const std::vector<GrammarItem>& items() const { return items_; }
    const std::vector<GrammarCountRule>& counts() const { return counts_; }

private:
    std::vector<GrammarItem> items_;
    std::vector<GrammarCountRule> counts_;
    std::set<std::string> allSigs_, multi_, groups_;
};

}  // namespace skqe
