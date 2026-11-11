"""The record grammar ("_records" in data/patterns.json) and the field order
the writer places new fields by.

  seed   writes "_records" into data/patterns.json from the CK project's
         ck_field_order.csv, mechanically and with no new claims: one field
         item per position, in order; the signatures that share a position
         share the item; min 1 where the field is not conditional; max "*"
         where it repeats; the group column kept as the item's label. The
         one exception: consecutive "extra-list" rows become one unordered
         group named extra-list.
  csv    writes data/field_order.csv back from "_records": depth first,
         positions counting field items from 1, one row per signature, the
         group column from the item's label or the enclosing group's name,
         always = 1 for min 1 and max 1.

usage: python -I tools/gen_field_order.py seed [ck_field_order.csv] [patterns.json]
       python -I tools/gen_field_order.py csv [patterns.json] [field_order.csv]
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


def write_csv(patterns_path, out_path):
    cat = load(patterns_path)
    rows = 0
    with open(out_path, "w", encoding="utf-8", newline="") as out:
        w = csv.writer(out, lineterminator="\n")
        w.writerow(["record_type", "position", "field_signature", "group", "always"])
        for rt, g in cat.get("_records", {}).items():
            position = 0

            def walk(items, group_name):
                nonlocal position, rows
                for it in items:
                    if "group" in it:
                        walk(it["fields"], it["group"])
                        continue
                    position += 1
                    sigs = it["sig"] if isinstance(it["sig"], list) else [it["sig"]]
                    always = "1" if it.get("min", 0) == 1 and it.get("max", 1) == 1 else ""
                    for s in sigs:
                        w.writerow([rt, position, s, it.get("label", group_name), always])
                        rows += 1

            walk(g["fields"], "")
    print(f"field_order.csv: {rows} rows")


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in ("seed", "csv"):
        sys.exit(__doc__)
    if sys.argv[1] == "seed":
        seed(sys.argv[2] if len(sys.argv) > 2 else r"C:\projects\creation-kit\tools\out\ck_field_order.csv",
             sys.argv[3] if len(sys.argv) > 3 else DATA / "patterns.json")
    else:
        write_csv(sys.argv[2] if len(sys.argv) > 2 else DATA / "patterns.json",
                  sys.argv[3] if len(sys.argv) > 3 else DATA / "field_order.csv")
