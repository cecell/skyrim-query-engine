// spf: look inside a Skyrim SE plugin and export fields.
//
//   spf info   <plugin>                         masters and record counts by type
//   spf fields <plugin> <TYPE>                  which fields that type has here, with sub-fields
//   spf export <plugin> [options]               the chosen fields of the chosen forms
//       --type WEAP[,ARMO...]     only these record types
//       --form 00012EB7[,...]     only these forms (as stored, or Skyrim.esm:012EB7)
//       --fields EDID,DATA.damage only these fields ("*" or omitted = all);
//                                 a field is a subrecord, then a dotted path inside it
//       --csv | --json            output format (default json)
//       --out <file>              write there instead of the console
//   --patterns <file>             pattern catalog (default: patterns.json next to spf.exe)
//
// A field that occurs once is a value; one that repeats (SNAM, LVLO, CTDA...)
// is a list of its occurrences.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <io.h>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../src/plugin.h"

using namespace spf;

namespace {

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::vector<std::string> SplitList(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        if (!item.empty()) out.push_back(item);
    return out;
}

std::string Upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

// "00012EB7" (as stored) or "Skyrim.esm:012EB7" (owner + object ID).
bool ParseFormSpec(const Plugin& p, const std::string& spec, uint32_t* out) {
    size_t colon = spec.rfind(':');
    if (colon == std::string::npos) {
        *out = static_cast<uint32_t>(strtoul(spec.c_str(), nullptr, 16));
        return true;
    }
    std::string owner = spec.substr(0, colon);
    uint32_t local = static_cast<uint32_t>(strtoul(spec.c_str() + colon + 1, nullptr, 16)) & 0xFFFFFF;
    auto same = [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; };
    if (same(owner, p.name())) {
        *out = (static_cast<uint32_t>(p.masters().size()) << 24) | local;
        return true;
    }
    for (size_t i = 0; i < p.masters().size(); ++i) {
        if (same(owner, p.masters()[i])) {
            *out = (static_cast<uint32_t>(i) << 24) | local;
            return true;
        }
    }
    return false;
}

// One requested field: the subrecord signature and the path inside it.
struct FieldSpec {
    std::string text, sig, path;
};

std::vector<FieldSpec> ParseFields(const std::string& s) {
    std::vector<FieldSpec> out;
    for (const auto& item : SplitList(s)) {
        FieldSpec f;
        f.text = item;
        size_t sep = item.find_first_of(".[");
        f.sig = item.substr(0, sep);
        f.path = sep == std::string::npos ? "" : item.substr(sep);
        out.push_back(f);
    }
    return out;
}

// Every occurrence of the field in a record, narrowed by the path; one value,
// a list for several, null for none.
Value Gather(const std::vector<Field>& fields, const FieldSpec& spec) {
    std::vector<Value> found;
    for (const auto& f : fields) {
        if (_stricmp(f.sig.c_str(), spec.sig.c_str()) != 0) continue;
        if (spec.path.empty()) found.push_back(f.value);
        else for (auto& v : Select(f.value, spec.path)) found.push_back(std::move(v));
    }
    if (found.empty()) return Value();
    if (found.size() == 1) return std::move(found[0]);
    return Value::List(std::move(found));
}

// All fields of a record as one object; repeated subrecords become lists.
Value AllFields(const std::vector<Field>& fields) {
    std::map<std::string, size_t> count;
    for (const auto& f : fields) ++count[f.sig];
    Value out = Value::Map();
    std::map<std::string, Value> lists;
    for (const auto& f : fields) {
        if (count[f.sig] == 1) {
            out.Set(f.sig, f.value);
        } else {
            if (!out.Find(f.sig)) out.Set(f.sig, Value::List());
            lists[f.sig].Items().push_back(f.value);
        }
    }
    for (auto& [sig, list] : lists) {
        list = Value::List(std::move(lists[sig].Items()));
        out.Set(sig, std::move(list));
    }
    return out;
}

std::string CsvCell(const Value& v) {
    std::string s;
    if (v.IsNull()) return "";
    if (v.IsString()) s = v.AsString();
    else WriteJson(v, &s);
    if (s.find_first_of(",\"\r\n") != std::string::npos) {
        std::string q = "\"";
        for (char c : s) {
            if (c == '"') q += '"';
            q += c;
        }
        return q + "\"";
    }
    return s;
}

const char kHelp[] = R"HELP(spf - read Skyrim SE plugins (.esm / .esp / .esl) and export their fields

Use this instead of parsing plugin bytes yourself. It decodes every subrecord
with 1,893 patterns covering 134 record types, and the decoder was checked
value for value against a reference database built from the 80 Bethesda
plugins (5.7 million values, 0 mismatches). It already handles the format's
traps: compressed records, XXXX-sized subrecords, master-relative FormIDs,
localized strings, repeating subrecords, context-dependent unions (PERK
effects, conditions), and IMAD's non-ASCII signatures.

COMMANDS
  spf info   <plugin>             the plugin's masters and record counts by type
  spf fields <plugin> <TYPE>      every field that record type has in this plugin
  spf export <plugin> [options]   the fields you choose, of the forms you choose
  spf help [command]              this text, or one command's details
                                  (also: spf <command> --help)

