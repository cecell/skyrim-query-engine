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

Plugins are decoded in parallel and one thread writes. The Bethesda plugins
build in about 20 seconds (640 MB); a 4,883-plugin load order (5.6 million
forms, 19 million field rows, 13 million references) in about 85 seconds
(3.6 GB).

- `plugins`: load order, light slot, header facts, size, mtime, CRC32.
  Masters missing from the load order still get a row, and so does
  `SkyrimSE.exe` for the forms the engine defines itself (PlayerRef...).
- `plugin_masters`
- `forms`: one row per occurrence of every form (the owner and each
  override), with type, EDID, flags, the WRLD / CELL / DIAL it sits in, and
  `is_winner`.
- One table per record type (`weap`, `npc`, `refr`, ...) with a column per
  field (`weap.data_damage`, `npc.acbs_level`, `weap.full` as text), and
  child tables for repeating subrecords and FormID lists (`npc_snam`,
  `lvli_lvlo`, `weap_kwda`). Generated from the pattern catalog, so every
  record type has the same tables in every build. `winning_<type>` views.
- `strings`: every localized string of every plugin.
- `form_refs`: the xref, one row per FormID inside a record wherever it sits
  (columns, child tables, nested JSON), indexed both ways: what references a
  form, and what a form references. Views `winning_refs` (references the load
  order ends up with) and `dangling_refs` (references to forms nobody defines).
- `st_tables` / `st_columns`: the data dictionary (what each column is,
  which subrecord and path it comes from, what a key refers to).
- `subrecord_overflow` / `subrecord_raw`: whatever did not fit, so nothing
  is lost silently.
- Views `forms_v` (readable `Skyrim.esm:012EB7` keys, in-game FormIDs) and
  `winning_forms`.

Navmesh geometry, landscape data (terrain, height maps, occlusion) and
worldspace large-reference lists are left out for now; they will be optional
modules.

The tables were checked cell for cell against `spf export` on 21 record
types of Skyrim.esm and Dawnguard.esm (12.8 million values, 0 mismatches).

Every form has one integer key, `form_key = owner plugin_no * 16777216 +
object ID`, computed from the bytes. It is the same whichever plugin a
reference is read from, so joins are plain integer joins. `spf build-db
--help` has the details.

## Damaged and hostile files

Every size and count spf reads from a file is checked before it is used:
group and record sizes against their parent group and the file, a
compressed record's claimed size against what deflate can produce (and 256
MB), list counts against the bytes left, strings files and plugin headers
against sane limits. BSAs are read with
[bsa-reader](https://github.com/cecell/bsa-reader), which validates every
table.

- A plugin that cannot be read is refused with a reason ("damaged plugin: a
  group at offset 1234 has an impossible size"). `build-db` reports it,
  records the reason in `plugins.load_error`, leaves its rows out entirely,
  and builds the rest.
- A subrecord whose bytes do not fit its pattern is kept as raw hex
  (`subrecord_raw` in the database, `"#0A1B..."` in exports); the rest of the
  record still decodes.

## Building

Visual Studio 2026 with the MSVC 14.44 toolset, CMake and Ninja: run
`build.cmd`. Output: `build\spf.exe` with `patterns.json` next to it. The
first configure downloads bsa-reader from GitHub (pinned to a commit in
`CMakeLists.txt`).

## Testing

- `ctest --test-dir build` (or `build\spf_tests.exe`): unit tests with
  hand-built valid and damaged plugins, strings files and zlib streams. CI
  runs them on every push.
- `tools/verify_tables.py <db> build/spf.exe <data dir> <plugin> <TYPE>...`:
  checks a database cell for cell against `spf export`.
- Fuzzing, with MSVC's libFuzzer and AddressSanitizer:

  ```
  cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPF_FUZZ=ON
  cmake --build build-fuzz --target spf_fuzz
  build-fuzz\spf_fuzz.exe <corpus dir> -max_total_time=600 -max_len=262144
  ```

  Every input is tried as a plugin (every field of every record decoded), a
  strings file and a zlib stream. Seed the corpus with small plugins (or
  `spf_tests --seeds <dir>` from a normal build); run it from a vcvars prompt
  so the ASan runtime DLL is found. A fuzz build only builds `spf_fuzz`. CI
  fuzzes for 5 minutes on every push.

## Layout

- `src/plugin.*`: the Plugin / Record / Field objects and path selection
- `src/pattern_engine.*`, `src/pattern_value.*`: the pattern decoder and its value type
- `src/inflate.*`: zlib for compressed records
- `src/strings.*`: localized strings, from loose files or BSAs
- `src/db_build.*`: `spf build-db`
- `src/db_schema.*`: build-db's per-record-type tables, generated from the catalog
- `tools/export_catalog_meta.py`: adds `_multi` (repeating subrecords) and
  `_display` (xEdit names) to the catalog
- `tools/verify_tables.py`: checks a built database against `spf export`
- `tests/test_spf.cpp`: unit tests; `tests/fuzz_plugin.cpp`: the libFuzzer target
- `third_party/sqlite`: the SQLite amalgamation (3.54.1, public domain)
- `tools/spf.cpp`: the command-line tool
- `data/patterns.json`: the pattern catalog, with the strings file each localized field uses
