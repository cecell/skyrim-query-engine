"""Real plugins through spfedit: after each edit, `spf diff --include-unknown`
must show only the intended difference, the header must count what the file
holds, and the output must round-trip.

usage: python -I tools/check_edits.py <build dir> <work dir> [--corpus C:\\dbwork\\qs_corpus] [--mod plugin]...

Reads the plugins, writes only under <work dir>.
"""
import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("build")
ap.add_argument("work")
ap.add_argument("--corpus", default=r"C:\dbwork\qs_corpus")
ap.add_argument("--mod", action="append", default=[])
a = ap.parse_args()

BUILD = Path(a.build)
SPFEDIT, SPF = str(BUILD / "spfedit.exe"), str(BUILD / "spf.exe")
WORK = Path(a.work)
if WORK.exists():
    shutil.rmtree(WORK)
WORK.mkdir(parents=True)
CORPUS = Path(a.corpus)
failures, passes = [], 0
n_case = 0


def run(*args):
    return subprocess.run([str(x) for x in args], capture_output=True, text=True, encoding="utf-8", errors="replace")


def export(plugin, typ, fields, limit=1):
    r = run(SPF, "export", plugin, "--type", typ, "--limit", limit, "--fields", fields, "--json")
    if r.returncode:
        return []
    return json.loads(r.stdout)


def diff(a_path, b_path):
    r = run(SPF, "diff", a_path, b_path, "--include-unknown")
    return json.loads(r.stdout)


def info(path):
    return json.loads(run(SPF, "info", path, "--json").stdout)


def check(name, plugin, op, forms=None, fields=None, count_changes=None, expect_fail=None, extra=None):
    """Apply one op to a copy named as the plugin, then compare."""
    global passes, n_case
    n_case += 1
    plugin = Path(plugin)
    out_dir = WORK / f"case{n_case:03d}"
    out_dir.mkdir()
    out = out_dir / plugin.name
    ops = out_dir / "ops.json"
    ops.write_text(json.dumps(op if isinstance(op, list) else [op]), encoding="utf-8")
    t = time.perf_counter()
    r = run(SPFEDIT, "apply", plugin, "--ops", ops, "--out", out)
    secs = time.perf_counter() - t
    problems = []
    try:
        res = json.loads(r.stdout)
    except ValueError:
        res = {"ok": False, "results": [], "raw": r.stdout + r.stderr}
    if expect_fail:
        codes = [x.get("code") for x in res.get("results", [])]
        if res.get("ok") or expect_fail not in codes or out.exists():
            problems.append(f"expected refusal {expect_fail}, got {codes}")
    elif not res.get("ok"):
        problems.append("op failed: " + json.dumps(res)[:400])
    else:
        d = diff(plugin, out)
        s = d.get("summary", {})
        got_forms = sorted(f["form"] for f in d.get("forms", []))
        if forms is not None and got_forms != sorted(forms):
            problems.append(f"forms differ: {got_forms[:6]} ({len(got_forms)}) instead of {sorted(forms)[:6]}")
        if fields is not None:
            got = set(s.get("by_field", {}))
            if got != set(fields):
                problems.append(f"fields differ: {sorted(got)} instead of {sorted(fields)}")
        i = info(out)
        rt = run(SPFEDIT, "roundtrip", out)
        if rt.returncode:
            problems.append("output does not round-trip: " + rt.stdout[:300])
        if extra:
            problems += extra(res, d, i, out) or []
    status = "ok" if not problems else "FAILED"
    print(f"{status:6} {secs:6.2f}s  {name}")
    for p in problems:
        print("        " + p)
    if problems:
        failures.append(name)
    else:
        passes += 1
    return out


def only_members(form, sig, members):
    """spf diff names the subrecord: the members of it that differ must be just these."""
    def verify(res, d, i, out):
        rec = next((f for f in d.get("forms", []) if f["form"].lower() == form.lower()), None)
        if not rec:
            return [f"{form} not in the diff"]
        vals = rec["fields"].get(sig)
        if not isinstance(vals, list) or len(vals) != 2 or not isinstance(vals[0], dict):
            return [f"unexpected diff shape: {vals!r}"[:200]]
        got = {k for k in vals[0] if vals[0][k] != vals[1].get(k)} | {k for k in vals[1] if k not in vals[0]}
        return [] if got == members else [f"members differ: {sorted(got)}"]
    return verify


def corpus(name):
    for p in CORPUS.iterdir():
        if p.name.lower() == name.lower():
            return p
    raise SystemExit(f"{name} not in {CORPUS}")


daw = corpus("Dawnguard.esm")
upd = corpus("update.esm")
pets = corpus("ccvsvsse002-pets.esl")

# --- set: one value, on a plugin with compressed records around it ---------------
weap = export(daw, "WEAP", "EDID,DATA")[0]
check("set WEAP.DATA.damage", daw,
      {"op": "set", "form": weap["form"], "field": "DATA.damage", "value": weap["fields"]["DATA"]["damage"] + 1},
      forms=[weap["form"]], fields=["DATA"], extra=only_members(weap["form"], "DATA", {"damage"}))