  Start with `info` to see what a plugin contains, then `fields <TYPE>` to see
  which fields and sub-fields exist, then `export` what you need.

DATA MODEL
  plugin -> records grouped by type (WEAP, NPC_, CELL, ...)
         -> one record per form, keyed by FormID
         -> fields = subrecords (EDID, FULL, DATA, ...), each decoded into a
            value: text, number, list, or an object of named sub-fields
  A field path is the 4-letter subrecord signature, then a dotted path inside
  its value: EDID   DATA.damage   SNAM.faction   CTDA[0].func_idx

READING THE OUTPUT
  form      "Owner.esm:XXXXXX" - the plugin that owns the form and its 6-digit
            object ID. FormIDs inside values look the same. Never the raw
            32-bit number, whose first byte means a different master in every
            plugin. A null FormID ([00000000]) comes out as null.
  type      the record signature.
  in        the WRLD / CELL / DIAL the record sits in (placed objects in their
            cell, cells in their worldspace, INFO lines in their topic).
            Missing for top-level records.
  deleted   true when the record carries the deleted flag (it has no fields).
  fields    the requested fields. A subrecord that repeats within a record
            (factions, leveled list entries, conditions, keywords...) gives a
            list of its occurrences; one that occurs once gives its value; one
            that is absent gives null.
  Localized names / descriptions are their text, read from the plugin's
  Strings\ folder, its .bsa, or Skyrim - Interface.bsa (English). If the
  strings cannot be found the value stays "[LSTRING:0xXXXXXXXX]".
  "#0A1B2C..." (hex) means the subrecord had no pattern, or its bytes did not
  fit it; the raw bytes are given instead.
  Field and sub-field names come from the pattern catalog; names like
  "unknown1" or "unused" are fields whose meaning is not documented.

LIMITS
  - One plugin at a time: you see that plugin's version of a form, not the
    winning override in a load order.
  - Subrecords that belong together (LVLO + COED, CTDA + CIS1/CIS2, NPC tint
    layers) come out as separate lists in file order, not paired.
  - Strings: English only.

EXIT CODES
  0 ok   1 the plugin could not be opened or is not a plugin   2 bad arguments
  or the pattern catalog (patterns.json next to spf.exe) could not be loaded

OPTIONS FOR ALL COMMANDS
  --patterns <file>   use another pattern catalog
)HELP";

const char kHelpInfo[] = R"HELP(spf info <plugin>

Prints the plugin name (and "(localized)" when its strings live in separate
files), its masters with their index - the first byte of a stored FormID - and
the number of records of each type. Plain text.

  spf info "D:\Games\Skyrim Special Edition\Data\Update.esm"
  plugin: Update.esm (localized)
    [00] Skyrim.esm
  records: 16011
    ACHR 39
    ...
)HELP";

const char kHelpFields[] = R"HELP(spf fields <plugin> <TYPE>

Lists every field (subrecord) that records of TYPE have in this plugin, how
many records have it, and the sub-fields inside it: exactly the paths you can
pass to `spf export --fields`. Plain text. TYPE is a record signature such as
WEAP, ARMO, NPC_, LVLI, CELL, REFR (case does not matter).

  spf fields Skyrim.esm WEAP
  WEAP: 2484 records
    DATA  (in 2484)
      DATA.damage
      DATA.value
      DATA.weight
    KWDA  (in 2470)
    ...

Fields only appear if at least one record in THIS plugin has them; another
plugin may use more.
)HELP";

const char kHelpExport[] = R"HELP(spf export <plugin> [--type T,..] [--form F,..] [--fields P,..] [--csv | --json] [--out file]

