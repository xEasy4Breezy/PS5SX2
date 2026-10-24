#!/usr/bin/env python3
"""Makes ps5/frontend/assets/launchbox-covers.txt: for each PS2 serial the LaunchBox Games Database has art for, the file names of its box
front, box back, spine and disc images (fe_launchbox.cpp turns them into https://images.launchbox-app.com/<file> addresses at run time; the app
downloads one image when the game is on the shelf, as it does with its other cover source).

2.11, AI-assisted (swordpdf: "we can use this for the best covers, we can also use the circular disc cover for when a game is dumping").

Where the data comes from
  * Metadata.zip, LaunchBox's daily bulk export of the Games Database (https://gamesdb.launchbox-app.com/Metadata.zip, about 108 MB, XML):
    <Game> (Name, Platform, DatabaseID), <GameAlternateName>, <GameImage> (DatabaseID, FileName, Type, Region). Only "Sony Playstation 2" is kept.
  * PCSX2's GameIndex.yaml (bin/resources): serial -> name, name-en, region. LaunchBox knows no serials, so a game is matched by title.

How a serial is matched (tier "m" in the file; 1 is the most certain)
  1  the game's name (name-en first, then name) is a LaunchBox name or alternate name, after lower-casing, dropping accents, punctuation, "the",
     and reading Roman numerals as digits
  2  the same after leaving out the game's edition notes: [Limited Edition], [PlayStation2 the Best], (Disc 1) ... (never for a demo, trial,
     preview or beta: those would take the retail game's box)
  3  (only with --loose) also the LaunchBox name's notes left out
  A name that several LaunchBox games share goes first to the one whose own NAME (not an alternate name) it is, then to the one that has art for
  the serial's region, then to the one with most art.

Leaving matches out
  launchbox_exclude.json (beside this tool) lists serials whose match was judged wrong or doubtful: 3,542 of the uncertain matches (every tier-2 match,
  every one made through an alternate name, and a random 450 of the rest) were checked by two independent reviewers (one on identity, one on
  box art: demo and trial discs, expansions, regional editions with their own box ...) and 174 that either called "different" (or both "unsure")
  are in it. The own-name tier-1 matches were sampled (2 of 450 left out), not all checked. Edit the file by hand to add or remove serials.

Which image (per kind, per serial)
  The serial's region decides: "x":1 means the image is from the serial's own region (NTSC-U -> North America/United States/Canada, NTSC-J ->
  Japan, PAL-G -> Germany, ...), 0 that it is another region's (World first, then NA/Europe/...). The app puts region-exact LaunchBox art before its
  other cover source and other regions' art after it. Within a region: the scan ("Box - Front") before the reconstruction, JPEG before PNG,
  uuid names before "r2_" ones (those are often multi-megabyte PNGs); the disc is always a PNG with a transparent surround.

The file: one line per serial, "SLUS-21005 f=<file>* b=<file>* d=<file> s=<file> m=1": f front, b back, d disc, s spine (the case's side), a trailing * for art from the serial's
own region, m the match tier. Lines starting with # are comments. The app reads it with fe_launchbox.h; a file of the same shape in the
cache folder (/data/PCSX2/cache/launchbox-covers.txt) replaces entries of the built-in one.

Usage:
  python3 make_launchbox_covers.py --metadata <Metadata.xml> [--gameindex <GameIndex.yaml>] [--out <file>] [--audit <file>] [--cache <ps2.json>]
  --cache  keeps the PS2 part of Metadata.xml in a small JSON file (the XML takes ~15 s to read) and reads it back when --metadata is missing.
  --audit  writes every match with the names compared, for checking by eye.
Nothing is downloaded by this tool: run it on the file you fetched.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import json
import os
import re
import sys
import unicodedata
import xml.etree.ElementTree as ET

PLATFORM = "Sony Playstation 2"
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_GAMEINDEX = os.path.normpath(os.path.join(HERE, "..", "..", "bin", "resources", "GameIndex.yaml"))
DEFAULT_OUT = os.path.normpath(os.path.join(HERE, "..", "frontend", "assets", "launchbox-covers.txt"))
DEFAULT_EXCLUDE = os.path.join(HERE, "launchbox_exclude.json")

KIND_TYPES = {  # image kind -> LaunchBox "Type" names, best first
    "f": ["Box - Front", "Box - Front - Reconstructed"],
    "b": ["Box - Back", "Box - Back - Reconstructed"],
    "d": ["Disc"],
    "s": ["Box - Spine"],  # the case's side
}

# LaunchBox region names per GameIndex region: (exact, others in order of preference)
NA = ["North America", "United States", "Canada"]
EU = ["Europe", "United Kingdom", "Australia", "Oceania"]
ANY = ["World", "North America", "Europe", "United Kingdom", "Australia", "Oceania", "Japan", "United States", "Canada", "Germany", "France",
       "Spain", "Italy", "The Netherlands", "Korea", "Asia", "Russia", "Finland", "Sweden", "Norway", "Brazil", "China", "Greece", "South America",
       "Hong Kong", ""]
REGION_EXACT = {
    "NTSC-U": NA,
    "NTSC-J": ["Japan"],
    "NTSC-K": ["Korea"],
    "NTSC-C": ["China", "Asia", "Hong Kong"],
    "NTSC-Unk": [],
    "PAL-E": EU,
    "PAL-G": ["Germany"],
    "PAL-F": ["France"],
    "PAL-I": ["Italy"],
    "PAL-S": ["Spain"],
    "PAL-A": ["Australia", "Oceania"],
    "PAL-R": ["Russia"],
    "PAL-Unk": EU,
    "PAL-SC": ["Sweden", "Norway", "Finland"],
    "PAL-NL": ["The Netherlands"],
    "PAL-E-G": EU + ["Germany"],
    "PAL-F-G": ["France", "Germany"],
}
for _m in ("PAL-M3", "PAL-M4", "PAL-M5", "PAL-M6", "PAL-M7"):  # multi-language releases: the generic European box
    REGION_EXACT[_m] = EU


def exact_regions(region):
    if region in REGION_EXACT:
        return REGION_EXACT[region]
    return EU if region.startswith("PAL") else []


# ---- LaunchBox ----

def read_launchbox(path):
    """The PS2 part of Metadata.xml: games {id: {name, ...}}, alternate names {id: [(name, region)]}, images {id: [{f, t, r}]}."""
    games, alts, images = {}, {}, {}
    for _, el in ET.iterparse(path, events=("end",)):
        tag = el.tag
        if tag == "Game":
            if el.findtext("Platform") == PLATFORM:
                games[el.findtext("DatabaseID")] = {
                    "name": el.findtext("Name") or "",
                    "date": el.findtext("ReleaseDate") or el.findtext("ReleaseYear") or "",
                }
            el.clear()
        elif tag == "GameAlternateName":
            alts.setdefault(el.findtext("DatabaseID"), []).append((el.findtext("AlternateName") or "", el.findtext("Region") or ""))
            el.clear()
        elif tag == "GameImage":
            images.setdefault(el.findtext("DatabaseID"), []).append(
                {"f": el.findtext("FileName") or "", "t": el.findtext("Type") or "", "r": el.findtext("Region") or ""})
            el.clear()
    return ({i: g for i, g in games.items()}, {i: v for i, v in alts.items() if i in games}, {i: v for i, v in images.items() if i in games})


# ---- GameIndex ----

def read_gameindex(path):
    games, cur = {}, None
    key_line = re.compile(r"^([A-Za-z0-9._-]+):\s*$")
    field = re.compile(r"^  (name|name-en|region):\s*(.*?)\s*$")
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line and line[0] not in " #\r\n":
                m = key_line.match(line)
                if m:
                    cur = m.group(1)
                    games[cur] = {}
                continue
            if cur is None or not line.startswith("  ") or line.startswith("   "):
                continue
            m = field.match(line)
            if not m:
                continue
            val = m.group(2)
            if val.startswith('"'):
                try:
                    val = json.loads(val)
                except ValueError:
                    val = val.strip('"')
            elif val.startswith("'"):
                val = val.strip("'")
            games[cur][m.group(1)] = val
    return games


# ---- titles ----

ROMAN = {"ii": "2", "iii": "3", "iv": "4", "v": "5", "vi": "6", "vii": "7", "viii": "8", "ix": "9", "x": "10", "xi": "11", "xii": "12"}
NOTE_WORDS = re.compile(
    r"limited|special|collector|deluxe|premium|first (?:press|print)|greatest hits|platinum|\bbest\b|budget|ultimate hits|essentials|"
    r"player'?s choice|demo|trial|promo|not for resale|\bdisc\b|\bcd\s*\d|\bdvd\b|bonus|\bpack\b|edition|version|complete|import|\busa\b|"
    r"europe|japan|\bntsc\b|\bpal\b|\bmega\b|\bkorea|\bcomplete\b|\bmovie\b",
    re.I)


def squash(s):
    s = unicodedata.normalize("NFKC", s)
    s = "".join(c for c in unicodedata.normalize("NFKD", s) if not unicodedata.combining(c))
    s = s.lower().replace("&", " and ").replace("+", " plus ")
    s = re.sub(r"[™®©]", "", s)
    s = re.sub(r"[^\w]+", " ", s, flags=re.UNICODE).replace("_", " ")
    toks = [t for t in s.split() if t]
    if toks and toks[0] == "the":
        toks = toks[1:]
    toks = [ROMAN.get(t, t) if i > 0 else t for i, t in enumerate(toks)]
    return " ".join(toks)


# A demo, trial, preview or beta disc: never given the retail game's box in tier 2.
NO_RETAIL = re.compile(r"[\[\(][^\]\)]*(\b(demo|trial|preview|beta|sampler|prototype|kiosk|not for resale|promo)\b|体験版|体験|試遊|デモ|非売品|見本)[^\]\)]*[\]\)]", re.I)


def strip_notes(s):
    """Edition notes out: every [bracket], and every (parenthesis) that holds an edition/region word; a trailing ' - Platinum'-like tail."""
    s = re.sub(r"\[[^\]]*\]", " ", s)
    s = re.sub(r"\(([^)]*)\)", lambda m: " " if NOTE_WORDS.search(m.group(1)) else m.group(0), s)
    return s


def keys_for(name):
    """Match keys of one title, strongest first: exact, then with the notes dropped."""
    out = []
    for cand in (name, strip_notes(name)):
        k = squash(cand)
        if k and k not in out:
            out.append(k)
    return out


# ---- choosing images ----

def region_rank(image_region, exact):
    if image_region in exact:
        return exact.index(image_region)
    if image_region in ANY:
        return 100 + ANY.index(image_region)
    return 200


def pick(images, kind, exact):
    """The best image of `kind` for a serial with these exact regions: (file, is_region_exact) or None."""
    types = KIND_TYPES[kind]
    best, best_key = None, None
    for im in images:
        if im["t"] not in types or not im["f"]:
            continue
        ext = im["f"].rsplit(".", 1)[-1].lower()
        key = (
            region_rank(im["r"], exact),
            types.index(im["t"]),
            0 if (ext in ("png",) if kind == "d" else ext in ("jpg", "jpeg")) else 1,
            1 if im["f"].startswith("r2_") else 0,
            im["f"],
        )
        if best_key is None or key < best_key:
            best, best_key = im, key
    if best is None:
        return None
    return best["f"], 1 if best["r"] in exact else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--metadata")
    ap.add_argument("--gameindex", default=DEFAULT_GAMEINDEX)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--audit")
    ap.add_argument("--exclude", default=DEFAULT_EXCLUDE, help="serials to leave out (JSON: {serial: {note...}}); default launchbox_exclude.json")
    ap.add_argument("--cache")
    ap.add_argument("--loose", action="store_true", help="tier 3: also leave the notes out of the LaunchBox name (less sure)")
    a = ap.parse_args()

    if a.metadata:
        games, alts, images = read_launchbox(a.metadata)
        if a.cache:
            json.dump({"games": games, "alts": alts, "images": images}, open(a.cache, "w"), ensure_ascii=False)
    elif a.cache and os.path.exists(a.cache):
        d = json.load(open(a.cache))
        games, alts, images = d["games"], {i: [tuple(x) for x in v] for i, v in d["alts"].items()}, d["images"]
    else:
        sys.exit("need --metadata <Metadata.xml> (or --cache <file> made by an earlier run)")
    index = read_gameindex(a.gameindex)
    excluded = set(json.load(open(a.exclude, encoding="utf-8"))) if a.exclude and os.path.exists(a.exclude) else set()
    if excluded:
        print("leaving out %d serials (%s)" % (len(excluded), os.path.basename(a.exclude)))
    print("LaunchBox PS2 games: %d (with art: %d); GameIndex serials: %d" % (len(games), len(images), len(index)))

    by_key = {}   # full title key -> [(game id, True when it is the game's own name)]
    by_loose = {}  # title key with the notes left out -> [game ids]
    for gid, g in games.items():
        seen, seen_loose = set(), set()
        for own, n in [(True, g["name"])] + [(False, a) for a, _ in alts.get(gid, [])]:
            ks = keys_for(n)
            if ks[0] not in seen:
                seen.add(ks[0])
                by_key.setdefault(ks[0], []).append((gid, own))
            if ks[-1] not in seen_loose:
                seen_loose.add(ks[-1])
                by_loose.setdefault(ks[-1], []).append(gid)

    out, audit = {}, []
    tier_count = {1: 0, 2: 0, 3: 0}
    ambiguous = 0
    for serial in sorted(index):
        if serial in excluded:
            continue
        e = index[serial]
        names = [n for n in (e.get("name-en"), e.get("name")) if n]
        exact = exact_regions(e.get("region", ""))
        found = None  # (tier, [(gid, own)], matched name)
        for tier in (1, 2, 3):
            if tier == 3 and not a.loose:
                break
            for n in names:
                ks = keys_for(n)
                if tier == 2 and (len(ks) < 2 or NO_RETAIL.search(n)):
                    continue  # no notes to leave out, or a demo that must not take the retail box
                k = ks[0] if tier == 1 else ks[-1]
                if tier == 3:
                    hit = [(g, False) for g in by_loose.get(k, [])]
                else:
                    hit = by_key.get(k, [])
                if hit:
                    found = (tier, hit, n)
                    break
            if found:
                break
        if not found:
            continue
        tier, hits, matched = found
        own_of = {}
        for g, own in hits:
            own_of[g] = own_of.get(g, False) or own
        gids = [g for g in own_of if g in images]
        if not gids:
            continue

        def score(gid):
            imgs = images[gid]
            fr = pick(imgs, "f", exact)
            return (0 if own_of[gid] else 1, 0 if fr and fr[1] else 1, 0 if fr else 1, -len(imgs), int(gid))
        gids.sort(key=score)
        if len(gids) > 1:
            ambiguous += 1
        gid = gids[0]
        rec = {}
        for kind in ("f", "b", "d", "s"):
            p = pick(images[gid], kind, exact)
            if p:
                rec[kind] = p[0]
                if p[1]:
                    rec[kind + "x"] = 1
        if "f" not in rec and "b" not in rec and "d" not in rec and "s" not in rec:
            continue
        rec["m"] = tier
        out[serial] = rec
        tier_count[tier] += 1
        audit.append({"serial": serial, "region": e.get("region", ""), "gameindex": names, "matched_on": matched, "tier": tier,
                      "launchbox": games[gid]["name"], "own_name": own_of[gid], "id": gid, "date": games[gid]["date"], "alternatives": [games[g]["name"] for g in gids[1:4]],
                      "has": sorted(k for k in rec if k in "fbds")})

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# LaunchBox Games Database art by PS2 serial (Metadata.zip matched to PCSX2's GameIndex by title; tools/make_launchbox_covers.py)\n")
        f.write("# SERIAL f=<front> b=<back> d=<disc> s=<spine> m=<match tier>; a file name with a trailing * is art from the serial's own region\n")
        for serial in sorted(out):
            r = out[serial]
            parts = [serial]
            for kind in "fbds":
                if kind in r:
                    parts.append("%s=%s%s" % (kind, r[kind], "*" if (kind + "x") in r else ""))
            parts.append("m=%d" % r["m"])
            f.write(" ".join(parts) + "\n")
    if a.audit:
        json.dump(audit, open(a.audit, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    kinds = {k: sum(1 for r in out.values() if k in r) for k in "fbds"}
    exact_n = {k: sum(1 for r in out.values() if k + "x" in r) for k in "fbds"}
    print("matched %d of %d serials (tier 1: %d, tier 2: %d, tier 3: %d; %d with several LaunchBox candidates)" % (
        len(out), len(index), tier_count[1], tier_count[2], tier_count[3], ambiguous))
    print("front/back/disc: %s; from the serial's own region: %s" % (kinds, exact_n))
    print("wrote %s (%d bytes)" % (a.out, os.path.getsize(a.out)))


if __name__ == "__main__":
    main()
