// skqe: look inside a Skyrim SE plugin and export fields.
//
//   skqe info   <plugin> [--json]                header facts, masters, record counts by type
//   skqe fields <plugin> <TYPE>                  which fields that type has here, with sub-fields
//   skqe export <plugin> [--form ..] [<plugin> [--form ..]]... [options]
//   skqe export --request <file|-> [options]     the chosen fields of the chosen forms
//   skqe export --load-order <plugins.txt> --data <folder> [--form ..] [--conflicts | --conflict-summary]
//                                               every copy across a load order, winner marked
//       --type WEAP[,ARMO...]     only these record types
//       --form 00012EB7[,...]     only these forms (as stored, or Skyrim.esm:012EB7)
//       --fields EDID,DATA.damage only these fields ("*" or omitted = all);
//                                 a field is a subrecord, then a dotted path inside it
//       --csv | --psv | --json    output format (default json)
//       --out <file>              write there instead of the console
//   --patterns <file>             pattern catalog (default: see DATA FILES in the help)
//   skqe build-db --out <db> --data <Data> [--load-order plugins.txt | --plugins a,b,..]
//                                               a SQLite database of a whole load order
//   skqe staging [--data <Data>] [--manager vortex|mo2] [--json]   where Vortex or MO2 keeps the mods
//   skqe search <text or wildcard> [--all] [--json]   files by name in Data and its BSAs, and staging with --all
//   skqe build-assets --out <db> --data <Data> [--staging <root>|auto | --mo2 <instance> --profile <name>]
//                                               a SQLite database of every asset file
//
// A field that occurs once is a value; one that repeats (SNAM, LVLO, CTDA...)
// is a list of its occurrences.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../src/asset_build.h"
#include "../src/asset_search.h"
#include "../src/db_build.h"
#include "../src/export_request.h"
#include "../src/data_files.h"
#include "../src/plugin.h"
#include "sqlite3.h"
#include "../src/staging_find.h"

using namespace skqe;

std::wstring WidenUtf8(const std::string& s);

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
    if (same(owner, p.nameUtf8())) {
        *out = (static_cast<uint32_t>(p.masters().size()) << 24) | local;
        return true;
    }
    const auto masters = p.mastersUtf8();
    for (size_t i = 0; i < masters.size(); ++i) {
        if (same(owner, masters[i])) {
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

// The column separator of --csv and --psv, set once before any output. Both
// quote the standard way, which is what Excel and Google Sheets read.
std::string gSep = ",";

std::string CsvCell(const Value& v) {
    std::string s;
    if (v.IsNull()) return "";
    if (v.IsString()) s = v.AsString();
    else WriteJson(v, &s);
    if (s.find_first_of(gSep + "\"\r\n") != std::string::npos) {
        std::string q = "\"";
        for (char c : s) {
            if (c == '"') q += '"';
            q += c;
        }
        return q + "\"";
    }
    return s;
}

const char kHelp[] = R"HELP(skqe - read Skyrim SE plugins (.esm / .esp / .esl) and export their fields

Use this instead of parsing plugin bytes yourself. It decodes every subrecord
with 1,893 patterns covering 134 record types, and the decoder was checked
value for value against a reference database built from the 80 Bethesda
plugins (5.7 million values, 0 mismatches). It already handles the format's
traps: compressed records, XXXX-sized subrecords, master-relative FormIDs,
localized strings, repeating subrecords, context-dependent unions (PERK
effects, conditions), and IMAD's non-ASCII signatures.

COMMANDS
  skqe info   <plugin> [--json]    the plugin's header, masters and record counts by type
  skqe fields <plugin> <TYPE>      every field that record type has in this plugin;
                                  --names adds xEdit's name for each
  skqe export <plugin> [options]   the fields you choose, of the forms you choose. Also:
                                  several plugins, each with its own forms (--request
                                  for long lists); every copy of a form across a load
                                  order with the winner marked (--load-order), and
                                  only what differs (--conflicts, --conflict-summary)
                                  Only the plugins named are opened, never their masters.
  skqe diff   <copy> <copy>...     what differs between copies of a plugin (or
                                  --name X.esp --in <folder>: every copy of a name)
  skqe build-db [options]          a SQLite database of a whole load order
  skqe db add <module> --db x.db   add an optional module (navmesh, landscape...) later
  skqe build-assets [options]      a SQLite database of every asset file (Data, mod
                                  staging folders, MO2, BSA contents) with winners
  skqe search <text>               find files by name in Data and its BSAs;
                                  --all adds the staging folder, each file once
  skqe staging                     where Vortex or MO2 keeps this game's mods
  skqe help [command]              this text, or one command's details
                                  (also: skqe <command> --help)

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
  - Strings: English only (build-db: --language).
  - A damaged plugin is refused with the reason (exit code 1); a subrecord
    whose bytes do not fit its pattern comes out as raw hex. Sizes and
    counts from the file are checked before use, so a damaged or crafted
    file cannot make skqe read out of bounds or allocate gigabytes.

EXIT CODES
  0 ok   1 the plugin could not be opened or is not a plugin   2 bad arguments
  or the pattern catalog (patterns.json) could not be loaded

DATA FILES
  patterns.json (the catalog), ut_reference.sql (the lookup tables) and
  search_filters.txt (search's rules) are built into the released skqe.exe.
  A copy on disk wins: first in the folder SKQE_DATA names, then next to
  skqe.exe. skqe --version says which copy of each is in use.

OPTIONS FOR ALL COMMANDS
  --patterns <file>   use another pattern catalog
)HELP";

// The named bits of the TES4 flags, with any other bits in hex.
std::vector<std::string> HeaderFlagNames(uint32_t flags) {
    std::vector<std::string> names;
    if (flags & 0x1) names.push_back("ESM");
    if (flags & 0x80) names.push_back("Localized");
    if (flags & 0x200) names.push_back("ESL");
    if (uint32_t rest = flags & ~uint32_t{0x281}) {
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%X", rest);
        names.push_back(buf);
    }
    return names;
}

// skqe info, plain text.
std::string InfoText(const Plugin& plugin, const std::map<std::string, size_t>& counts) {
    const PluginHeader& h = plugin.info();
    std::string out;
    char buf[64];
    out += "plugin: " + plugin.nameUtf8() + (plugin.localized() ? " (localized)" : "") + "\n";
    snprintf(buf, sizeof(buf), "version: %.2f\n", h.version);
    out += buf;
    out += "form version: " + std::to_string(plugin.header().formVersion) + "\n";
    snprintf(buf, sizeof(buf), "flags: 0x%08X", h.flags);
    out += buf;
    const auto flagNames = HeaderFlagNames(h.flags);
    for (size_t i = 0; i < flagNames.size(); ++i) out += (i ? ", " : " (") + flagNames[i];
    out += flagNames.empty() ? "\n" : ")\n";
    snprintf(buf, sizeof(buf), "next object id: 0x%08X\n", h.nextObjectId);
    out += buf;
    out += "header record count: " + std::to_string(h.numRecords) + "\n";
    out += "overrides listed (ONAM): " + std::to_string(h.overrideCount) + "\n";
    if (h.intv) out += "INTV: " + std::to_string(*h.intv) + "\n";
    if (h.incc) out += "INCC: " + std::to_string(*h.incc) + "\n";
    if (!h.author.empty()) out += "author: " + h.author + "\n";
    if (!h.description.empty()) out += "description: " + h.description + "\n";
    out += "masters: " + std::to_string(plugin.masters().size()) + "\n";
    const auto masters = plugin.mastersUtf8();
    for (size_t i = 0; i < masters.size(); ++i) {
        snprintf(buf, sizeof(buf), "  [%02zX] ", i);
        out += buf + masters[i] + " (size " + std::to_string(h.masterSizes[i]) + ")\n";
    }
    out += "records: " + std::to_string(plugin.records().size() - 1) + "\n";
    for (const auto& [t, n] : counts)
        if (t != "TES4") out += "  " + t + " " + std::to_string(n) + "\n";
    return out;
}

// skqe info --json: one object on one line.
std::string InfoJson(const Plugin& plugin, const std::map<std::string, size_t>& counts) {
    const PluginHeader& h = plugin.info();
    auto hex = [](uint32_t v) {
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%08X", v);
        return Value::Str(buf);
    };
    auto optional = [](const std::optional<uint32_t>& v) { return v ? Value::Int(*v) : Value(); };
    Value flagNames = Value::List();
    for (const auto& n : HeaderFlagNames(h.flags)) flagNames.Items().push_back(Value::Str(n));
    Value masters = Value::List();
    const auto names = plugin.mastersUtf8();
    for (size_t i = 0; i < names.size(); ++i) {
        Value m = Value::Map();
        m.Set("index", Value::Int(static_cast<int64_t>(i)));
        m.Set("name", Value::Str(names[i]));
        m.Set("size", Value::Int(static_cast<int64_t>(h.masterSizes[i])));
        masters.Items().push_back(std::move(m));
    }
    Value byType = Value::Map();
    for (const auto& [t, n] : counts)
        if (t != "TES4") byType.Set(t, Value::Int(static_cast<int64_t>(n)));
    Value o = Value::Map();
    o.Set("plugin", Value::Str(plugin.nameUtf8()));
    o.Set("version", Value::Float(std::round(h.version * 100.0) / 100.0));
    o.Set("form_version", Value::Int(plugin.header().formVersion));
    o.Set("flags", hex(h.flags));
    o.Set("flag_names", std::move(flagNames));
    o.Set("localized", Value::Bool(h.localized));
    o.Set("loads_as_master", Value::Bool(h.master));
    o.Set("loads_light", Value::Bool(h.light));
    o.Set("next_object_id", hex(h.nextObjectId));
    o.Set("header_record_count", Value::Int(h.numRecords));
    o.Set("overrides_listed", Value::Int(h.overrideCount));
    o.Set("intv", optional(h.intv));
    o.Set("incc", optional(h.incc));
    o.Set("author", Value::Str(h.author));
    o.Set("description", Value::Str(h.description));
    o.Set("masters", std::move(masters));
    o.Set("records", Value::Int(static_cast<int64_t>(plugin.records().size() - 1)));
    o.Set("records_by_type", std::move(byType));
    std::string out;
    WriteJson(o, &out);
    return out + "\n";
}

const char kHelpInfo[] = R"HELP(skqe info <plugin> [--json] [--out file]

Prints the plugin name (and "(localized)" when its strings live in separate
files), the facts from its TES4 header, its masters with their index - the
first byte of a stored FormID - and the size stored for each, and the number
of records of each type. Plain text, or with --json one JSON object, the way
a script or an agent should read it:

  skqe info Update.esm --json
  {"plugin":"Update.esm","version":1.71,"form_version":44,"flags":"0x00000081",
   "flag_names":["ESM","Localized"],"localized":true,"loads_as_master":true,
   "loads_light":false,"next_object_id":"0xFF003289","header_record_count":20874,
   "overrides_listed":2044,"intv":404,"incc":88,"author":"bnesmith","description":"",
   "masters":[{"index":0,"name":"Skyrim.esm","size":0}],"records":16065,
   "records_by_type":{"ACHR":39,...}}

intv and incc are null when the plugin has none; author and description are "".

Header facts: version (HEDR: 0.94, 1.70, 1.71), form version, flags, next
object id, header record count (as the CK counted them, groups included; the
"records:" line below is what the file really holds), the number of overrides
listed in ONAM, INTV and INCC when present, author and description when
present. Flags are named for ESM, Localized and ESL; a .esl file loads light
even without the ESL flag.

  skqe info "D:\Games\Skyrim Special Edition\Data\Update.esm"
  plugin: Update.esm (localized)
  version: 1.71
  form version: 44
  flags: 0x00000081 (ESM, Localized)
  ...
  masters: 1
    [00] Skyrim.esm (size 0)
  records: 16065
    ACHR 39
    ...
)HELP";

const char kHelpSearch[] = R"HELP(skqe search <text or wildcard> [--all | --winner] [--anywhere | --bsa-only] [options]
skqe search --show-filters

Finds files by name, including the files packed in BSAs. It only reads. Case
never matters. Plain text matches anywhere in the path, folders included. With
* or ? it's a wildcard on the file name, or if it has a \ or / on the path from
any folder down, so armor\iron* finds textures\armor\iron.dds.

  --regex           the text is a regular expression, searched for in the whole
                    path. Use / between folders.

  --all             also search the staging folder: every mod folder and the
                    BSAs directly inside each.
  --winner          Data only: for each path, the one copy the game uses. A
                    loose file wins, else the last-loaded archive holding it.
                    A path only in archives the game doesn't load isn't listed.
  --load-order <f>  with --winner: plugins.txt. Default: the game's own, else
                    every plugin in Data.
  --anywhere        ignore the filters below.
  --bsa-only        only the files packed in BSAs, ignoring the filters. Loose
                    folders aren't walked.
  --data <folder>   the Data folder. Default: the saved one, else the installed game.
  --staging <folder>  the staging folder, for this search. Implies --all.
                    Default: the saved one, else `skqe staging` finds it.
  --manager m       with --all: vortex or mo2, if detection picks the wrong one.
  --limit N         stop after N paths (the first N by name).
  --json            one object per path: {"path":..,"copies":[{"archive":..,"in":[..],"size":..}]}
  --threads n       default: one per logical core.

Each path is listed once, with its distinct copies. A file hardlinked between
Data and a mod is one copy found in both, and a hardlinked BSA is read once.
Copies that are separate files stay separate, even with the same contents.

  skqe search "barenziah"
  skqe search "*.hkx" --all --limit 50
  skqe search "armor\iron*.dds" --json
  skqe search "armor/iron.*_[nm]\.dds$" --regex

  textures\armor\iron\cuirass.dds
    loose: Data, Some Armor Mod
    in Skyrim - Textures3.bsa: Data

The file and copy counts go to stderr, so the output stays clean.

FILTERS
A file type is looked for only in its usual folders: .dds in textures, .pex in
scripts, plugins directly in Data. A search for one type (*.dds, iron.dds) walks
only those folders, which is much faster. A type with no rule is searched for
everywhere. A file in an unusual place needs --anywhere.

The rules are in search_filters.txt, built into skqe.exe. To change them, put
your own copy in %LOCALAPPDATA%\skqe\search_filters.txt (start from the one in
the source, data\search_filters.txt). --show-filters says which is in use.
)HELP";