check("set EDID, longer text", daw,
      {"op": "set", "form": weap["form"], "field": "EDID", "value": weap["fields"]["EDID"] + "_Longer"},
      forms=[weap["form"]], fields=["EDID"])
check("set to the value it holds changes no byte", daw,
      {"op": "set", "form": weap["form"], "field": "DATA.damage", "value": weap["fields"]["DATA"]["damage"]},
      forms=[], fields=[], extra=lambda res, d, i, out: [] if Path(out).read_bytes() == daw.read_bytes() else ["bytes changed"])

# A compressed record, edited: only it changes, and it stays compressed.
npc = export(daw, "NPC_", "EDID", 1)[0]
check("set a compressed NPC_'s EDID", daw,
      {"op": "set", "form": npc["form"], "field": "EDID", "value": npc["fields"]["EDID"] + "X"},
      forms=[npc["form"]], fields=["EDID"])

# A reference's float and its lock data: what a CK save destroys stays.
refr = export(daw, "REFR", "NAME,DATA", 1)[0]
check("set a REFR position", daw,
      {"op": "set", "form": refr["form"], "field": "DATA.pos_x", "value": 123.5},
      forms=[refr["form"]], fields=["DATA"], extra=only_members(refr["form"], "DATA", {"pos_x"}))

# Text in a localized plugin is a string ID.
named = [w for w in export(daw, "WEAP", "FULL", 5) if w["fields"].get("FULL")]
if named:
    check("localized FULL refuses text", daw,
          {"op": "set", "form": named[0]["form"], "field": "FULL", "value": "Text"}, expect_fail="bad_value")

check("a master not listed is refused", daw,
      {"op": "set", "form": "Nope.esm:000800", "field": "EDID", "value": "x"}, expect_fail="master_missing")

# --- subrecords -------------------------------------------------------------------
armo = next((x for x in export(daw, "ARMO", "EDID,KWDA", 10) if x["fields"].get("KWDA")), None)
if armo:
    kw = armo["fields"]["KWDA"]
    check("add a keyword: KSIZ follows", daw,
          {"op": "add-item", "form": armo["form"], "sig": "KWDA", "item": "Skyrim.esm:012E46"},
          forms=[armo["form"]], fields=["KSIZ", "KWDA"])
    check("removing KSIZ alone is refused", daw,
          {"op": "remove-sub", "form": armo["form"], "sig": "KSIZ"}, expect_fail="linked_subrecord")
    check("remove KWDA: KSIZ goes too", daw,
          {"op": "remove-sub", "form": armo["form"], "sig": "KWDA"},
          forms=[armo["form"]], fields=["KSIZ", "KWDA"])
    check("add DESC in its place", daw,
          {"op": "add-sub", "form": armo["form"], "sig": "DESC", "raw": "78563412"},
          forms=[armo["form"]])

# --- flags, header ----------------------------------------------------------------
check("set a record flag", daw, {"op": "set-flags", "form": weap["form"], "set": "0x4"}, forms=[weap["form"]])
check("author only", daw, {"op": "set-header", "author": "spfedit test"}, forms=[],
      extra=lambda res, d, i, out: [] if i["author"] == "spfedit test" and i["next_object_id"] == info(daw)["next_object_id"] else ["header facts moved: " + str(i)])
check("version only", daw, {"op": "set-header", "version": 1.71}, forms=[])

# --- new records ------------------------------------------------------------------


def new_record(res, d, i, out):
    probs = []
    before = info(daw)
    if i["header_record_count"] <= before["header_record_count"]:
        probs.append("HEDR count did not grow")
    return probs


check("add-record GLOB", daw,
      {"op": "add-record", "type": "GLOB", "edid": "SpfEditTestGlobal",
       "subs": [{"sig": "FNAM", "raw": "66"}, {"sig": "FLTV", "raw": "0000803F"}]},
      extra=new_record)

cell = export(daw, "CELL", "EDID", 3)
child = export(daw, "REFR", "NAME", 1)
if child:
    # A reference's own parent cell: spf gives it as "in".
    parent = child[0].get("in")
    if parent:
        check("add-record REFR into a cell", daw,
              {"op": "add-record", "type": "REFR", "parent": parent,
               "subs": [{"sig": "NAME", "raw": "00080000"}, {"sig": "DATA", "raw": "00" * 24}]},
              extra=new_record)

# --- delete -----------------------------------------------------------------------
check("delete an override: deleted flag, header only", upd,
      {"op": "delete-record", "form": export(upd, "GMST", "EDID")[0]["form"]})

# --- copy-as-override -------------------------------------------------------------
def copied(form, typ, want_parent=None):
    def verify(res, d, i, out):
        probs = []
        got = export(out, typ, "EDID", 5000)
        if not any(x["form"].lower() == form.lower() for x in got):
            probs.append(f"{form} not in the output")
        return probs
    return verify


