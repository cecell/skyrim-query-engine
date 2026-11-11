"""Adds catalog facts from the reference database to data/patterns.json.

  _multi    "RT.FS" subrecords that occur more than once in one record
            (269 on the Bethesda plugins). build-db stores them in child
            tables; the patterns cannot say this, a subrecord's pattern
            decodes one occurrence.
  _display  "RT.FS" -> xEdit's name for the subrecord ("Game Data"), used
            as the column description in the st_ data dictionary.
  _names    names for unlabeled tuple items (OBND x1..z2, CNTO item/count),
            from xEdit's definitions; build-db names its columns with them.

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
# Destruction stage fields (one set per stage) on record types where no
# Bethesda plugin has them: they repeat as they do on the types that have.
multi |= {
    "ALCH.DMDL", "ALCH.DMDT", "ALCH.DSTD", "ALCH.DSTF", "AMMO.DMDL", "AMMO.DMDT", "AMMO.DSTD",
    "AMMO.DSTF", "ARMO.DMDL", "ARMO.DMDT", "ARMO.DSTD", "ARMO.DSTF", "BOOK.DMDL", "BOOK.DMDT",
    "CONT.DMDL", "CONT.DMDT", "CONT.DSTD", "CONT.DSTF", "DOOR.DMDL", "DOOR.DMDT", "DOOR.DSTD",
    "DOOR.DSTF", "FLOR.DMDL", "FLOR.DMDT", "FLOR.DSTD", "FLOR.DSTF", "FURN.DMDL", "FURN.DMDT",
    "INGR.DMDL", "INGR.DMDT", "INGR.DSTD", "INGR.DSTF", "KEYM.DMDL", "KEYM.DMDT", "KEYM.DSTD",
    "KEYM.DSTF", "LIGH.DMDL", "LIGH.DMDT", "LIGH.DSTD", "LIGH.DSTF", "MISC.DMDL", "MISC.DMDT",
    "MISC.DSTD", "MISC.DSTF", "NPC_.DMDL", "NPC_.DMDT", "PROJ.DMDL", "PROJ.DMDT", "SCRL.DMDL",
    "SCRL.DMDT", "SCRL.DSTD", "SCRL.DSTF", "TACT.DMDL", "TACT.DMDT", "TACT.DSTD", "TACT.DSTF",
    "WEAP.DMDL", "WEAP.DMDT", "WEAP.DSTD", "WEAP.DSTF",
}
# DMDS, the stage's alternate textures, repeats per stage like DMDL and DMDT.
multi |= {"ALCH.DMDS", "AMMO.DMDS", "ARMO.DMDS", "BOOK.DMDS", "CONT.DMDS", "DOOR.DMDS", "FLOR.DMDS", "FURN.DMDS", "INGR.DMDS", "KEYM.DMDS", "LIGH.DMDS", "MISC.DMDS", "MSTT.DMDS", "NPC_.DMDS", "PROJ.DMDS", "SCRL.DMDS", "SLGM.DMDS", "TACT.DMDS", "WEAP.DMDS"}
multi = sorted(multi)

# Names for unlabeled tuple items, from xEdit's definitions
# (Core/wbDefinitionsTES5.pas, wbDefinitionsCommon.pas, xEdit 4.1.5q).
# Key "RT.FS" or "*.FS" (every record type); inner key = where the tuple sits
# in the decoded value ("" the subrecord itself, "[]" each list element).
# An empty name puts that item's own fields directly under the parent.
ALT_TEXTURE = {"[]": ["shape_name", "texture", "shape_index"]}  # xEdit: 3D Name, New Texture, 3D Index
NAMES = {
    "*.OBND": {"": ["x1", "y1", "z1", "x2", "y2", "z2"]},
    "*.MODS": ALT_TEXTURE, "*.MO2S": ALT_TEXTURE, "*.MO3S": ALT_TEXTURE,
    "*.MO4S": ALT_TEXTURE, "*.MO5S": ALT_TEXTURE, "*.DMDS": ALT_TEXTURE,
    "*.CNTO": {"": ["item", "count"]},
    "*.LVLO": {"": ["level", "unused1", "reference", "count", "unused2"]},
    "*.EFIT": {"": ["magnitude", "area", "duration"]},
    "*.VMAD": {"": ["version", "object_format", "script_count", "scripts", "fragments"]},
    "*.XWCN": {"": ["velocity_count"]},
    "*.XIS2": {"": ["present"]},
    "*.XPPA": {"": ["present"]},
    "*.XRGD": {"": ["bones"]},
    "*.XWCU": {"": ["velocities"]},
    "REFR.XLOC": {"": ["level", "unused1", "key", "flags", "unused2", "unknown1", "unknown2"]},
    "SHOU.SNAM": {"": ["word", "spell", "recovery_time"]},
    "IPDS.PNAM": {"": ["material", "impact"]},
    "CPTH.ANAM": {"": ["parent", "previous"]},
    "CPTH.CTDA": {"": ["type_flags", "comp_value", "func_idx", "param1", "param2", "run_on", "reference", "param3"]},
    "CELL.DATA": {"": [""]},
    "RACE.DATA": {"": [""]},
    "DEBR.DATA": {"": ["percentage", "model", "flags"]},
    "ECZN.DATA": {"": ["owner", "location", ""]},
    "IMGS.HNAM": {"": ["eye_adapt_speed", "bloom_blur_radius", "bloom_threshold", "bloom_scale",
                       "receive_bloom_threshold", "white", "sunlight_scale", "sky_scale", "eye_adapt_strength"]},
    "IMGS.CNAM": {"": ["saturation", "brightness", "contrast"]},
    "IMGS.TNAM": {"": ["amount", "red", "green", "blue"]},
    "IMGS.ENAM": {"": ["hdr_eye_adapt_speed", "hdr_bloom_blur_radius", "hdr_bloom_threshold", "hdr_bloom_scale",
                       "hdr_receive_bloom_threshold", "hdr_sunlight_scale", "hdr_sky_scale",
                       "cinematic_saturation", "cinematic_brightness", "cinematic_contrast",
                       "tint_amount", "tint_red", "tint_green", "tint_blue"]},
    "RACE.MPAV": {"": ["flags"] + [f"unknown{i}" for i in range(1, 8)]},
    "LGTM.DALC": {"": ["x_pos", "x_neg", "y_pos", "y_neg", "z_pos", "z_neg", "specular", "scale"]},
}
display = {}
for rt, fs, _, d in rows:
    if d and " - " in d:
        display[f"{rt}.{fs}"] = d.split(" - ", 1)[1]
cat["_multi"] = multi
cat["_display"] = dict(sorted(display.items()))
cat["_names"] = NAMES
with open(path, "wb") as f:
    f.write(json.dumps(cat, separators=(",", ":"), ensure_ascii=False).encode("utf-8"))
print(f"{len(multi)} multi, {len(display)} display names, {len(NAMES)} name lists")