const char kHelpStaging[] = R"HELP(skqe staging [--data <folder>] [--manager vortex|mo2] [--json]
skqe staging --set-data <folder> | --set-staging <folder> | --clear | --show

Finds where the user's mod manager keeps its mods for this game. It only reads.

To set the folders yourself, --set-data and --set-staging save them for every
later command, and they beat detection. --clear forgets them, and --show lists
them. --data and --staging on a single command beat the saved folders. A
--manager asks for detection of that manager, ignoring a saved staging folder.

  --data <folder>   the game's Data folder. Default: the installed game, from
                    the registry.
  --manager m       only look for vortex or mo2.
  --json            one JSON object: data, manager, staging, instance, profile,
                    source, deployed, and also_found for anything else found.

Vortex: the "stagingPath" in Data\vortex.deployment.json, written when Vortex
deploys. Its default folder is used only when that doesn't exist and the folder
has mods in it.

MO2: global instances in %LOCALAPPDATA%\ModOrganizer, and portable ones next to
a ModOrganizer.exe named in nxmhandler.ini, when the instance's game folder is
this Data folder's. Custom mod folders in ModOrganizer.ini are followed.

If both manage this game, Vortex wins when it has files deployed, because MO2
leaves nothing in Data. Otherwise MO2 wins. Between MO2 instances, MO2's current
one wins. If it can't tell, it lists them and exits with 1.

  skqe staging
  manager: vortex
  staging: D:\Mods\Staging
  found by: Vortex's deployment manifest in Data
  deployed: yes
)HELP";

const char kHelpFields[] = R"HELP(skqe fields <plugin> <TYPE> [--names]

Lists every field (subrecord) that records of TYPE have in this plugin, how
many records have it, and the sub-fields inside it: exactly the paths you can
pass to `skqe export --fields`. Plain text. TYPE is a record signature such as
WEAP, ARMO, NPC_, LVLI, CELL, REFR (case does not matter).

  skqe fields Skyrim.esm WEAP
  WEAP: 2484 records
    DATA  (in 2484)
      DATA.damage
      DATA.value
      DATA.weight
    KWDA  (in 2470)
    ...

--names adds xEdit's name for each field, when the catalog has one:

  skqe fields Skyrim.esm BOOK --names
  BOOK: 821 records
    CNAM - Description (in 821)
    DATA - Data (in 821)
    ...

Fields only appear if at least one record in THIS plugin has them; another
plugin may use more.
)HELP";

const char kHelpExport[] = R"HELP(skqe export <plugin> [--form F,..] [<plugin> [--form F,..]]... [--limit N [--random [--seed S]]] [options]
skqe export --request <file | -> [options]
skqe export --load-order <plugins.txt> --data <folder> [--form F,.. | --form-file <file | ->] [--type T,..]
           [--conflicts | --conflict-summary] [--active-only | --all-plugins] [options]

Exports fields of plugins' records. Only the plugins you name are opened: a
plugin's masters are never needed, whatever its forms refer to, because a
reference comes out as Owner:ObjectID from the plugin's own header.

Several plugins: each --form list belongs to the plugin before it. The output
is one list, and each row also says "plugin" (a leading plugin column in CSV).

  --request <file>        read plugins and forms from a file ("-" = stdin). Use
                          this for long lists: a Windows command line stops at
                          32,767 characters, and at 8,191 through skqe.cmd.
                            [Skyrim.esm]
                            Skyrim.esm:0132AA, Skyrim.esm:01367B
                            # comments are fine; a list can span lines
                            [C:\mods\MyMod.esp]
                            MyMod.esp:000800 Skyrim.esm:012EB7
                          A plugin with no forms means all of its forms.
  --data <folder>         where to find plugins given by name alone (default:
                          the current folder). A path is used as it is.

  --type WEAP,ARMO        only records of these types (default: every type)
  --load-order <file>     every copy of each form across a whole load order,
  --plugins a.esm,b.esp   as build-db works it out: plugins.txt's active plugins
                          after the base masters and Skyrim.ccc, or the explicit
                          list; ESM-flagged plugins first either way. Needs
                          --data, and --form, --form-file or --type. Forms must
                          be Owner:ObjectID. Rows are grouped by form, in load
                          order, and also say "plugin", "load_order" and
                          "winner" (true on the last copy, the one that wins):
                            {"plugin":"Update.esm","load_order":1,"winner":false,"form":...}
                            {"plugin":"MyMod.esp","load_order":90,"winner":true,"form":...}
                          CSV leads with plugin,load_order,winner. Plugins are
                          only opened, never written; masters missing from
                          the list don't matter.
  --form-file <file>      forms for --load-order from a file ("-" = stdin),
                          separated by commas, spaces or lines.
  --conflicts             with a load order: the master and every override, with
                          only the fields whose value differs between any two
                          copies, and those fields on every copy, an ITM copy
                          included (null where a copy lacks one). Left out:
                          anything named unknown or unused, and whole subrecords
                          xEdit labels so. Record Flags is compared and shown
                          (hex, without Compressed, which is only how a record is
                          stored); Data Size, Form Version and Version Control
                          Info are not fields here. Everything else is compared
                          as it reads, capitals and list order included. This is
                          not xEdit's conflict status, which has a longer list of
                          ignored fields.
                          A form named with --form always answers, with empty
                          "fields" if nothing differs; a --type sweep leaves
                          out forms with no difference. --fields limits what
                          is compared. JSON only.
  --conflict-summary      the same comparison, one row per form, no values:
                            {"form":"Skyrim.esm:013BB9","type":"NPC_",
                             "plugins":["Skyrim.esm","MyMod.esp"],
                             "conflicts":["ACBS","PNAM"]}
                          or "conflicts":"no conflicts".
  --limit N               at most N records of each plugin named, after --type and
                          --form: the first N in the file, or with --random N
                          picked at random, none twice, in random order. Not
                          with a load order.
  --random                pick with the operating system's random source, so
                          every record is equally likely and each run differs.
  --seed S                with --random: the same S and the same plugin give the
                          same N records, to repeat a test.
  --active-only           plugins.txt's active plugins (the default for
                          --load-order) after the base masters and Skyrim.ccc.
  --all-plugins           every plugin plugins.txt lists, active or not, in its
                          order, if its file is in Data. Rows also say "active".
                          "winner" stays the game's winner: the last ACTIVE copy.
  --form F,F,...          only these forms. Each F is either the FormID as
                          stored in this plugin (00012EB7) or Owner:ObjectID
                          (Skyrim.esm:012EB7), which works in any plugin.
                          Missing forms are reported on stderr. A plugin that
                          cannot be opened is too: the others still come out,
                          and the exit code is 1.
  --fields P,P,...        only these field paths (see `skqe fields`). Omitted
                          or "*" = every field of each record (JSON only).
  --json                  (default) a JSON array, one object per record:
                            [{"form":"Skyrim.esm:012EB7","type":"WEAP",
                              "in":"Skyrim.esm:00003C",       (if it has a parent)
                              "deleted":true,                 (if deleted)
                              "fields":{"EDID":"IronSword","DATA.damage":7}}]
                          With --fields, "fields" is keyed by the paths you
                          asked for; with all fields, by subrecord signature.
  --labels                (JSON) names beside coded values, from
                          ut_reference.sql (built in): an enumeration
                          field gets "<field>_label" with its value's name, a
                          flag field the names of its set bits, and a condition
                          (CTDA) "function", its function's name:
                            "PKDT":{"type":18,"type_label":"Package",...}
                            "CTDA":{"func_idx":59,"function":"GetStageDone",
                                    "run_on":0,"run_on_label":"Subject",...}
                          The same links build-db stores in st_columns.
  --psv                   the same as --csv, separated by | instead of commas.
                          Text with a |, a " or a line break is quoted, the
                          standard way. For long text such as book pages.
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
  skqe export Skyrim.esm --type WEAP --fields EDID,FULL,DATA.damage,DATA.value --csv
  skqe export MyMod.esp --form Skyrim.esm:012EB7
  skqe export Skyrim.esm --form Skyrim.esm:0132AA,Skyrim.esm:01367B Dawnguard.esm --form Dawnguard.esm:002B74 --fields EDID,PNAM
  skqe export --request npcs.txt --data "D:\Skyrim\Data" --fields EDID,PNAM --out headparts.json
  skqe export --load-order "%LOCALAPPDATA%\Skyrim Special Edition\plugins.txt" --data "D:\Skyrim\Data" --form Skyrim.esm:013BB9 --fields EDID,PNAM
  skqe export Skyrim.esm --type NPC_ --limit 25 --random --fields EDID,PNAM
  skqe export MyMod.esp --type LVLI --fields EDID,LVLO.reference,LVLO.level
  skqe export MyMod.esp --type REFR --fields NAME,DATA.pos_x,DATA.pos_y,DATA.pos_z --out refs.json
)HELP";

const char kHelpDiff[] = R"HELP(skqe diff <copy> <copy> [<copy>...] [options]
skqe diff --name <plugin> --in <folder> [--in <folder>...] [options]

What differs between copies of a plugin: two versions of a mod, or the same
plugin name in several staging folders. Prints one JSON object. Masters are never
opened, and nothing is written except --out.

  --name X.esp --in <f>   compare every file of that name below these folders
                          (any depth), for example your mod staging folders.
  --type T,T              only these record types.
  --form F,F              only these forms, as Owner:ObjectID.
  --form-file <file>      the same from a file ("-" = stdin): commas, spaces or lines.
  --fields P,P            only these field paths.
  --summary               name the fields that differ, without their values.
  --include-unknown       also compare fields named unknown or unused, which are
                          left out by default, as in export --conflicts.
  --out <file>            write to a file instead of the console.

Output
  {"copies":[{"copy":0,"path":"...","size":1234,"identical_to":null},
             {"copy":1,"path":"...","size":1240,"identical_to":null}],
   "header":{"version":[1.7,1.71]},            header facts that differ, by copy
   "summary":{"identical":false,"forms":50,"forms_with_different_fields":2,
              "forms_not_in_every_copy":1,"by_type":{...},"by_field":{...}},
   "forms":[
    {"form":"X.esp:000800","type":"WEAP","edid":"MySword",
     "fields":{"DATA":[{"damage":7},{"damage":9}]}},
    {"form":"X.esp:000801","type":"ARMO","edid":"MyArmor","in_copies":[1],
     "fields":{}}
   ]}

  Each value list has one entry per copy, in the order of "copies". null means
  that copy lacks the field, or lacks the form. "in_copies" appears when a form
  isn't in every copy; fields are compared only among the copies that have it.
  "identical_to" names an earlier copy with exactly the same bytes.

What counts
  Text is compared with capitals, and lists with their order. Left out: anything
  named unknown or unused, and whole subrecords xEdit labels so (see
  --include-unknown). Record Flags is compared, without Compressed, which is only
  how a record is stored. "Parent" is the WRLD, CELL or DIAL a record sits in.
  Forms are matched by owner and object ID, so a reordered master list on its own
  is not a difference, but it does appear under "header".

Examples
  skqe diff "D:\Mods\A\X.esp" "D:\Mods\B\X.esp" --summary
  skqe diff --name X.esp --in D:\Mods\Staging --in D:\OtherMods --out x_diff.json
)HELP";

const char kHelpBuildDb[] = R"HELP(skqe build-db --out <file.db> --data <Data folder> [options]

Builds one SQLite database of a whole load order, for any app or script to
query with plain SQLite. Plugins are decoded in parallel: the Bethesda
plugins take about 20 seconds, a 4,883-plugin load order about 85.

OPTIONS
  --out <file.db>          the database to write. It is built as
                           <file.db>.building and replaces <file.db> only
                           when the build succeeds.
  --data <folder>          the game's Data folder (plugins, Strings\, BSAs).
  --load-order <file>      plugins.txt: its active (*) plugins, after the
                           base masters and Skyrim.ccc's Creation Club
                           plugins, which the game loads unlisted.
  --plugins a.esm,b.esp    an explicit load order instead.
                           Neither: every plugin in the Data folder (base
                           masters, Skyrim.ccc order, then by name).
                           Either way masters (ESM flag) load first.
  --language <name>        strings language (default english).
  --with m1,m2             build optional modules too (see MODULES).
  --skip e1,e2             leave out extras built by default (see EXTRAS).
  --threads <n>            decode threads (default: logical cores - 1).
  --quiet                  no progress output.

