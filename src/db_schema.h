// build-db's per-record-type tables, generated from the pattern catalog.
//
//   <rt>            one row per occurrence of a form of that type (form_key,
//                   plugin_no), one column per leaf of each subrecord that
//                   occurs once: weap.data_damage, weap.full, npc.acbs_level
//   <rt>_<fs>       child table, one row per item (form_key, plugin_no, idx):
//                   for subrecords that repeat within a record (npc_snam,
//                   lvli_lvlo) and for lists of FormIDs inside one (weap_kwda)
//
// FormIDs are form_key integers; localized strings are their text; lists
// that hold no FormID, and anything nested deeper, are JSON text.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "pattern_engine.h"

namespace spf {

struct PathStep {
    bool index = false;
    std::string key;  // map member
    size_t pos = 0;   // list position
};

struct ColumnDef {
    std::string name;     // SQL column name
    std::string sqlType;  // INTEGER / REAL / TEXT, empty = any
    std::string kind;     // int real text bool formid lstring marker flags json any
    std::string path;     // spf export path: "DATA.damage", "SNAM.Faction"
    std::vector<PathStep> steps;  // from the subrecord's value (or the list element)
};

struct TableDef {
    std::string name;
    std::string rt, fs;  // fs empty = the record type's own table
    std::string description;
    std::vector<ColumnDef> columns;  // after form_key, plugin_no (and idx)
};

struct SubrecordDef {
    int table = -1;
    bool child = false;
    bool elementRows = false;  // child rows = the elements of the decoded list
    size_t firstColumn = 0, columnCount = 0;
    std::string module;  // non-empty: kept out of the core (e.g. "navmesh")
};

class Schema {
public:
    // `isUsableName` says whether a column name works unquoted in SQL.
    void Build(const PatternCatalog& catalog, bool (*isUsableName)(const std::string&));

    const std::vector<TableDef>& tables() const { return tables_; }
    const SubrecordDef* Find(const std::string& rt, const std::string& fs) const;
    int TypeTable(const std::string& rt) const;  // -1 = no patterns for that type

    // A record type or subrecord signature as an SQL name part ("NPC_" -> "npc").
    static std::string SigName(const std::string& sig);

private:
    std::vector<TableDef> tables_;
    std::unordered_map<std::string, SubrecordDef> subs_;  // "RT.FS"
    std::unordered_map<std::string, int> typeTables_;
};

}  // namespace spf
