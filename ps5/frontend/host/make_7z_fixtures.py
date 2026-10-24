#!/usr/bin/env python3
"""PS5 port frontend: makes the 7z test archives (2026-10-16, AI-assisted). A small 7z WRITER (pure Python: the container by hand, the streams by the lzma module)
and four commands. Stdlib only; nothing here ships and nothing big is committed: the archives are made when a test runs.

  make_7z_fixtures.py corpus <dir> [--big]     <dir>/<name>.7z and <name>.json for sevenzip_check.py: {"expect": "ok" | "err:CODE|CODE2", "files": [[name, size, crc32 hex]], "args": [...]}
  make_7z_fixtures.py texpacks <dir>           the archives texpacks_test.cpp installs through the manager (names below), each with <name>.manifest ("<name under replacements/> <size> <md5>")
  make_7z_fixtures.py write <out.7z> <files> <MiB per file> <dict MiB> [compressibility]    one big solid archive (memory runs)
  make_7z_fixtures.py seeds <dir>              38 small archives (every coder, solid / non-solid / blocks, plain and packed headers) to start libFuzzer from (fuzz7z.cpp)

Every stream here was made by liblzma, not by 7-Zip itself (no 7-Zip exists on the test machine); 7-Zip's multithreaded LZMA2 (one stream of independent
blocks, a dictionary reset at the start of each) is imitated by "mt" below. The real 7-Zip headers of the 10 probed Drive packs are exercised separately
(sevenzip_check.py, SZ7_REAL_PROBES).

Copyright (C) 2026 swordpdf
SPDX-License-Identifier: GPL-3.0-or-later
"""
import hashlib, json, lzma, os, random, struct, sys, zlib


# ---------------------------------------------------------------- the writer
def num(v):
    for i in range(8):
        if v < (1 << (7 * (i + 1))):
            first = ((0xFF << (8 - i)) & 0xFF) | (v >> (8 * i))
            return bytes([first]) + (v & ((1 << (8 * i)) - 1)).to_bytes(i, "little")
    return b"\xff" + v.to_bytes(8, "little")


def crc(b):
    return zlib.crc32(b) & 0xffffffff


def bits(flags):
    out = bytearray((len(flags) + 7) // 8)
    for i, f in enumerate(flags):
        if f:
            out[i >> 3] |= 0x80 >> (i & 7)
    return bytes(out)


def lzma2_prop(dict_size):
    for p in range(41):
        d = 0xFFFFFFFF if p == 40 else (2 | (p & 1)) << (p // 2 + 11)
        if d >= dict_size:
            return p
    return 40


def lzma_props(dict_size, lc=3, lp=0, pb=2):
    return bytes([(pb * 5 + lp) * 9 + lc]) + struct.pack("<I", dict_size)


MT_BLOCK = 3 << 20


def compress(coder, data, dict_size, declared_dict, mt=False, lzma1_props=None):
    """-> (packed bytes, coder id bytes, props bytes)"""
    dd = declared_dict if declared_dict is not None else dict_size
    if coder == "lzma2":
        def one(chunk):
            c = lzma.LZMACompressor(format=lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA2, "preset": 1, "dict_size": max(dict_size, 4096)}])
            return c.compress(chunk) + c.flush()
        if mt:
            # 7-Zip -mmt: ONE stream of independent blocks, each starting with a dictionary reset; only the last carries the 0x00 end marker
            n = max(1, (len(data) + MT_BLOCK - 1) // MT_BLOCK)
            parts = []
            for i in range(n):
                p = one(data[i * MT_BLOCK:(i + 1) * MT_BLOCK])
                assert p[-1] == 0
                parts.append(p if i == n - 1 else p[:-1])
            packed = b"".join(parts)
        else:
            packed = one(data)
        return packed, b"\x21", bytes([lzma2_prop(dd)])
    if coder == "lzma":
        c = lzma.LZMACompressor(format=lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA1, "preset": 1, "dict_size": max(dict_size, 4096), "lc": 3, "lp": 0, "pb": 2}])
        packed = c.compress(data) + c.flush()
        props = lzma1_props if lzma1_props is not None else lzma_props(max(dd, 4096))
        return packed, b"\x03\x01\x01", props
    if coder == "copy":
        return data, b"\x00", b""
    raise ValueError(coder)


