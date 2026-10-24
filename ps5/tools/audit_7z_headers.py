#!/usr/bin/env python3
"""Judges the .7z packs of ps5/frontend/assets/hd-pack-sources.json the way the app will, from each file's head and tail only (b176, 2026-10-16, AI-assisted).

Why: the 192 Bl4ckH4nd packs are 7z files of 75 MB to 2.2 GB. The app (frontend/fe_sevenzip.c) refuses a pack that needs a dictionary over 64 MiB, uses a filter (BCJ ...)
or a second coder, is password-protected, has no CRCs, has more than 65,536 entries, a wanted file over 256 MiB, or unpacks to over 12 GiB; and it refuses a pack that holds
textures for another game than the list says. It only finds out once the whole file is downloaded and its header read. This tool finds out first, without downloading the packs:
a 7z keeps its header at the END of the file (50 to 650 KB), so two small ranged GETs per pack are enough: bytes 0-31 (the start header) and the last 256 KiB or so. They are put into a
SPARSE file of the pack's full size (zeros between) and the app's own reader is run on it (host/sevenzip_test --scan-only, built by host/build-sevenzip-driver.sh from the same
fe_sevenzip.c the app links), so the verdicts are the app's, not a second parser's.

  ps5/frontend/host/build-sevenzip-driver.sh                                  once (clang, zlib)
  python3 ps5/tools/audit_7z_headers.py                                      all .7z packs of the shipped list: about 2 requests each, ~250 KB each, 2 s apart
  python3 ps5/tools/audit_7z_headers.py --only Zapper --only "Black"         just those (name contains)
  python3 ps5/tools/audit_7z_headers.py --out audit.json --csv audit.csv      the report as files (the table is printed anyway)

Verdicts: ok | DICT_TOO_BIG | TOO_MANY_FILES | HEADER_TOO_BIG | ENTRY_TOO_BIG | TOO_BIG | UNSUPPORTED | ENCRYPTED | NO_CRC | NO_MEMORY (the app refuses it: say so in the shelf's words)
          | serial_mismatch (it holds textures only for other serials than the list says) | serial_partial (for some of the serials the list gives, not all)
          | nothing_wanted (no replacements/ folder) | unreadable (damaged / cut / not a 7z)
          | drive: <status> (Drive did not give the bytes: too many downloads of the file, removed ...)
Also per pack: entries, unpacked size, dictionary buffer, file table, and the free flexible memory the app would want before it unpacks it ("gate": dictionary + chunks + file table
+ 24 MiB a game + index + 48 MiB margin; the same sum as TexturePackManager::Unpack7z).

Public pages only, no pack file is downloaded (only 32 bytes and a tail of each), no credentials anywhere. Be polite: --pause (default 2 s) between requests; the tool stops after
3 requests in a row that Drive refuses.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import csv
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

DRIVE_URL = "https://drive.usercontent.google.com/download?id=%s&export=download&confirm=t"
UA = "Mozilla/5.0 (PS5SX2 pack audit tool)"
SIG = b"7z\xbc\xaf\x27\x1c"
MAX_HEADER = 16 << 20  # fe_sevenzip.c: Sz7Limits::max_header_bytes
REFUSALS = {"DICT_TOO_BIG", "TOO_MANY_FILES", "HEADER_TOO_BIG", "ENTRY_TOO_BIG", "TOO_BIG", "UNSUPPORTED", "ENCRYPTED", "NO_CRC", "NO_MEMORY"}
SERIAL = re.compile(r"[A-Z]{4}-\d{5}")
MIB = 1 << 20


class DriveRefused(Exception):
    pass


def read_number(b, i):
    """A 7z NUMBER: the leading 1-bits of the first byte say how many bytes follow."""
    first = b[i]
    mask = 0x80
    value = 0
    for k in range(8):
        if not (first & mask):
            return value + ((first & (mask - 1)) << (8 * k)), i + 1 + k
        value |= b[i + 1 + k] << (8 * k)
        mask >>= 1
    return value, i + 9


def packed_header_span(end_header):
    """(absolute position, size) of the packed header when the end header is an 'encoded header' (0x17: PackInfo says where the real header is), else None."""
    try:
        if not end_header or end_header[0] != 0x17 or end_header[1] != 0x06:
            return None
        pos, i = read_number(end_header, 2)
        count, i = read_number(end_header, i)
        sizes = []
        while i < len(end_header):
            pid = end_header[i]
            i += 1
            if pid == 0x00:
                break
            if pid == 0x09:
                for _ in range(count):
                    s, i = read_number(end_header, i)
                    sizes.append(s)
                break
            return None
        return (32 + pos, sum(sizes)) if sizes else None
    except IndexError:
        return None


def ranged(url_template, fid, a, b, timeout=60):
    """bytes a..b of the file: (status, total size, content type, data). Reads at most b-a+1 bytes."""
    req = urllib.request.Request(url_template % fid, headers={"User-Agent": UA, "Range": "bytes=%d-%d" % (a, b)})
    want = b - a + 1
    try:
        r = urllib.request.urlopen(req, timeout=timeout)
    except urllib.error.HTTPError as e:
        return e.code, 0, "", b""
    except (urllib.error.URLError, OSError):
        return -1, 0, "", b""
    cr = r.headers.get("Content-Range", "")
    total = int(cr.rsplit("/", 1)[1]) if "/" in cr and cr.rsplit("/", 1)[1].isdigit() else 0
    ctype = r.headers.get("Content-Type", "")
    data = r.read(want + 1)
    r.close()
    return r.status, total, ctype, data[:want]


def parse_driver(text):
    out = {"names": []}
    for line in text.splitlines():
        if line.startswith("INFO "):
            body = line[5:]
            method = re.search(r'method="([^"]*)"', body)
            out["method"] = method.group(1) if method else ""
            for k, v in re.findall(r"(\w+)=(\d+)", body):
                out[k] = int(v)
        elif line.startswith("SCAN "):
            for k, v in re.findall(r"(\w+)=(\d+)", line):
                out["scan_" + k] = int(v)
        elif line.startswith("REPL "):
            for k, v in re.findall(r"(\w+)=(\d+)", line):
                out["repl_" + k] = int(v)
        elif line.startswith("SERIALS "):
            s = line[8:].strip()
            out["serials"] = [] if s == "-" else s.split(",")
        elif line.startswith("TEXTURES "):  # (review) what the app takes as a texture: <first serial folder>/replacements/<file>, or replacements/<file> with no serial folder
            m = re.match(r"TEXTURES serials=(\S+) files=(\d+) bytes=(\d+) noserial=(\d+)", line)
            if m:
                out["tex_serials"] = [] if m.group(1) == "-" else m.group(1).split(",")
                out["tex_files"], out["tex_bytes"], out["tex_noserial"] = int(m.group(2)), int(m.group(3)), int(m.group(4))
        elif line.startswith("NAME "):
            out["names"].append(line[5:])
        elif line.startswith("ERR "):
            parts = line.split(" ", 2)
            out["err"] = parts[1]
            out["err_text"] = parts[2] if len(parts) > 2 else ""
    return out


def gate_mib(info, n_serials):
    """the free flexible memory TexturePackManager::Unpack7z asks for (MiB), from what the driver printed"""
    need = (max(info.get("dict_need", 0), 4096) + (1 << 20) + (256 << 10) + (96 << 10) + info.get("header_live", 0) + max(n_serials, 1) * (24 << 20) +
            info.get("tex_files", info.get("repl_files", 0)) * 256 + (48 << 20))
    return need / MIB


def audit_one(entry, a):
    rec = {"id": entry["id"], "name": entry["name"], "serials": entry["serials"]}
    st, total, ctype, head = ranged(a.url, entry["id"], 0, 31)
    time.sleep(a.pause)
    if st not in (200, 206) or len(head) != 32 or total <= 0 or ctype.startswith("text/html"):
        rec["verdict"] = "drive: %s" % (st if not ctype.startswith("text/html") else "a web page instead of the file (%s)" % st)
        rec["drive_refused"] = True
        return rec
    rec["bytes"] = total
    if head[:6] != SIG:
        rec["verdict"] = "unreadable"
        rec["why"] = "not a 7z file"
        return rec
    off, size, _crc = struct.unpack("<QQI", head[12:32])
    end = 32 + off + size
    if size > MAX_HEADER:
        rec["verdict"] = "HEADER_TOO_BIG"
        rec["why"] = "the end header is %d bytes" % size
        return rec
    if end > total:
        rec["verdict"] = "unreadable"
        rec["why"] = "the end header lies outside the file (cut)"
        return rec
    window = min(end - 32, max(a.window_kib * 1024, size + 64))
    for attempt in range(4):
        start = end - window
        st, _t, _ct, tail = ranged(a.url, entry["id"], start, end - 1)
        time.sleep(a.pause)
        if st != 206 or len(tail) != window:  # (review) a 200 is the file's START (the range was ignored), never its tail
            rec["verdict"] = "drive: %s" % st
            rec["drive_refused"] = True
            return rec
        span = packed_header_span(tail[-size:] if size else b"")
        if span and span[0] < start and window < end - 32:  # the packed header starts before the window: take more
            window = min(end - 32, max(window * 4, end - span[0] + 64))
            if window > 16 * MIB:
                rec["verdict"] = "HEADER_TOO_BIG"
                rec["why"] = "the packed header is %d bytes" % span[1]
                return rec
            continue
        break
    rec["fetched"] = 32 + window
    path = None
    try:
        fd, path = tempfile.mkstemp(prefix="audit7z.", suffix=".7z", dir=a.tmp)
        with os.fdopen(fd, "wb") as f:
            f.truncate(total)
            f.seek(0)
            f.write(head)
            f.seek(start)
            f.write(tail)
        p = subprocess.run([a.driver, path, "--scan-only", "--names", "3"], capture_output=True, text=True, timeout=120)
    finally:
        if path and os.path.exists(path):
            os.unlink(path)
    d = parse_driver(p.stdout)
    rec["names"] = d["names"]
    for k in ("files", "folders", "pack", "unpack", "dict_declared", "dict_need", "header_live", "header_peak", "scan_kept"):
        if k in d:
            rec[k] = d[k]
    rec["method"] = d.get("method", "")
    # (review) the serials that have TEXTURES (the app's rule: <serial>/replacements/<file>), not every serial-looking folder: a readme in a game's own folder is no texture
    tex_serials = d.get("tex_serials", d.get("serials", []))
    tex_files = d.get("tex_files", d.get("repl_files", 0))
    rec["found_serials"] = tex_serials
    rec["replacements"] = tex_files
    if "err" in d:
        rec["why"] = d.get("err_text", "")
        rec["verdict"] = d["err"] if d["err"] in REFUSALS else "unreadable"
        return rec
    if "files" not in d:
        rec["verdict"] = "unreadable"
        rec["why"] = "the reader said nothing: " + (p.stderr.strip()[:120] or "exit %s" % p.returncode)
        return rec
    rec["gate_mib"] = round(gate_mib(d, len(tex_serials) + (1 if d.get("tex_noserial", 0) else 0)), 1)
    if tex_files == 0:
        rec["verdict"] = "nothing_wanted"
        rec["why"] = "no texture (nothing under a <serial>/replacements/ folder)"
    else:
        found = set(tex_serials)
        if d.get("tex_noserial", 0):  # textures in a replacements/ folder with no serial folder around it: the app installs them under the game's own serial
            found |= set(entry["serials"])
        missing = [s for s in entry["serials"] if s not in found]
        if len(missing) == len(entry["serials"]):
            rec["verdict"] = "serial_mismatch"
            rec["why"] = "the list says %s, the pack holds %s" % (",".join(entry["serials"]), ",".join(sorted(found)) or "no serial folder")
        elif missing:
            rec["verdict"] = "serial_partial"
            rec["why"] = "the pack has no folder for %s (the app refuses it for that game; it has %s)" % (",".join(missing), ",".join(sorted(found)))
        else:
            rec["verdict"] = "ok"
    return rec


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="Judge the .7z packs of the shipped list from their heads and tails (see the file's docstring).")
    ap.add_argument("--json", default=os.path.join(here, "..", "frontend", "assets", "hd-pack-sources.json"))
    ap.add_argument("--driver", default=os.path.join(here, "..", "frontend", "host", "sevenzip_test"), help="host/sevenzip_test (host/build-sevenzip-driver.sh makes it)")
    ap.add_argument("--only", action="append", default=[], help="only packs whose file name contains this (repeatable)")
    ap.add_argument("--limit", type=int, default=0, help="at most this many packs")
    ap.add_argument("--pause", type=float, default=2.0, help="seconds between requests")
    ap.add_argument("--window-kib", type=int, default=256, help="how much of the end of each file is asked for first (more is asked for when the header needs it)")
    ap.add_argument("--url", default=DRIVE_URL, help="the address of a file with %%s for its id (tests use a local server)")
    ap.add_argument("--tmp", default=None, help="where the sparse files are made (default: the system's)")
    ap.add_argument("--out", help="the report as JSON")
    ap.add_argument("--csv", help="the report as CSV")
    a = ap.parse_args(argv)
    if not os.path.exists(a.driver):
        print("no driver at %s: run ps5/frontend/host/build-sevenzip-driver.sh first" % a.driver, file=sys.stderr)
        return 2
    packs = [p for p in json.load(open(a.json, encoding="utf-8"))["packs"] if p["name"].lower().endswith(".7z")]
    if a.only:
        packs = [p for p in packs if any(o.lower() in p["name"].lower() for o in a.only)]
    if a.limit:
        packs = packs[: a.limit]
    print("%d .7z packs, about 2 requests each (%.0f KiB), %.1f s apart: roughly %.0f minutes" % (len(packs), 32 / 1024 + a.window_kib, a.pause, len(packs) * 2 * (a.pause + 0.5) / 60))
    report = []
    refused_in_a_row = 0
    for n, entry in enumerate(packs, 1):
        try:
            rec = audit_one(entry, a)
        except Exception as e:  # noqa: BLE001 - one bad pack must not stop the audit
            rec = {"id": entry["id"], "name": entry["name"], "serials": entry["serials"], "verdict": "error", "why": repr(e)}
        report.append(rec)
        refused_in_a_row = refused_in_a_row + 1 if rec.get("drive_refused") else 0
        print("%3d/%d %-48s %-16s %s" % (n, len(packs), entry["name"][:48], rec["verdict"], rec.get("why", rec.get("method", ""))[:90]), flush=True)
        if refused_in_a_row >= 3:
            print("Drive refused 3 requests in a row: stopping (try again later; the quota is per file and per day)", file=sys.stderr)
            break
    counts = {}
    for r in report:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    print("\nverdicts: " + ", ".join("%s %d" % kv for kv in sorted(counts.items())))
    ok = [r for r in report if r["verdict"] == "ok"]
    if ok:
        worst = max(ok, key=lambda r: r.get("gate_mib", 0))
        print("of the %d accepted, the biggest memory ask is %.0f MiB (%s); the biggest dictionary buffer is %.0f MiB" %
              (len(ok), worst["gate_mib"], worst["name"], max(r.get("dict_need", 0) for r in ok) / MIB))
    if a.out:
        json.dump(report, open(a.out, "w", encoding="utf-8"), indent=1)
    if a.csv:
        fields = ["name", "serials", "verdict", "why", "bytes", "files", "unpack", "dict_declared", "dict_need", "header_live", "gate_mib", "found_serials", "id"]
        with open(a.csv, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(fields)
            for r in report:
                w.writerow([",".join(r[k]) if isinstance(r.get(k), list) else r.get(k, "") for k in fields])
    return 0


if __name__ == "__main__":
    sys.exit(main())