WHAT IS IN IT (schema version 9)
  plugins         one row per plugin: plugin_no (its number in this DB),
                  load_order, mod_index (light slot for light plugins),
                  is_light, is_master, header facts, size, mtime, crc32.
                  Masters that are not in the load order get a row with
                  load_order NULL (file_exists says whether it is on disk).
                  load_error: why a plugin could not be read; such a plugin
                  is reported, has no rows, and the build carries on.
  plugin_masters  plugin_no, idx, master_no.
  forms           one row per OCCURRENCE of a form: the plugin that owns it
                  and every plugin that overrides it. form_key identifies
                  the form, plugin_no where this copy is recorded; type,
                  edid, record flags, parent_key (the WRLD / CELL / DIAL it
                  sits in), stored_formid, is_winner (the copy loaded last).
  strings         plugin_no, kind (strings / dlstrings / ilstrings), id, text.
  forms_v         view: forms with readable keys ("Skyrim.esm:012EB7"), the
                  in-game load-order FormID, recorded_in, is_override,
                  is_deleted.  winning_forms = forms_v of the winners.
  skqe_meta        build settings and versions; skqe_modules = what is built.

  One table per record type (weap, npc, cell, refr, ...): one row per
  occurrence (form_key, plugin_no), one column per field of each subrecord
  that occurs once, named subrecord_field: weap.data_damage, npc.acbs_level,
  weap.full (the name, as text). winning_<type> views hold the winners.
  Child tables <type>_<subrecord> (form_key, plugin_no, idx): subrecords
  that repeat in a record (npc_snam factions, lvli_lvlo entries, conditions
  *_ctda) and lists of FormIDs inside one (weap_kwda keywords).
  FormID columns hold form_key integers. Other lists, and anything nested
  below them, are JSON text. Bitfield flags are a JSON list of names.
  Raw byte runs are BLOBs: SELECT hex(col) gives the hex text skqe export
  shows (inside JSON they stay hex text).
  MODULES (optional; their tables exist in every build, empty until built):
    navmesh          NAVM geometry (vertices, triangles, edge and door
                     links) and NAVI
    landscape        LAND terrain (heights, normals, colours, texture
                     layers), CELL / WRLD height maps, occlusion, LOD offsets
    wrld_large_refs  WRLD.RNAM large-reference lists
  Build them with --with, or add them later: skqe db add <module> --db x.db.
  EXTRAS (built from the database itself; on by default, --skip to leave
  out, skqe db add to add later):
    fts_names        full-text search over every form occurrence's editor
                     ID and name: form_names (the text), fts_names (words,
                     prefixes: MATCH 'iron sword', 'iro*'), fts_names_tri
                     (substrings of 3+ characters: MATCH 'onswo')
    eslable          plugin_facts.master_forms, eslable (Already, True, Save
                     in CK, False, Corrupted), esl_needs_1130
    deleted          plugin_facts.deleted_refs (placed objects), deleted_navm
    missing_masters  plugin_facts.missing_masters (view missing_masters
                     lists them)
    invalid_refs     invalid_refs: references to forms no plugin defines;
                     plugin_facts.invalid_refs counts only those held by a
                     form another form references (most others are
                     leftovers nothing uses), invalid_refs_all counts all
    asset_refs       asset_refs: every file a form names (models, textures,
                     sounds, music, VMAD scripts as scripts\<name>.pex),
                     normalized like assets.db paths so the two join on
                     path; path_id -> ref_paths says which field. Sound
                     fields name .wav; the game also plays .xwm / .fuz.
                     idx_asset_refs indexes path (which forms use a file).
    itm              OFF by default (--with itm): itm_forms, overrides
                     identical to their masters' version, compared as
                     xEdit's Quick Auto Clean does minus its known false
                     positives; plugin_facts.itm. Reads every plugin and
                     its masters (slowest extra)
  INDEXES are extras too, all on by default; --skip any to save disk and
  build time (lookups then scan the table), skqe db add to build later:
    idx_forms_key, idx_forms_plugin, idx_forms_type, idx_forms_edid,
    idx_forms_parent, idx_forms_flags (deleted / persistent), idx_plugin_masters,
    idx_leftovers (overflow / raw), idx_refs_to (what references X),
    idx_refs_from (what X references), idx_table_keys (every type and
    child table by form_key, plugin_no)
  skqe_modules lists what is built; st_tables.module says which tables
  belong to which module or extra.
  form_refs       the xref: one row per FormID inside a record, wherever it
                  sits (form_key, plugin_no = the occurrence; ref_key = the
                  form it points at; path_id -> ref_paths: record type and
                  field path such as "SNAM.Faction" or "KWDA"). Indexed both
                  ways: what references X, and what X references.
                  form_refs_v readable; winning_refs = references made by
                  the winning copies; dangling_refs = references to forms no
                  plugin defines. "Sits in this cell" is forms.parent_key,
                  not a reference.
  subrecord_overflow  extra occurrences of subrecords expected once.
  subrecord_raw       subrecords with no pattern or bytes that did not fit
                      (bytes: a BLOB).
  ut_*            lookup tables carried forward from the old database:
                  ut_ctda_functions (condition functions and their
                  parameter formats), ut_enum_kinds / ut_enum_values,
                  ut_flag_kinds / ut_flag_values, ut_data_types,
                  ut_stored_formats, ut_disp_transforms.
                  st_columns.flag_kind / enum_kind bind a column to its
                  names. record_flags_v: record header flag names per type;
                  form_flag_names_v: every set flag of every form by name;
                  conditions_v: every condition (CTDA) of every record type
                  with its function's name.
  st_tables, st_columns  the data dictionary: every table and column, with
                  its subrecord, its skqe export path, what a key column
                  refers to, xEdit's name for the subrecord, and notes
                  (how to read a BLOB column).

KEYS
  form_key = owner plugin_no * 16777216 + object ID. It is the same in every
  table, whichever plugin a reference was read from, so joins are plain
  integer joins. Owner: form_key >> 24. Object ID: form_key & 0xFFFFFF.
  In light plugins the object ID is 12-bit, as the game reads it.

EXAMPLES
  skqe build-db --out skyrim.db --data "C:\Games\Skyrim Special Edition\Data"
  skqe build-db --out lo.db --data D:\Data --load-order "%LOCALAPPDATA%\Skyrim Special Edition\plugins.txt"

  SELECT form, recorded_in FROM forms_v WHERE edid = 'IronSword';
  SELECT recorded_in, count(*) FROM forms_v WHERE is_override GROUP BY 1;
  SELECT edid, full, data_damage FROM winning_weap ORDER BY data_damage DESC;
  SELECT w.edid, k.edid AS keyword FROM winning_weap w
    JOIN weap_kwda x USING (form_key, plugin_no)
    JOIN winning_forms k ON k.form_key = x.kwda;
  SELECT column_name, kind, path, description FROM st_columns WHERE table_name = 'npc';
  SELECT n.edid, n.name, n.plugin_no FROM fts_names
    JOIN form_names n ON n.id = fts_names.rowid WHERE fts_names MATCH 'iron sword';
  SELECT form, type, path FROM winning_refs
    WHERE ref_key = (SELECT form_key FROM winning_forms WHERE edid = 'IronSword');
)HELP";

const char kHelpDb[] = R"HELP(skqe db add <module>[,<module>] --db <file.db> [--threads n] [--quiet]

Adds optional modules to a database build-db made: re-reads only those
modules' subrecords from the same plugins and fills their tables (plus xref
rows for any FormIDs in them). Core tables are not touched. Extras left out
with build-db --skip are added the same way, from the database alone:

  fts_names        full-text search over editor IDs and names
  eslable, deleted, missing_masters, invalid_refs   plugin facts
  itm              ITM records (reads the plugins: refused if one changed)

  navmesh          NAVM geometry (vertices, triangles, edge and door links)
                   and NAVI
  landscape        LAND terrain (heights, normals, colours, texture layers),
                   CELL / WRLD height maps, occlusion, LOD offsets
  wrld_large_refs  WRLD.RNAM large-reference lists

It runs in one transaction: if anything fails, nothing is added. It refuses
when a plugin changed (size or date) since the build, because the new rows
would not match the core ones; rebuild then (extras alone do not check). A module already built is
refused too. skqe_modules lists what is built.

  skqe db add navmesh --db skyrim.db
  skqe db add landscape,wrld_large_refs --db skyrim.db
  skqe db add fts_names --db skyrim.db

skqe db drop <extra>[,<extra>] --db <file.db>

Removes built extras: an index is dropped, a fact table is emptied. db add
builds them again. Modules cannot be dropped. Predesigned indexes, off by
default, are meant for this: build one for the queries that need it, drop it
when done:

  idx_persistent_type  persistent forms by type ("only persistent" filters)
  idx_ref_base         placed objects by base object (filter references by
                       the base's EditorID, name or type)

  skqe db add idx_ref_base --db skyrim.db
  skqe db drop idx_ref_base --db skyrim.db
)HELP";

const char kHelpBuildAssets[] = R"HELP(skqe build-assets --out <assets.db> --data <Data folder> [options]

Catalogues every asset file into its own SQLite database: every file in the
Data folder, in mod staging folders, and packed in their BSAs, with header
facts per type and which copy the game loads. ATTACH it to a build-db
database and join on paths.path and plugin names.

OPTIONS
  --out <file.db>          the database to write, built as <file.db>.building
                           and moved over <file.db> when the build succeeds.
  --data <folder>          the game's Data folder. Always scanned.
  --staging <root>         a mod staging root (any manager): each immediate
                           subfolder is one mod (roots.kind 'staging'). A
                           Vortex staging folder is recognised (manager).
                           `--staging auto` finds Vortex's or MO2's for this
                           Data folder; see `skqe staging`. --manager picks one.
  --mo2 <instance> --profile <name>
                           an MO2 instance (the folder with ModOrganizer.ini,
                           mods, profiles, overwrite): every mod folder,
                           enabled flags and priority from the profile's
                           modlist.txt, overwrite highest, the profile's
                           plugins.txt and inis. With nothing deployed in
                           Data, winners come from the virtual Data.
  --load-order <file>      plugins.txt (its active * plugins, after the base
                           masters and Skyrim.ccc). Default: the MO2
                           profile's, else every plugin in Data.
  --hash                   SHA-256 of every file (packed: of its unpacked
                           bytes). Off by default: it reads everything.
  --skip e1,e2             leave out extras (see EXTRAS).
  --threads <n>            read threads (default: logical cores - 1).
  --quiet                  no progress output.

WINNERS
  in_data = 1: the game sees this copy in Data. A staging file that is the
  same physical file as the Data file at its path (a hardlink: same volume
  serial and NTFS file ID) is stored once, with its staging root. A Data
  file no staging folder holds keeps the Data root. A staging copy that
  differs from Data has in_data = 0. Packed files take their archive's.
  is_winner = 1: the copy the game loads. A loose in_data copy wins. With
  none, the copy in the loaded archive with the highest load_rank wins:
  archives in the ini lists (sResourceArchiveList, sResourceArchiveList2 of
  Skyrim.ini / SkyrimCustom.ini in Documents\My Games\Skyrim Special
  Edition; the vanilla list when neither sets one) load first, then
  <Plugin>.bsa and <Plugin> - Textures.bsa of active plugins, in load
  order. Paths the game does not see have no winner.

WHAT IS IN IT (schema version 1)
  roots      where files come from: data | staging | overwrite; manager,
             enabled, priority; file_count / total_size / max_mtime.
  paths      every normalized relative path once (lower case, backslashes,
             relative to Data): dir, name, ext, kind (texture mesh audio
             voice script animation plugin archive strings interface
             facegen bodyslide other).
  files      every copy of every file: path_id, root_id, archive_id (the
             .bsa it is packed in; NULL = loose), size (packed: unpacked),
             packed_size, mtime (loose), in_data, is_winner, hash, error
             (why the file or its header could not be read).
  archives   per .bsa: version, flags, file_flags, file_count, compressed,
             loaded_by (ini | plugin | NULL), plugin, load_rank.
  dds tga nif audio pex hkx tri tri_morphs
             header facts by type, keyed by files.file_id, for loose and
             packed files alike. nif.game: se (BSTriShape family), le (strips,
             in shapes or skin partitions: SE cannot use them), both
             (NiTriShape without strips, or no geometry), unknown; Cathedral
             Assets Optimizer's rule, never the BS version.
  voice      Sound\Voice\<plugin>\<voicetype>\<x>_<formid>_<n>.<ext>: plugin,
             voice_type, object_id (the INFO), response.
  facegen    FaceGenData\FaceGeom|FaceTint\<plugin>\<formid>: kind, plugin,
             object_id.
  winning_files  view: the copy of each path the game loads.
  conflicts      view: paths with more than one copy, the winner marked.
  skqe_meta, st_tables, st_columns   settings and counts; the dictionary.

EXTRAS (on by default; --skip to leave out)
  nif_textures     every texture path a mesh expects (BSShaderTextureSet,
                   BSEffectShaderProperty), normalized; path_id NULL when no
                   such file exists anywhere.
  files_path files_root files_archive paths_kind
                   the indexes on files(path_id), files(root_id),
                   files(archive_id) and paths(kind).

Files are only read: Data and staging folders are never written.

EXAMPLES
  skqe build-assets --out assets.db --data "C:\Games\Skyrim Special Edition\Data"
      --load-order "%LOCALAPPDATA%\Skyrim Special Edition\plugins.txt"
  skqe build-assets --out assets.db --data D:\Skyrim\Data --staging D:\Vortex\skyrimse
  skqe build-assets --out assets.db --data D:\Skyrim\Data --mo2 D:\MO2 --profile Default

  SELECT path, root, archive FROM winning_files WHERE path LIKE 'meshes\armor\iron\%';
  SELECT * FROM conflicts WHERE kind = 'texture' LIMIT 20;
  SELECT texture FROM nif_textures WHERE path_id IS NULL;
  SELECT r.name, count(*) FROM files f JOIN roots r USING (root_id)
    WHERE f.in_data = 0 AND f.archive_id IS NULL GROUP BY 1 ORDER BY 2 DESC;
  ATTACH 'skyrim.db' AS lo;  -- the plugin DB
)HELP";

int Usage() {
    fprintf(stderr,
            "usage:\n"
            "  skqe info   <plugin> [--json] [--out file]\n"
            "  skqe fields <plugin> <TYPE>\n"
            "  skqe export <plugin> [--form ID,..] [<plugin> [--form ID,..]].. [--type T,..] [--limit N [--random [--seed S]]] [--fields F,..] [--csv|--json] [--out file]\n"
            "  skqe export --request <file|-> [--data <folder>] [--type T,..] [--fields F,..] [--csv|--json] [--out file]\n"
            "  skqe export --load-order <plugins.txt> --data <folder> [--form ID,..] [--form-file <file|->] [--type T,..] [--fields F,..] [--csv|--json] [--out file]\n"
            "  skqe diff <copy> <copy>.. | --name X.esp --in <folder>.. [--type T,..] [--form ID,..] [--fields F,..] [--summary] [--include-unknown] [--out file]\n"
            "  skqe build-db --out <db> --data <Data> [--load-order plugins.txt | --plugins a,b,..] [--with m,..] [--skip e,..]\n"
            "  skqe db add <module>[,..] --db <db>\n"
            "  skqe build-assets --out <db> --data <Data> [--staging <root> | --mo2 <dir> --profile <p>]\n"
            "  skqe help [command]      (full explanation)\n");
    return 2;
}

int Help(const std::string& topic) {
    const char* text = topic == "info" ? kHelpInfo : topic == "fields" ? kHelpFields : topic == "staging" ? kHelpStaging : topic == "search" ? kHelpSearch
                     : topic == "export" ? kHelpExport : topic == "diff" ? kHelpDiff : topic == "build-db" ? kHelpBuildDb
                     : topic == "db" ? kHelpDb : topic == "build-assets" ? kHelpBuildAssets : kHelp;
    _setmode(_fileno(stdout), _O_BINARY);
    fwrite(text, 1, strlen(text), stdout);
    return 0;
}

