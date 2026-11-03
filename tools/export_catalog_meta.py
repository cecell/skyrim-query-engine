"""Adds catalog facts from the reference database to data/patterns.json.

  _multi    "RT.FS" subrecords that occur more than once in one record
            (269 on the Bethesda plugins). build-db stores them in child
            tables; the patterns cannot say this, a subrecord's pattern
            decodes one occurrence.
  _display  "RT.FS" -> xEdit's name for the subrecord ("Game Data"), used
            as the column description in the st_ data dictionary.

Usage: python -I tools/export_catalog_meta.py <reference.db> [data/patterns.json]
"""
import json
import sqlite3
import sys

ref = sys.argv[1]
path = sys.argv[2] if len(sys.argv) > 2 else "data/patterns.json"
with open(path, "rb") as f:
    raw = f.read()
cat = json.loads(raw)
# Round-trip check: the rewrite must not change anything else.
assert json.dumps(cat, separators=(",", ":"), ensure_ascii=False).encode("utf-8") == raw, "format would change"

db = sqlite3.connect(f"file:{ref}?mode=ro", uri=True)
rows = db.execute(
    "SELECT rt.sig, c.fs_sig, c.is_multi, c.display FROM st_rt_fs_columns c "
    "JOIN st_record_types rt ON rt.id = c.fk_record_type WHERE c.status = 'active'"
).fetchall()
multi = {f"{rt}.{fs}" for rt, fs, m, _ in rows if m}
# Repeating in mods although never in the Bethesda plugins (seen in a
# 4,883-plugin load order): conditions with their parameters, extra data,
# linked references, attack data, lens flare sprites. A subrecord that
# repeats without being listed lands in subrecord_overflow.
ALWAYS_MULTI = {"CTDA", "CIS1", "CIS2", "COED", "XAPR", "XLKR", "PDTO", "ATKD", "ATKE"}
for key in cat:
    if not key.startswith("_") and key.split(".", 1)[1] in ALWAYS_MULTI:
        multi.add(key)
multi |= {"LENS.DNAM", "LENS.LFSD"}
multi = sorted(multi)
display = {}
for rt, fs, _, d in rows:
    if d and " - " in d:
        display[f"{rt}.{fs}"] = d.split(" - ", 1)[1]
cat["_multi"] = multi
cat["_display"] = dict(sorted(display.items()))
with open(path, "wb") as f:
    f.write(json.dumps(cat, separators=(",", ":"), ensure_ascii=False).encode("utf-8"))
print(f"{len(multi)} multi, {len(display)} display names")