def build_header_streams(folders, entries, no_crc=False):
    h = bytearray()
    if not folders:
        return b""  # no streams: 7-Zip leaves MainStreamsInfo out
    h += b"\x04"
    h += b"\x06" + num(0) + num(len(folders)) + b"\x09" + b"".join(num(len(f["packed"])) for f in folders) + b"\x00"
    h += b"\x07\x0b" + num(len(folders)) + b"\x00"
    for f in folders:
        if f.get("chain"):  # a BCJ-like filter + the main coder
            h += num(2)
            cp = f["chain"]["props"]
            h += bytes([len(f["chain"]["id"]) | (0x20 if cp else 0)]) + f["chain"]["id"] + ((num(len(cp)) + cp) if cp else b"")
            flags = len(f["coder_id"]) | (0x20 if f["props"] else 0)
            h += bytes([flags]) + f["coder_id"] + ((num(len(f["props"])) + f["props"]) if f["props"] else b"")
            h += num(0) + num(1)
        else:
            h += num(1)
            flags = len(f["coder_id"]) | (0x20 if f["props"] else 0)
            h += bytes([flags]) + f["coder_id"] + ((num(len(f["props"])) + f["props"]) if f["props"] else b"")
    h += b"\x0c"
    for f in folders:
        h += num(f["unpack_size"]) * (2 if f.get("chain") else 1)
    if not no_crc and any(f.get("folder_crc") is not None for f in folders):
        h += b"\x0a"
        if all(f.get("folder_crc") is not None for f in folders):
            h += b"\x01" + b"".join(struct.pack("<I", f["folder_crc"]) for f in folders)
        else:
            h += b"\x00" + bits([f.get("folder_crc") is not None for f in folders]) + b"".join(struct.pack("<I", f["folder_crc"]) for f in folders if f.get("folder_crc") is not None)
    h += b"\x00"
    h += b"\x08"  # SubStreamsInfo
    if any(len(f["files"]) != 1 for f in folders):
        h += b"\x0d" + b"".join(num(len(f["files"])) for f in folders)
    if any(len(f["files"]) > 1 for f in folders):
        h += b"\x09"
        for f in folders:
            if len(f["files"]) > 1:
                h += b"".join(num(len(entries[i]["data"])) for i in f["files"][:-1])
    need = []
    for f in folders:
        if len(f["files"]) == 1 and f.get("folder_crc") is not None and not no_crc:
            continue
        need += [entries[i]["crc"] for i in f["files"]]
    if need and not no_crc:
        h += b"\x0a\x01" + b"".join(struct.pack("<I", c) for c in need)
    h += b"\x00"
    h += b"\x00"
    return bytes(h)


def build_files_info(entries):
    n = len(entries)
    h = bytearray(b"\x05" + num(n))
    empty_stream = [e["dir"] or len(e["data"]) == 0 for e in entries]
    if any(empty_stream):
        es = bits(empty_stream)
        h += b"\x0e" + num(len(es)) + es
        ef = bits([not e["dir"] for e, s in zip(entries, empty_stream) if s])
        if any(not e["dir"] for e, s in zip(entries, empty_stream) if s):
            h += b"\x0f" + num(len(ef)) + ef
    names = b"".join(e["name"].encode("utf-16-le", "surrogatepass") + b"\x00\x00" for e in entries)
    h += b"\x11" + num(len(names) + 1) + b"\x00" + names
    attrs = b"".join(struct.pack("<I", e["attr"] if e.get("attr") is not None else (0x10 if e["dir"] else 0x20)) for e in entries)
    h += b"\x15" + num(len(attrs) + 2) + b"\x01\x00" + attrs
    h += b"\x00"
    return bytes(h)


