#!/usr/bin/env python3
"""Makes ps5/frontend/assets/hd-pack-sources.json: the HD texture packs of the community's Google Drive folders
(https://drive.google.com/drive/folders/1l_dkIJpO2-fO9OwnGybH4on16JaxwQma), for the shelf's HD texture pack row (fe_texpacks.cpp).

2.11, AI-assisted (swordpdf: "look at the structure of this folder and figure out how to integrate the links into the hd pack downloader").

What the folder is: four subfolders and a sheet, "PS2 HD Textures" (columns Game Title | Title ID | Links, a section per maker), as of 2026-10-16:
  Bl4ckH4nd     192 .7z files named by title only ("007 - Everything or Nothing.7z"): the serials come from the sheet's Bl4ckH4nd section
                (its Media Fire link names the same file)
  CCKrizalid    35 .rar files named "<title> PS2 Upscale Textures [SLUS-21376].rar": the serials are in the names
  SomberShroud  1 .zip file (the sheet's section lists most of that maker's packs on Media Fire / OneDrive, which are not read)
  IA            323 .rar files, a copy of archive.org's item the downloader already reads (pcsx2-hd-texture-packs): left out
The list has 228 packs for 223 serials. b176: the app reads all three formats: .rar and .zip with libarchive and .7z with fe_sevenzip.c (the LZMA
SDK; a pack that needs more than the console has is refused with a message once its header is read). The flag file texpack_no7z gives build 175's
behaviour back: the 192 .7z packs are not offered and the app counts and logs them as left out (36 packs offered).

Usage:
  python3 make_hd_pack_sources.py                        fetches the folders and the sheet (needs the network) and writes the asset
  python3 make_hd_pack_sources.py --from-dir <folder>    reads folder_<id>.html and sheet.csv saved there earlier
  python3 make_hd_pack_sources.py --out <file>           another output file
Public pages only (Drive's embedded folder view, the sheet's CSV export); no pack file is downloaded.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import csv
import html
import io
import json
import os
import re
import sys
import urllib.request

ROOT = "1l_dkIJpO2-fO9OwnGybH4on16JaxwQma"
SHEET = "1MSVA0IiaNo-nY1nY65n2NVXAeorkqOSm_gnLgVYEDLI"
FOLDERS = {  # maker -> folder id
    "Bl4ckH4nd": "17nrSX8BChE9xyoYH36uBCvriPuLMC4Xp",
    "CCKrizalid": "1PwNF1adKSNIAYa8rYhfeZZNmc6wsNqWG",
    "SomberShroud": "1PaPgXZsQuI-wjNe8qnQBdfc-psjdHCjR",  # .zip files (the sheet's section lists most of them on Media Fire / OneDrive)
}
SHEET_SECTION = {"Bl4ckH4nd": "Bl4ckH4nd's HD Textures - Media Fire", "SomberShroud": "SomberShroud HD Texures"}
SERIAL = re.compile(r"([A-Z]{4})-?(\d{5})")
# Where the sheet's Title ID cell is wrong: "We Love Katamari" has "replacements" there (PCSX2's GameIndex.yaml: SLUS-21230 is the USA disc).
SERIAL_FIXES = {"We Love Katamari.7z": ["SLUS-21230"]}


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (PS5SX2 pack list tool)"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read().decode("utf-8")


def folder_entries(page):
    """[(drive file id, file name)] from Drive's embedded folder view."""
    found = re.findall(r'<div class="flip-entry" id="entry-([^"]+)".*?<div class="flip-entry-title">(.*?)</div>', page, re.S)
    return [(i, html.unescape(t).strip()) for i, t in found]


def serials_in(text):
    out = []
    for letters, digits in SERIAL.findall(text.upper()):
        s = f"{letters}-{digits}"
        if s not in out:
            out.append(s)
    return out


