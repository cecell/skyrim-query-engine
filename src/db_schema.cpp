#include "db_schema.h"

#include <cstdio>
#include <set>

namespace spf {
namespace {

using K = Shape::Kind;

// Subrecords kept out of the core tables: big geometry that belongs to an
// opt-in module (BACKLOG.md, modules).
const char* ModuleOf(const std::string& rt, const std::string& fs) {
    if (rt == "NAVI" || (rt == "NAVM" && fs == "NVNM")) return "navmesh";
    if (rt == "WRLD" && fs == "RNAM") return "wrld_large_refs";  // per-cell lists of large references
    if ((rt == "LAND" && (fs == "VHGT" || fs == "VNML" || fs == "VCLR" || fs == "ATXT" || fs == "VTXT" ||
                          fs == "BTXT")) ||
        (rt == "CELL" && (fs == "MHDT" || fs == "TVDT")) || (rt == "WRLD" && (fs == "MHDT" || fs == "OFST")))
        return "landscape";  // terrain, height maps, occlusion and LOD offsets
    return nullptr;
}

}  // namespace

const std::vector<std::pair<std::string, std::string>>& Schema::Modules() {
    static const std::vector<std::pair<std::string, std::string>> kModules = {
        {"navmesh", "navmesh geometry: NAVM.NVNM (vertices, triangles, edge and door links) and NAVI"},
        {"landscape", "terrain and height data: LAND VHGT/VNML/VCLR/ATXT/VTXT/BTXT, CELL MHDT/TVDT, WRLD MHDT/OFST"},
        {"wrld_large_refs", "worldspace large-reference lists: WRLD.RNAM"},
    };
    return kModules;
}

namespace {

std::string Clean(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c)) out += static_cast<char>(tolower(c));
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

// Names for an unlabeled tuple's items, from the catalog (_names).
void NameTuple(Shape* s, const std::vector<std::string>* names) {
    if (!names || s->kind != K::Tuple) return;
    for (size_t i = 0; i < s->fields.size() && i < names->size(); ++i) s->fields[i].first = (*names)[i];
}

bool HasFormId(const Shape& s) {
    if (s.kind == K::FormId || s.kind == K::Unknown) return s.kind == K::FormId;
    for (const auto& f : s.fields)
        if (HasFormId(f.second)) return true;
    for (const auto& e : s.elem)
        if (HasFormId(e)) return true;
    return false;
}

struct Columns {
    std::vector<ColumnDef>* out;
    std::set<std::string>* used;
    bool (*usable)(const std::string&);

    void Add(std::string name, const char* sqlType, const char* kind, const std::string& path,
             const std::vector<PathStep>& steps) {
        if (name.empty()) name = "value";
        if (isdigit(static_cast<unsigned char>(name[0]))) name = "f" + name;
        if (!usable(name)) name += "_";
        std::string unique = name;
        for (int n = 2; used->count(unique); ++n) unique = name + "_" + std::to_string(n);
        used->insert(unique);
        out->push_back({unique, sqlType, kind, path, steps});
    }

    // One column per leaf; lists (and anything below them) become JSON.
    void Flatten(const Shape& s, const std::string& name, const std::string& path, std::vector<PathStep> steps) {
        switch (s.kind) {
            case K::Map:
            case K::Tuple:
                for (size_t i = 0; i < s.fields.size(); ++i) {
                    PathStep step;
                    step.index = s.kind == K::Tuple;
                    step.key = s.fields[i].first;
                    step.pos = i;
                    steps.push_back(step);
                    // An item named "" (catalog _names) puts its fields
                    // directly under the parent; an unnamed one ("_3") gets
                    // its position.
                    const bool inlined = s.fields[i].first.empty();
                    std::string part = Clean(s.fields[i].first);
                    if (part.empty() && !inlined) part = std::to_string(i);
                    Flatten(s.fields[i].second, inlined ? name : name.empty() ? part : name + "_" + part,
                            s.kind == K::Tuple ? path + "[" + std::to_string(i) + "]" : path + "." + s.fields[i].first,
                            steps);
                    steps.pop_back();
                }
                if (s.fields.empty()) Add(name, "", "any", path, steps);
                break;
            case K::Int: Add(name, "INTEGER", "int", path, steps); break;
            case K::Real: Add(name, "REAL", "real", path, steps); break;
            case K::Text: Add(name, "TEXT", "text", path, steps); break;
            case K::Bool: Add(name, "INTEGER", "bool", path, steps); break;
            case K::FormId: Add(name, "INTEGER", "formid", path, steps); break;
            case K::LString: Add(name, "TEXT", "lstring", path, steps); break;
            case K::Marker: Add(name, "INTEGER", "marker", path, steps); break;
            case K::Flags: Add(name, "TEXT", "flags", path, steps); break;
            case K::List: Add(name, "TEXT", "json", path, steps); break;
            case K::Unknown: Add(name, "", "any", path, steps); break;
        }
    }
};

}  // namespace

