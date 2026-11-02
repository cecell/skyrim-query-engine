# skyrim-plugin-format

A complete parsing system utilizing a pattern based system loosely based on kaitai patterns.

Reads Skyrim SE plugins (`.esm` / `.esp` / `.esl`) and decodes every
subrecord into named fields using `data/patterns.json` (1,893 patterns, 134
record types). The decoder is a C++ port of the pattern engine the reference
database is built with, and was checked against that database value for value
on the 80 Bethesda plugins (1,251,209 records, 5,763,688 values, 0
mismatches).

## The `spf` tool

```
spf info   <plugin>                       masters and record counts by type
spf fields <plugin> <TYPE>                which fields that type has, with sub-fields
spf export <plugin> [options]             chosen fields of chosen forms, as JSON or CSV
    --type WEAP[,ARMO...]                 only these record types
    --form 00012EB7[,Skyrim.esm:012EB7]   only these forms (as stored, or owner:objectID)
    --fields EDID,FULL,DATA.damage        only these fields; omitted or * = all
    --csv | --json                        default json
    --out <file>
```

A field is a subrecord signature, optionally followed by a dotted path into
its decoded value (`DATA.damage`, `SNAM.faction`, `CTDA[0].func_idx`).
Subrecords that repeat in a record (factions, leveled list entries,
conditions) come out as a list of their occurrences.

In the output:
- FormIDs are `Plugin.esm:XXXXXX`: the plugin that owns the form and its
  object ID, not the raw master-relative number.
- Localized names and descriptions are their text, read from the plugin's
  `Strings\` folder, its `.bsa`, or `Skyrim - Interface.bsa` (English).
- Each record says which WRLD / CELL / DIAL it sits in (`in`), and whether it
  is deleted.

```
spf export Skyrim.esm --type WEAP --fields EDID,FULL,DATA.damage --csv
form,type,in,EDID,FULL,DATA.damage
Skyrim.esm:012EB7,WEAP,,IronSword,Iron Sword,7
```

## `spf build-db`: a whole load order as one SQLite database

```
spf build-db --out skyrim.db --data "<Skyrim>\Data" [--load-order plugins.txt | --plugins a.esm,b.esp]
```

Plugins are decoded in parallel and one thread writes. The 80 Bethesda
plugins build in about 15 seconds; a 4,883-plugin load order (5.6 million
forms) in about 25. The core tables:

- `plugins`: load order, light slot, header facts, size, mtime, CRC32.
  Masters missing from the load order still get a row.
- `plugin_masters`
- `forms`: one row per occurrence of every form (the owner and each
  override), with type, EDID, flags, the WRLD / CELL / DIAL it sits in, and
  `is_winner`.
- `strings`: every localized string of every plugin.
- Views `forms_v` (readable `Skyrim.esm:012EB7` keys, in-game FormIDs) and
  `winning_forms`.

Every form has one integer key, `form_key = owner plugin_no * 16777216 +
object ID`, computed from the bytes. It is the same whichever plugin a
reference is read from, so joins are plain integer joins. `spf build-db
--help` has the details.

## Building

Visual Studio 2026 with the MSVC 14.44 toolset, CMake and Ninja: run
`build.cmd`. Output: `build\spf.exe` with `patterns.json` next to it.

## Layout

- `src/plugin.*`: the Plugin / Record / Field objects and path selection
- `src/pattern_engine.*`, `src/pattern_value.*`: the pattern decoder and its value type
- `src/inflate.*`: zlib for compressed records
- `src/strings.*`: localized strings, from loose files or BSAs
- `src/db_build.*`: `spf build-db`
- `third_party/sqlite`: the SQLite amalgamation (3.54.1, public domain)
- `tools/spf.cpp`: the command-line tool
- `data/patterns.json`: the pattern catalog, with the strings file each localized field uses
