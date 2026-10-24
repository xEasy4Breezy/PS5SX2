#!/usr/bin/env python3
"""PS5 port frontend: 7z archives made with the LZMA SDK's own encoder, read by the 7z reader (2026-10-16, review, AI-assisted).

make_7z_fixtures.py makes its streams with liblzma. 7-Zip makes its LZMA2 and LZMA streams with the SDK's Lzma2Enc.c and LzmaEnc.c (host/sdk_encode.c is the tool that
runs them): with several threads LZMA2 is cut into independent blocks that each start with a dictionary reset, incompressible stretches go into uncompressed chunks,
and the chunk and reset patterns are the encoder's own. This wraps those raw streams in the container writer of make_7z_fixtures.py (whose headers match two real
7-Zip headers) and checks that the reader gives back every file exactly: solid, non-solid and in blocks, 1 to 64 MiB dictionaries, 1 to 8 MiB LZMA2 blocks, one to
four encoder threads, LZMA1, only-uncompressed data. Then it flips bits, edits chunk headers, splices, duplicates, deletes and cuts the PACKED streams (--fuzz N times),
and the reader must answer every time without a sanitizer report, a hang or a crash.

  sdk_corpus_check.py <driver with sanitizers (sevenzip_test)> <sdk_encode> <work dir> [--fuzz N] [--seed S] [--big]

--big adds one archive of 180 MiB with a 64 MiB dictionary (the largest the app takes). test-sevenzip.sh builds the tools and calls this.

Copyright (C) 2026 swordpdf
SPDX-License-Identifier: GPL-3.0-or-later
"""
import collections, json, os, random, struct, subprocess, sys, tempfile

args = sys.argv[1:]
drv, enc, work = args[0], args[1], args[2]
FUZZ = int(args[args.index("--fuzz") + 1]) if "--fuzz" in args else 300
SEED = int(args[args.index("--seed") + 1]) if "--seed" in args else 1
BIG = "--big" in args
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_7z_fixtures as mk  # noqa: E402

os.makedirs(work, exist_ok=True)
ENV = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:allocator_may_return_null=1", UBSAN_OPTIONS="print_stacktrace=1")
MiB = 1 << 20
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL", what, flush=True)
    return cond


def sanitizer_report(err):
    return "ERROR: AddressSanitizer" in err or "runtime error" in err or "LeakSanitizer" in err