Exports fields of the plugin's records.

  --type WEAP,ARMO        only records of these types (default: every type)
  --form F,F,...          only these forms. Each F is either the FormID as
                          stored in this plugin (00012EB7) or Owner:ObjectID
                          (Skyrim.esm:012EB7), which works in any plugin.
                          Missing forms are reported on stderr.
  --fields P,P,...        only these field paths (see `spf fields`). Omitted
                          or "*" = every field of each record (JSON only).
  --json                  (default) a JSON array, one object per record:
                            [{"form":"Skyrim.esm:012EB7","type":"WEAP",
                              "in":"Skyrim.esm:00003C",       (if it has a parent)
                              "deleted":true,                 (if deleted)
                              "fields":{"EDID":"IronSword","DATA.damage":7}}]
                          With --fields, "fields" is keyed by the paths you
                          asked for; with all fields, by subrecord signature.
  --csv                   one row per record: form,type,in,<your fields>.
                          Needs --fields. Lists and objects are written as
                          JSON text inside the cell.
  --out <file>            write to a file (UTF-8) instead of the console

Field paths
  EDID              the whole value of that subrecord
  DATA.damage       a named sub-field
  CTDA[0]           the first element of a list
  SNAM.faction      a name applied to a list applies to every element, so
                    this gives the faction of every SNAM occurrence
  A subrecord that repeats gives a list; a path that matches nothing gives null.

Examples
  spf export Skyrim.esm --type WEAP --fields EDID,FULL,DATA.damage,DATA.value --csv
  spf export MyMod.esp --form Skyrim.esm:012EB7
  spf export MyMod.esp --type LVLI --fields EDID,LVLO.reference,LVLO.level
  spf export MyMod.esp --type REFR --fields NAME,DATA.pos_x,DATA.pos_y,DATA.pos_z --out refs.json
)HELP";

int Usage() {
    fprintf(stderr,
            "usage:\n"
            "  spf info   <plugin>\n"
            "  spf fields <plugin> <TYPE>\n"
            "  spf export <plugin> [--type T,..] [--form ID,..] [--fields F,..] [--csv|--json] [--out file]\n"
            "  spf help [command]      (full explanation)\n");
    return 2;
}

int Help(const std::string& topic) {
    const char* text = topic == "info" ? kHelpInfo : topic == "fields" ? kHelpFields
                     : topic == "export" ? kHelpExport : kHelp;
    _setmode(_fileno(stdout), _O_BINARY);
    fwrite(text, 1, strlen(text), stdout);
    return 0;
}

