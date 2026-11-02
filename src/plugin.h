// A Skyrim SE plugin as nested objects:
//
//   Plugin            the file: header facts, masters, every record
//    └ Record         one form (or the TES4 header): type, FormID, flags, where it sits
//       └ Field       one subrecord occurrence, decoded by the pattern catalog
//
// Records are indexed when the plugin is opened (cheap: headers only); their
// fields are decoded on demand. FormIDs inside decoded values are shown as
// "Plugin.esm:XXXXXX" (owner and object ID), localized strings as their text.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pattern_engine.h"
#include "strings.h"

namespace spf {

struct Record {
    std::string type;        // "WEAP", "NPC_", ...
    uint32_t formId = 0;     // as stored in the file (high byte = master index)
    uint32_t flags = 0;      // record header flags (0x20 deleted, 0x40000 compressed, ...)
    uint16_t formVersion = 0;
    uint32_t parent = 0;     // stored FormID of the WRLD / CELL / DIAL it sits in, 0 = none
    size_t offset = 0;       // in the file
};

struct Field {
    std::string sig;   // subrecord signature, e.g. "DATA"
    Value value;       // decoded; raw hex string ("#...") when no pattern fits
    bool decoded = false;
};

class Plugin {
public:
    // `catalog` must outlive the plugin.
    static std::unique_ptr<Plugin> Open(const std::wstring& path, const PatternCatalog& catalog, std::string* error);
    ~Plugin();

    const std::string& name() const { return name_; }  // file name as given
    const std::vector<std::string>& masters() const { return masters_; }
    bool localized() const { return localized_; }
    const Record& header() const { return records_.front(); }  // TES4
    const std::vector<Record>& records() const { return records_; }

    const Record* Find(uint32_t formId) const;
    std::vector<const Record*> OfType(const std::string& type) const;

    // Every field of a record, in file order (repeated subrecords appear
    // repeatedly). Values have FormIDs and localized strings resolved.
    std::vector<Field> Fields(const Record& r) const;

    // "Skyrim.esm:012EB7" for a FormID stored in this plugin.
    std::string FormKey(uint32_t formId) const;

private:
    Plugin() = default;
    void Resolve(const std::string& rt, const std::string& fs, Value* v) const;

    std::string name_;
    std::wstring dir_;
    const PatternCatalog* catalog_ = nullptr;
    std::vector<std::string> masters_;
    bool localized_ = false;
    std::vector<Record> records_;
    std::vector<std::pair<uint32_t, size_t>> byFormId_;  // sorted, into records_
    StringTables strings_;
    // The mapped file.
    void* file_ = nullptr;
    void* mapping_ = nullptr;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

// Walks a decoded value by a dotted path: "damage", "scripts.name",
// "entries[2].level". A name applied to a list applies to every element.
// Returns every value found.
std::vector<Value> Select(const Value& v, const std::string& path);

}  // namespace spf
