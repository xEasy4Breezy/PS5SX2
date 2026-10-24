#!/usr/bin/env python3
"""PS5 port frontend: runs the 7z reader's driver (sevenzip_test) over a corpus, a mutation fuzz and the memory-limited runs (2026-10-16, AI-assisted).
test-sevenzip.sh builds the two drivers (sanitizers on / off) and calls this.

  sevenzip_check.py <driver with sanitizers> <driver without> <work dir> [--fuzz N] [--seed S] [--big] [--only NAME ...]

  1. the corpus (make_7z_fixtures.py corpus): every archive's result and file list against its json, under ASAN + UBSAN + LSAN
  2. cancel, a consumer that fails, a file left out
  3. memory: peak RSS of a 64 MiB-dictionary run, a run under an address-space limit, ASAN's max_allocation_size_mb as a stand-in for the console's budget
  4. the mutation fuzz (headers mutated BEFORE they are encoded with their CRCs fixed, packed data flipped, tails cut): no sanitizer report, no hang, no crash
  5. SZ7_REAL_PROBES=<dir>: the real 7-Zip headers of probed Drive packs (<label>.sig/.end/.packed/.json) as sparse files: the header must open and the files list

Copyright (C) 2026 swordpdf
SPDX-License-Identifier: GPL-3.0-or-later
"""
import collections, glob, json, os, random, resource, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_7z_fixtures as mk  # noqa: E402

args = sys.argv[1:]
san, plain, work = args[0], args[1], args[2]
FUZZ = int(args[args.index("--fuzz") + 1]) if "--fuzz" in args else 600
SEED = int(args[args.index("--seed") + 1]) if "--seed" in args else 1
BIG = "--big" in args
ONLY = set(args[args.index("--only") + 1:]) if "--only" in args else set()
os.makedirs(work, exist_ok=True)
ENV = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:allocator_may_return_null=1", UBSAN_OPTIONS="print_stacktrace=1")
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL", what)
    return cond


def run(exe, path, extra=(), env=ENV, timeout=600, limit_as=None, rss=False):
    """-> (exit status or None on a timeout, stdout, stderr, peak RSS of this child in KiB (rss=True: through /usr/bin/time), seconds)"""
    pre = None
    if limit_as:
        pre = lambda: resource.setrlimit(resource.RLIMIT_AS, (limit_as, limit_as))  # noqa: E731
    cmd = [exe, path, *extra]
    rss_file = None
    if rss:
        rss_file = tempfile.mktemp(prefix="rss.", dir=work)
        cmd = ["/usr/bin/time", "-f", "%M", "-o", rss_file] + cmd
    t0 = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, preexec_fn=pre)
    try:
        out, err = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
        return None, out.decode("utf-8", "surrogateescape"), "TIMEOUT", 0, time.time() - t0
    peak = 0
    if rss_file and os.path.exists(rss_file):
        try:
            peak = int(open(rss_file).read().split()[-1])
        except (ValueError, IndexError):
            peak = 0
        os.unlink(rss_file)
    return p.returncode, out.decode("utf-8", "surrogateescape"), err.decode("utf-8", "replace"), peak, time.time() - t0


def parse(out):
    files, status, info = [], None, ""
    for l in out.splitlines():
        if l.startswith("F "):
            _, size, c, nm = l.split(" ", 3)
            files.append([nm, int(size), c])
        elif l.startswith("ERR "):
            status = "err:" + l.split()[1]
        elif l.startswith("OK "):
            status = "ok"
        elif l.startswith("INFO "):
            info = l
    return status, files, info


def sanitizer_report(err):
    return "ERROR: AddressSanitizer" in err or "runtime error" in err or "LeakSanitizer" in err or "ThreadSanitizer" in err


