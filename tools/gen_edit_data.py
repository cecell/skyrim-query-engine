"""Regenerates the writer's two data files from their sources.

  data/field_order.csv   the order the Creation Kit writes each record type's
                         subrecords, from the CK project's ck_field_order.csv
                         (machine-code walk of every SaveData, doc 80).
  data/catalog_open.csv  the catalog ledger's rows still open, so an edit of a
                         field whose layout is in question can warn or refuse.

usage: python -I tools/gen_edit_data.py [ck_field_order.csv] [spf_ledger.csv]
"""
import csv
import re
import sys
from pathlib import Path

ORDER = Path(sys.argv[1] if len(sys.argv) > 1 else r"C:\projects\creation-kit\tools\out\ck_field_order.csv")
LEDGER = Path(sys.argv[2] if len(sys.argv) > 2 else r"C:\projects\shared-skyrim-assets\docs\catalogue_corrections_ck\spf_ledger.csv")
DATA = Path(__file__).resolve().parent.parent / "data"

rows = 0
with ORDER.open(encoding="utf-8", newline="") as f, (DATA / "field_order.csv").open("w", encoding="utf-8", newline="") as out:
    w = csv.writer(out, lineterminator="\n")
    w.writerow(["record_type", "position", "field_signature", "group", "always"])
    for r in csv.DictReader(f):
        # "alt: FNAM", "MNAM / FNAM": every spelling shares the position.
        for sig in re.findall(r"(?<![A-Za-z0-9_])[A-Z0-9_]{4}(?![A-Za-z0-9_])", r["field_signature"]):
            always = "1" if r["conditional"] == "no" and r["repeats"] == "no" else ""
            w.writerow([r["record_type"], r["position"], sig, r["group"], always])
            rows += 1
print(f"field_order.csv: {rows} rows")

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
