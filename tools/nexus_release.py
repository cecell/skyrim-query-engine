#!/usr/bin/env python3
"""Uploads a release to the Nexus Mods page, as the last step of .github/workflows/release.yml.

    python tools/nexus_release.py --tag v0.90 --zip dist/spf-0.90.zip --notes notes.md [--dry-run]

The first upload creates the file on the page; later ones add a version of it, and the old version
moves to Old files. The page's version number follows the file. The per-file note is the release's
first line, the zip's SHA-256 and, when given, the VirusTotal report. The changelog comes from the
release notes: the first line, then each section heading and bullet, with Markdown stripped, except
the "Upgrading..." and "Verification" sections. The page's own text is never touched. Re-running is
safe: a version already there isn't uploaded again, and one with changelog entries gets no more.

The API key comes from NEXUS_API_KEY and is never printed. Standard library only.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

V3 = "https://api.nexusmods.com/v3"
V1 = "https://api.nexusmods.com/v1"
GAME = "skyrimspecialedition"
MOD = "198184"
FILE_NAME = "Skyrim Plugin Output"
UA = "skyrim-plugin-format-release/1.0"
SKIP_SECTIONS = ("upgrading", "verification")


def key() -> str:
    k = os.environ.get("NEXUS_API_KEY", "").strip()
    if not k:
        raise SystemExit("NEXUS_API_KEY is not set")
    return k


def call(method: str, url: str, body: dict | None = None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers={
        "apikey": key(), "Accept": "application/json", "User-Agent": UA,
        **({"Content-Type": "application/json"} if data is not None else {})})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            raw = r.read()
            return json.loads(raw) if raw else None
    except urllib.error.HTTPError as e:
        raise SystemExit(f"{method} {url} -> HTTP {e.code}: {e.read().decode('utf-8', 'replace')[:1500]}")


def put_file(url: str, data: bytes, filename: str, md5_raw: bytes) -> None:
    # The presigned URL signs all three headers; urllib's default Content-Type breaks it.
    req = urllib.request.Request(url, data=data, method="PUT", headers={
        "Content-Disposition": f'attachment; filename="{filename}"',
        "Content-MD5": base64.b64encode(md5_raw).decode(),
        "Content-Type": "application/octet-stream",
        "Content-Length": str(len(data))})
    for attempt in range(1, 6):
        try:
            with urllib.request.urlopen(req, timeout=600):
                return
        except urllib.error.HTTPError as e:
            raise SystemExit(f"PUT presigned -> HTTP {e.code}: {e.read().decode('utf-8', 'replace')[:800]}")
        except urllib.error.URLError as e:      # the storage host sometimes drops the connection mid-send
            if attempt == 5:
                raise
            print(f"PUT attempt {attempt} failed ({e.reason}); retrying", flush=True)
            time.sleep(5 * attempt)


def plain(md: str) -> str:
    """Markdown inline -> plain text: [text](url) -> text, drop ** and backticks."""
    md = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", md)
    return md.replace("**", "").replace("`", "").strip()


def parse_notes(body: str) -> tuple[str, list[str]]:
    lines = body.replace("\r\n", "\n").split("\n")
    summary = next((plain(ln) for ln in lines if ln.strip() and not ln.startswith("#")), "")
    if not summary:
        raise SystemExit("release notes have no summary line")
    out, keep = [summary], False
    for ln in lines:
        s = ln.strip()
        if s.startswith("## "):
            heading = plain(s[3:])
            keep = not heading.lower().startswith(SKIP_SECTIONS)
            if keep:
                out.append(heading)
        elif keep and s.startswith(("- ", "* ")):
            out.append(plain(s[2:]))
        elif keep and s:
            out.append(plain(s))
    return summary, out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", required=True, help="release tag, e.g. v0.90")
    ap.add_argument("--zip", required=True, type=Path, help="the release zip, uploaded unchanged")
    ap.add_argument("--notes", required=True, type=Path, help="the release notes (Markdown)")
    ap.add_argument("--virustotal", default="", help="link to the zip's VirusTotal report")
    ap.add_argument("--dry-run", action="store_true", help="read only: show what would be uploaded")
    a = ap.parse_args()

    version = a.tag[1:] if a.tag.startswith("v") else a.tag
    data = a.zip.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    summary, changelog = parse_notes(a.notes.read_text(encoding="utf-8"))
    note = f"{summary} SHA-256 of this zip: {sha}."
    if a.virustotal:
        note += f" VirusTotal: {a.virustotal}"

    mod_id = call("GET", f"{V3}/games/{GAME}/mods/{MOD}")["data"]["id"]
    groups = call("GET", f"{V3}/mods/{mod_id}/files")["data"]["mod_files"]
    named = [g for g in groups if g.get("name") == FILE_NAME]
    if len(named) > 1:
        raise SystemExit(f"{len(named)} files are named {FILE_NAME!r} on the page")
    if groups and not named:
        raise SystemExit(f"the page has files but none named {FILE_NAME!r}: {[g.get('name') for g in groups]}")
    group = named[0]["id"] if named else None
    versions = call("GET", f"{V3}/mod-files/{group}/versions")["data"]["versions"] if group else []
    on_nexus = any(v["version"] == version for v in versions)
    logged = call("GET", f"{V1}/games/{GAME}/mods/{MOD}/changelogs.json") or {}
    has_log = bool(logged.get(version))

    print(f"Nexus mod {MOD} (v3 {mod_id}), version {version}, {'file ' + str(group) if group else 'no file yet: the first upload creates it'}")
    print(f"archive: {a.zip.name} ({len(data):,} bytes)")
    print(f"file note: {note}")
    print("changelog:" + "".join(f"\n  {ln}" for ln in changelog))
    summ = os.environ.get("GITHUB_STEP_SUMMARY")

    def report(text: str) -> None:
        print(text)
        if summ:
            with open(summ, "a", encoding="utf-8") as f:
                f.write(text + "\n")

    if a.dry_run:
        report(f"Nexus dry run: upload {'skipped, already there' if on_nexus else 'would happen'}; "
               f"changelog {'skipped, already there' if has_log else 'would be added'}.")
        return 0

    report("## Nexus Mods")
    if on_nexus:
        report(f"- {version} is already on Nexus; not uploaded again.")
    else:
        md5 = hashlib.md5(data)
        up = call("POST", f"{V3}/uploads", {"size_bytes": len(data), "filename": a.zip.name, "md5": md5.hexdigest()})
        uid = up["data"]["id"]
        put_file(up["data"]["presigned_url"], data, a.zip.name, md5.digest())
        call("POST", f"{V3}/uploads/{uid}/finalise")
        for _ in range(90):
            if call("GET", f"{V3}/uploads/{uid}")["data"]["state"] == "available":
                break
            time.sleep(2)
        else:
            raise SystemExit(f"upload {uid} never became available")
        fields = {"upload_id": uid, "name": FILE_NAME, "version": version, "description": note, "file_category": "main",
                  "show_requirements_pop_up": False, "allow_mod_manager_download": True, "update_mod_version": True}
        if group:
            call("POST", f"{V3}/mod-files/{group}/versions", dict(fields, archive_existing_file=True))
            report(f"- Uploaded {a.zip.name} as version {version}; the previous version moved to Old files.")
        else:
            call("POST", f"{V3}/mod-files", dict(fields, mod_id=mod_id))
            report(f"- Created the file with {a.zip.name}, version {version}.")
    if has_log:
        report(f"- The changelog already has {version}; nothing added.")
    else:
        for ln in changelog:
            call("POST", f"{V3}/mods/{mod_id}/changelogs", {"version": version, "changelog": ln})
        report(f"- Changelog: {len(changelog)} lines added for {version}.")
    report(f"- Page: https://www.nexusmods.com/{GAME}/mods/{MOD}?tab=files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