// Collects the shape of a value: "DATA", "DATA.damage", "DATA.flags[]"...
void CollectPaths(const Value& v, const std::string& prefix, std::set<std::string>* out) {
    out->insert(prefix);
    if (v.kind() == Value::Kind::Map) {
        for (const auto& m : v.Members()) CollectPaths(m.second, prefix + "." + m.first, out);
    } else if (v.kind() == Value::Kind::List) {
        for (const auto& it : v.Items())
            if (it.kind() == Value::Kind::Map) CollectPaths(it, prefix, out);
    }
}

// skqe db add|drop <name>[,<name>...] --db <file.db>
int DbCommand(int argc, wchar_t** argv) {
    std::string sub = argc > 2 ? Utf8(argv[2]) : "";
    if (sub != "add" && sub != "drop") {
        fprintf(stderr, "usage: skqe db add|drop <name>[,<name>] --db <file.db> (skqe db --help)\n");
        return 2;
    }
    AddOptions opt;
    for (int i = 3; i < argc; ++i) {
        std::string a = Utf8(argv[i]);
        bool hasNext = i + 1 < argc;
        if (a == "--db" && hasNext) opt.db = argv[++i];
        else if (a == "--threads" && hasNext) opt.threads = _wtoi(argv[++i]);
        else if (a == "--quiet") opt.quiet = true;
        else if (a == "--patterns" && hasNext) opt.patterns = argv[++i];
        else if (!a.empty() && a[0] != '-') {
            for (const auto& m : SplitList(a)) opt.modules.push_back(m);
        } else {
            fprintf(stderr, "db %s: unknown option %s (skqe db --help)\n", sub.c_str(), a.c_str());
            return 2;
        }
    }
    if (opt.db.empty() || opt.modules.empty()) {
        fprintf(stderr, "db %s: a name and --db are required (skqe db --help)\n", sub.c_str());
        return 2;
    }
    std::string err;
    int rc = sub == "add" ? AddModules(opt, &err) : DropExtras(opt, &err);
    if (rc) fprintf(stderr, "db %s: %s\n", sub.c_str(), err.c_str());
    return rc;
}

int BuildDbCommand(int argc, wchar_t** argv) {
    BuildOptions opt;
    for (int i = 2; i < argc; ++i) {
        std::string a = Utf8(argv[i]);
        bool hasNext = i + 1 < argc;
        if (a == "--out" && hasNext) opt.out = argv[++i];
        else if (a == "--data" && hasNext) opt.data = argv[++i];
        else if (a == "--load-order" && hasNext) opt.loadOrderFile = argv[++i];
        else if (a == "--plugins" && hasNext) {
            std::string list = Utf8(argv[++i]);
            for (size_t p = 0; p <= list.size();) {
                size_t c = std::min(list.find(',', p), list.size());
                if (c > p) opt.plugins.push_back(list.substr(p, c - p));
                p = c + 1;
            }
        } else if (a == "--language" && hasNext) opt.language = Utf8(argv[++i]);
        else if (a == "--threads" && hasNext) opt.threads = _wtoi(argv[++i]);
        else if (a == "--quiet") opt.quiet = true;
        else if (a == "--patterns" && hasNext) opt.patterns = argv[++i];
        else if (a == "--with" && hasNext) {
            for (const auto& m : SplitList(Utf8(argv[++i]))) opt.modules.push_back(m);
        } else if (a == "--skip" && hasNext) {
            for (const auto& m : SplitList(Utf8(argv[++i]))) opt.skip.push_back(m);
        } else {
            fprintf(stderr, "build-db: unknown option %s (skqe build-db --help)\n", a.c_str());
            return 2;
        }
    }
    if (opt.out.empty() || opt.data.empty()) {
        fprintf(stderr, "build-db: --out and --data are required (skqe build-db --help)\n");
        return 2;
    }
    std::string err;
    int rc = BuildDb(opt, &err);
    if (rc) fprintf(stderr, "build-db: %s\n", err.c_str());
    return rc;
}

int SearchCommand(int argc, wchar_t** argv) {
    std::wstring data, staging;
    std::wstring loadOrder;
    std::string manager, pattern;
    bool json = false, all = false, winner = false, anywhere = false, bsaOnly = false, showFilters = false, regex = false;
    int threads = 0;
    size_t limit = 0;
    for (int i = 2; i < argc; ++i) {
        std::string a = Utf8(argv[i]);
        bool hasNext = i + 1 < argc;
        if (a == "--data" && hasNext) data = argv[++i];
        else if (a == "--staging" && hasNext) { staging = argv[++i]; all = true; }
        else if (a == "--manager" && hasNext) manager = Utf8(argv[++i]);
        else if (a == "--limit" && hasNext) limit = static_cast<size_t>(_wtoi64(argv[++i]));
        else if (a == "--threads" && hasNext) threads = _wtoi(argv[++i]);
        else if (a == "--load-order" && hasNext) loadOrder = argv[++i];
        else if (a == "--all") all = true;
        else if (a == "--regex") regex = true;
        else if (a == "--winner") winner = true;
        else if (a == "--anywhere") anywhere = true;
        else if (a == "--bsa-only") bsaOnly = true;
        else if (a == "--show-filters") showFilters = true;
        else if (a == "--json") json = true;
        else if (a.rfind("--", 0) == 0) {
            fprintf(stderr, "search: unknown option %s (skqe search --help)\n", a.c_str());
            return 2;
        } else if (pattern.empty()) pattern = a;
        else {
            fprintf(stderr, "search: one search text at a time; quote it if it has spaces\n");
            return 2;
        }
    }
    if (winner && all) {
        fprintf(stderr, "search: --winner looks only in Data, so it can't be used with --all or --staging\n");
        return 2;
    }
    if (winner && bsaOnly) {
        fprintf(stderr, "search: --winner looks at loose files first, so it can't be used with --bsa-only\n");
        return 2;
    }
    if (pattern.empty() && !showFilters) {
        fprintf(stderr, "search: what to search for? (skqe search --help)\n");
        return 2;
    }
    const StagingEnv env = CurrentStagingEnv();
    std::wstring dataDir;
    std::string err;

    // A user's own copy, so an update can't replace it, else the one that ships
    // (SKQE_DATA, next to the exe, or built in: src/data_files.h).
    std::wstring filtersFile = env.localAppData + L"\\skqe\\search_filters.txt";
    std::string filtersText;
    bool haveFilters = false;
    if (GetFileAttributesW(filtersFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::ifstream ff(filtersFile, std::ios::binary);
        std::stringstream ss;
        ss << ff.rdbuf();
        filtersText = ss.str();
        haveFilters = true;
    } else {
        haveFilters = ReadDataFile(L"search_filters.txt", &filtersText, &filtersFile);
    }
    SearchFilters filters;
    if (haveFilters && !filters.Parse(filtersText, &err)) {
        fprintf(stderr, "search: %s: %s\n", Utf8(filtersFile).c_str(), err.c_str());
        return 1;
    }
    if (showFilters) {
        if (!haveFilters) {
            printf("no search_filters.txt found; searches use no filters\n");
            return 0;
        }
        printf("%s\n%zu rules, %zu excluded names\n", Utf8(filtersFile).c_str(), filters.RuleCount(), filters.ExcludeCount());
        return 0;
    }
    if (!haveFilters && !anywhere && !bsaOnly) fprintf(stderr, "warning: no search_filters.txt found, so nothing is filtered\n");
    if (!ResolveDataFolder(data, env, &dataDir, &err)) {
        fprintf(stderr, "search: %s\n", err.c_str());
        return 1;
    }
    AssetSearchOptions opt;
    opt.pattern = pattern;
    opt.threads = threads;
    opt.roots.push_back({dataDir, "Data"});
    opt.bsaOnly = bsaOnly;
    opt.regex = regex;
    if (haveFilters && !anywhere && !bsaOnly) opt.filters = &filters;
    std::unordered_map<std::string, int> ranks;
    if (winner) {
        if (loadOrder.empty()) {
            const std::wstring own = env.localAppData + L"\\Skyrim Special Edition\\plugins.txt";
            if (GetFileAttributesW(own.c_str()) != INVALID_FILE_ATTRIBUTES) loadOrder = own;
        }
        if (!LoadedArchiveRanks(dataDir, loadOrder, &ranks, &err)) {
            fprintf(stderr, "search: %s\n", err.c_str());
            return 1;
        }
        opt.winnerRanks = &ranks;
    }
    if (all) {
        std::wstring root = staging, overwrite;
        if (root.empty()) {
            StagingResult found;
            if (!FindStaging(dataDir, manager, env, &found, &err)) {
                fprintf(stderr, "search: %s\n", err.c_str());
                return 1;
            }
            root = found.chosen.staging;
            if (found.chosen.manager == "mo2") overwrite = ResolveMo2Folders(found.chosen.instance).overwrite;
        }
        auto mods = StagingRoots(root);
        if (mods.empty()) {
            fprintf(stderr, "search: no mod folders in %s\n", Utf8(root).c_str());
            return 1;
        }
        opt.roots.insert(opt.roots.end(), mods.begin(), mods.end());
        if (!overwrite.empty() && GetFileAttributesW(overwrite.c_str()) != INVALID_FILE_ATTRIBUTES) opt.roots.push_back({overwrite, "overwrite"});
    }
    const ULONGLONG start = GetTickCount64();
    AssetSearchResult r;
    if (!SearchAssets(opt, &r, &err)) {
        fprintf(stderr, "search: %s\n", err.c_str());
        return 1;
    }
    const size_t total = r.files.size();
    const size_t shown = limit && limit < total ? limit : total;
    std::string out;
    size_t copies = 0;
    for (const auto& f : r.files) copies += f.copies.size();
    if (json) out = "[\n";
    for (size_t i = 0; i < shown; ++i) {
        const auto& f = r.files[i];
        if (json) {
            std::vector<Value> list;
            for (const auto& c : f.copies) {
                Value v = Value::Map();
                if (!c.archive.empty()) v.Set("archive", Value::Str(c.archive));
                std::vector<Value> in;
                for (const auto& n : c.in) in.push_back(Value::Str(n));
                v.Set("in", Value::List(std::move(in)));
                if (c.size >= 0) v.Set("size", Value::Int(c.size));
                list.push_back(std::move(v));
            }
            Value obj = Value::Map();
            obj.Set("path", Value::Str(f.path));
            obj.Set("copies", Value::List(std::move(list)));
            WriteJson(obj, &out);
            out += i + 1 < shown ? ",\n" : "\n";
        } else {
            out += f.path + "\n";
            for (const auto& c : f.copies) {
                out += c.archive.empty() ? std::string("  loose: ") : "  in " + c.archive + ": ";
                for (size_t k = 0; k < c.in.size(); ++k) out += (k ? ", " : "") + c.in[k];
                out += "\n";
            }
        }
    }
    if (json) out += "]\n";
    _setmode(_fileno(stdout), _O_BINARY);
    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
    for (const auto& w : r.warnings) fprintf(stderr, "warning: %s\n", w.c_str());
    if (r.narrowedToDataRoot || !r.narrowedTo.empty()) {
        std::string where = r.narrowedToDataRoot ? "Data's top folder" : "";
        for (const auto& f : r.narrowedTo) where += (where.empty() ? "" : ", ") + f;
        fprintf(stderr, "filter: looked only in %s (--anywhere to search everywhere)\n", where.c_str());
    }
    fprintf(stderr, "%zu paths, %zu distinct copies%s; %lld hardlinks merged; searched %lld loose files and %lld packed in %lld BSAs in %.2f s\n",
            total, copies, shown < total ? " (output limited)" : "", static_cast<long long>(r.hardlinksMerged),
            static_cast<long long>(r.looseScanned), static_cast<long long>(r.packedScanned),
            static_cast<long long>(r.archivesRead), (GetTickCount64() - start) / 1000.0);
    return 0;
}

int StagingCommand(int argc, wchar_t** argv) {
    std::wstring data, setData, setStaging;
    std::string manager;
    bool json = false, clear = false, show = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = Utf8(argv[i]);
        bool hasNext = i + 1 < argc;
        if (a == "--data" && hasNext) data = argv[++i];
        else if (a == "--manager" && hasNext) manager = Utf8(argv[++i]);
        else if (a == "--set-data" && hasNext) setData = argv[++i];
        else if (a == "--set-staging" && hasNext) setStaging = argv[++i];
        else if (a == "--clear") clear = true;
        else if (a == "--show") show = true;
        else if (a == "--json") json = true;
        else {
            fprintf(stderr, "staging: unknown option %s (skqe staging --help)\n", a.c_str());
            return 2;
        }
    }
    if (clear || show || !setData.empty() || !setStaging.empty()) {
        const StagingEnv env = CurrentStagingEnv();
        StagingSettings s = clear ? StagingSettings() : LoadStagingSettings(env);
        auto setFolder = [](const std::wstring& in, std::wstring* out) {
            wchar_t buf[MAX_PATH * 2];
            DWORD n = GetFullPathNameW(in.c_str(), MAX_PATH * 2, buf, nullptr);
            const std::wstring full(buf, n);
            DWORD at = GetFileAttributesW(full.c_str());
            if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) {
                fprintf(stderr, "staging: no folder at %s\n", Utf8(full).c_str());
                return false;
            }
            *out = full;
            return true;
        };
        if (!setData.empty() && !setFolder(setData, &s.data)) return 1;
        if (!setStaging.empty() && !setFolder(setStaging, &s.staging)) return 1;
        std::string err;
        if ((clear || !setData.empty() || !setStaging.empty()) && !SaveStagingSettings(env, s, &err)) {
            fprintf(stderr, "staging: %s\n", err.c_str());
            return 1;
        }
        std::string out = "data: " + (s.data.empty() ? std::string("(not set)") : Utf8(s.data)) + "\nstaging: " +
                          (s.staging.empty() ? std::string("(not set)") : Utf8(s.staging)) + "\n";
        fwrite(out.data(), 1, out.size(), stdout);
        return 0;
    }
    if (!manager.empty() && manager != "vortex" && manager != "mo2") {
        fprintf(stderr, "staging: --manager is vortex or mo2\n");
        return 2;
    }
    StagingResult r;
    std::string err;
    if (!FindStaging(data, manager, CurrentStagingEnv(), &r, &err)) {
        fprintf(stderr, "staging: %s\n", err.c_str());
        return 1;
    }
    const StagingCandidate& c = r.chosen;
    auto isChosen = [&](const StagingCandidate& o) { return o.manager == c.manager && o.staging == c.staging; };
    std::string out;
    if (json) {
        auto describe = [&](const StagingCandidate& o, Value* v) {
            v->Set("manager", Value::Str(o.manager));
            v->Set("staging", Value::Str(Utf8(o.staging)));
            if (o.manager == "mo2") {
                v->Set("instance", Value::Str(Utf8(o.instance)));
                v->Set("profile", Value::Str(o.profile));
                v->Set("matches_data", Value::Bool(o.matchesData));
            } else {
                v->Set("deployed", Value::Bool(o.deployed));
            }
            v->Set("source", Value::Str(o.source));
        };
        Value obj = Value::Map();
        obj.Set("data", Value::Str(Utf8(r.data)));
        describe(c, &obj);
        std::vector<Value> also;
        for (const auto& o : r.all) {
            if (isChosen(o)) continue;
            Value v = Value::Map();
            describe(o, &v);
            also.push_back(std::move(v));
        }
        obj.Set("also_found", Value::List(std::move(also)));
        WriteJson(obj, &out);
        out += "\n";
    } else {
        out = "manager: " + c.manager + "\nstaging: " + Utf8(c.staging) + "\n";
        if (c.manager == "mo2") out += "instance: " + Utf8(c.instance) + "\nprofile: " + c.profile + "\n";
        out += "found by: " + c.source + "\n";
        if (c.manager == "vortex") out += std::string("deployed: ") + (c.deployed ? "yes" : "no") + "\n";
        for (const auto& o : r.all) {
            if (isChosen(o)) continue;
            out += "also found: " + o.manager + " " + Utf8(o.manager == "mo2" ? o.instance : o.staging);
            if (o.manager == "mo2" && !o.matchesData) out += " (a different game folder)";
            out += "\n";
        }
    }
    _setmode(_fileno(stdout), _O_BINARY);
    fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}