# ---------------------------------------------------------------- 1. the corpus
corp = os.path.join(work, "corpus")
mk.corpus(corp, BIG)
print("---- corpus")
total = bad = 0
for jp in sorted(glob.glob(os.path.join(corp, "*.json"))):
    name = os.path.basename(jp)[:-5]
    if ONLY and name not in ONLY:
        continue
    meta = json.load(open(jp, encoding="utf-8"))
    total += 1
    rc, out, err, _, dt = run(san, os.path.join(corp, name + ".7z"), meta["args"])
    status, files, info = parse(out)
    ok, why = False, ""
    if sanitizer_report(err) or rc is None:
        why = "SANITIZER/TIMEOUT: " + err.strip().splitlines()[0][:150] if err.strip() else "timeout"
    elif meta["expect"] == "ok":
        ok = status == "ok" and files == meta["files"]
        why = f"status {status}" if status != "ok" else ("files differ" if not ok else "")
        if status == "ok" and not ok:
            exp, got = {tuple(f) for f in meta["files"]}, {tuple(f) for f in files}
            why += f" (missing {len(exp - got)}, extra {len(got - exp)}, same order {files == meta['files']})"
    else:
        alts = meta["expect"][4:].split("|")
        ok = status is not None and status.startswith("err:") and status[4:] in alts
        why = "" if ok else f"expected one of {alts}, got {status}"
    if not ok:
        bad += 1
    print(("PASS " if ok else "FAIL ") + f"{name:32s} {dt:6.2f}s  {why}")
check(bad == 0, f"{bad} of {total} corpus archives failed")

# ---------------------------------------------------------------- 2. cancel, a failing consumer, a file left out
print("---- consumer")
wrap = os.path.join(corp, "wrap_1mib_dict.7z")
if os.path.exists(wrap) and not ONLY:
    rc, out, err, _, _ = run(san, wrap, ["--quiet", "--cancel-after-bytes", "2000000"])
    check(parse(out)[0] == "err:CANCELLED" and not sanitizer_report(err), "cancel mid-stream: CANCELLED, nothing leaked")
    rc, out, err, _, _ = run(san, wrap, ["--quiet", "--sink-fail-after-bytes", "5000000"])
    check(parse(out)[0] == "err:SINK" and not sanitizer_report(err), "a failing consumer: SINK, nothing leaked")
    rc, out, err, _, _ = run(san, wrap, ["--quiet", "--scan-only"])
    check(rc == 0 and "SCAN kept=2 " in out, "scan only: no decoding, 2 files")