def encode(data, kind, dict_size, level, threads, block):
    """-> (raw stream, the LZMA properties (5 bytes) for kind 'lzma')"""
    tmp = tempfile.mkdtemp(prefix="sdk.", dir=work)
    i, o = os.path.join(tmp, "in"), os.path.join(tmp, "out")
    open(i, "wb").write(data)
    r = subprocess.run([enc, i, o, kind, str(max(dict_size, 4096)), str(level), str(threads), str(block)], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("sdk_encode failed: %s %s" % (r.returncode, r.stderr))
    props = None
    for l in r.stdout.splitlines():
        if l.startswith("PROPS"):
            props = bytes.fromhex(l.split()[1])
    stream = open(o, "rb").read()
    os.unlink(i)
    os.unlink(o)
    os.rmdir(tmp)
    return stream, props


CONF = {"level": 5, "threads": 4, "block": "0"}


def sdk_compress(coder, data, dict_size, declared_dict, mt=False, lzma1_props=None):
    """make_7z_fixtures.compress, with the SDK's encoder behind it"""
    if coder == "copy":
        return data, b"\x00", b""
    dd = declared_dict if declared_dict is not None else dict_size
    kind = "lzma2" if coder == "lzma2" else "lzma"
    stream, props = encode(data, kind, dict_size, CONF["level"], CONF["threads"], CONF["block"])
    if kind == "lzma2":
        return stream, b"\x21", bytes([mk.lzma2_prop(dd)])
    return stream, b"\x03\x01\x01", props


def mixed(seed, n):
    """texture-like data: repeats of earlier parts, zero runs, noise (stored in uncompressed chunks) and headers"""
    r = random.Random(seed)
    b = bytearray()
    while len(b) < n:
        k = r.random()
        if k < 0.35 and b:
            l = min(r.randrange(8, 20000), n - len(b))
            start = max(0, len(b) - r.randrange(32, 3_000_000))
            b += b[start:start + l]
        elif k < 0.5:
            b += bytes(min(r.randrange(100, 70000), n - len(b)))
        elif k < 0.75:
            b += r.randbytes(min(r.randrange(100, 90000), n - len(b)))
        else:
            b += (b"DDS |\x7c\x00\x00\x00 texture header " + r.randbytes(4)) * min(r.randrange(2, 400), max(1, (n - len(b)) // 40))
    return bytes(b[:n])


def tree(serial, count, seed, sizes):
    es = [mk.entry("Pack", dir=True), mk.entry("Pack/" + serial, dir=True), mk.entry("Pack/" + serial + "/replacements", dir=True)]
    r = random.Random(seed)
    for i in range(count):
        es.append(mk.entry("Pack/%s/replacements/%016x-%016x-%08x.dds" % (serial, r.getrandbits(64), r.getrandbits(64), i), mixed(seed * 1000 + i, r.choice(sizes))))
    es.append(mk.entry("Pack/" + serial + "/dumps/readme.txt", b"a dump"))
    es.append(mk.entry("Pack/" + serial + "/replacements/empty.dds", b""))
    return es


def run_driver(path, extra=(), timeout=300):
    p = subprocess.run([drv, path, *extra], capture_output=True, env=ENV, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "surrogateescape"), p.stderr.decode("utf-8", "replace")


def parse(out):
    files, status = [], None
    for l in out.splitlines():
        if l.startswith("F "):
            _, size, c, nm = l.split(" ", 3)
            files.append([nm, int(size), c])
        elif l.startswith("ERR "):
            status = "err:" + l.split()[1]
        elif l.startswith("OK "):
            status = "ok"
    return status, files


# ---------------------------------------------------------------- 1. archives from the SDK's encoder: every file back, exactly
mk.compress = sdk_compress
out_dir = os.path.join(work, "corpus")
os.makedirs(out_dir, exist_ok=True)
print("---- SDK-encoded archives")
T1 = tree("SLUS-20776", 50, 1, [1, 100, 3000, 70000, 300000, 1 * MiB])
T2 = tree("SLES-50974", 60, 2, [5000, 90000, 400000, 2 * MiB])
cases = []  # (name, config, builder)
for dict_mib, blk, lvl, thr in [(1, 1 * MiB, 5, 4), (4, 1 * MiB, 5, 4), (16, 4 * MiB, 5, 4), (16, 0, 5, 4), (32, 8 * MiB, 7, 4), (2, 1 * MiB, 1, 4), (16, "solid", 5, 1),
                                (1, 1 * MiB, 9, 2)]:
    nm = "lzma2_d%d_b%s_l%d_t%d" % (dict_mib, blk if blk in (0, "solid") else "%dm" % (blk // MiB), lvl, thr)
    cases.append((nm, dict(level=lvl, threads=thr, block=blk), T1, lambda e, d=dict_mib: mk.build_archive(e, "solid", dict_size=d * MiB)))
cases += [
    ("nonsolid", dict(level=5, threads=4, block=1 * MiB), T1, lambda e: mk.build_archive(e, "nonsolid", dict_size=1 * MiB)),
    ("blocks_400k", dict(level=5, threads=4, block=1 * MiB), T1, lambda e: mk.build_archive(e, "blocks", dict_size=4 * MiB, block_bytes=400000)),
    ("folder_crc_solid", dict(level=5, threads=4, block=1 * MiB), T2, lambda e: mk.build_archive(e, "solid", dict_size=4 * MiB, folder_crc=True)),
    ("lzma1_solid", dict(level=5, threads=2, block=0), T1, lambda e: mk.build_archive(e, "solid", coder="lzma", dict_size=4 * MiB)),
    ("lzma1_plainhdr", dict(level=5, threads=2, block=0), T1, lambda e: mk.build_archive(e, "solid", coder="lzma", dict_size=1 * MiB, encode_header=False)),
]
W = [mk.entry("A/SLES-50974/replacements/big.dds", mixed(7, 24 * MiB)), mk.entry("A/SLES-50974/replacements/after.dds", mixed(8, 3 * MiB))]
cases.append(("wrap_1mib_dict", dict(level=5, threads=4, block=1 * MiB), W, lambda e: mk.build_archive(e, "solid", dict_size=1 * MiB)))
R = [mk.entry("R/SLUS-12345/replacements/%04d.dds" % i, os.urandom(300000 + i * 1111)) for i in range(30)]
cases.append(("incompressible", dict(level=5, threads=4, block=1 * MiB), R, lambda e: mk.build_archive(e, "solid", dict_size=1 * MiB)))
if BIG:
    B = [mk.entry("B/SLUS-55555/replacements/%03d.dds" % i, mixed(900 + i, 3 * MiB)) for i in range(60)]
    cases.append(("dict64_180mib", dict(level=5, threads=4, block=16 * MiB), B, lambda e: mk.build_archive(e, "solid", dict_size=64 * MiB)))

stream_checked = False
for name, conf, ents, build in cases:
    CONF.update(conf)
    a = build(ents)
    path = os.path.join(out_dir, name + ".7z")
    open(path, "wb").write(a)
    rc, so, se = run_driver(path)
    status, files = parse(so)
    want = mk.manifest(ents)
    ok = (not sanitizer_report(se)) and status == "ok" and files == want
    check(ok, "SDK-encoded " + name + ": " + str(status) + (" " + se.strip().splitlines()[0][:100] if se.strip() else ""))
    print(("PASS " if ok else "FAIL ") + "%-28s %9d bytes" % (name, len(a)), flush=True)
    if name == "lzma2_d1_b1m_l5_t4":
        # the point of this corpus: the stream has independent blocks (dictionary resets in its middle) and uncompressed chunks, like 7-Zip's multithreaded LZMA2
        p = a[32:]
        i, resets, stored = 0, 0, 0
        while i < len(p):
            ctl = p[i]
            if ctl == 0:
                break
            if ctl < 0x80:
                resets += ctl == 1
                stored += 1
                i += 3 + struct.unpack(">H", p[i + 1:i + 3])[0] + 1
            else:
                resets += ((ctl >> 5) & 3) == 3
                i += (6 if ((ctl >> 5) & 3) >= 2 else 5) + struct.unpack(">H", p[i + 3:i + 5])[0] + 1
        print("  the stream of %s: %d dictionary resets, %d uncompressed chunks" % (name, resets, stored))
        check(resets >= 8 and stored >= 8, "the SDK's multithreaded LZMA2 stream has independent blocks and uncompressed chunks")
        stream_checked = True
check(stream_checked, "the multithreaded stream was looked at")

# ---------------------------------------------------------------- 2. the PACKED streams damaged
print("---- damaged streams (%d runs, seed %d)" % (FUZZ, SEED))
if FUZZ:
    rng = random.Random(SEED)

    def chunk_offsets(p, kind):
        offs, i = [], 0
        if kind != "lzma2":
            return offs
        while i < len(p):
            ctl = p[i]
            offs.append(i)
            if ctl == 0:
                break
            if ctl < 0x80:
                if ctl > 2 or i + 3 > len(p):
                    break
                i += 3 + struct.unpack(">H", p[i + 1:i + 3])[0] + 1
            else:
                if i + 5 > len(p):
                    break
                i += (6 if ((ctl >> 5) & 3) >= 2 else 5) + struct.unpack(">H", p[i + 3:i + 5])[0] + 1
        return offs

    def mutate(p, kind):
        b = bytearray(p)
        offs = chunk_offsets(bytes(p), kind)
        for _ in range(rng.choice([1, 1, 2, 3, 6])):
            if not b:
                break
            op = rng.random()
            if op < 0.30:
                b[rng.randrange(len(b))] ^= 1 << rng.randrange(8)
            elif op < 0.42:
                b[rng.randrange(len(b))] = rng.choice([0, 1, 2, 0x7f, 0x80, 0xa0, 0xc0, 0xe0, 0xff])
            elif op < 0.65 and offs and max(offs) + 6 < len(b):
                o = rng.choice(offs)
                f = rng.random()
                if f < 0.35:
                    b[o] = rng.choice([0, 1, 2, 3, 0x80, 0x9f, 0xa0, 0xc0, 0xe0, 0xff, rng.randrange(256)])
                elif f < 0.7:
                    b[o + 1 + rng.randrange(2)] = rng.randrange(256)
                else:
                    b[o + 3 + rng.randrange(2)] = rng.randrange(256)
            elif op < 0.75 and len(offs) > 2 and max(offs) < len(b):
                k = rng.randrange(len(offs) - 1)
                a0, z0 = offs[k], offs[k + 1]
                if rng.random() < 0.5:
                    del b[a0:z0]
                else:
                    b[a0:a0] = b[a0:z0]
            elif op < 0.85:
                p0 = rng.randrange(len(b))
                del b[p0:p0 + rng.randrange(1, 30)]
            elif op < 0.93:
                p0 = rng.randrange(len(b))
                b[p0:p0] = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 12)))
            else:
                b = b[:rng.randrange(1, len(b) + 1)]
        return bytes(b)

    bases = []
    for k, (kind, d, lvl, thr, blk, mode) in enumerate([("lzma2", 1 * MiB, 5, 4, 1 * MiB, "solid"), ("lzma2", 4 * MiB, 5, 4, 1 * MiB, "solid"), ("lzma2", 64 << 10, 1, 4, 1 * MiB, "solid"),
                                                       ("lzma2", 1 * MiB, 5, 1, "solid", "solid"), ("lzma2", 1 * MiB, 5, 4, 1 * MiB, "nonsolid"), ("lzma", 1 * MiB, 5, 2, 0, "solid"),
                                                       ("lzma2", 2 * MiB, 5, 4, 1 * MiB, "blocks")]):
        ents = tree("SLUS-%05d" % (10000 + k), 14, k + 1, [200, 3000, 40000, 150000, 400000])
        files = [e for e in ents if not e["dir"] and e["data"]]
        if mode == "solid":
            groups = [files]
        elif mode == "nonsolid":
            groups = [[e] for e in files]
        else:
            groups, cur, size = [], [], 0
            for e in files:
                cur.append(e)
                size += len(e["data"])
                if size >= 400000:
                    groups.append(cur)
                    cur, size = [], 0
            if cur:
                groups.append(cur)
        streams, props = [], None
        for g in groups:
            s, props = encode(b"".join(e["data"] for e in g), kind, d, lvl, thr, blk)
            streams.append(s)
        bases.append((ents, kind, d, mode, streams, props))

    hist, bad = collections.Counter(), []
    path = os.path.join(work, "damaged.7z")
    for it in range(FUZZ):
        ents, kind, d, mode, streams, props = rng.choice(bases)
        which = rng.randrange(len(streams))
        mutated = list(streams)
        mutated[which] = mutate(streams[which], kind)
        state = {"i": 0}

        def fake(coder, data, dict_size, declared_dict, mt=False, lzma1_props=None, mutated=mutated, state=state, kind=kind, props=props, d=d):
            s = mutated[state["i"]]
            state["i"] += 1
            return (s, b"\x21", bytes([mk.lzma2_prop(d)])) if coder == "lzma2" else (s, b"\x03\x01\x01", props)

        mk.compress = fake
        a = mk.build_archive(ents, mode, coder=("lzma2" if kind == "lzma2" else "lzma"), dict_size=d, block_bytes=400000, encode_header=rng.random() < 0.7)
        if rng.random() < 0.15:
            a = a[:len(a) - rng.randrange(1, 60)]
        open(path, "wb").write(a)
        rc, so, se = run_driver(path, ["--quiet", "--max-dict-mib", "64"], timeout=120)
        status = parse(so)[0]
        if sanitizer_report(se) or rc not in (0, 2) or status is None:
            bad.append((it, kind, mode, (se.strip().splitlines() or ["exit %d" % rc])[0][:160]))
            os.rename(path, os.path.join(work, "bad_%d.7z" % it))
            continue
        hist[(kind, mode, status)] += 1
    print("  %d runs: %d bad" % (FUZZ, len(bad)))
    for k, v in sorted(hist.items()):
        print("   ", k, v)
    for b in bad[:10]:
        print("  BAD", b)
    check(not bad, "damaged SDK-encoded streams: no sanitizer report, hang or crash")

print("SDK ENCODER CHECK: %d failure(s)" % failures if failures else "SDK ENCODER CHECK PASS")
sys.exit(1 if failures else 0)
