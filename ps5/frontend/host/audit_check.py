#!/usr/bin/env python3
"""PS5 port frontend: tests ps5/tools/audit_7z_headers.py against a LOCAL server that serves generated .7z files with Range support (2026-10-16, AI-assisted). Nothing leaves
the machine. Checks the verdicts the tool gives (the app's own reader judges a sparse copy made from 32 bytes of head and the tail), that it asks for the tail only (and again, bigger,
when the packed header starts before the window), that a web page or a 404 from "Drive" is told as such, and how many bytes it takes.

  audit_check.py <sevenzip_test driver> <work dir>            (test-sevenzip.sh runs it)

Copyright (C) 2026 swordpdf
SPDX-License-Identifier: GPL-3.0-or-later
"""
import http.server
import json
import os
import re
import sys
import threading
import zlib

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
sys.path.insert(0, os.path.join(here, "..", "..", "tools"))
import make_7z_fixtures as mk  # noqa: E402
import audit_7z_headers as audit  # noqa: E402

driver, work = sys.argv[1], sys.argv[2]
os.makedirs(work, exist_ok=True)
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL", what)
    return cond


# ---------------------------------------------------------------- the archives
def tree(serial, n=30, seed=1, extra=()):
    es = [mk.entry("Game", dir=True), mk.entry("Game/" + serial, dir=True), mk.entry("Game/" + serial + "/replacements", dir=True)]
    for i in range(n):
        es.append(mk.entry(f"Game/{serial}/replacements/{i:04x}.dds", mk.blob(seed + i, 2000 + i * 77)))
    es.extend(extra)
    return es


FILES = {}      # id -> (bytes, json entry fields)
SPECIAL = {}    # id -> "html" | status code


def add(fid, name, serials, data):
    FILES[fid] = data
    return {"id": fid, "name": name, "title": name[:-3], "label": "Bl4ckH4nd", "maker": "Bl4ckH4nd", "serials": serials}


packs = []
packs.append(add("OKOKOKOKOK0001", "Ok Pack.7z", ["SLUS-20776"], mk.build_archive(tree("SLUS-20776"), "solid", dict_size=1 << 20, declared_dict=16 << 20)))
packs.append(add("PLAINHDR000001", "Plain Header.7z", ["SLUS-20776"], mk.build_archive(tree("SLUS-20776"), "solid", encode_header=False)))
big = tree("SLUS-30000", n=6000, extra=[])
for e in big:  # long names make the packed header bigger than a 4 KiB window
    e["name"] = e["name"].replace(".dds", "-" + "x" * 60 + str(zlib.crc32(e["name"].encode())) + ".dds")
packs.append(add("BIGHEADER00001", "Big Header.7z", ["SLUS-30000"], mk.build_archive(big, "solid")))
zeros = [mk.entry("A/SLUS-20776/replacements/z.dds", bytes(100 << 20))]
large = [mk.entry(f"Game/SLUS-20776/replacements/{i}.dds", mk.blob(500 + i, 2500000, 0.2)) for i in range(8)]
packs.append(add("LARGE000000001", "Large.7z", ["SLUS-20776"], mk.build_archive(large, "solid", dict_size=1 << 20)))
packs.append(add("DICT0000000001", "Big Dict.7z", ["SLUS-20776"], mk.build_archive(zeros, "solid", dict_size=1 << 20, declared_dict=512 << 20)))
packs.append(add("FILTER00000001", "Filter.7z", ["SLUS-20776"], mk.build_archive(tree("SLUS-20776"), "solid", coder="bcj_lzma2")))
packs.append(add("SECRET00000001", "Secret.7z", ["SLUS-20776"], mk.build_archive(tree("SLUS-20776"), "solid", coder="aes_fake")))
packs.append(add("NOSUMS00000001", "No Sums.7z", ["SLUS-20776"], mk.build_archive(tree("SLUS-20776"), "solid", no_crc=True)))
packs.append(add("WRONG000000001", "Typo.7z", ["SLUS-21722"], mk.build_archive(tree("SLUS-21752"), "solid")))
two = tree("SLUS-21180", n=5) + tree("SLUS-21362", n=5)
packs.append(add("TWODISCS000001", "Two Discs.7z", ["SLUS-21180", "SLUS-21362"], mk.build_archive(two, "solid")))
packs.append(add("PARTIAL0000001", "Half.7z", ["SLUS-21180", "SLUS-21362"], mk.build_archive(tree("SLUS-21180"), "solid")))
packs.append(add("NOTHING0000001", "Nothing.7z", ["SLUS-20776"], mk.build_archive([mk.entry("Other/readme.txt", b"hi")], "solid")))
# (review) the app's rule for what a texture is: <first serial folder>/replacements/<file>. A readme in the game's own folder is no texture (the textures are the neighbour's:
# refused as for the wrong game); a replacements/ folder with no serial folder around it is installed under the game's own serial (accepted)
packs.append(add("READMEONLY00001", "Readme Only.7z", ["SLUS-20776"], mk.build_archive(
    [mk.entry("Game", dir=True), mk.entry("Game/SLUS-20776", dir=True), mk.entry("Game/SLUS-20776/readme.txt", b"a readme"),
     mk.entry("Game/SLUS-20777/replacements/a.dds", mk.blob(1, 5000))], "solid")))
packs.append(add("NOSERIAL0000001", "No Serial Folder.7z", ["SLUS-20776"], mk.build_archive(
    [mk.entry("Game/replacements/a.dds", mk.blob(2, 5000)), mk.entry("Game/replacements/b.dds", mk.blob(3, 4000))], "solid")))
