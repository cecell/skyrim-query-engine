// Edits on a Document, as data. One JSON-shaped op goes in, one structured
// result comes out, so the CLI, a script and a future UI share one path.
//
//   {"op":"set","form":"Skyrim.esm:012EB7","field":"DATA.damage","value":9}
//
// What every op promises: bytes it was not asked to change stay as they were,
// and a failed op leaves the document exactly as it found it. See OpHelp() for
// the op list.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "edit/document.h"
#include "pattern_engine.h"
#include "strings.h"

namespace skqe::edit {

// The order the Creation Kit writes a type's subrecords (data/field_order.csv).
class FieldOrder {
public:
    bool Load(const std::string& csvText);
    struct Entry {
        Sig sig;
        int position;
        bool extraList;
        bool always;  // the Creation Kit writes it unconditionally
    };
    const std::vector<Entry>* Of(const std::string& recordType) const;

private:
    std::map<std::string, std::vector<Entry>> byType_;
};

// The catalog ledger's open rows (data/catalog_open.csv).
class CatalogOpen {
public:
    bool Load(const std::string& csvText);
    struct Row {
        int offset;  // -1: anywhere in the subrecord
        int size;    // -1: to the end
        std::string correction;
    };
    // Open rows of this field that overlap the byte range.
    std::vector<Row> Overlapping(const std::string& rt, const std::string& fs, size_t offset, size_t size) const;

private:
    std::map<std::string, std::vector<Row>> byKey_;
};

struct EditData {
    const PatternCatalog* catalog = nullptr;
    FieldOrder order;
    CatalogOpen open;
};

// Loads field_order.csv and catalog_open.csv from a folder. The order file is
// required, the ledger optional (an edit then has no catalog warnings).
bool LoadEditData(const std::wstring& folder, const PatternCatalog* catalog, EditData* out, std::string* error);

struct Change {
    std::string form;   // Owner.esm:XXXXXX
    std::string what;   // "set", "add-sub", ...
    std::string field;  // what it touched
    std::string before, after;  // JSON values, when they mean something
};

struct OpResult {
    bool ok = false;
    std::string code;     // on failure: a stable name, e.g. "not_found", "layout_dependency"
    std::string message;  // on failure: why
    std::vector<std::string> warnings;
    std::vector<Change> changes;
    std::string newForm;  // add-record: the form it made
    std::vector<std::string> possibleMisses;  // add-master, copy: values that look like FormIDs the catalog does not mark
};

struct SaveOptions {
    bool overwrite = false;  // replace an existing output file (never the input)
};

class Editor {
public:
    Editor(Document* doc, const EditData* data);

    OpResult Apply(const Value& op);

    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    bool Undo();
    bool Redo();
    bool Dirty() const { return doc_->rootPtr() != original_; }

    // Writes to a temp file beside `path`, re-reads it, then renames it into
    // place. Refuses: the input itself (by file ID, so hard links count), any
    // Skyrim Special Edition\Data folder, an existing file unless overwrite.
    bool Save(const std::wstring& path, const SaveOptions& opt, std::string* error);

    // Every value of a record, with byte ranges. For inspection.
    OpResult Describe(const std::string& form, std::string* json);

    const std::vector<std::string>& masters() const { return masters_; }
    float version() const { return version_; }
    bool localized() const { return localized_; }

private:
    struct Impl;
    friend struct Impl;
    Document* doc_;
    const EditData* data_;
    NodePtr original_;
    std::vector<NodePtr> undo_, redo_;
    std::vector<std::string> masters_;
    float version_ = 0;
    bool localized_ = false;
    std::map<std::wstring, std::unique_ptr<Document>> sources_;  // plugins copied from
    std::map<std::string, std::unique_ptr<StringTables>> sourceStrings_;  // their text, by plugin name + language
    void LoadHeaderFacts();
};

// Corpus self-check of one plugin, read-only.
struct SelfCheck {
    std::string refused;
    uint64_t records = 0, subs = 0, leaves = 0, undecoded = 0;
    // The tracer against the decoder.
    uint64_t traceDiffers = 0, leafOutOfRange = 0, leafPathWrong = 0;
    std::string firstProblem;
    // Encoding what decode shows, per leaf type.
    struct Enc {
        uint64_t total = 0, identical = 0, sameDisplay = 0, differs = 0, refused = 0;
        std::string example;
    };
    std::map<std::string, Enc> encode;
    // Group placement, against the rules the writer uses for new records.
    uint64_t topOutOfOrder = 0, cellsChecked = 0, cellGroupWrong = 0, childGroupWrong = 0, orphanGroups = 0;
    std::string firstPlacement;
};
SelfCheck RunSelfCheck(const std::wstring& path, const PatternCatalog& catalog);

const char* OpHelp();

// One result as JSON.
std::string ResultJson(const OpResult& r);

}  // namespace skqe::edit