int BuildAssetsCommand(int argc, wchar_t** argv) {
    AssetOptions opt;
    std::string manager;
    for (int i = 2; i < argc; ++i) {
        std::string a = Utf8(argv[i]);
        bool hasNext = i + 1 < argc;
        if (a == "--out" && hasNext) opt.out = argv[++i];
        else if (a == "--data" && hasNext) opt.data = argv[++i];
        else if (a == "--staging" && hasNext) opt.staging = argv[++i];
        else if (a == "--mo2" && hasNext) opt.mo2 = argv[++i];
        else if (a == "--manager" && hasNext) manager = Utf8(argv[++i]);
        else if (a == "--profile" && hasNext) opt.profile = argv[++i];
        else if (a == "--load-order" && hasNext) opt.loadOrderFile = argv[++i];
        else if (a == "--hash") opt.hash = true;
        else if (a == "--threads" && hasNext) opt.threads = _wtoi(argv[++i]);
        else if (a == "--quiet") opt.quiet = true;
        else if (a == "--skip" && hasNext) {
            for (const auto& m : SplitList(Utf8(argv[++i]))) opt.skip.push_back(m);
        } else {
            fprintf(stderr, "build-assets: unknown option %s (skqe build-assets --help)\n", a.c_str());
            return 2;
        }
    }
    if (opt.out.empty() || opt.data.empty()) {
        fprintf(stderr, "build-assets: --out and --data are required (skqe build-assets --help)\n");
        return 2;
    }
    std::string err;
    if (opt.staging == L"auto") {
        StagingResult found;
        if (!FindStaging(opt.data, manager, CurrentStagingEnv(), &found, &err)) {
            fprintf(stderr, "build-assets: %s\n", err.c_str());
            return 1;
        }
        opt.staging.clear();
        if (found.chosen.manager == "mo2") {
            opt.mo2 = found.chosen.instance;
            if (opt.profile.empty()) opt.profile = WidenUtf8(found.chosen.profile);
        } else {
            opt.staging = found.chosen.staging;
        }
        if (!opt.quiet)
            fprintf(stderr, "staging: %s, %s\n", found.chosen.manager.c_str(), Utf8(found.chosen.staging).c_str());
    }
    int rc = BuildAssets(opt, &err);
    if (rc) fprintf(stderr, "build-assets: %s\n", err.c_str());
    return rc;
}

}  // namespace

bool LoadCatalog(std::wstring patternsPath, PatternCatalog* cat) {
    // --patterns, else SKQE_DATA, next to the exe, or built in (src/data_files.h).
    std::string text;
    if (patternsPath.empty()) {
        ReadDataFile(L"patterns.json", &text);
    } else {
        std::ifstream pf(patternsPath, std::ios::binary);
        std::stringstream ss;
        ss << pf.rdbuf();
        text = ss.str();
    }
    std::string err;
    if (!cat->Load(text, &err)) {
        fprintf(stderr, "patterns: %s\n", err.empty() ? "cannot read patterns.json" : err.c_str());
        return false;
    }
    return true;
}

std::wstring WidenUtf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// --labels: names for coded values, from ut_reference.sql next to the exe (the
// tables build-db loads). A field bound to an enumeration gets a sibling
// "<field>_label" with its value's name; a field bound to a flag set gets the
// names of its set bits (an unnamed bit as "bit N"). A condition (CTDA) gets
// "function", the condition function's name. Binding is build-db's: the
// earlier database's field links first, then <type>_<subrecord>_<field> by name.
struct Labels {
    std::map<std::string, std::pair<std::string, std::string>> links;  // "RT|FS|leaf" -> enum kind, flag kind
    std::map<std::string, std::map<int64_t, std::string>> enums, flags;
    std::map<int64_t, std::string> functions;

    bool Load(const std::string& sql, std::string* error) {
        sqlite3* db = nullptr;
        if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
            *error = "cannot open an in-memory database";
            return false;
        }
        char* msg = nullptr;
        bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &msg) == SQLITE_OK;
        if (!ok) *error = std::string("ut_reference.sql: ") + (msg ? msg : "");
        sqlite3_free(msg);
        auto each = [&](const char* sql, auto&& row) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
            while (sqlite3_step(st) == SQLITE_ROW) row(st);
            sqlite3_finalize(st);
        };
        auto text = [](sqlite3_stmt* st, int i) {
            const unsigned char* t = sqlite3_column_text(st, i);
            return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
        };
        if (ok) {
            each("SELECT record_type, subrecord, leaf, enum_kind, flag_kind FROM ut_field_links", [&](sqlite3_stmt* st) {
                links[text(st, 0) + "|" + text(st, 1) + "|" + text(st, 2)] = {text(st, 3), text(st, 4)};
            });
            each("SELECT k.kind_name, v.position, v.label FROM ut_enum_values v JOIN ut_enum_kinds k ON k.id = v.fk_kind",
                 [&](sqlite3_stmt* st) { enums[text(st, 0)][sqlite3_column_int64(st, 1)] = text(st, 2); });
            each("SELECT k.kind_name, v.bit, v.label FROM ut_flag_values v JOIN ut_flag_kinds k ON k.id = v.fk_kind",
                 [&](sqlite3_stmt* st) { flags[text(st, 0)][sqlite3_column_int64(st, 1)] = text(st, 2); });
            each("SELECT func_index, name FROM ut_ctda_functions",
                 [&](sqlite3_stmt* st) { functions[sqlite3_column_int64(st, 0)] = text(st, 1); });
        }
        sqlite3_close(db);
        return ok;
    }

    // The label of `v` as field `leaf` of subrecord `fs` in record type `rt`;
    // null when the field has no enumeration or flag set, or v isn't a number.
    Value Label(const std::string& rt, const std::string& fs, const std::string& leaf, const Value& v) const {
        if (v.kind() != Value::Kind::Int) return Value();
        std::string e, g;
        auto it = links.find(rt + "|" + fs + "|" + leaf);
        if (it != links.end()) {
            e = it->second.first;
            g = it->second.second;
        } else {
            auto lower = [](std::string t) {
                for (char& c : t) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
                return t;
            };
            const std::string base = lower(rt) + "_" + lower(fs) + (leaf.empty() ? "" : "_" + leaf);
            if (flags.count(base)) g = base;
            else if (flags.count(base + "_flags")) g = base + "_flags";
            else if (enums.count(base)) e = base;
            else if (enums.count(base + "_type")) e = base + "_type";
        }
        if (!e.empty()) {
            auto k = enums.find(e);
            if (k == enums.end()) return Value();
            auto n = k->second.find(v.AsInt());
            return n == k->second.end() ? Value() : Value::Str(n->second);
        }
        if (!g.empty()) {
            auto k = flags.find(g);
            if (k == flags.end()) return Value();
            Value list = Value::List();
            const uint64_t bits = static_cast<uint64_t>(v.AsInt());
            for (int b = 0; b < 64; ++b) {
                if (!(bits >> b & 1)) continue;
                auto n = k->second.find(b);
                list.Items().push_back(Value::Str(n != k->second.end() ? n->second : "bit " + std::to_string(b)));
            }
            return list;
        }
        return Value();
    }

    // A subrecord's value with labels added beside its coded fields, at any depth.
    Value Apply(const std::string& rt, const std::string& fs, const Value& v) const {
        if (v.kind() == Value::Kind::List) {
            Value out = Value::List();
            for (const auto& item : v.Items()) out.Items().push_back(Apply(rt, fs, item));
            return out;
        }
        if (v.kind() != Value::Kind::Map) return v;
        Value out = Value::Map();
        for (const auto& [key, member] : v.Members()) {
            out.Set(key, Apply(rt, fs, member));
            if (fs == "CTDA" && key == "func_idx" && member.kind() == Value::Kind::Int) {
                auto n = functions.find(member.AsInt());
                if (n != functions.end()) out.Set("function", Value::Str(n->second));
            }
            Value label = Label(rt, fs, key, member);
            if (label.kind() != Value::Kind::Null) out.Set(key + "_label", std::move(label));
        }
        return out;
    }

    // A record's "fields" object: whole subrecords keyed by signature, or the
    // paths --fields chose ("DATA.damage").
    Value ApplyFields(const std::string& rt, const Value& fields) const {
        Value out = Value::Map();
        for (const auto& [key, v] : fields.Members()) {
            const size_t dot = key.find('.');
            const std::string fs = key.substr(0, dot);
            const std::string leaf = dot == std::string::npos ? "" : key.substr(key.find_last_of('.') + 1);
            out.Set(key, Apply(rt, fs, v));
            Value label = Label(rt, fs, leaf, v);
            if (label.kind() == Value::Kind::Null && leaf.empty()) {
                // A one-value subrecord: its link names the subrecord's only field.
                const std::string prefix = rt + "|" + fs + "|";
                auto it = links.lower_bound(prefix);
                if (it != links.end() && it->first.compare(0, prefix.size(), prefix) == 0)
                    label = Label(rt, fs, it->first.substr(prefix.size()), v);
            }
            if (label.kind() != Value::Kind::Null) out.Set(key + "_label", std::move(label));
        }
        return out;
    }
};
const Labels* gLabels = nullptr;  // set by export --labels

// One exported record: form, type, in, deleted, fields.
void RowJson(const Plugin& plugin, const Record& r, const std::vector<FieldSpec>& specs, Value* obj) {
    auto fields = plugin.Fields(r);
    obj->Set("form", Value::Str(plugin.FormKey(r.formId)));
    obj->Set("type", Value::Str(r.type));
    if (r.parent) obj->Set("in", Value::Str(plugin.FormKey(r.parent)));
    if (r.flags & 0x20) obj->Set("deleted", Value::Bool(true));
    if (specs.empty()) {
        obj->Set("fields", AllFields(fields));
    } else {
        Value sel = Value::Map();
        for (const auto& s : specs) sel.Set(s.text, Gather(fields, s));
        obj->Set("fields", std::move(sel));
    }
    if (gLabels) {
        Value labelled = gLabels->ApplyFields(r.type, *obj->Find("fields"));
        obj->Set("fields", std::move(labelled));
    }
}

// The same as CSV cells: form,type,in,<fields>, no line end.
std::string RowCsv(const Plugin& plugin, const Record& r, const std::vector<FieldSpec>& specs) {
    auto fields = plugin.Fields(r);
    Value parent = r.parent ? Value::Str(plugin.FormKey(r.parent)) : Value();
    std::string out = CsvCell(Value::Str(plugin.FormKey(r.formId))) + gSep + r.type + gSep + CsvCell(parent);
    for (const auto& s : specs) out += gSep + CsvCell(Gather(fields, s));
    return out;
}