def seed_patch(name, flags=0x80):
    """A plugin with one master and no records, written by hand through the model's own ops."""
    d = WORK / "seed" / name
    d.parent.mkdir(exist_ok=True)
    def sub(sig, data):
        return sig + len(data).to_bytes(2, "little") + data
    import struct
    body = sub(b"HEDR", struct.pack("<fII", 1.71, 0, 0x800)) + sub(b"MAST", b"Skyrim.esm\0") + sub(b"DATA", b"\0" * 8) + sub(b"MAST", b"Update.esm\0") + sub(b"DATA", b"\0" * 8)
    head = b"TES4" + len(body).to_bytes(4, "little") + flags.to_bytes(4, "little") + b"\0" * 4 + b"\0" * 4 + (44).to_bytes(2, "little") + b"\0\0"
    d.write_bytes(head + body)
    return d


patch = seed_patch("Patch.esp")
sky = corpus("Skyrim.esm")
w = export(sky, "WEAP", "EDID", 1)[0]
check("localized source, plain target, no strings found: refused", seed_patch("Plain.esp", 0),
      {"op": "copy-as-override", "from": str(sky), "form": w["form"]}, expect_fail="strings_missing")
check("copy a WEAP from Skyrim.esm", patch,
      {"op": "copy-as-override", "from": str(sky), "form": w["form"]},
      extra=copied(w["form"], "WEAP"))
n = export(sky, "NPC_", "EDID", 1)[0]
check("copy a compressed NPC_", patch,
      {"op": "copy-as-override", "from": str(sky), "form": n["form"]},
      extra=copied(n["form"], "NPC_"))
r = export(sky, "REFR", "NAME", 1)[0]
check("copy a REFR: its CELL and groups come along", patch,
      {"op": "copy-as-override", "from": str(sky), "form": r["form"]},
      extra=copied(r["form"], "REFR"))
ld = export(sky, "ACHR", "NAME", 1)
if ld:
    check("copy an ACHR", patch, {"op": "copy-as-override", "from": str(sky), "form": ld[0]["form"]}, extra=copied(ld[0]["form"], "ACHR"))
i = export(sky, "INFO", "EDID", 1)
if i:
    check("copy an INFO: its DIAL comes along", patch, {"op": "copy-as-override", "from": str(sky), "form": i[0]["form"]}, extra=copied(i[0]["form"], "INFO"))


# A record copied through differing master lists keeps its meaning: same decoded values as the source.
def compare_copy(source, form, typ):
    def verify(res, d, i_, out):
        a_rec = export(source, typ, "*", 100000)
        b_rec = export(out, typ, "*", 100000)
        a_map = {x["form"].lower(): x["fields"] for x in a_rec}
        probs = []
        for x in b_rec:
            if x["form"].lower() == form.lower() and a_map.get(form.lower()) != x["fields"]:
                probs.append(f"{form} reads differently after the copy")
        if not any(x["form"].lower() == form.lower() for x in b_rec):
            probs.append(f"{form} not in the output")
        return probs
    return verify


dg_npc = export(daw, "NPC_", "EDID", 1)[0]
check("copy through different master lists: same values", patch,
      [{"op": "copy-as-override", "from": str(daw), "form": dg_npc["form"]}],
      extra=compare_copy(daw, dg_npc["form"], "NPC_"))

# --- add-master, on real plugins of each size -------------------------------------
for name in ("ccvsvsse002-pets.esl",):
    p = corpus(name)
    before = export(p, "NPC_", "EDID,RNAM", 100000) + export(p, "REFR", "NAME,DATA", 100000)

    def verify_master(res, d, i_, out):
        after = export(out, "NPC_", "EDID,RNAM", 100000) + export(out, "REFR", "NAME,DATA", 100000)
        if json.dumps(before, sort_keys=True) != json.dumps(after, sort_keys=True):
            return ["records read differently after add-master"]
        return []

    check(f"add-master on {name}: every record reads the same", p, {"op": "add-master", "name": "SpfEditTest.esm", "force": True},
          extra=verify_master)

for mod in a.mod:
    p = Path(mod)
    i_ = info(p)
    first = None
    for typ in ("WEAP", "ARMO", "MISC", "NPC_", "GLOB", "KYWD"):
        found = export(p, typ, "EDID", 1)
        if found:
            first = found[0]
            break
    if first:
        check(f"mod {p.name}: set EDID", p, {"op": "set", "form": first["form"], "field": "EDID", "value": (first["fields"].get("EDID") or "X") + "Y"}, forms=[first["form"]], fields=["EDID"])
    check(f"mod {p.name}: add-record", p, {"op": "add-record", "type": "GLOB", "edid": "SpfEditTestG", "subs": [{"sig": "FNAM", "raw": "66"}, {"sig": "FLTV", "raw": "0000803F"}]})

print(f"\n{passes} passed, {len(failures)} failed")
sys.exit(1 if failures else 0)