def build_archive(entries, mode="solid", coder="lzma2", dict_size=1 << 20, declared_dict=None, encode_header=True, folder_crc=False, block_bytes=1 << 20,
                  corrupt=None, crc_lie=False, header_hook=None, unpack_lie=0, hdr_unpack_lie=0, mt=False, no_crc=False, lzma1_props=None):
    """entries: dicts name=, data=bytes|None, dir=bool, attr=int|None. mode: solid (one block) | nonsolid (a block a file) | blocks (a block each block_bytes).
    coder: lzma2 | lzma | copy | bcj_lzma2 | deflate_fake | aes_fake. The knobs make bad archives (declared_dict lies about the dictionary; corrupt(packed) damages
    the stream; crc_lie flips stored CRCs; header_hook(header) mutates the plain header before it is encoded; unpack_lie / hdr_unpack_lie lie about sizes)."""
    ents = []
    for e in entries:
        e = dict(e)
        e.setdefault("dir", False)
        e["data"] = b"" if e["dir"] else (e.get("data") or b"")
        e["crc"] = crc(e["data"]) ^ (1 if crc_lie and e["data"] else 0)
        ents.append(e)
    nonempty = [i for i, e in enumerate(ents) if not e["dir"] and len(e["data"]) > 0]
    groups = []
    if mode == "solid":
        groups = [nonempty] if nonempty else []
    elif mode == "nonsolid":
        groups = [[i] for i in nonempty]
    elif mode == "blocks":
        cur, size = [], 0
        for i in nonempty:
            cur.append(i)
            size += len(ents[i]["data"])
            if size >= block_bytes:
                groups.append(cur)
                cur, size = [], 0
        if cur:
            groups.append(cur)
    folders, packed_all = [], bytearray()
    for g in groups:
        raw = b"".join(ents[i]["data"] for i in g)
        real = {"bcj_lzma2": "lzma2", "deflate_fake": "copy", "aes_fake": "copy"}.get(coder, coder)
        packed, cid, props = compress(real, raw, dict_size, declared_dict, mt=mt, lzma1_props=lzma1_props)
        if corrupt:
            packed = corrupt(packed)
        f = {"packed": packed, "coder_id": cid, "props": props, "unpack_size": len(raw) + unpack_lie, "files": g}
        if coder == "bcj_lzma2":
            f["chain"] = {"id": b"\x03\x03\x01\x03", "props": b""}
        if coder == "deflate_fake":
            f["coder_id"], f["props"] = b"\x04\x01\x08", b""
        if coder == "aes_fake":
            f["coder_id"], f["props"] = b"\x06\xf1\x07\x01", b"\x00"
        if folder_crc or mode == "nonsolid":
            f["folder_crc"] = crc(raw) ^ (1 if crc_lie else 0)
        folders.append(f)
        packed_all += packed
    header = b"\x01" + build_header_streams(folders, ents, no_crc=no_crc) + build_files_info(ents) + b"\x00"
    if header_hook:
        header = header_hook(header)
    body = bytearray(packed_all)
    if encode_header:
        hp = lzma.LZMACompressor(format=lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA1, "preset": 1, "dict_size": 1 << 20, "lc": 3, "lp": 0, "pb": 2}])
        hpacked = hp.compress(header) + hp.flush()
        pack_pos = len(body)
        body += hpacked
        end = bytearray(b"\x17")
        end += b"\x06" + num(pack_pos) + num(1) + b"\x09" + num(len(hpacked)) + b"\x00"
        end += b"\x07\x0b" + num(1) + b"\x00" + num(1) + bytes([0x23]) + b"\x03\x01\x01" + num(5) + lzma_props(1 << 20)
        end += b"\x0c" + num(len(header) + hdr_unpack_lie) + b"\x0a\x01" + struct.pack("<I", crc(header)) + b"\x00"
        end += b"\x00"
        end_header = bytes(end)
    else:
        end_header = header
    start = struct.pack("<QQI", len(body), len(end_header), crc(end_header))
    sig = b"7z\xbc\xaf\x27\x1c" + b"\x00\x04" + struct.pack("<I", crc(start)) + start
    return sig + bytes(body) + end_header


def sig_with(archive, next_off=None, next_size=None):
    """the start header rewritten (CRCs fixed) with another next-header offset/size: a hostile archive"""
    off, size, ncrc = struct.unpack("<QQI", archive[12:32])
    off = off if next_off is None else next_off
    size = size if next_size is None else next_size
    start = struct.pack("<QQI", off, size, ncrc)
    return archive[:8] + struct.pack("<I", crc(start)) + start + archive[32:]


# ---------------------------------------------------------------- data
def blob(seed, n, comp=0.5):
    """n bytes: `comp` of it repeats earlier parts (like textures), the rest is noise"""
    r = random.Random(seed)
    b = bytearray()
    while len(b) < n:
        if b and r.random() < comp:
            l = min(r.randrange(64, 4096), n - len(b))
            start = max(0, len(b) - r.randrange(64, 200000))
            b += b[start:start + l]
        else:
            b += r.randbytes(min(r.randrange(16, 2048), n - len(b)))
    return bytes(b[:n])