std::string LowerAscii(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// skqe export --load-order / --plugins: every copy of each form across a load
// order, grouped by form, in load order, the last copy marked as the winner.
struct LoadOrderExport {
    BuildOptions order;
    std::vector<std::string> forms;  // Owner:ObjectID; empty = every form of `types`
    std::set<std::string> types;
    std::vector<FieldSpec> specs;
    bool csv = false;
    int threads = 0;
    enum class Conflicts { Off, Values, Summary } conflicts = Conflicts::Off;
    bool allPlugins = false;            // every plugin in plugins.txt, active or not
    std::set<std::string> activeLower;  // with allPlugins: the plugins the game loads
};

// Whole subrecords xEdit labels Unused or Unknown.
const struct {
    const char* record;
    const char* sig;
} kUnusedSubrecords[] = {
#include "../src/unused_subrecords.inc"
};

bool IsUnusedSubrecord(const std::string& record, const std::string& sig) {
    for (const auto& u : kUnusedSubrecords)
        if (sig == u.sig && record == u.record) return true;
    return false;
}

bool NamedUnknownOrUnused(const std::string& name) {
    const std::string n = LowerAscii(name);
    return n.find("unknown") != std::string::npos || n.find("unused") != std::string::npos;
}

// The value without anything named unknown or unused, at any depth. False when
// that was all it held, so a subrecord of only those doesn't count as present.
bool StripUnknown(const Value& v, Value* out) {
    if (v.kind() == Value::Kind::Map) {
        *out = Value::Map();
        for (const auto& [k, item] : v.Members()) {
            if (NamedUnknownOrUnused(k)) continue;
            Value kept;
            if (StripUnknown(item, &kept)) out->Set(k, std::move(kept));
        }
        return !out->Members().empty() || v.Members().empty();
    }
    if (v.kind() == Value::Kind::List) {
        *out = Value::List();
        for (const auto& item : v.Items()) {
            Value kept;
            if (StripUnknown(item, &kept)) out->Items().push_back(std::move(kept));
        }
        return true;
    }
    *out = v;
    return true;
}

// Compressed is how a record is stored, not what it says.
const uint32_t kStorageFlags = 0x00040000;

// The fields a conflict is judged on: Record Flags, then what remains of each field.
void ConflictFields(const Record& r, const std::vector<FieldSpec>& specs, Value* row, bool keepUnknown = false) {
    const Value* all = row->Find("fields");
    Value kept = Value::Map();
    bool wantFlags = specs.empty();
    for (const auto& s : specs) wantFlags = wantFlags || s.text == "Record Flags";
    if (wantFlags) {
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%08X", r.flags & ~kStorageFlags);
        kept.Set("Record Flags", Value::Str(buf));
    }
    for (const auto& [k, v] : all->Members()) {
        if (k == "Record Flags") continue;
        if (keepUnknown) {
            kept.Set(k, v);
            continue;
        }
        if (IsUnusedSubrecord(r.type, k)) continue;
        Value clean;
        if (StripUnknown(v, &clean)) kept.Set(k, std::move(clean));
    }
    row->Set("fields", std::move(kept));
}

// A value as compared between copies: its JSON, with the owner in FormIDs lower
// case, since plugins spell a master's name differently in their headers, and
// one spelling of zero.
void CompareKey(const Value& v, std::string* out) {
    switch (v.kind()) {
        case Value::Kind::String: {
            const std::string& t = v.AsString();
            size_t n = t.size();
            bool formKey = n > 11 && t[n - 7] == ':' && t[n - 11] == '.' &&
                           t.find_first_not_of("0123456789ABCDEF", n - 6) == std::string::npos;
            WriteJson(formKey ? Value::Str(LowerAscii(t)) : v, out);
            break;
        }
        case Value::Kind::Float:  // -0.0 and 0.0 are the same value in different bytes
            WriteJson(v.AsFloat() == 0 ? Value::Float(0.0) : v, out);
            break;
        case Value::Kind::List:
            *out += '[';
            for (const auto& item : v.Items()) {
                CompareKey(item, out);
                *out += ',';
            }
            *out += ']';
            break;
        case Value::Kind::Map:
            *out += '{';
            for (const auto& [k, item] : v.Members()) {
                *out += k + '=';
                CompareKey(item, out);
                *out += ',';
            }
            *out += '}';
            break;
        default:
            WriteJson(v, out);
    }
}

int ExportLoadOrder(const LoadOrderExport& req, const PatternCatalog& cat, std::string* out) {
    std::string err;
    const std::vector<std::string> names = LoadOrder(req.order, nullptr, &err);
    if (names.empty()) {
        fprintf(stderr, "load order: %s\n", err.empty() ? "no plugins found" : err.c_str());
        return 1;
    }
    // Requested forms: owner (lower case) and object ID. Repeats count once.
    struct Wanted {
        std::string spec, owner;
        uint32_t local;
    };
    std::vector<Wanted> wanted;
    std::set<std::string> seenSpec;
    for (const auto& spec : req.forms) {
        size_t colon = spec.rfind(':');
        if (colon == std::string::npos) {
            fprintf(stderr, "a load order needs forms as Owner:ObjectID, not %s\n", spec.c_str());
            return 2;
        }
        uint32_t local = static_cast<uint32_t>(strtoul(spec.c_str() + colon + 1, nullptr, 16)) & 0xFFFFFF;
        std::string owner = LowerAscii(spec.substr(0, colon));
        char key[16];
        snprintf(key, sizeof(key), ":%06X", local);
        if (seenSpec.insert(owner + key).second) wanted.push_back({spec, owner, local});
    }

    // Each plugin is opened, searched and rendered by one worker, then closed.
    struct Row {
        std::string key;      // the form, lower case; no form list
        size_t wantedNo = 0;  // the form; with a form list
        Value json = Value::Map();
        std::string csv;
    };
    std::vector<std::vector<Row>> rowsOf(names.size());
    std::vector<std::string> shownOf(names.size());
    std::atomic<size_t> next{0};
    std::atomic<int> status{0};
    std::mutex errMutex;
    auto work = [&] {
        for (size_t i; (i = next++) < names.size();) {
            std::wstring path = req.order.data;
            if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
            path += WidenUtf8(names[i]);
            std::string openErr;
            auto plugin = Plugin::Open(path, cat, &openErr);
            shownOf[i] = names[i];
            if (!plugin) {
                std::lock_guard<std::mutex> lock(errMutex);
                fprintf(stderr, "%s: %s\n", names[i].c_str(), openErr.c_str());
                status = 1;
                continue;
            }
            auto add = [&](const Record& r, size_t wantedNo) {
                Row row;
                row.wantedNo = wantedNo;
                if (req.forms.empty()) row.key = LowerAscii(plugin->FormKey(r.formId));
                if (req.csv) row.csv = RowCsv(*plugin, r, req.specs);
                else RowJson(*plugin, r, req.specs, &row.json);
                if (req.conflicts != LoadOrderExport::Conflicts::Off) ConflictFields(r, req.specs, &row.json);
                rowsOf[i].push_back(std::move(row));
            };
            if (!req.forms.empty()) {
                // A form's owner is this plugin or one of its masters; otherwise it can't have a copy.
                std::map<std::string, uint32_t> indexOf;
                const auto masters = plugin->mastersUtf8();
                for (size_t m = 0; m < masters.size(); ++m) indexOf.emplace(LowerAscii(masters[m]), static_cast<uint32_t>(m));
                indexOf.emplace(LowerAscii(plugin->nameUtf8()), static_cast<uint32_t>(masters.size()));
                for (size_t w = 0; w < wanted.size(); ++w) {
                    auto it = indexOf.find(wanted[w].owner);
                    if (it == indexOf.end()) continue;
                    const Record* r = plugin->Find((it->second << 24) | wanted[w].local);
                    if (r && (req.types.empty() || req.types.count(r->type))) add(*r, w);
                }
            } else {
                for (const auto& r : plugin->records())
                    if (r.type != "TES4" && req.types.count(r.type)) add(r, 0);
            }
        }
    };
    int threads = req.threads > 0 ? req.threads : std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    threads = std::min<size_t>(threads, names.size());
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back(work);
    for (auto& t : pool) t.join();

    const bool conflictMode = req.conflicts != LoadOrderExport::Conflicts::Off;
    auto isActive = [&](size_t plugin) { return !req.allPlugins || req.activeLower.count(LowerAscii(names[plugin])) > 0; };

    // Group by form, keeping load order inside each group. Requested forms come
    // in the order asked; otherwise each form comes where it first appears.
    struct Copy {
        size_t plugin;
        Row* row;
    };
    std::vector<std::vector<Copy>> groups(wanted.size());
    std::unordered_map<std::string, size_t> groupOfKey;
    for (size_t i = 0; i < names.size(); ++i) {
        for (auto& row : rowsOf[i]) {
            size_t g = row.wantedNo;
            if (req.forms.empty()) {
                auto [it, isNew] = groupOfKey.emplace(row.key, groups.size());
                if (isNew) groups.emplace_back();
                g = it->second;
            }
            groups[g].push_back({i, &row});
        }
    }
    if (req.csv) {
        for (const char* name : {"plugin", "load_order", "winner", "form", "type"}) *out += name + gSep;
        *out += "in";
        for (const auto& s : req.specs) *out += gSep + CsvCell(Value::Str(s.text));
        *out += "\n";
    } else {
        *out += "[\n";
    }
    bool first = true;
    for (size_t g = 0; g < groups.size(); ++g) {
        if (groups[g].empty() && !req.forms.empty())
            fprintf(stderr, "no such form in this load order: %s\n", wanted[g].spec.c_str());
        // Only an active copy can win in game.
        size_t winnerNo = groups[g].size();
        for (size_t c = groups[g].size(); c-- > 0;) {
            if (isActive(groups[g][c].plugin)) {
                winnerNo = c;
                break;
            }
        }
        // Fields whose value differs between any two copies; a copy without the field counts as null.
        std::vector<std::string> conflicting;
        if (conflictMode && !groups[g].empty()) {
            std::vector<std::string> keys;
            std::set<std::string> known;
            for (const Copy& copy : groups[g]) {
                const Value* fields = copy.row->json.Find("fields");
                if (!fields) continue;
                for (const auto& [k, v] : fields->Members())
                    if (known.insert(k).second) keys.push_back(k);
            }
            for (const auto& key : keys) {
                std::string base;
                bool differs = false;
                for (size_t c = 0; c < groups[g].size() && !differs; ++c) {
                    const Value* v = groups[g][c].row->json.Find("fields")->Find(key);
                    std::string text;
                    if (v) CompareKey(*v, &text);
                    else text = "null";
                    if (c == 0) base = std::move(text);
                    else differs = text != base;
                }
                if (differs) conflicting.push_back(key);
            }
            // --type sweeps show only what conflicts; a form asked for by name always answers.
            if (conflicting.empty() && req.forms.empty()) continue;
        }
        if (req.conflicts == LoadOrderExport::Conflicts::Summary) {
            if (groups[g].empty()) continue;
            const Value& head = groups[g][0].row->json;
            Value obj = Value::Map();
            obj.Set("form", *head.Find("form"));
            obj.Set("type", *head.Find("type"));
            Value plugins = Value::List();
            for (const Copy& copy : groups[g]) plugins.Items().push_back(Value::Str(names[copy.plugin]));
            obj.Set("plugins", std::move(plugins));
            if (conflicting.empty()) {
                obj.Set("conflicts", Value::Str("no conflicts"));
            } else {
                Value list = Value::List();
                for (const auto& k : conflicting) list.Items().push_back(Value::Str(k));
                obj.Set("conflicts", std::move(list));
            }
            if (!first) *out += ",\n";
            first = false;
            WriteJson(obj, out);
            continue;
        }
        for (size_t c = 0; c < groups[g].size(); ++c) {
            const Copy& copy = groups[g][c];
            const bool winner = c == winnerNo;
            if (req.csv) {
                *out += CsvCell(Value::Str(shownOf[copy.plugin])) + gSep + std::to_string(copy.plugin) + gSep +
                        (winner ? "true" : "false") + gSep + copy.row->csv + "\n";
            } else {
                Value obj = Value::Map();
                obj.Set("plugin", Value::Str(shownOf[copy.plugin]));
                obj.Set("load_order", Value::Int(static_cast<int64_t>(copy.plugin)));
                obj.Set("winner", Value::Bool(winner));
                if (req.allPlugins) obj.Set("active", Value::Bool(isActive(copy.plugin)));
                for (const auto& [k, v] : copy.row->json.Members()) {
                    if (conflictMode && k == "fields") {
                        Value only = Value::Map();
                        for (const auto& key : conflicting) {
                            const Value* field = v.Find(key);
                            only.Set(key, field ? *field : Value());
                        }
                        obj.Set(k, std::move(only));
                    } else {
                        obj.Set(k, v);
                    }
                }
                if (!first) *out += ",\n";
                first = false;
                WriteJson(obj, out);
            }
        }
    }
    if (!req.csv) *out += "\n]\n";
    return status;
}

// skqe export: one or many plugins, each with its own forms.
int ExportCommand(int argc, wchar_t** argv) {
    std::vector<ExportTarget> targets;
    std::string types, fieldList, outPath, requestPath, formFile, explicitOrder;
    std::vector<std::string> looseForms;  // --form with no plugin before it: for a load order
    std::wstring patternsPath, dataDir, loadOrderFile;
    bool csv = false, activeOnly = false, allPlugins = false, randomPick = false, labels = false;
    size_t limit = 0;
    std::string limitText, seedText;
    auto conflicts = LoadOrderExport::Conflicts::Off;
    for (int i = 2; i < argc; ++i) {
        std::string a = Utf8(argv[i]);
        auto next = [&](std::string* dst) {
            if (i + 1 < argc) *dst = Utf8(argv[++i]);
        };
        if (a == "--type") next(&types);
        else if (a == "--form") {
            std::string list;
            next(&list);
            auto& dst = targets.empty() ? looseForms : targets.back().forms;
            for (auto& spec : SplitList(list)) dst.push_back(std::move(spec));
        }
        else if (a == "--fields") next(&fieldList);
        else if (a == "--out") next(&outPath);
        else if (a == "--request") next(&requestPath);
        else if (a == "--form-file") next(&formFile);
        else if (a == "--labels") labels = true;
        else if (a == "--conflicts") conflicts = LoadOrderExport::Conflicts::Values;
        else if (a == "--conflict-summary") conflicts = LoadOrderExport::Conflicts::Summary;
        else if (a == "--limit") next(&limitText);
        else if (a == "--seed") next(&seedText);
        else if (a == "--random") randomPick = true;
        else if (a == "--active-only") activeOnly = true;
        else if (a == "--all-plugins") allPlugins = true;
        else if (a == "--plugins") next(&explicitOrder);
        else if (a == "--load-order" && i + 1 < argc) loadOrderFile = argv[++i];
        else if (a == "--csv") { csv = true; gSep = ","; }
        else if (a == "--psv") { csv = true; gSep = "|"; }
        else if (a == "--json") csv = false;
        else if (a == "--data" && i + 1 < argc) dataDir = argv[++i];
        else if (a == "--patterns" && i + 1 < argc) patternsPath = argv[++i];
        else if (a.rfind("--", 0) == 0) return Usage();
        else targets.push_back({a, {}});
    }
    Labels labelData;
    if (labels) {
        if (csv) {
            fprintf(stderr, "--labels adds names to JSON output; it doesn't combine with --csv or --psv\n");
            return 2;
        }
        // Next to an explicit --patterns first, else where data files are looked for.
        std::string sql, error;
        if (!patternsPath.empty()) {
            std::ifstream f(patternsPath.substr(0, patternsPath.find_last_of(L"\\/") + 1) + L"ut_reference.sql",
                            std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            sql = ss.str();
        }
        if (sql.empty() && !ReadDataFile(L"ut_reference.sql", &sql)) {
            fprintf(stderr, "--labels: cannot find ut_reference.sql (in SKQE_DATA, next to skqe.exe, or built in)\n");
            return 2;
        }
        if (!labelData.Load(sql, &error)) {
            fprintf(stderr, "--labels: %s\n", error.c_str());
            return 2;
        }
        gLabels = &labelData;
    }
    if (!limitText.empty()) {
        char* end = nullptr;
        unsigned long long n = strtoull(limitText.c_str(), &end, 10);
        if (*end || n == 0 || limitText[0] == '-') {
            fprintf(stderr, "--limit needs a number above 0, not %s\n", limitText.c_str());
            return 2;
        }
        limit = static_cast<size_t>(n);
    }
    if ((randomPick || !seedText.empty()) && !limit) {
        fprintf(stderr, "--random and --seed need --limit\n");
        return 2;
    }
    if (!seedText.empty() && !randomPick) {
        fprintf(stderr, "--seed needs --random\n");
        return 2;
    }
    if (limit && (!loadOrderFile.empty() || !explicitOrder.empty())) {
        fprintf(stderr, "--limit picks records of one plugin: it doesn't combine with --load-order or --plugins\n");
        return 2;
    }
    std::mt19937_64 rng;
    if (randomPick) {
        if (!seedText.empty()) {
            rng.seed(std::strtoull(seedText.c_str(), nullptr, 10));
        } else {
            std::random_device device;  // the operating system's random source
            std::seed_seq seq{device(), device(), device(), device(), device(), device(), device(), device()};
            rng.seed(seq);
        }
    }
    auto readAll = [](const std::string& path, std::string* text) {
        if (path == "-") {
            _setmode(_fileno(stdin), _O_BINARY);
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) text->append(buf, n);
            return true;
        }
        std::ifstream rf(WidenUtf8(path), std::ios::binary);
        if (!rf) {
            fprintf(stderr, "cannot read %s\n", path.c_str());
            return false;
        }
        std::stringstream rs;
        rs << rf.rdbuf();
        *text = rs.str();
        return true;
    };
    if (loadOrderFile.empty() && explicitOrder.empty() &&
        (conflicts != LoadOrderExport::Conflicts::Off || activeOnly || allPlugins)) {
        fprintf(stderr, "--conflicts, --conflict-summary, --active-only and --all-plugins need --load-order\n");
        return 2;
    }
    if (!loadOrderFile.empty() || !explicitOrder.empty()) {
        if (activeOnly && allPlugins) {
            fprintf(stderr, "--active-only and --all-plugins exclude each other\n");
            return 2;
        }
        if ((activeOnly || allPlugins) && loadOrderFile.empty()) {
            fprintf(stderr, "--active-only and --all-plugins read plugins.txt: they need --load-order\n");
            return 2;
        }
        if (csv && conflicts != LoadOrderExport::Conflicts::Off) {
            fprintf(stderr, "--conflicts and --conflict-summary are JSON only: each form has its own fields\n");
            return 2;
        }
        if (!targets.empty() || !requestPath.empty()) {
            fprintf(stderr, "--load-order / --plugins replace naming plugins and --request\n");
            return 2;
        }
        if (dataDir.empty()) {
            fprintf(stderr, "--load-order / --plugins need --data <folder>\n");
            return 2;
        }
        LoadOrderExport req;
        req.order.data = dataDir;
        req.order.loadOrderFile = loadOrderFile;
        req.order.plugins = SplitList(explicitOrder);
        req.conflicts = conflicts;
        if (allPlugins) {
            // Base masters, Skyrim.ccc, then every line of plugins.txt with a file in Data.
            req.allPlugins = true;
            std::wstring dir = dataDir;
            if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
            std::vector<std::string> list;
            auto present = [&](const std::string& n) {
                return GetFileAttributesW((dir + WidenUtf8(n)).c_str()) != INVALID_FILE_ATTRIBUTES;
            };
            auto readLines = [](const std::wstring& path) {
                std::vector<std::string> lines;
                std::ifstream in(path, std::ios::binary);
                std::string line;
                while (std::getline(in, line)) {
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                    if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
                    if (!line.empty() && line[0] != '#') lines.push_back(line);
                }
                return lines;
            };
            auto addActive = [&](const std::string& n) {
                if (present(n) && req.activeLower.insert(LowerAscii(n)).second) list.push_back(n);
            };
            for (const char* n : {"Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"}) addActive(n);
            for (const auto& n : readLines(dir + L"..\\Skyrim.ccc")) addActive(n);
            std::ifstream check(loadOrderFile, std::ios::binary);
            if (!check) {
                fprintf(stderr, "cannot read %s\n", Utf8(loadOrderFile).c_str());
                return 2;
            }
            std::set<std::string> listed(req.activeLower);
            for (const auto& raw : readLines(loadOrderFile)) {
                const bool on = raw[0] == '*';
                std::string n = on ? raw.substr(1) : raw;
                if (on) req.activeLower.insert(LowerAscii(n));
                if (present(n) && listed.insert(LowerAscii(n)).second) list.push_back(n);
            }
            req.order.loadOrderFile.clear();
            req.order.plugins = std::move(list);
        }
        req.forms = std::move(looseForms);
        if (!formFile.empty()) {
            std::string text, reqErr;
            if (!readAll(formFile, &text)) return 2;
            std::vector<ExportTarget> one;
            if (text.substr(0, 3) == "\xEF\xBB\xBF") text.erase(0, 3);
            if (!ParseExportRequest("[forms]\n" + text, &one, &reqErr)) {
                fprintf(stderr, "%s: %s\n", formFile.c_str(), reqErr.c_str());
                return 2;
            }
            for (auto& spec : one[0].forms) req.forms.push_back(std::move(spec));
        }
        for (const auto& t : SplitList(types)) req.types.insert(Upper(t));
        if (req.forms.empty() && req.types.empty()) {
            fprintf(stderr, "a load order needs --form, --form-file or --type: every form of every plugin is too much\n");
            return 2;
        }
        if (!(fieldList.empty() || fieldList == "*")) req.specs = ParseFields(fieldList);
        req.csv = csv;
        if (conflicts == LoadOrderExport::Conflicts::Off && csv && req.specs.empty()) {
            fprintf(stderr, "--csv and --psv need --fields (a fixed set of columns)\n");
            return 2;
        }
        PatternCatalog cat;
        if (!LoadCatalog(patternsPath, &cat)) return 2;
        std::string out;
        int rc = ExportLoadOrder(req, cat, &out);
        if (!outPath.empty()) {
            std::ofstream o(WidenUtf8(outPath), std::ios::binary);
            o << out;
        } else {
            _setmode(_fileno(stdout), _O_BINARY);
            fwrite(out.data(), 1, out.size(), stdout);
        }
        return rc;
    }
    if (!looseForms.empty()) {
        fprintf(stderr, "--form needs a plugin before it, or --load-order\n");
        return 2;
    }
    const bool manyPlugins = !requestPath.empty() || targets.size() > 1;
    if (!requestPath.empty()) {
        std::string text;
        if (!readAll(requestPath, &text)) return 2;
        std::string reqErr;
        if (!ParseExportRequest(text, &targets, &reqErr)) {
            fprintf(stderr, "%s: %s\n", requestPath.c_str(), reqErr.c_str());
            return 2;
        }
    }
    if (targets.empty()) return Usage();

    std::vector<FieldSpec> specs = fieldList.empty() || fieldList == "*" ? std::vector<FieldSpec>{}
                                                                          : ParseFields(fieldList);
    if (csv && specs.empty()) {
        fprintf(stderr, "--csv and --psv need --fields (a fixed set of columns)\n");
        return 2;
    }
    std::set<std::string> typeSet;
    for (const auto& t : SplitList(types)) typeSet.insert(Upper(t));
    PatternCatalog cat;
    if (!LoadCatalog(patternsPath, &cat)) return 2;
    if (!dataDir.empty() && dataDir.back() != L'\\' && dataDir.back() != L'/') dataDir += L'\\';

    std::string out;
    if (csv) {
        if (manyPlugins) out += "plugin" + gSep;
        out += "form" + gSep + "type" + gSep + "in";
        for (const auto& s : specs) out += gSep + CsvCell(Value::Str(s.text));
        out += "\n";
    } else {
        out += "[\n";
    }
    int status = 0;
    bool first = true;
    for (const auto& target : targets) {
        // A name is looked up in --data; anything with a folder in it is a path.
        std::wstring path = WidenUtf8(target.plugin);
        if (path.find_first_of(L"\\/:") == std::wstring::npos) path = dataDir + path;
        std::string err;
        auto plugin = Plugin::Open(path, cat, &err);
        if (!plugin) {
            fprintf(stderr, "%s: %s\n", target.plugin.c_str(), err.c_str());
            status = 1;
            continue;
        }
        const std::string shown = Utf8(path.substr(path.find_last_of(L"\\/") + 1));
        std::vector<const Record*> recs;
        if (!target.forms.empty()) {
            for (const auto& spec : target.forms) {
                uint32_t fid;
                const Record* r = ParseFormSpec(*plugin, spec, &fid) ? plugin->Find(fid) : nullptr;
                if (!r) fprintf(stderr, "no such form in %s: %s\n", shown.c_str(), spec.c_str());
                else if (typeSet.empty() || typeSet.count(r->type)) recs.push_back(r);
            }
        } else {
            for (const auto& r : plugin->records())
                if (r.type != "TES4" && (typeSet.empty() || typeSet.count(r.type))) recs.push_back(&r);
        }
        if (limit && recs.size() > limit) {
            if (randomPick) {
                // A partial shuffle: every record equally likely, none twice, in random order.
                for (size_t i = 0; i < limit; ++i) {
                    std::uniform_int_distribution<size_t> pick(i, recs.size() - 1);
                    std::swap(recs[i], recs[pick(rng)]);
                }
            }
            recs.resize(limit);
        }
        for (const Record* r : recs) {
            if (csv) {
                if (manyPlugins) out += CsvCell(Value::Str(shown)) + gSep;
                out += RowCsv(*plugin, *r, specs) + "\n";
            } else {
                Value obj = Value::Map();
                if (manyPlugins) obj.Set("plugin", Value::Str(shown));
                RowJson(*plugin, *r, specs, &obj);
                if (!first) out += ",\n";
                first = false;
                WriteJson(obj, &out);
            }
        }
    }
    if (!csv) out += "\n]\n";
    if (!outPath.empty()) {
        std::ofstream o(WidenUtf8(outPath), std::ios::binary);
        o << out;
    } else {
        _setmode(_fileno(stdout), _O_BINARY);
        fwrite(out.data(), 1, out.size(), stdout);
    }
    return status;
}

// skqe diff: what differs between copies of a plugin.
int DiffCommand(int argc, wchar_t** argv) {
    std::vector<std::wstring> paths, roots;
    std::string nameWanted, types, fieldList, outPath, formList, formFile;
    std::wstring patternsPath;
    bool summaryOnly = false, keepUnknown = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = Utf8(argv[i]);
        auto next = [&](std::string* dst) {
            if (i + 1 < argc) *dst = Utf8(argv[++i]);
        };
        if (a == "--name") next(&nameWanted);
        else if (a == "--in" && i + 1 < argc) roots.push_back(argv[++i]);
        else if (a == "--type") next(&types);
        else if (a == "--form") {
            std::string more;
            next(&more);
            formList += (formList.empty() ? "" : ",") + more;
        }
        else if (a == "--form-file") next(&formFile);
        else if (a == "--fields") next(&fieldList);
        else if (a == "--out") next(&outPath);
        else if (a == "--summary") summaryOnly = true;
        else if (a == "--include-unknown") keepUnknown = true;
        else if (a == "--patterns" && i + 1 < argc) patternsPath = argv[++i];
        else if (a.rfind("--", 0) == 0) return Usage();
        else paths.push_back(argv[i]);
    }
    // --name X --in root: every file of that name below the roots.
    if (nameWanted.empty() != roots.empty()) {
        fprintf(stderr, "--name and --in go together\n");
        return 2;
    }
    if (!nameWanted.empty()) {
        const std::string lower = LowerAscii(nameWanted);
        std::vector<std::wstring> found;
        for (const auto& r : roots) {
            std::error_code ec;
            for (std::filesystem::recursive_directory_iterator it(r, std::filesystem::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec) && LowerAscii(Utf8(it->path().filename().wstring())) == lower)
                    found.push_back(it->path().wstring());
            }
            if (ec) fprintf(stderr, "%s: %s\n", Utf8(r).c_str(), ec.message().c_str());
        }
        std::sort(found.begin(), found.end());
        paths.insert(paths.end(), found.begin(), found.end());
    }
    if (paths.size() < 2) {
        fprintf(stderr, "skqe diff needs at least 2 copies; found %zu\n", paths.size());
        return 2;
    }
    PatternCatalog cat;
    if (!LoadCatalog(patternsPath, &cat)) return 2;

    std::vector<std::unique_ptr<Plugin>> plugins;
    std::vector<std::wstring> opened;
    int status = 0;
    for (const auto& p : paths) {
        std::string err;
        auto plugin = Plugin::Open(p, cat, &err);
        if (!plugin) {
            fprintf(stderr, "%s: %s\n", Utf8(p).c_str(), err.c_str());
            status = 1;
            continue;
        }
        plugins.push_back(std::move(plugin));
        opened.push_back(p);
    }
    if (plugins.size() < 2) {
        fprintf(stderr, "skqe diff needs at least 2 readable copies\n");
        return 1;
    }
    const size_t n = plugins.size();
    for (size_t i = 1; i < n; ++i)
        if (LowerAscii(plugins[i]->nameUtf8()) != LowerAscii(plugins[0]->nameUtf8())) {
            fprintf(stderr, "note: the file names differ, so FormIDs naming the plugin itself will differ too\n");
            break;
        }

    // Byte-identical copies are compared once.
    std::vector<std::string> bytes(n);
    std::vector<size_t> sameAs(n);
    for (size_t i = 0; i < n; ++i) {
        std::ifstream in(opened[i], std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        bytes[i] = ss.str();
        sameAs[i] = i;
        for (size_t j = 0; j < i; ++j)
            if (bytes[j] == bytes[i]) {
                sameAs[i] = j;
                break;
            }
    }
    std::vector<size_t> distinct;
    for (size_t i = 0; i < n; ++i)
        if (sameAs[i] == i) distinct.push_back(i);

    Value copies = Value::List();
    for (size_t i = 0; i < n; ++i) {
        Value c = Value::Map();
        c.Set("copy", Value::Int(static_cast<int64_t>(i)));
        c.Set("path", Value::Str(Utf8(opened[i])));
        c.Set("size", Value::Int(static_cast<int64_t>(bytes[i].size())));
        c.Set("identical_to", sameAs[i] == i ? Value() : Value::Int(static_cast<int64_t>(sameAs[i])));
        copies.Items().push_back(std::move(c));
    }
    bytes.clear();

    // Header facts that differ.
    std::vector<Value> heads(n);
    for (size_t i = 0; i < n; ++i) {
        std::map<std::string, size_t> counts;
        for (const auto& r : plugins[i]->records()) ++counts[r.type];
        std::string text, err;
        text = InfoJson(*plugins[i], counts);
        ParseJson(text, &heads[i], &err);
        Value flat = Value::Map();
        for (const auto& [k, v] : heads[i].Members()) {
            if (k == "plugin" || k == "records_by_type" || k == "flag_names") continue;
            if (k == "masters") {
                Value names = Value::List();
                for (const auto& m : plugins[i]->mastersUtf8()) names.Items().push_back(Value::Str(m));
                flat.Set(k, std::move(names));
            } else {
                flat.Set(k, v);
            }
        }
        heads[i] = std::move(flat);
    }
    Value headDiff = Value::Map();
    for (const auto& [k, v] : heads[0].Members()) {
        std::string base;
        bool differs = false;
        for (size_t i = 0; i < n; ++i) {
            std::string text;
            CompareKey(*heads[i].Find(k), &text);
            if (i == 0) base = text;
            else differs = differs || text != base;
        }
        if (!differs) continue;
        Value list = Value::List();
        for (size_t i = 0; i < n; ++i) list.Items().push_back(*heads[i].Find(k));
        headDiff.Set(k, std::move(list));
    }

    // Every form of every distinct copy; a master's name may differ in case, and
    // the plugin itself counts as one owner whatever its file is called.
    std::set<std::string> typeSet, formSet;
    for (const auto& t : SplitList(types)) typeSet.insert(Upper(t));
    auto keyOf = [](const Plugin& p, uint32_t formId) {
        std::string k = LowerAscii(p.FormKey(formId));
        const std::string self = LowerAscii(p.nameUtf8());
        if (k.rfind(self + ":", 0) == 0) k = "<self>" + k.substr(self.size());
        return k;
    };
    std::vector<std::string> forms = SplitList(formList);
    if (!formFile.empty()) {
        std::string text;
        if (formFile == "-") {
            _setmode(_fileno(stdin), _O_BINARY);
            char buf[65536];
            size_t got;
            while ((got = fread(buf, 1, sizeof(buf), stdin)) > 0) text.append(buf, got);
        } else {
            std::ifstream in(WidenUtf8(formFile), std::ios::binary);
            if (!in) {
                fprintf(stderr, "cannot read %s\n", formFile.c_str());
                return 2;
            }
            std::stringstream ss;
            ss << in.rdbuf();
            text = ss.str();
        }
        if (text.substr(0, 3) == "\xEF\xBB\xBF") text.erase(0, 3);
        std::vector<ExportTarget> one;
        std::string err;
        ParseExportRequest("[forms]\n" + text, &one, &err);
        forms.insert(forms.end(), one[0].forms.begin(), one[0].forms.end());
    }
    const std::string selfLower = LowerAscii(plugins[0]->nameUtf8());
    for (const auto& spec : forms) {
        size_t colon = spec.rfind(':');
        if (colon == std::string::npos) {
            fprintf(stderr, "skqe diff needs forms as Owner:ObjectID, not %s\n", spec.c_str());
            return 2;
        }
        std::string owner = LowerAscii(spec.substr(0, colon));
        char id[16];
        snprintf(id, sizeof(id), ":%06x", static_cast<uint32_t>(strtoul(spec.c_str() + colon + 1, nullptr, 16)) & 0xFFFFFF);
        formSet.insert((owner == selfLower ? std::string("<self>") : owner) + id);
    }
    struct Entry {
        std::vector<const Record*> copy;  // by distinct copy
    };
    std::vector<Entry> entries;
    std::unordered_map<std::string, size_t> entryOf;
    for (size_t d = 0; d < distinct.size(); ++d) {
        const Plugin& p = *plugins[distinct[d]];
        for (const auto& r : p.records()) {
            if (r.type == "TES4" || (!typeSet.empty() && !typeSet.count(r.type))) continue;
            std::string key = keyOf(p, r.formId);
            if (!formSet.empty() && !formSet.count(key)) continue;
            auto [it, isNew] = entryOf.emplace(std::move(key), entries.size());
            if (isNew) entries.push_back({std::vector<const Record*>(distinct.size(), nullptr)});
            entries[it->second].copy[d] = &r;
        }
    }

    const std::vector<FieldSpec> specs = fieldList.empty() || fieldList == "*" ? std::vector<FieldSpec>{} : ParseFields(fieldList);
    struct Outcome {
        std::string json, type;
        bool partial = false;
        std::vector<std::string> fields;
    };
    std::vector<Outcome> outcomes(entries.size());
    std::atomic<size_t> nextEntry{0};
    auto work = [&] {
        for (size_t e; (e = nextEntry++) < entries.size();) {
            const Entry& entry = entries[e];
            std::vector<Value> rows(distinct.size(), Value::Map());
            std::vector<std::string> keys;
            std::set<std::string> known;
            size_t present = 0, firstPresent = distinct.size();
            for (size_t d = 0; d < distinct.size(); ++d) {
                if (!entry.copy[d]) continue;
                ++present;
                if (firstPresent == distinct.size()) firstPresent = d;
                const Plugin& p = *plugins[distinct[d]];
                RowJson(p, *entry.copy[d], specs, &rows[d]);
                ConflictFields(*entry.copy[d], specs, &rows[d], keepUnknown);
                const Value* fields = rows[d].Find("fields");
                for (const auto& [k, v] : fields->Members())
                    if (known.insert(k).second) keys.push_back(k);
                if (const Value* parent = rows[d].Find("in"); parent && specs.empty() && known.insert("Parent").second)
                    keys.push_back("Parent");
            }
            auto valueOf = [&](size_t d, const std::string& key) -> const Value* {
                if (key == "Parent") return rows[d].Find("in");
                return rows[d].Find("fields")->Find(key);
            };
            Outcome& out = outcomes[e];
            out.type = entry.copy[firstPresent]->type;
            out.partial = present < distinct.size();
            for (const auto& key : keys) {
                std::string base;
                bool started = false, differs = false;
                for (size_t d = 0; d < distinct.size() && !differs; ++d) {
                    if (!entry.copy[d]) continue;
                    std::string text;
                    if (const Value* v = valueOf(d, key)) CompareKey(*v, &text);
                    else text = "null";
                    if (!started) {
                        base = std::move(text);
                        started = true;
                    } else {
                        differs = text != base;
                    }
                }
                if (differs) out.fields.push_back(key);
            }
            if (out.fields.empty() && !out.partial) continue;
            Value obj = Value::Map();
            obj.Set("form", *rows[firstPresent].Find("form"));
            obj.Set("type", Value::Str(out.type));
            if (const Value* edid = rows[firstPresent].Find("fields")->Find("EDID")) obj.Set("edid", *edid);
            if (out.partial) {
                Value in = Value::List();
                for (size_t i = 0; i < n; ++i) {
                    size_t d = std::find(distinct.begin(), distinct.end(), sameAs[i]) - distinct.begin();
                    if (entry.copy[d]) in.Items().push_back(Value::Int(static_cast<int64_t>(i)));
                }
                obj.Set("in_copies", std::move(in));
            }
            if (summaryOnly) {
                Value names = Value::List();
                for (const auto& k : out.fields) names.Items().push_back(Value::Str(k));
                obj.Set("fields", std::move(names));
            } else {
                Value values = Value::Map();
                for (const auto& k : out.fields) {
                    Value perCopy = Value::List();
                    for (size_t i = 0; i < n; ++i) {
                        size_t d = std::find(distinct.begin(), distinct.end(), sameAs[i]) - distinct.begin();
                        const Value* v = entry.copy[d] ? valueOf(d, k) : nullptr;
                        perCopy.Items().push_back(v ? *v : Value());
                    }
                    values.Set(k, std::move(perCopy));
                }
                obj.Set("fields", std::move(values));
            }
            WriteJson(obj, &out.json);
        }
    };
    int threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back(work);
    for (auto& t : pool) t.join();

    size_t different = 0, partial = 0;
    std::map<std::string, std::pair<size_t, size_t>> byType;
    std::map<std::string, size_t> byField;
    for (const auto& o : outcomes) {
        if (o.json.empty()) continue;
        if (!o.fields.empty()) {
            ++different;
            ++byType[o.type].first;
        }
        if (o.partial) {
            ++partial;
            ++byType[o.type].second;
        }
        for (const auto& k : o.fields) ++byField[k];
    }
    Value summary = Value::Map();
    summary.Set("identical", Value::Bool(distinct.size() == 1));
    summary.Set("forms", Value::Int(static_cast<int64_t>(entries.size())));
    summary.Set("forms_with_different_fields", Value::Int(static_cast<int64_t>(different)));
    summary.Set("forms_not_in_every_copy", Value::Int(static_cast<int64_t>(partial)));
    Value typeCounts = Value::Map();
    for (const auto& [t, c] : byType) {
        Value one = Value::Map();
        one.Set("different", Value::Int(static_cast<int64_t>(c.first)));
        one.Set("not_in_every_copy", Value::Int(static_cast<int64_t>(c.second)));
        typeCounts.Set(t, std::move(one));
    }
    summary.Set("by_type", std::move(typeCounts));
    Value fieldCounts = Value::Map();
    for (const auto& [k, c] : byField) fieldCounts.Set(k, Value::Int(static_cast<int64_t>(c)));
    summary.Set("by_field", std::move(fieldCounts));

    std::string out = "{\"copies\":";
    WriteJson(copies, &out);
    out += ",\n\"header\":";
    WriteJson(headDiff, &out);
    out += ",\n\"summary\":";
    WriteJson(summary, &out);
    out += ",\n\"forms\":[";
    bool first = true;
    for (const auto& o : outcomes) {
        if (o.json.empty()) continue;
        out += first ? "\n" : ",\n";
        first = false;
        out += o.json;
    }
    out += "\n]}\n";
    if (!outPath.empty()) {
        std::ofstream o(WidenUtf8(outPath), std::ios::binary);
        o << out;
    } else {
        _setmode(_fileno(stdout), _O_BINARY);
        fwrite(out.data(), 1, out.size(), stdout);
    }
    return status;
}

