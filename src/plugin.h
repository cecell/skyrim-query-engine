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
#include <functional>
#include <memory>
#include <optional>
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

// Facts from the TES4 header record.
struct PluginHeader {
    float version = 0;          // HEDR: 0.94, 1.7, 1.71
    uint32_t numRecords = 0;    // HEDR: records + groups, as the CK counted them
    uint32_t nextObjectId = 0;  // HEDR
    uint32_t flags = 0;         // TES4 record flags
    bool esm = false;           // flag 0x1
    bool esl = false;           // flag 0x200
    bool localized = false;     // flag 0x80
    bool master = false;        // ESM flag or .esm/.esl extension (loads as a master)
    bool light = false;         // ESL flag or .esl extension (loads in the FE slot)
    std::string author;         // CNAM, UTF-8
    std::string description;    // SNAM, UTF-8
    std::vector<uint64_t> masterSizes;  // DATA after each MAST, same order as masters()
    uint32_t overrideCount = 0;         // ONAM: number of FormIDs listed
    std::optional<uint32_t> intv;       // INTV, when present
    std::optional<uint32_t> incc;       // INCC, when present
};

struct Field {
    std::string sig;   // subrecord signature, e.g. "DATA"
    Value value;       // decoded; raw hex string ("#...") when no pattern fits
    bool decoded = false;
};

class Plugin {
public:
    // `catalog` must outlive the plugin.
    static std::unique_ptr<Plugin> Open(const std::wstring& path, const PatternCatalog& catalog, std::string* error,
                                        const std::string& language = "english");
    // A plugin already in memory (copied; no strings tables are loaded).
    static std::unique_ptr<Plugin> FromMemory(const uint8_t* bytes, size_t size, const std::string& name,
                                              const PatternCatalog& catalog, std::string* error);
    ~Plugin();

    const std::string& name() const { return name_; }  // file name as given
    const std::vector<std::string>& masters() const { return masters_; }  // raw bytes (Windows-1252)
    std::vector<std::string> mastersUtf8() const;                         // the same names as UTF-8
    const PluginHeader& info() const { return info_; }
    bool localized() const { return localized_; }
    const Record& header() const { return records_.front(); }  // TES4
    const std::vector<Record>& records() const { return records_; }

    const Record* Find(uint32_t formId) const;
    std::vector<const Record*> OfType(const std::string& type) const;

    // Every field of a record, in file order (repeated subrecords appear
    // repeatedly). Values have FormIDs and localized strings resolved.
    // `formIds`, when given, replaces each FormID (stored, non-zero) instead
    // of the "Owner.esm:XXXXXX" text. It is called with the stored FormID and
    // its path in the subrecord ("SNAM.Faction", "KWDA"; list positions left
    // out).
    // `wanted`, when given, picks the subrecords to return (by signature).
    using FormIdMap = std::function<Value(uint32_t, const std::string&)>;
    using SigFilter = std::function<bool(const std::string&)>;
    std::vector<Field> Fields(const Record& r, const FormIdMap* formIds = nullptr,
                              const SigFilter* wanted = nullptr) const;

    // "Skyrim.esm:012EB7" for a FormID stored in this plugin.
    std::string FormKey(uint32_t formId) const;

    // A record's subrecord bytes, inflated when compressed (into `scratch`).
    bool Body(const Record& r, std::vector<uint8_t>* scratch, const uint8_t** body, size_t* size) const;

    const StringTables& strings() const { return strings_; }
    const uint8_t* bytes() const { return data_; }  // the whole file
    size_t size() const { return size_; }

private:
    Plugin() = default;
    bool Parse(std::string* error);
    void Resolve(const std::string& rt, const std::string& fs, const FormIdMap* formIds, Value* v,
                 std::string* path) const;

    std::string name_;
    std::wstring dir_;
    const PatternCatalog* catalog_ = nullptr;
    std::vector<std::string> masters_;
    bool localized_ = false;
    PluginHeader info_;
    std::vector<Record> records_;
    std::vector<std::pair<uint32_t, size_t>> byFormId_;  // sorted, into records_
    StringTables strings_;
    // The mapped file.
    void* file_ = nullptr;
    void* mapping_ = nullptr;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    bool mapped_ = false;          // data_ is a view of file_
    std::vector<uint8_t> owned_;   // FromMemory: the bytes
};

// Walks a decoded value by a dotted path: "damage", "scripts.name",
// "entries[2].level". A name applied to a list applies to every element.
// Returns every value found.
std::vector<Value> Select(const Value& v, const std::string& path);

}  // namespace spf