def entry(name, data=None, dir=False, attr=None):
    return {"name": name, "data": data, "dir": dir, "attr": attr}


def manifest(entries, skip=lambda n: False):
    return [[e["name"], len(e["data"] or b""), "%08x" % (zlib.crc32(e["data"] or b"") & 0xffffffff)] for e in entries if not e["dir"] and not skip(e["name"])]


def pack_tree(serial="SLUS-20776", n=40, seed=1):
    es = [entry("Game", dir=True), entry("Game/" + serial, dir=True), entry("Game/" + serial + "/replacements", dir=True)]
    sizes = [0, 1, 2, 4095, 4096, 4097, 70000, 300000]
    for i in range(n):
        s = sizes[i % len(sizes)] if i % 3 == 0 else random.Random(seed + i).randrange(100, 200000)
        es.append(entry(f"Game/{serial}/replacements/{i:04x}-{i * 7:04x}.dds", blob(seed + i, s)))
    es.append(entry("Game/" + serial + "/replacements/éè 日本 \U0001F600.png", blob(99, 5000)))
    es.append(entry("Game/" + serial + "/replacements/" + "d" * 40 + "/" + "x" * 500 + ".dds", blob(98, 777)))
    es.append(entry("Game/" + serial + "/dumps/readme.txt", b"a dump"))
    es.append(entry("Game/" + serial + "/replacements/empty1.dds", b""))
    es.append(entry("Game/" + serial + "/replacements/empty2.dds", b""))
    return es


