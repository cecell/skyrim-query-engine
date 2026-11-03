"""Checks build-db's type / child tables against `spf export` (same decoder,
FormIDs as text): every column of every record of the chosen types.

usage: python -I verify_tables.py <db> <spf.exe> <data dir> <plugin> <TYPE>...
"""
import json
import re
import sqlite3
import subprocess
import sys

db_path, spf, data, plugin = sys.argv[1:5]
types = sys.argv[5:]
db = sqlite3.connect(db_path)
no_by_name = {n.lower(): no for no, n in db.execute("select plugin_no, name from plugins")}
name_by_no = {no: n for n, no in no_by_name.items()}
pno = no_by_name[plugin.lower()]
import os
MULTI = set(json.load(open(os.path.join(os.path.dirname(spf), "patterns.json"), encoding="utf-8"))["_multi"])
FORM = re.compile(r"^(.+\.(?:esm|esp|esl)):([0-9A-F]{6})$", re.I)


def key_of(text):
    m = FORM.match(text)
    return no_by_name[m.group(1).lower()] * 16777216 + int(m.group(2), 16)


def keys(v):  # export value -> DB value (FormID text -> key)
    if isinstance(v, str) and FORM.match(v):
        return key_of(v)
    if isinstance(v, list):
        return [keys(x) for x in v]
    if isinstance(v, dict):
        return {k: keys(x) for k, x in v.items()}
    return v


def walk(v, segs):
    for s in segs:
        if isinstance(s, int):
            if not isinstance(v, list) or s >= len(v):
                return None, False
            v = v[s]
        else:
            if not isinstance(v, dict) or s not in v:
                return None, False
            v = v[s]
    return v, True


def segments(path):  # "DATA.damage" -> ["damage"]; "OBND[2]" -> [2]; "KWDA[]" -> []
    out = []
    for name, idx in re.findall(r"\.([^.\[]+)|\[(\d*)\]", path[4:]):
        if name:
            out.append(name)
        elif idx != "":
            out.append(int(idx))
    return out


def cell_equal(dbv, ev, kind):
    if kind == "marker":
        return dbv == 1
    if ev is None:
        return dbv is None
    if isinstance(ev, bool):
        return dbv == int(ev)
    if isinstance(ev, (list, dict)):
        return isinstance(dbv, str) and json.loads(dbv) == ev
    if isinstance(ev, float) and isinstance(dbv, (int, float)):
        return abs(dbv - ev) <= 1e-9 * max(1.0, abs(ev))
    return dbv == ev


bad = checked = 0
for rt in types:
    out = subprocess.run([spf, "export", f"{data}/{plugin}", "--type", rt, "--fields", "*"],
                         capture_output=True, check=True).stdout
    records = json.loads(out)
    # Tables of modules this database did not build are empty by design.
    tables = {t: (fs, kind) for t, fs, kind in db.execute(
        "select table_name, subrecord, kind from st_tables where record_type=? and kind<>'core' "
        "and (module is null or module in (select name from spf_modules))", (rt,))}
    cols = {}
    for t in tables:
        cols[t] = db.execute("select column_name, kind, subrecord, path from st_columns where table_name=? "
                             "and kind<>'key' order by position", (t,)).fetchall()
    # A type whose subrecords all belong to a module (NAVI) has no type table.
    type_table = next((t for t, (fs, k) in tables.items() if k == "type"), None)
    for rec in records:
        key = key_of(rec["form"])
        fields = keys(rec["fields"])
        row, type_cols = {}, []
        if type_table:
            row = db.execute(f'select * from "{type_table}" where form_key=? and plugin_no=?', (key, pno)).fetchone()
            names = [d[0] for d in db.execute(f'select * from "{type_table}" limit 0').description]
            row = dict(zip(names, row))
            type_cols = cols[type_table]
        for name, kind, fs, path in type_cols:
            checked += 1
            if fs not in fields:
                ok = row[name] is None
            else:
                ev = fields[fs]
                ov = db.execute("select count(*) from subrecord_overflow where form_key=? and plugin_no=? and sig=?",
                                (key, pno, fs)).fetchone()[0]
                if ov:  # export lists every occurrence; the type table has the first
                    ev = ev[0]
                v, found = walk(ev, segments(path))
                ok = cell_equal(row[name], v if found else None, kind)
            if not ok:
                bad += 1
                if bad <= 20:
                    print("TYPE", rt, rec["form"], name, repr(row[name])[:80], "!=", repr(fields.get(fs))[:80])
        for t, (fs, k) in tables.items():
            if k != "child":
                continue
            rows = db.execute(f'select * from "{t}" where form_key=? and plugin_no=? order by idx', (key, pno)).fetchall()
            names = [d[0] for d in db.execute(f'select * from "{t}" limit 0').description]
            elem_rows = cols[t] and cols[t][0][3].startswith(fs + "[]")
            if fs not in fields:
                items = []
            elif elem_rows:
                items = fields[fs]
                # A repeating subrecord whose value is a list: its rows are
                # the elements of every occurrence.
                if f"{rt}.{fs}" in MULTI and items and all(isinstance(x, list) for x in items):
                    items = [e for occ in items for e in occ]
            else:
                items = [fields[fs]] if len(rows) == 1 else fields[fs]
            checked += 1
            if len(items) != len(rows):
                bad += 1
                if bad <= 20:
                    print("COUNT", rt, rec["form"], t, len(rows), "!=", len(items))
                continue
            for r, item in zip(rows, items):
                r = dict(zip(names, r))
                for name, kind, _, path in cols[t]:
                    checked += 1
                    segs = segments(path)
                    v, found = walk(item, segs)
                    if not cell_equal(r[name], v if found else None, kind):
                        bad += 1
                        if bad <= 20:
                            print("CHILD", rt, rec["form"], t, name, repr(r[name])[:60], "!=", repr(v)[:60])
    print(rt, len(records), "records")
print(f"checked {checked} cells, {bad} mismatches")
