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
spf info   <plugin> [--json]              TES4 header facts, masters, record counts by type
spf fields <plugin> <TYPE>                which fields that type has, with sub-fields
spf export <plugin> [options]             chosen fields of chosen forms, as JSON or CSV;
                                          several plugins, or every copy across a load order with the winner and
                                          what conflicts: `spf help export`
spf diff   <copy> <copy>...               what differs between copies of a plugin, as JSON
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
  which subrecord and path it comes from, what a key refers to, notes such
  as how to read a BLOB).
- `form_names` with full-text indexes `fts_names` (words, prefixes) and
  `fts_names_tri` (substrings): search every form occurrence by editor ID or
  name. An extra, on by default: `--skip fts_names` leaves it out,
  `spf db add fts_names` adds it later.
- Plugin facts (extras, each on by default, `--skip` any): `plugin_facts`
  holds per plugin the master form count and ESLable verdict, deleted
  placed objects and navmeshes, missing masters (view `missing_masters`) and
  invalid references. `invalid_refs` lists every reference to a form no
  plugin defines; the plugin count keeps only those held by a form another
  form references, since most others sit in leftovers nothing uses.
- Every index is optional: each is an extra (`idx_forms_key`,
  `idx_refs_to`, `idx_table_keys`, ...), on by default, `--skip` to leave it
  out and `spf db add` to build it later. On a 192-plugin load order the
  indexes are 29% of the file (about 320 MB of 1.16 GB) and about 10 s of
  build time; without them a lookup scans its table (about 100 ms instead of
  under 1 ms at 2.3M forms).
- `asset_refs`: every file a form names (models, textures, sounds, music,
  VMAD scripts as `scripts\<name>.pex`), normalized exactly like
  `assets.db` paths, so `ATTACH 'assets.db'` and join on `path` to find a
  form's files or a file's users. On a 192-plugin load order: 150,946
  references from 141 fields; 95-99.9% resolve to existing files (sound
  fields name `.wav`, the game also plays `.xwm` / `.fuz`).
- `plugin_facts_v`: the shell extension's columns per plugin by name (master
  forms, ESLable, deleted, ITM, dirty, missing masters, invalid references).
- `spf db drop <extra>` removes a built extra (drops an index, empties a fact
  table); `spf db add` builds it again. Predesigned indexes ship off by
  default for queries that need them: `idx_ref_base` (references by their
  base object: 221 ms -> 5 ms on a 192-plugin load order) and
  `idx_persistent_type` (persistent forms by type).
- ITM records (`itm_forms`, `plugin_facts.itm`): an extra that is off by
  default (`--with itm`), since it reads every plugin and its masters.
  Compared the way xEdit's Quick Auto Clean does, without its known false
  positives (an added INFO PNAM or a DIAL subtype fix is a real change).
- Raw byte runs are stored as BLOBs (`kind` = `bytes`); `SELECT hex(col)`
  gives the hex text `spf export` shows. Inside JSON values they stay hex text.
- `subrecord_overflow` / `subrecord_raw`: whatever did not fit, so nothing
  is lost silently.
- Views `forms_v` (readable `Skyrim.esm:012EB7` keys, in-game FormIDs) and
  `winning_forms`.

- `tes4`, `tes4_mast`, `tes4_onam`: each plugin's header record (HEDR,
  author, masters, the overridden-forms list ONAM).

Optional modules hold the bulky geometry. Their tables exist in every build
and stay empty until the module is built:

| Module | What | Bethesda plugins |
|---|---|---|
| `navmesh` | NAVM geometry (vertices, triangles, edge and door links), NAVI | ~0.5 GB |
| `landscape` | LAND terrain (heights, normals, colours, texture layers), CELL/WRLD height maps, occlusion, LOD offsets | ~1.5 GB |
| `wrld_large_refs` | WRLD.RNAM large-reference lists | small |

```
spf build-db ... --with navmesh,landscape       build them with the core
spf db add navmesh --db skyrim.db               or add one later
```

`db add` re-reads only that module's subrecords, fills only its tables (plus
xref rows), runs in one transaction, and refuses if a plugin changed since the
build. Building with `--with` and adding later give identical databases.

The tables were checked cell for cell against `spf export` on 21 record
types of Skyrim.esm and Dawnguard.esm (12.8 million values, 0 mismatches).

Every form has one integer key, `form_key = owner plugin_no * 16777216 +
object ID`, computed from the bytes. It is the same whichever plugin a
reference is read from, so joins are plain integer joins. `spf build-db
--help` has the details.