# ---------------------------------------------------------------- the corpus
def corpus(out, big=False):
    os.makedirs(out, exist_ok=True)

    def save(name, archive, entries, expect="ok", args=None, skip=lambda n: False, note=""):
        open(os.path.join(out, name + ".7z"), "wb").write(archive)
        json.dump({"expect": expect, "files": manifest(entries, skip) if expect == "ok" else [], "args": args or [], "note": note, "bytes": len(archive)},
                  open(os.path.join(out, name + ".json"), "w"), indent=1)
        print(f"{name:30s} {len(archive):>10d} bytes  expect {expect}")

    T = pack_tree()
    a = build_archive(T, "solid")
    save("solid_basic", build_archive(T, "solid", dict_size=1 << 20, declared_dict=16 << 20), T)
    save("solid_plainhdr", build_archive(T, "solid", encode_header=False), T)
    save("nonsolid", build_archive(T, "nonsolid"), T)
    # a block a file, 3,000 of them: every block opens its own decoder, dictionary and input buffer, and frees them (ASAN/LSAN: no leak per block)
    NS3 = [entry(f"N/SLUS-33336/replacements/{i:05d}.dds", blob(9000 + i, 200 + (i * 37) % 3000, 0.3)) for i in range(3000)]
    save("nonsolid_3000_blocks", build_archive(NS3, "nonsolid"), NS3)
    save("blocks", build_archive(T, "blocks", block_bytes=400000), T)
    save("lzma1_solid", build_archive(T, "solid", coder="lzma", dict_size=1 << 20), T)
    save("copy_solid", build_archive(T, "solid", coder="copy"), T)
    save("folder_crc_solid", build_archive(T, "solid", folder_crc=True), T)
    save("skip_dumps_and_unwanted", a, T, args=["--skip", "/dumps/"], skip=lambda n: "/dumps/" in n, note="--skip makes the driver leave a file out: it is still decoded and checked")
    # a file longer than the dictionary buffer: the circular dictionary wraps (24 MiB through a 1 MiB dictionary)
    W = [entry("A/SLES-50974/replacements/big.dds", blob(7, 24 << 20, 0.6)), entry("A/SLES-50974/replacements/after.dds", blob(8, 3 << 20))]
    save("wrap_1mib_dict", build_archive(W, "solid", dict_size=1 << 20), W)
    # 7-Zip -mmt: independent blocks (a dictionary reset in the middle of the stream) through a small circular buffer
    MT = [entry(f"M/SLUS-12345/replacements/{i:04d}.dds", blob(500 + i, 350000, 0.5) if i % 4 else os.urandom(300000)) for i in range(60)]
    save("mt_style_dict1m", build_archive(MT, "solid", dict_size=1 << 20, mt=True), MT, note="7-Zip multithreaded LZMA2, dict 1 MiB, blocks of 3 MiB")
    save("mt_style_declared4m", build_archive(MT, "solid", dict_size=1 << 20, declared_dict=4 << 20, mt=True), MT)
    save("mt_style_small_dict_big_declared", build_archive(MT, "solid", dict_size=64 << 10, declared_dict=32 << 20, mt=True), MT)
    # declared dictionary 1 GiB (prop 36), small data: the buffer is min(dict, unpack) = 2.4 MB
    D = [entry("A/SLES-50974/replacements/x.dds", blob(11, 2 << 20)), entry("A/SLES-50974/replacements/y.dds", blob(12, 400000))]
    save("declared_1gib_small_data", build_archive(D, "solid", dict_size=1 << 20, declared_dict=1 << 30), D, note="the buffer is min(dict, unpack): accepted")
    Z = [entry("A/SLES-50974/replacements/zeros.dds", bytes(100 << 20)), entry("A/SLES-50974/replacements/n.dds", blob(13, 1000))]
    save("dict_over_limit", build_archive(Z, "solid", dict_size=1 << 20, declared_dict=512 << 20), Z, expect="err:DICT_TOO_BIG")
    save("dict_4gib", build_archive(Z, "solid", dict_size=1 << 20, declared_dict=0xFFFFFFFF), Z, expect="err:DICT_TOO_BIG")
    save("dict_over_limit_but_flag", build_archive(Z, "solid", dict_size=1 << 20, declared_dict=512 << 20), Z, args=["--max-dict-mib", "128"], note="the same pack under a 128 MiB allowance")
    # damaged
    save("bad_crc_stored", build_archive(T, "solid", crc_lie=True), T, expect="err:CRC")
    save("corrupt_stream", build_archive(T, "solid", corrupt=lambda p: p[:len(p) // 2] + bytes(b ^ 0x5a for b in p[len(p) // 2:len(p) // 2 + 64]) + p[len(p) // 2 + 64:]), T, expect="err:DATA|CRC")
    save("truncated_pack", build_archive(T, "solid", corrupt=lambda p: p[:len(p) // 2]), T, expect="err:EOF|DATA")
    save("truncated_tail", a[:-100], T, expect="err:EOF|HEADER_BAD|NOT_7Z")
    save("truncated_head16", a[:16], T, expect="err:NOT_7Z")
    save("garbage", b"PK\x03\x04" + os.urandom(2000), T, expect="err:NOT_7Z")
    save("header_size_bomb", sig_with(a, next_size=100 << 20), T, expect="err:HEADER_TOO_BIG")
    save("header_offset_outside", sig_with(a, next_off=len(a) + 5), T, expect="err:EOF")
    save("header_offset_wraps", sig_with(a, next_off=(1 << 64) - 8, next_size=100), T, expect="err:EOF")
    save("header_unpack_bomb_8gib", build_archive(T, "solid", hdr_unpack_lie=8 << 30), T, expect="err:NO_MEMORY")
    save("folder_unpack_claims_1tb", build_archive(T, "solid", unpack_lie=10**12), T, expect="err:TOO_BIG|ENTRY_TOO_BIG|EOF|DATA|NO_MEMORY|HEADER_BAD")
    save("unsupported_bcj_chain", build_archive(T, "solid", coder="bcj_lzma2"), T, expect="err:UNSUPPORTED")
    save("unsupported_deflate", build_archive(T, "solid", coder="deflate_fake"), T, expect="err:UNSUPPORTED")
    save("unsupported_lzma_lc_lp", build_archive(T, "solid", coder="lzma", lzma1_props=bytes([(0 * 5 + 2) * 9 + 3]) + struct.pack("<I", 1 << 20)), T, expect="err:UNSUPPORTED", note="lc 3 + lp 2")
    save("encrypted_aes", build_archive(T, "solid", coder="aes_fake"), T, expect="err:ENCRYPTED")
    save("no_crc_wanted", build_archive(T, "solid", no_crc=True), T, expect="err:NO_CRC", note="7-Zip always writes CRCs; an archive without them can't be vouched for")
    # sizes
    save("entry_over_limit", build_archive(D, "solid"), D, expect="err:ENTRY_TOO_BIG", args=["--max-entry-mib", "1"])
    save("entry_over_limit_but_skipped", build_archive(D, "solid"), D, args=["--max-entry-mib", "1", "--skip", "x.dds"], skip=lambda n: n.endswith("x.dds"),
         note="the big file isn't wanted, so its size doesn't matter")
    save("total_over_limit", build_archive(Z, "solid", dict_size=1 << 20, declared_dict=2 << 20), Z, expect="err:TOO_BIG", args=["--max-total-mib", "50"], note="100 MiB of zeros against a 50 MiB allowance")
    # names
    N = [entry("ok/one.dds", blob(1, 1000)), entry("../evil.dds", b"x"), entry("/abs/evil.dds", b"y"), entry("a/../../b.dds", b"z"), entry("w\\in.dds", b"w"),
         entry("ctl\x01.dds", b"c"), entry("ok/two.dds", blob(2, 2000)), entry("lone\ud800surrogate.dds", b"s"), entry("L" * 1500 + ".dds", b"long"),
         entry("ok/three.dds", blob(3, 3000)),
         entry("link", b"/etc/passwd", attr=0x8000 | 0x20 | ((0o120777) << 16)), entry("reparse", b"r", attr=0x400 | 0x20), entry("ok/plain-unix.dds", b"u", attr=0x8000 | 0x20 | ((0o100644) << 16))]
    save("names_hostile", build_archive(N, "solid"), N,
         skip=lambda n: n in ("../evil.dds", "/abs/evil.dds", "a/../../b.dds", "w\\in.dds", "ctl\x01.dds", "lone\ud800surrogate.dds", "link", "reparse") or len(n) > 1000,
         note="the driver leaves out unsafe names; links, reparse points and the unreadable names never reach it")
    # counts
    M = [entry(f"M/SLUS-11111/replacements/{i:06d}.dds", blob(i, 8 + i % 40, 0.2)) for i in range(70000)]
    save("many_files_70k_over_default", build_archive(M, "solid"), M, expect="err:TOO_MANY_FILES")
    save("many_files_70k_allowed", build_archive(M, "solid"), M, args=["--max-files", "100000"], note="the default allows 65,536")
    save("many_files_over_limit", build_archive(M[:6000], "solid"), M[:6000], expect="err:TOO_MANY_FILES", args=["--max-files", "5000"])
    # 65,536 entries with 100-character names (a header of about 16 MB decoded): must fit the 48 MiB header allowance
    L = [entry("L/SLUS-11112/replacements/" + ("p%05d" % i).ljust(70, "n") + ".dds", b"x" * (i % 5)) for i in range(65536 - 3)]
    save("many_files_65536_long_names", build_archive(L, "solid"), L, note="the biggest file count the default allows, with long names")
    E = [entry("only/empty", dir=True), entry("only/empty/a.dds", b""), entry("only/empty/b.dds", b"")]
    save("only_empty_entries", build_archive(E, "solid"), E)
    save("header_budget_exceeded", build_archive(M, "solid"), M, expect="err:NO_MEMORY|TOO_MANY_FILES", args=["--max-hdr-mib", "2", "--max-files", "100000"])
    save("header_budget_exceeded_plainhdr", build_archive(M[:20000], "solid", encode_header=False), M[:20000], expect="err:NO_MEMORY", args=["--max-hdr-mib", "1", "--max-files", "100000"])
    if big:
        B = [entry(f"Big/SLUS-99999/replacements/{i:03d}.dds", blob(1000 + i, 6 << 20, 0.7)) for i in range(100)]
        save("big_600mib_32mib_dict", build_archive(B, "solid", dict_size=32 << 20), B)


# ---------------------------------------------------------------- the manager's archives (texpacks_test.cpp)
def texpacks(out):
    os.makedirs(out, exist_ok=True)

    def save(name, archive, entries, under="replacements/"):
        open(os.path.join(out, name), "wb").write(archive)
        last = {}  # a later entry of one name wins, as in the pack
        for e in entries:
            if not e["dir"] and "/replacements/" in "/" + e["name"]:
                last[e["name"].split("/replacements/", 1)[1]] = e["data"] or b""
        with open(os.path.join(out, name + ".manifest"), "w") as m:
            for rel, data in last.items():
                m.write("%s %d %s\n" % (rel, len(data), hashlib.md5(data).hexdigest()))
        print(name, len(archive))

    # a Bl4ckH4nd-style pack: <Title>/<serial>/replacements/..., a dumps/ and a readme beside it
    serial = "SLUS-20776"
    G = [entry("Spider Pack", dir=True), entry("Spider Pack/" + serial, dir=True), entry("Spider Pack/" + serial + "/replacements", dir=True)]
    G.append(entry(f"Spider Pack/{serial}/replacements/a.dds", blob(1, 3 * 1024 * 1024, 0.4)))
    G.append(entry(f"Spider Pack/{serial}/replacements/01/b.png", b"the second texture"))
    G.append(entry(f"Spider Pack/{serial}/dumps/c.png", b"a dump"))
    G.append(entry(f"Spider Pack/{serial}/replacements.pak", b"not a pack: the pack file's place is the unpacker's"))
    G.append(entry(f"Spider Pack/{serial}/replacements/01/b.png", b"the second texture, again"))
    G.append(entry(f"Spider Pack/{serial}/replacements/empty.dds", b""))
    G.append(entry("readme.txt", b"thanks"))
    save("sz-good.7z", build_archive(G, "solid", dict_size=1 << 20, declared_dict=16 << 20), G)
    # the same tree as a zip: the manager must give the same pack file from either reader (byte for byte)
    import zipfile
    with zipfile.ZipFile(os.path.join(out, "sz-good.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        for e in G:
            if not e["dir"]:
                z.writestr(e["name"], e["data"] or b"")
    # hostile names beside one good file: nothing may land outside the textures folder, and only the good file is installed
    H = [entry(f"Evil/{serial}/replacements/ok.dds", b"the only good one"), entry(f"Evil/{serial}/replacements/../../../escape.dds", b"x"),
         entry("../../escape2.dds", b"y"), entry(f"/abs/{serial}/replacements/abs.dds", b"z"), entry(f"Evil/{serial}/replacements/sub\\back.dds", b"w"),
         entry(f"Evil/{serial}/replacements/ctl\x01.dds", b"c"), entry(f"Evil/{serial}/replacements.pak", b"not a pack"),
         entry(f"Evil/{serial}/replacements/link.dds", b"/etc/passwd", attr=0x8000 | 0x20 | ((0o120777) << 16))]
    save("sz-hostile.7z", build_archive(H, "solid"), H[:1])
    # a loose file inside a kept folder (a pack's own extra file) next to the pack's textures
    LO = [entry(f"Loose Pack/{serial}/replacements/x.png", b"a texture"), entry(f"Loose Pack/{serial}/notes/n.txt", b"a note beside the replacements"),
          entry(f"Loose Pack/{serial}/notes/huge.bin", bytes(9 << 20)), entry(f"Loose Pack/{serial}/notes/small.bin", blob(5, 40000))]
    save("sz-loose.7z", build_archive(LO, "solid"), LO)
    with zipfile.ZipFile(os.path.join(out, "zip-loose.zip"), "w", zipfile.ZIP_DEFLATED) as z:  # the same tree for the libarchive path's loose-file cap
        for e in LO:
            z.writestr(e["name"], e["data"] or b"")
    # two discs
    TWO = [entry("Two Discs/SLUS-21180/replacements/d1.png", b"disc one"), entry("Two Discs/SLUS-21362/replacements/d2.png", b"disc two")]
    save("sz-two-discs.7z", build_archive(TWO, "solid"), TWO)
    # entries across the pack writer's 8 MiB buffers, written as 7-Zip -mmt writes them
    big = [("a8.bin", 8 << 20), ("b8p1.bin", (8 << 20) + 1), ("c20.bin", 20 << 20), ("d1.bin", 1), ("e0.bin", 0), ("f3a.bin", 3 << 20), ("f3b.bin", 3 << 20), ("f3c.bin", 3 << 20)]
    B = [entry("Big/SLUS-77777/replacements/" + n, blob(zlib.crc32(n.encode()) & 0xffff, size, 0.3) if size else b"") for n, size in big]
    save("sz-big-mt.7z", build_archive(B, "solid", dict_size=1 << 20, mt=True), B)
    # a nonsolid pack, a pack in a stored (Copy) folder
    NS = [entry(f"NS/SLUS-33334/replacements/{i}.png", blob(40 + i, 5000 + i)) for i in range(25)]
    save("sz-nonsolid.7z", build_archive(NS, "nonsolid"), NS)
    # damaged (a flipped byte in the middle of the stream: CRC/data error), no textures for the serial, and the refusals
    save("sz-damaged.7z", build_archive(G, "solid", corrupt=lambda p: p[:len(p) // 2] + bytes(b ^ 0x5a for b in p[len(p) // 2:len(p) // 2 + 64]) + p[len(p) // 2 + 64:]), G)
    NO = [entry("Other/readme.txt", b"hello"), entry("Other/pics/a.png", b"a")]
    save("sz-nothing.7z", build_archive(NO, "solid"), NO)
    # textures for another game than the one the list says (a typo in the sheet the list was made from)
    WS = [entry("Typo Pack/SLUS-21752/replacements/a.png", b"for the real game"), entry("Typo Pack/SLUS-21752/replacements/b.png", b"also")]
    save("sz-wrongserial.7z", build_archive(WS, "solid"), WS)
    # (review) a folder of the game that holds no texture: only a readme and a note (nothing for the game to load: said so, remembered), and a pack whose textures are
    # all in another game's folder while this game's folder holds only a readme (refused as for the wrong game; the first rule took the readme's folder for the game's)
    LOS = [entry("Loose Only/SLUS-30004/notes/n.txt", b"a note"), entry("Loose Only/SLUS-30004/readme.txt", b"thanks")]
    save("sz-looseonly.7z", build_archive(LOS, "solid"), LOS)
    OT = [entry("Other Tex/SLUS-30005/readme.txt", b"a readme"), entry("Other Tex/SLUS-30006/replacements/a.png", b"for the neighbour")]
    save("sz-othertex.7z", build_archive(OT, "solid"), OT)
    BIGDICT = [entry(f"BD/SLUS-33335/replacements/z.dds", bytes(70 << 20))]
    save("sz-bigdict.7z", build_archive(BIGDICT, "solid", dict_size=1 << 20, declared_dict=128 << 20), BIGDICT)
    save("sz-bcj.7z", build_archive(G, "solid", coder="bcj_lzma2"), G)
    save("sz-aes.7z", build_archive(G, "solid", coder="aes_fake"), G)
    save("sz-nocrc.7z", build_archive(G, "solid", no_crc=True), G)


def seeds(out):
    os.makedirs(out, exist_ok=True)
    n = 0
    for coder in ("lzma2", "lzma", "copy"):
        for mode in ("solid", "nonsolid", "blocks"):
            for enc in (False, True):
                for crc_mode in (False, True):
                    ents = [entry("G", dir=True), entry("G/SLUS-1", dir=True), entry("G/SLUS-1/replacements", dir=True)]
                    for i in range(8):
                        ents.append(entry(f"G/SLUS-1/replacements/f{i:02d}.dds", blob(i + n, 0 if i == 3 else 40 + i * 31)))
                    ents.append(entry("G/SLUS-1/dumps/x.bin", b"dump"))
                    a = build_archive(ents, mode, coder=coder, dict_size=1 << 16, encode_header=enc, block_bytes=200, folder_crc=crc_mode)
                    open(os.path.join(out, f"seed{n:03d}.7z"), "wb").write(a)
                    n += 1
    ents = [entry(f"M/SLUS-1/replacements/{i}.dds", blob(100 + i, 5000)) for i in range(6)]  # 7-Zip multithreaded stream shape
    open(os.path.join(out, f"seed{n:03d}.7z"), "wb").write(build_archive(ents, "solid", dict_size=1 << 16, mt=True))
    n += 1
    E = [entry("only/empty", dir=True), entry("only/empty/a.dds", b""), entry("only/empty/b.dds", b"")]
    open(os.path.join(out, f"seed{n:03d}.7z"), "wb").write(build_archive(E, "solid"))
    n += 1
    print(n, "seeds")


def write_one(path, nfiles, mib, dict_mib, comp=0.7):
    B = [entry(f"Big/SLUS-99999/replacements/{i:03d}.dds", blob(1000 + i, mib << 20, comp)) for i in range(nfiles)]
    a = build_archive(B, "solid", dict_size=dict_mib << 20)
    open(path, "wb").write(a)
    print(path, len(a), "bytes;", nfiles * mib, "MiB unpacked")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "corpus":
        corpus(sys.argv[2], "--big" in sys.argv)
    elif cmd == "texpacks":
        texpacks(sys.argv[2])
    elif cmd == "seeds":
        seeds(sys.argv[2])
    elif cmd == "write":
        write_one(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), float(sys.argv[6]) if len(sys.argv) > 6 else 0.7)
    else:
        print(__doc__)
        sys.exit(2)