// Collects the shape of a value: "DATA", "DATA.damage", "DATA.flags[]"...
void Shape(const Value& v, const std::string& prefix, std::set<std::string>* out) {
    out->insert(prefix);
    if (v.kind() == Value::Kind::Map) {
        for (const auto& m : v.Members()) Shape(m.second, prefix + "." + m.first, out);
    } else if (v.kind() == Value::Kind::List) {
        for (const auto& it : v.Items())
            if (it.kind() == Value::Kind::Map) Shape(it, prefix, out);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // spf help [cmd], spf --help, spf <cmd> --help
    std::string arg1 = argc > 1 ? Utf8(argv[1]) : "";
    if (arg1 == "help" || arg1 == "--help" || arg1 == "-h" || arg1 == "/?")
        return Help(argc > 2 ? Utf8(argv[2]) : "");
    for (int i = 2; i < argc; ++i) {
        std::string a = Utf8(argv[i]);
        if (a == "--help" || a == "-h") return Help(arg1);
    }
    if (argc < 3) return argc == 1 ? Help("") : Usage();
    std::string cmd = Utf8(argv[1]);
    std::wstring pluginPath = argv[2];
    std::string types, forms, fieldList, outPath, positional;
    std::wstring patternsPath;
    bool csv = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = Utf8(argv[i]);
        auto next = [&](std::string* dst) {
            if (i + 1 < argc) *dst = Utf8(argv[++i]);
        };
        if (a == "--type") next(&types);
        else if (a == "--form") next(&forms);
        else if (a == "--fields") next(&fieldList);
        else if (a == "--out") next(&outPath);
        else if (a == "--csv") csv = true;
        else if (a == "--json") csv = false;
        else if (a == "--patterns" && i + 1 < argc) patternsPath = argv[++i];
        else positional = a;
    }
    if (patternsPath.empty()) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        patternsPath = exe;
        patternsPath = patternsPath.substr(0, patternsPath.find_last_of(L"\\/") + 1) + L"patterns.json";
    }
    std::ifstream pf(patternsPath, std::ios::binary);
    std::stringstream ss;
    ss << pf.rdbuf();
    PatternCatalog cat;
    std::string err;
    if (!cat.Load(ss.str(), &err)) {
        fprintf(stderr, "patterns: %s\n", err.empty() ? "cannot read patterns.json" : err.c_str());
        return 2;
    }
    auto plugin = Plugin::Open(pluginPath, cat, &err);
    if (!plugin) {
        fprintf(stderr, "%s: %s\n", Utf8(pluginPath).c_str(), err.c_str());
        return 1;
    }

    std::string out;
    if (cmd == "info") {
        std::map<std::string, size_t> counts;
        for (const auto& r : plugin->records()) ++counts[r.type];
        out += "plugin: " + plugin->name() + (plugin->localized() ? " (localized)" : "") + "\n";
        for (size_t i = 0; i < plugin->masters().size(); ++i) {
            char buf[16];
            snprintf(buf, sizeof(buf), "  [%02zX] ", i);
            out += buf + plugin->masters()[i] + "\n";
        }
        out += "records: " + std::to_string(plugin->records().size() - 1) + "\n";
        for (const auto& [t, n] : counts)
            if (t != "TES4") out += "  " + t + " " + std::to_string(n) + "\n";
    } else if (cmd == "fields") {
        std::string type = Upper(positional);
        if (type.empty()) return Usage();
        std::map<std::string, std::pair<size_t, std::set<std::string>>> seen;  // sig -> (records, shapes)
        auto recs = plugin->OfType(type);
        for (const Record* r : recs) {
            std::set<std::string> inThis;
            for (const auto& f : plugin->Fields(*r)) {
                Shape(f.value, f.sig, &seen[f.sig].second);
                if (inThis.insert(f.sig).second) ++seen[f.sig].first;
            }
        }
        out += type + ": " + std::to_string(recs.size()) + " records\n";
        for (const auto& [sig, info] : seen) {
            out += "  " + sig + "  (in " + std::to_string(info.first) + ")\n";
            for (const auto& s : info.second)
                if (s != sig) out += "    " + s + "\n";
        }
    } else if (cmd == "export") {
        std::set<std::string> typeSet;
        for (const auto& t : SplitList(types)) typeSet.insert(Upper(t));
        std::vector<const Record*> recs;
        if (!forms.empty()) {
            for (const auto& spec : SplitList(forms)) {
                uint32_t fid;
                const Record* r = ParseFormSpec(*plugin, spec, &fid) ? plugin->Find(fid) : nullptr;
                if (!r) fprintf(stderr, "no such form here: %s\n", spec.c_str());
                else if (typeSet.empty() || typeSet.count(r->type)) recs.push_back(r);
            }
        } else {
            for (const auto& r : plugin->records())
                if (r.type != "TES4" && (typeSet.empty() || typeSet.count(r.type))) recs.push_back(&r);
        }
        std::vector<FieldSpec> specs = fieldList.empty() || fieldList == "*" ? std::vector<FieldSpec>{}
                                                                              : ParseFields(fieldList);
        if (csv) {
            if (specs.empty()) {
                fprintf(stderr, "--csv needs --fields (a fixed set of columns)\n");
                return 2;
            }
            out += "form,type,in";
            for (const auto& s : specs) out += "," + CsvCell(Value::Str(s.text));
            out += "\n";
        } else {
            out += "[\n";
        }
        bool first = true;
        for (const Record* r : recs) {
            auto fields = plugin->Fields(*r);
            Value form = Value::Str(plugin->FormKey(r->formId));
            Value parent = r->parent ? Value::Str(plugin->FormKey(r->parent)) : Value();
            if (csv) {
                out += CsvCell(form) + "," + r->type + "," + CsvCell(parent);
                for (const auto& s : specs) out += "," + CsvCell(Gather(fields, s));
                out += "\n";
            } else {
                Value obj = Value::Map();
                obj.Set("form", form);
                obj.Set("type", Value::Str(r->type));
                if (!parent.IsNull()) obj.Set("in", parent);
                if (r->flags & 0x20) obj.Set("deleted", Value::Bool(true));
                if (specs.empty()) {
                    obj.Set("fields", AllFields(fields));
                } else {
                    Value sel = Value::Map();
                    for (const auto& s : specs) sel.Set(s.text, Gather(fields, s));
                    obj.Set("fields", std::move(sel));
                }
                if (!first) out += ",\n";
                first = false;
                WriteJson(obj, &out);
            }
        }
        if (!csv) out += "\n]\n";
    } else {
        return Usage();
    }

    if (!outPath.empty()) {
        std::ofstream f(outPath, std::ios::binary);
        f << out;
    } else {
        _setmode(_fileno(stdout), _O_BINARY);
        fwrite(out.data(), 1, out.size(), stdout);
    }
    return 0;
}