int wmain(int argc, wchar_t** argv) {
    // skqe help [cmd], skqe --help, skqe <cmd> --help
    std::string arg1 = argc > 1 ? Utf8(argv[1]) : "";
    if (arg1 == "--version" || arg1 == "version") {
        printf("skqe %s\nGPL-3.0. Credits and third-party notices: THIRD_PARTY_NOTICES.md\n", SKQE_VERSION);
        printf("data files (SKQE_DATA, next to the exe, then %s):\n",
               DataFilesEmbedded() ? "built in" : "none built into this build");
        for (const wchar_t* name : {L"patterns.json", L"ut_reference.sql", L"search_filters.txt"}) {
            const std::wstring from = DataFileSource(name);
            printf("  %-20s %s\n", Utf8(name).c_str(), from.empty() ? "NOT FOUND" : Utf8(from).c_str());
        }
        return 0;
    }
    if (arg1 == "help" || arg1 == "--help" || arg1 == "-h" || arg1 == "/?")
        return Help(argc > 2 ? Utf8(argv[2]) : "");
    for (int i = 2; i < argc; ++i) {
        std::string a = Utf8(argv[i]);
        if (a == "--help" || a == "-h") return Help(arg1);
    }
    if (arg1 == "build-db") return BuildDbCommand(argc, argv);
    if (arg1 == "db") return DbCommand(argc, argv);
    if (arg1 == "build-assets") return BuildAssetsCommand(argc, argv);
    if (arg1 == "staging") return StagingCommand(argc, argv);
    if (arg1 == "search") return SearchCommand(argc, argv);
    if (arg1 == "export") return ExportCommand(argc, argv);
    if (arg1 == "diff") return DiffCommand(argc, argv);
    if (argc < 3) return argc == 1 ? Help("") : Usage();
    std::string cmd = Utf8(argv[1]);
    std::wstring pluginPath = argv[2];
    std::string types, forms, fieldList, outPath, positional;
    std::wstring patternsPath;
    bool csv = false;
    bool json = false;  // skqe info --json (export's default)
    bool names = false;  // skqe fields --names
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
        else if (a == "--json") { csv = false; json = true; }
        else if (a == "--names") names = true;
        else if (a == "--patterns" && i + 1 < argc) patternsPath = argv[++i];
        else positional = a;
    }
    PatternCatalog cat;
    std::string err;
    if (!LoadCatalog(patternsPath, &cat)) return 2;
    auto plugin = Plugin::Open(pluginPath, cat, &err);
    if (!plugin) {
        fprintf(stderr, "%s: %s\n", Utf8(pluginPath).c_str(), err.c_str());
        return 1;
    }

    std::string out;
    if (cmd == "info") {
        std::map<std::string, size_t> counts;
        for (const auto& r : plugin->records()) ++counts[r.type];
        out += json ? InfoJson(*plugin, counts) : InfoText(*plugin, counts);
    } else if (cmd == "fields") {
        std::string type = Upper(positional);
        if (type.empty()) return Usage();
        std::map<std::string, std::pair<size_t, std::set<std::string>>> seen;  // sig -> (records, shapes)
        auto recs = plugin->OfType(type);
        for (const Record* r : recs) {
            std::set<std::string> inThis;
            for (const auto& f : plugin->Fields(*r)) {
                CollectPaths(f.value, f.sig, &seen[f.sig].second);
                if (inThis.insert(f.sig).second) ++seen[f.sig].first;
            }
        }
        out += type + ": " + std::to_string(recs.size()) + " records\n";
        for (const auto& [sig, info] : seen) {
            std::string name = names ? cat.Display(type, sig) : std::string();
            out += "  " + sig + (name.empty() ? "  " : " - " + name + " ") + "(in " + std::to_string(info.first) + ")\n";
            for (const auto& s : info.second)
                if (s != sig) out += "    " + s + "\n";
        }
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