## `spf build-assets`: every asset file as one SQLite database

```
spf build-assets --out assets.db --data "<Skyrim>\Data"
                 [--staging <root> | --mo2 <instance> --profile <name>]
                 [--load-order plugins.txt] [--hash] [--skip e,..] [--threads n] [--quiet]
```

Catalogues every file under the Data folder and every mod staging folder,
and every file packed in their BSAs, in its own database (ATTACH it to a
build-db database; join on `paths.path` and plugin names). Folders are walked
from directory listings with their NTFS file IDs (no file is opened for
that), headers are read by a thread pool, largest work first, and one thread
writes. Data and staging folders are only read.

- `roots`: the Data folder, each staging subfolder (Vortex recognised), MO2
  mod folders (enabled, priority from modlist.txt) and overwrite, with a
  file count / total size / newest mtime fingerprint.
- `paths`: every normalized relative path once (lower case, backslashes,
  relative to Data), with dir, name, ext and kind.
- `files`: every copy of every file, loose or packed (`archive_id`), with
  size, packed size, mtime, `in_data`, `is_winner`, optional SHA-256 and
  `error` (why the file or its header could not be read).
- `archives`: per BSA, its header facts and whether, why (`ini` / `plugin`)
  and in which order (`load_rank`) the game loads it.
- Header facts, loose and packed alike: `dds`, `tga`, `nif`, `audio` (from
  [beth-asset-metadata](https://github.com/cecell/beth-asset-metadata)),
  `pex` (script and parent), `hkx` (Havok version, pointer size = LE / SE),
  `tri` / `tri_morphs` (FaceGen and BodySlide morph names); `voice` and
  `facegen` from their path conventions (plugin + object id).
  `nif.game` comes from the block types, never the BS version.
- Extras, on by default (`--skip` leaves them out): `nif_textures` (every
  texture path a mesh expects, `path_id` NULL when no such file exists) and
  the indexes `files_path`, `files_root`, `files_archive`, `paths_kind`.
- Views `winning_files` and `conflicts`; `st_tables` / `st_columns`.

Winners: a staging file that is the same physical file as the Data file at
its path (a hardlink) is stored once, with its staging root, `in_data = 1`;
Data-only files keep the Data root. A loose `in_data` copy wins; otherwise
the copy in the loaded archive with the highest `load_rank` (archives in
the Skyrim.ini / SkyrimCustom.ini archive lists first, then
`<Plugin>.bsa` / `<Plugin> - Textures.bsa` of active plugins in load order).
With MO2 and nothing deployed, the virtual Data (enabled mods by priority,
overwrite highest, real Data lowest) decides `in_data`.

The real Data folder (870,401 loose files, 892 BSAs, 1.53 million rows)
builds in about 100 seconds (1.3 GB).

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
- A subrecord whose bytes do not fit its pattern is kept raw
  (`subrecord_raw.bytes` BLOB in the database, `"#0A1B..."` in exports); the rest of the
  record still decodes.

## Building

Visual Studio 2026 with the MSVC 14.44 toolset, CMake and Ninja: run
`build.cmd`. Output: `build\spf.exe` with `patterns.json` next to it. The
first configure downloads bsa-reader and beth-asset-metadata from GitHub
(pinned to commits in `CMakeLists.txt`).

## Testing

- `ctest --test-dir build` (or `build\spf_tests.exe`): unit tests with
  hand-built valid and damaged plugins, strings files and zlib streams, the
  asset readers on synthetic bytes, and build-assets' winners on a fixture
  tree (hardlinks, BSAs, an MO2 instance) written to `%TEMP%\spf_tests_assets`.
  CI runs them on every push.
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
- `src/db_util.h`: SQLite / queue helpers shared by the builders
- `src/asset_build.*`: `spf build-assets`
- `src/asset_readers.*`: PEX, HKX, TRI and NIF-texture readers; asset path rules
- `tools/export_catalog_meta.py`: adds `_multi` (repeating subrecords) and
  `_display` (xEdit names) to the catalog
- `tools/verify_tables.py`: checks a built database against `spf export`
- `tests/test_spf.cpp`: unit tests; `tests/fuzz_plugin.cpp`: the libFuzzer target
- `third_party/sqlite`: the SQLite amalgamation (3.54.1, public domain)
- `tools/spf.cpp`: the command-line tool
- `data/patterns.json`: the pattern catalog, with the strings file each localized field uses
