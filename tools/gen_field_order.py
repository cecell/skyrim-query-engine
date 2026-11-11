"""Seeds the record grammar ("_records" in data/patterns.json), which holds
the order the writer places new fields by.

  seed   writes "_records" into data/patterns.json from a field order
         table (CSV with the columns record_type, position, field_signature,
         conditional, repeats, group), mechanically and with no new claims:
         one field item per position, in order; the signatures that share a position
         share the item; min 1 where the field is not conditional; max "*"
         where it repeats; the group column kept as the item's label. The
         one exception: consecutive "extra-list" rows become one unordered
         group named extra-list.

The grammar replaced data/field_order.csv; the seed was checked by writing
that file back from it byte for byte. Groups are written by hand from here
on, one record type at a time, so a new seed would undo them.

usage: python -I tools/gen_field_order.py seed <field_order.csv> [patterns.json]
"""
import csv
import json
import re
import sys
from pathlib import Path

DATA = Path(__file__).resolve().parent.parent / "data"
SIG = re.compile(r"(?<![A-Za-z0-9_])[A-Z0-9_]{4}(?![A-Za-z0-9_])")


def load(path):
    with open(path, "rb") as f:
        raw = f.read()
    cat = json.loads(raw)
    # The rewrite must not change anything else.
    assert json.dumps(cat, separators=(",", ":"), ensure_ascii=False).encode("utf-8") == raw, "format would change"
    return cat


def save(path, cat):
    with open(path, "wb") as f:
        f.write(json.dumps(cat, separators=(",", ":"), ensure_ascii=False).encode("utf-8"))


def seed(order_path, patterns_path):
    types = {}  # record type -> its rows in position order
    with open(order_path, encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            types.setdefault(r["record_type"], []).append(r)
    records = {}
    for rt, rows in types.items():
        rows.sort(key=lambda r: int(r["position"]))
        items = []
        extra = None
        for r in rows:
            sigs = SIG.findall(r["field_signature"])
            if not sigs:
                continue
            item = {"sig": sigs[0] if len(sigs) == 1 else sigs}
            if r["conditional"] == "no":
                item["min"] = 1
            if r["repeats"] == "yes":
                item["max"] = "*"
            if r["group"] == "extra-list":
                if extra is None:
                    extra = {"group": "extra-list", "unordered": True, "fields": []}
                    items.append(extra)
                extra["fields"].append(item)
                continue
            extra = None
            if r["group"]:
                item["label"] = r["group"]
            items.append(item)
        records[rt] = {"fields": items}
    cat = load(patterns_path)
    out = {}
    for k, v in cat.items():
        if k == "_records":
            continue
        out[k] = v
        if k == "_context":
            out["_records"] = records
    if "_records" not in out:
        out = {"_records": records, **out}
    save(patterns_path, out)
    print(f"_records: {len(records)} record types")


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] != "seed":
        sys.exit(__doc__)
    seed(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else DATA / "patterns.json")