packs.append(add("GARBAGE0000001", "Garbage.7z", ["SLUS-20776"], b"PK\x03\x04" + os.urandom(3000)))
packs.append(add("HTMLPAGE000001", "Web Page.7z", ["SLUS-20776"], b"<html>too many downloads</html>"))
SPECIAL["HTMLPAGE000001"] = "html"
packs.append(add("MISSING0000001", "Gone.7z", ["SLUS-20776"], b"x"))
SPECIAL["MISSING0000001"] = 404
packs.append(add("TRUNCATED00001", "Cut.7z", ["SLUS-20776"], mk.build_archive(tree("SLUS-20776"), "solid")[:-50]))
EXPECT = {
    "Large.7z": "ok", "Ok Pack.7z": "ok", "Plain Header.7z": "ok", "Big Header.7z": "ok", "Big Dict.7z": "DICT_TOO_BIG", "Filter.7z": "UNSUPPORTED", "Secret.7z": "ENCRYPTED", "No Sums.7z": "NO_CRC",
    "Typo.7z": "serial_mismatch", "Two Discs.7z": "ok", "Half.7z": "serial_partial", "Nothing.7z": "nothing_wanted", "Garbage.7z": "unreadable",
    "Readme Only.7z": "serial_mismatch", "No Serial Folder.7z": "ok",
    "Web Page.7z": "drive: a web page instead of the file (200)", "Gone.7z": "drive: 404", "Cut.7z": "unreadable",
}
# a zip-named entry and a rar are ignored by the tool (only .7z)
packs.append({"id": "SKIPPED00000001", "name": "Not 7z.rar", "title": "Not 7z", "label": "X", "maker": "X", "serials": ["SLUS-20776"]})
json_path = os.path.join(work, "list.json")
json.dump({"version": 1, "packs": packs}, open(json_path, "w"))

# ---------------------------------------------------------------- the local server
LOG = []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        fid = self.path.lstrip("/")
        data = FILES.get(fid)
        rng = self.headers.get("Range", "")
        m = re.match(r"bytes=(\d+)-(\d+)$", rng)
        LOG.append((fid, int(m.group(1)) if m else None, int(m.group(2)) if m else None))
        special = SPECIAL.get(fid)
        if data is None or special == 404:
            self.send_response(404)
            self.end_headers()
            return
        if special == "html":
            body = b"<html>Google Drive: too many users have downloaded this file</html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if not m:
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        a, b = int(m.group(1)), min(int(m.group(2)), len(data) - 1)
        body = data[a:b + 1]
        self.send_response(206)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Range", "bytes %d-%d/%d" % (a, b, len(data)))
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()
url = "http://127.0.0.1:%d/%%s" % port

# ---------------------------------------------------------------- runs
for window_kib in (4, 256):
    del LOG[:]
    out = os.path.join(work, "report-%d.json" % window_kib)
    csv_path = os.path.join(work, "report-%d.csv" % window_kib)
    print("---- window %d KiB" % window_kib)
    rc = audit.main(["--json", json_path, "--driver", driver, "--url", url, "--pause", "0", "--window-kib", str(window_kib), "--out", out, "--csv", csv_path, "--tmp", work])
    check(rc == 0, "the tool ran")
    rep = {r["name"]: r for r in json.load(open(out))}
    check(len(rep) == len(EXPECT) and "Not 7z.rar" not in rep, "only the .7z entries are judged (%d)" % len(rep))
    for name, want in EXPECT.items():
        got = rep.get(name, {}).get("verdict")
        check(got == want, "%s: %r, wanted %r (%s)" % (name, got, want, rep.get(name, {}).get("why", "")))
    # what it asked for: the first 32 bytes, then a tail; never a whole file
    per = {}
    for fid, a, b in LOG:
        per.setdefault(fid, []).append((a, b))
    for fid, reqs in per.items():
        size = len(FILES[fid])
        check(all(a is not None for a, b in reqs), "%s: every request is a range" % fid)
        check(reqs[0] == (0, 31), "%s: the first request is the start header" % fid)
        served = sum((b - a + 1) for a, b in reqs if a is not None and b is not None)
        check(len(reqs) <= 5, "%s: at most a few requests (%d)" % (fid, len(reqs)))
        if size > (4 << 20):
            check(served < 300000, "%s: %d bytes asked for of %d (a head and a tail, never the file)" % (fid, served, size))
    # the big header needs the second, bigger tail with a 4 KiB window and not with 256 KiB
    n_big = len(per.get("BIGHEADER00001", []))
    if window_kib == 4:
        check(n_big >= 3, "a packed header that starts before the window is asked for again (%d requests)" % n_big)
    else:
        check(n_big == 2, "a 256 KiB window is enough for it (%d requests)" % n_big)
    # the csv has a line a pack
    check(len(open(csv_path, encoding="utf-8").read().strip().splitlines()) == len(EXPECT) + 1, "the csv has a row for each pack")
    ok = rep["Ok Pack.7z"]
    check(ok.get("gate_mib", 0) > 70 and ok.get("files", 0) > 30 and ok.get("found_serials") == ["SLUS-20776"], "the numbers of an accepted pack (%s)" % {k: ok.get(k) for k in ("gate_mib", "files", "found_serials")})

server.shutdown()
print("AUDIT TOOL CHECK: %d failure(s)" % failures if failures else "AUDIT TOOL CHECK PASS")
sys.exit(1 if failures else 0)