# ---------------------------------------------------------------- 3. memory
print("---- memory")
if not ONLY:
    mem = os.path.join(work, "mem")
    os.makedirs(mem, exist_ok=True)
    # a 64 MiB dictionary on 160 MiB of data (hard to compress: the dictionary buffer is really touched), and a 32 MiB one
    for dict_mib, nfiles, mib in ((64, 20, 8), (32, 12, 8)):
        path = os.path.join(mem, f"d{dict_mib}.7z")
        mk.write_one(path, nfiles, mib, dict_mib, 0.2)
        rc, out, err, rss_kb, dt = run(plain, path, ["--quiet"], rss=True)
        status, _, info = parse(out)
        mib_rss = rss_kb / 1024.0
        # the process itself (libc, the driver) is a few MiB: the dictionary + 1 MiB out + 256 KiB in + the state
        bound = dict_mib + 14
        print(f"  {dict_mib} MiB dictionary, {nfiles * mib} MiB unpacked: {status}, peak RSS {mib_rss:.1f} MiB (bound {bound}), {dt:.1f} s")
        check(rss_kb > 0, "the peak RSS was measured (/usr/bin/time)")
        check(status == "ok" and 0 < mib_rss < bound, f"peak RSS of the {dict_mib} MiB dictionary run")
        rc, out, err, _, _ = run(plain, path, ["--quiet"], limit_as=(dict_mib + 60) << 20)
        check(parse(out)[0] == "ok", f"the {dict_mib} MiB run under an address-space limit of {dict_mib + 60} MiB")
        env = dict(ENV, ASAN_OPTIONS=ENV["ASAN_OPTIONS"] + f":max_allocation_size_mb={dict_mib + 2}")
        rc, out, err, _, _ = run(san, path, ["--quiet"], env=env)
        check(parse(out)[0] == "ok" and not sanitizer_report(err), f"no single allocation above {dict_mib + 2} MiB in the {dict_mib} MiB run")
        os.unlink(path)
    # --big: the ~600 MiB solid block with a 32 MiB dictionary must stay near the dictionary (the SDK's own SzArEx_Extract would allocate the whole block)
    big_path = os.path.join(corp, "big_600mib_32mib_dict.7z")
    if BIG and os.path.exists(big_path):
        rc, out, err, rss_kb, dt = run(plain, big_path, ["--quiet"], rss=True, limit_as=140 << 20)
        status, _, info = parse(out)
        print(f"  600 MiB solid block, 32 MiB dictionary, under an address-space limit of 140 MiB: {status}, peak RSS {rss_kb / 1024.0:.1f} MiB, {dt:.1f} s")
        check(status == "ok" and 0 < rss_kb / 1024.0 < 32 + 14, "peak RSS of the 600 MiB solid block")
    # a header that claims 8 GiB, with ASAN refusing any allocation over 100 MiB: NO_MEMORY, no abort
    env = dict(ENV, ASAN_OPTIONS=ENV["ASAN_OPTIONS"] + ":max_allocation_size_mb=100")
    for name in ("header_unpack_bomb_8gib", "header_budget_exceeded", "many_files_65536_long_names", "dict_over_limit"):
        p = os.path.join(corp, name + ".7z")
        if os.path.exists(p):
            rc, out, err, rss_kb, _ = run(san, p, ["--quiet"] + json.load(open(os.path.join(corp, name + ".json")))["args"], env=env)
            status, _, info = parse(out)
            exp = json.load(open(os.path.join(corp, name + ".json")))["expect"]
            ok = (status == "ok") if exp == "ok" else status in ["err:" + a for a in exp[4:].split("|")]
            check(ok and not sanitizer_report(err), f"{name} with no allocation over 100 MiB: {status}")
    # the header allowance really bounds a big file table: 65,536 entries must report a header peak under 48 MiB
    p = os.path.join(corp, "many_files_65536_long_names.7z")
    if os.path.exists(p):
        rc, out, err, _, _ = run(plain, p, ["--scan-only"])
        status, _, info = parse(out)
        peak = int(info.split("header_peak=")[1].split()[0]) if "header_peak=" in info else 0
        live = int(info.split("header_live=")[1].split()[0]) if "header_live=" in info else 0
        print(f"  65,536 entries with 100-character names: header peak {peak / 1048576:.1f} MiB, kept {live / 1048576:.1f} MiB")
        check(rc == 0 and 0 < peak <= 48 << 20, "the 65,536-entry header fits the 48 MiB allowance")

