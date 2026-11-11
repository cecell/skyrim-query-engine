"""Regenerates the writer's data file from its source.

  data/catalog_open.csv  the catalog ledger's rows still open, so an edit of a
                         field whose layout is in question can warn or refuse.

The order the Creation Kit writes each record type's fields is the record
grammar ("_records" in data/patterns.json), seeded by tools/gen_field_order.py.

usage: python -I tools/gen_edit_data.py [spf_ledger.csv]
"""
import csv
import sys
from pathlib import Path

LEDGER = Path(sys.argv[1] if len(sys.argv) > 1 else r"C:\projects\shared-skyrim-assets\docs\catalogue_corrections_ck\spf_ledger.csv")
DATA = Path(__file__).resolve().parent.parent / "data"

rows = 0
with LEDGER.open(encoding="utf-8", newline="") as f, (DATA / "catalog_open.csv").open("w", encoding="utf-8", newline="") as out:
    w = csv.writer(out, lineterminator="\n")
    w.writerow(["record_type", "field_signature", "offset", "size", "correction"])
    for r in csv.DictReader(f):
        if r["spf_status"] != "open":
            continue
        w.writerow([r["record_type"], r["field_signature"], r["offset"], r["size"], r["correction"]])
        rows += 1
print(f"catalog_open.csv: {rows} rows")