def sheet_sections(csv_text):
    """{section: [(title, title id cell, link)]}: a row with only a title opens a section."""
    sections, current = {}, None
    for row in list(csv.reader(io.StringIO(csv_text)))[1:]:
        row = [c.strip() for c in row]
        if not any(row):
            continue
        title, tid, link = (row + ["", "", ""])[:3]
        if title and not tid and not link:
            current = sections.setdefault(title, [])
            continue
        if current is not None:
            current.append((title, tid, link))
    return sections


def norm_title(text, loose=False):
    # lower case, letters and digits only; `loose` also leaves off the trailing region tag the sheet adds to some sections
    # ("Digimon World Data Squad (US)"): only as a second try, since "Auto Modellista (NTSC)" and "(PAL)" are two different packs
    if loose:
        text = re.sub(r"\s*\((us|eu|jp|uk|usa|europe|japan|pal|ntsc)\)\s*$", "", text.strip(), flags=re.I)
    return re.sub(r"[^a-z0-9]+", " ", text.lower()).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from-dir")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "frontend", "assets", "hd-pack-sources.json"))
    args = ap.parse_args()

    def load(name, url):
        if args.from_dir:
            with open(os.path.join(args.from_dir, name), encoding="utf-8") as f:
                return f.read()
        return fetch(url)

    sections = sheet_sections(load("sheet.csv", f"https://docs.google.com/spreadsheets/d/{SHEET}/gviz/tq?tqx=out:csv"))
    packs, problems = [], []
    for maker, folder in FOLDERS.items():
        entries = folder_entries(load(f"folder_{folder}.html", f"https://drive.google.com/embeddedfolderview?id={folder}"))
        rows = sections.get(SHEET_SECTION.get(maker, ""), [])
        by_title, by_leaf = {}, {}
        for title, tid, link in rows:
            # the sheet's title (it wins), and the file name in its link ("007_-_Everything_or_Nothing.7z"): the sheet has rows that
            # share a link ("Demolition Girl" points at Def_Jam_Vendetta.7z), so a leaf only answers where no title does
            for loose in (False, True):
                by_title.setdefault((norm_title(title, loose), loose), tid)
            leaf = [p for p in link.split("/") if p and p != "file"]
            if leaf:
                for loose in (False, True):
                    by_leaf.setdefault((norm_title(os.path.splitext(leaf[-1])[0], loose), loose), tid)
        for file_id, name in entries:
            stem = os.path.splitext(name)[0]
            serials = serials_in(name) if SERIAL.search(name.upper()) and "[" in name else []
            if not serials and name in SERIAL_FIXES:
                serials = SERIAL_FIXES[name]
            if not serials:
                tid = (by_title.get((norm_title(stem), False)) or by_leaf.get((norm_title(stem), False)) or
                       by_title.get((norm_title(stem, True), True)) or by_leaf.get((norm_title(stem, True), True)))
                serials = serials_in(tid) if tid else []
            if not serials:
                problems.append(f"{maker}: no serial for {name}")
                continue
            title = re.sub(r"\s*PS2 Upscale Textures.*$", "", re.sub(r"\s*\[[A-Z]{4}-\d{5}\]", "", stem)).strip() or stem
            packs.append({"id": file_id, "name": name, "title": title, "label": maker, "maker": maker, "serials": serials})
    packs.sort(key=lambda p: (p["maker"], p["title"].lower(), p["name"]))
    out = {"version": 1, "source": f"https://drive.google.com/drive/folders/{ROOT}", "packs": packs}
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        # one pack a line (a diff shows what changed)
        f.write('{"version":1,"source":%s,"packs":[\n' % json.dumps(out["source"]))
        f.write(",\n".join(json.dumps(p, ensure_ascii=False, separators=(",", ":")) for p in packs))
        f.write("\n]}\n")
    by_ext = {}
    for p in packs:
        ext = os.path.splitext(p["name"])[1].lower()
        by_ext[ext] = by_ext.get(ext, 0) + 1
    print(f"{len(packs)} packs ({by_ext}), {len({s for p in packs for s in p['serials']})} serials -> {os.path.normpath(args.out)}")
    for p in problems:
        print("left out:", p, file=sys.stderr)


if __name__ == "__main__":
    main()