# ---------------------------------------------------------------- 4. the mutation fuzz
print(f"---- fuzz ({FUZZ} runs, seed {SEED})")
if FUZZ and not ONLY:
    rng = random.Random(SEED)
    ents = [mk.entry("G", dir=True), mk.entry("G/SLUS-1", dir=True), mk.entry("G/SLUS-1/replacements", dir=True)]
    for i in range(24):
        ents.append(mk.entry(f"G/SLUS-1/replacements/f{i:02d}.dds", mk.blob(i, 0 if i % 7 == 3 else rng.randrange(1, 9000))))
    hist = collections.Counter()
    bad_runs = []
    tmp = tempfile.mkdtemp(prefix="fuzz7z.", dir=work)
    path = os.path.join(tmp, "f.7z")
    for it in range(FUZZ):
        mode = rng.choice(["solid", "nonsolid", "blocks"])
        enc = rng.random() < 0.6
        kind = rng.choice(["hdr", "hdr", "hdr", "data", "tail"])

        def hook(h, kind=kind):
            if kind != "hdr":
                return h
            b = bytearray(h)
            for _ in range(rng.choice([1, 1, 2, 3, 6])):
                op = rng.random()
                pos = rng.randrange(len(b))
                if op < 0.6:
                    b[pos] = rng.choice([0, 1, 0x7f, 0x80, 0xff, rng.randrange(256)])
                elif op < 0.8:
                    del b[pos:pos + rng.randrange(1, 6)]
                else:
                    b[pos:pos] = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 6)))
            return bytes(b)

        corrupt = None
        if kind == "data":
            def corrupt(p):
                b = bytearray(p)
                for _ in range(rng.choice([1, 2, 5])):
                    if b:
                        b[rng.randrange(len(b))] ^= 1 << rng.randrange(8)
                return bytes(b)
        a = mk.build_archive(ents, mode, dict_size=1 << 16, encode_header=enc, block_bytes=20000, corrupt=corrupt, header_hook=hook, mt=rng.random() < 0.2)
        if kind == "tail":
            a = a[:len(a) - rng.randrange(1, 40)] if rng.random() < 0.5 else a + os.urandom(rng.randrange(1, 40))
        open(path, "wb").write(a)
        rc, out, err, _, _ = run(san, path, ["--quiet", "--max-dict-mib", "64"], timeout=30)
        status = parse(out)[0]
        if rc is None or sanitizer_report(err) or rc not in (0, 2) or status is None:
            bad_runs.append((it, kind, (err.strip().splitlines() or [f"exit {rc}"])[0][:160]))
            os.rename(path, os.path.join(tmp, f"bad_{it}.7z"))
            continue
        hist[(kind, status)] += 1
    print(f"  {FUZZ} runs, seed {SEED}: {len(bad_runs)} bad")
    for k, v in sorted(hist.items()):
        print("   ", k, v)
    for b in bad_runs[:10]:
        print("  BAD", b)
    check(not bad_runs, "the fuzz found no sanitizer report, hang or crash")

# ---------------------------------------------------------------- 5. real 7-Zip headers (sparse files)
real = os.environ.get("SZ7_REAL_PROBES")
if real and os.path.isdir(real) and not ONLY:
    print("---- real headers", real)
    seen = 0
    for jp in sorted(glob.glob(os.path.join(real, "*.json"))):
        label = os.path.basename(jp)[:-5]
        r = json.load(open(jp, encoding="utf-8"))
        enc = r.get("encoded", {})
        if enc and enc.get("partial_read", 0) < enc.get("pack_size", 0):
            continue  # the probe read only part of this packed header: it can't be opened
        sp = os.path.join(work, label + ".sparse.7z")
        with open(sp, "wb") as f:
            f.truncate(r["total"])
            f.seek(0)
            f.write(open(os.path.join(real, label + ".sig"), "rb").read())
            f.seek(32 + r["sig"]["next_header_offset"])
            f.write(open(os.path.join(real, label + ".end"), "rb").read())
            if enc:
                f.seek(enc["pack_pos_abs"])
                f.write(open(os.path.join(real, label + ".packed"), "rb").read())
        rc, out, err, _, _ = run(san, sp, ["--scan-only", "--skip", "/dumps/"])
        l = [x for x in out.splitlines() if x.startswith("SCAN ")]
        info = [x for x in out.splitlines() if x.startswith("INFO ")]
        print(f"  {label}: exit {rc}; {info[0][:150] if info else out.strip()[:150]}; {l[0] if l else ''}")
        check(rc == 0 and l and not sanitizer_report(err), f"real header {label} opens and lists")
        os.unlink(sp)
        seen += 1
    check(seen > 0, "at least one probed header was complete")

print(f"SEVENZIP CHECK: {failures} failure(s)" if failures else "SEVENZIP CHECK PASS")
sys.exit(1 if failures else 0)