std::string Schema::SigName(const std::string& sig) {
    std::string out;
    for (unsigned char c : sig) {
        if (isalnum(c)) {
            out += static_cast<char>(tolower(c));
        } else if (c < 0x20 || c >= 0x7F) {  // IMAD's control-byte signatures
            char buf[4];
            snprintf(buf, sizeof(buf), "x%02x", c);
            out += buf;
        } else {
            out += '_';
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

void Schema::Build(const PatternCatalog& catalog, bool (*isUsableName)(const std::string&)) {
    tables_.clear();
    subs_.clear();
    typeTables_.clear();
    std::vector<std::set<std::string>> used;
    std::set<std::string> tableNames = {"forms", "plugins", "plugin_masters", "strings"};
    auto uniqueTable = [&](std::string name) {
        if (!isUsableName(name)) name += "_";
        std::string u = name;
        for (int n = 2; tableNames.count(u); ++n) u = name + "_" + std::to_string(n);
        tableNames.insert(u);
        return u;
    };
    for (const std::string& key : catalog.Keys()) {
        size_t dot = key.find('.');
        std::string rt = key.substr(0, dot), fs = key.substr(dot + 1);
        if (fs == "Record Header") continue;
        const Pattern* pat = catalog.Find(rt, fs);
        SubrecordDef def;
        // A module's subrecords always get tables of their own, so building a
        // module later only fills its tables and never alters core ones.
        const char* module = ModuleOf(rt, fs);
        if (module) def.module = module;
        auto tt = typeTables_.find(rt);
        if (!module && tt == typeTables_.end()) {
            TableDef t;
            t.name = uniqueTable(SigName(rt));
            t.rt = rt;
            t.description = "One row per occurrence of a " + rt + " form.";
            tt = typeTables_.emplace(rt, static_cast<int>(tables_.size())).first;
            tables_.push_back(std::move(t));
            used.emplace_back();
        }
        Shape shape = ShapeOf(*pat);
        NameTuple(&shape, catalog.Names(rt, fs, ""));
        if (shape.kind == K::List && !shape.elem.empty()) NameTuple(&shape.elem[0], catalog.Names(rt, fs, "[]"));
        bool multi = catalog.IsMulti(rt, fs);
        bool topList = shape.kind == K::List;
        def.child = module || multi || (topList && HasFormId(shape.elem[0]));
        def.elementRows = topList && (multi || HasFormId(shape.elem[0]));
        if (def.child) {
            TableDef t;
            t.name = uniqueTable(SigName(rt) + "_" + SigName(fs));
            t.rt = rt;
            t.fs = fs;
            if (module) t.module = module;
            std::string display = catalog.Display(rt, fs);
            t.description = rt + "." + fs + (display.empty() ? "" : " (" + display + ")") +
                            (def.elementRows ? ": one row per list element." : ": one row per occurrence.") +
                            (module ? std::string(" Module ") + module + ": empty until that module is built." : "");
            def.table = static_cast<int>(tables_.size());
            tables_.push_back(std::move(t));
            used.emplace_back(std::set<std::string>{"form_key", "plugin_no", "idx"});
            Columns c{&tables_[def.table].columns, &used[def.table], isUsableName};
            const Shape& row = def.elementRows ? shape.elem[0] : shape;
            std::string path = fs + (def.elementRows ? "[]" : "");
            c.Flatten(row, row.kind == K::Map || row.kind == K::Tuple ? "" : SigName(fs), path, {});
            def.columnCount = tables_[def.table].columns.size();
        } else {
            def.table = tt->second;
            auto& cols = tables_[def.table].columns;
            if (used[def.table].empty()) used[def.table] = {"form_key", "plugin_no"};
            def.firstColumn = cols.size();
            Columns c{&cols, &used[def.table], isUsableName};
            c.Flatten(shape, SigName(fs), fs, {});
            def.columnCount = cols.size() - def.firstColumn;
        }
        subs_[key] = def;
    }
}

const SubrecordDef* Schema::Find(const std::string& rt, const std::string& fs) const {
    auto it = subs_.find(rt + "." + fs);
    return it == subs_.end() ? nullptr : &it->second;
}

int Schema::TypeTable(const std::string& rt) const {
    auto it = typeTables_.find(rt);
    return it == typeTables_.end() ? -1 : it->second;
}

}  // namespace spf
