# PS5SX2 (2.11, AI-assisted): runs the CDVD thread benchmark matrix (bench_cdvdthread.cpp; built by test-cdvdthread.sh) and checks the RELATIVE claims.
#   python3 -I run_bench.py <build dir> <quick 0|1> <parallel jobs>
# The model is a ranking of designs, not a promise: real sleeps, +-30% per run. So each claim has a margin, a run of a pair that decides a claim is
# repeated (the design that must win takes its worst run, the one it must beat its best), and tiny waits get an absolute slack.
# SPDX-License-Identifier: GPL-3.0-or-later
import concurrent.futures
import os
import re
import subprocess
import sys

build_dir, quick, jobs = sys.argv[1], sys.argv[2] == "1", int(sys.argv[3])

SCENARIOS = ["stream", "loader", "two-streams", "stream+hops", "near-two-16", "near-two-64"]
# name -> (binary, speed, windows, switch files)
CONFIGS = {
    "upstream 2.0x": ("bench_ref", "2.0", "off", []),
    "174 2.0x": ("bench_ref", "2.0", "on", []),
    "174 3.2x": ("bench_ref", "3.2", "on", []),
    "new 2.0x": ("bench_new", "2.0", "on", []),
    "new 3.2x": ("bench_new", "3.2", "on", []),
    "new+discpipe 2.0x": ("bench_new", "2.0", "on", ["discpipe"]),
    "new+discpipe 3.2x": ("bench_new", "3.2", "on", ["discpipe"]),
    "pipeline, CPU reads 2.0x": ("bench_new", "2.0", "on", ["discblock", "discpipe"]),
    "pipeline, CPU reads 3.2x": ("bench_new", "3.2", "on", ["discblock", "discpipe"]),
}
QUICK_CONFIGS = ["174 2.0x", "174 3.2x", "new 2.0x", "new 3.2x", "pipeline, CPU reads 3.2x"]
QUICK_SCENARIOS = ["loader", "stream+hops"]
REPEAT = {  # the pairs that decide a claim run twice
    ("174 2.0x", "loader"): 2, ("174 3.2x", "loader"): 2, ("new 2.0x", "loader"): 2, ("new 3.2x", "loader"): 2,
    ("174 2.0x", "stream+hops"): 2, ("174 3.2x", "stream+hops"): 2, ("new 2.0x", "stream+hops"): 2, ("new 3.2x", "stream+hops"): 2,
    ("new 3.2x", "near-two-64"): 2, ("new+discpipe 3.2x", "near-two-64"): 2,
}

configs = QUICK_CONFIGS if quick else list(CONFIGS)
scenarios = QUICK_SCENARIOS if quick else SCENARIOS

tasks = []
for c in configs:
    for s in scenarios:
        for r in range(REPEAT.get((c, s), 1)):
            tasks.append((c, s, r))
# longest first
tasks.sort(key=lambda t: (t[1] != "loader", t[1] != "stream+hops", t[1] != "stream"))


def run(task):
    c, s, r = task
    exe, speed, windows, flags = CONFIGS[c]
    out = subprocess.run([os.path.join(build_dir, exe), str(SCENARIOS.index(s)), speed, windows] + flags, capture_output=True, text=True, timeout=300).stdout
    m = re.search(r"^RESULT (.*)$", out, re.M)
    if not m:
        raise RuntimeError("no RESULT from %s %s: %r" % (c, s, out[-300:]))
    d = dict(kv.split("=", 1) for kv in m.group(1).split())
    return task, {k: (float(v) if re.fullmatch(r"-?[0-9.]+", v) else v) for k, v in d.items()}


results = {}
with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
    for task, res in pool.map(run, tasks):
        results.setdefault((task[0], task[1]), []).append(res)


def waits(c, s):
    return [r["wait_ms"] for r in results[(c, s)]]


def best(c, s):
    return min(waits(c, s))


def worst(c, s):
    return max(waits(c, s))


print()
print("%-26s %-12s %8s %8s %7s %7s %7s %6s" % ("design", "scenario", "wait ms", "max ms", ">33 ms", "speed%", "seeks", "cmds"))
for c in configs:
    for s in scenarios:
        rs = results[(c, s)]
        r = sorted(rs, key=lambda x: x["wait_ms"])[0]
        print("%-26s %-12s %8.0f %8.0f %7d %7.0f %7d %6d%s" % (c, s, r["wait_ms"], r["max_ms"], r["over33"], r["speed_pct"], r["seeks"], r["cmds"],
                                                         ("   (best of %d: %s ms)" % (len(rs), ", ".join("%.0f" % x["wait_ms"] for x in rs))) if len(rs) > 1 else ""))
print()

failed = 0


def check(ok, what, detail=""):
    global failed
    print("%s  %s%s" % ("PASS" if ok else "FAIL", what, ("  -- " + detail) if detail else ""))
    if not ok:
        failed += 1


def have(*pairs):
    return all(p in results for p in pairs)


# build 174's runs vary (two readers fight for the head by chance): the margins are what its best of two runs leaves the new design in the model
# (loader at 2.0x: 174 9-12 s, new 7.1 s; stream+hops at 2.0x: 174 2.2-3.3 s, new 1.8 s; at 3.2x: loader 6-7 s against 3.4 s, stream+hops 1.3-2.8 s against 0.9 s)
MARGIN = {("2.0x", "loader"): 0.90, ("2.0x", "stream+hops"): 0.90, ("3.2x", "loader"): 0.78, ("3.2x", "stream+hops"): 0.85}
for speed in ("2.0x", "3.2x"):
    for s in ("loader", "stream+hops"):
        if have(("174 " + speed, s), ("new " + speed, s)):
            a, n = best("174 " + speed, s), worst("new " + speed, s)
            m = MARGIN[(speed, s)]
            check(n <= m * a, "new beats build 174 at %s on %s (wait under %d%% of 174's: the new design's worst run against 174's best)" % (speed, s, round(m * 100)),
                  "174 %.0f ms, new %.0f ms" % (a, n))
if have(("new 3.2x", "loader")):
    seeks = max(r["seeks"] for r in results[("new 3.2x", "loader")])
    files = results[("new 3.2x", "loader")][0]["files"]
    check(seeks <= 1.3 * files, "loader at 3.2x: at most 1.3 seeks per file", "%d seeks for %d files" % (seeks, files))

# not worse than build 174 anywhere (25% and 150 ms of slack: the small waits are mostly noise)
for speed in ("2.0x", "3.2x"):
    for s in scenarios:
        if have(("174 " + speed, s), ("new " + speed, s)):
            a, n = best("174 " + speed, s), worst("new " + speed, s)
            check(n <= 1.25 * a + 150, "new is no worse than build 174 at %s on %s (25%% + 150 ms)" % (speed, s), "174 %.0f ms, new %.0f ms" % (a, n))

# the guard: a thread rewrite whose CPU thread still reads the drive (build 174's mistake in the new thread) must stay worse than the design
if have(("pipeline, CPU reads 3.2x", "loader"), ("new 3.2x", "loader")):
    g, n = best("pipeline, CPU reads 3.2x", "loader"), worst("new 3.2x", "loader")
    check(g >= 1.3 * n, "guard: the pipeline thread with the CPU thread still reading is clearly worse than the design (loader, 3.2x)",
          "pipeline + CPU reads %.0f ms, new %.0f ms" % (g, n))
if have(("pipeline, CPU reads 2.0x", "loader"), ("new 2.0x", "loader")):
    g, n = best("pipeline, CPU reads 2.0x", "loader"), worst("new 2.0x", "loader")
    check(g >= 1.3 * n, "guard: the same at 2.0x (loader)", "pipeline + CPU reads %.0f ms, new %.0f ms" % (g, n))

# the optional pipeline (switch file discpipe)
if have(("new+discpipe 3.2x", "near-two-64"), ("new 3.2x", "near-two-64")):
    p, n = worst("new+discpipe 3.2x", "near-two-64"), best("new 3.2x", "near-two-64")
    check(p < 0.7 * n, "discpipe: two streams alternating 64 sectors (3.2x): the pipeline's wait is under 70% of the default's", "default %.0f ms, discpipe %.0f ms" % (n, p))
for s in scenarios:
    if have(("new+discpipe 3.2x", s), ("new 3.2x", s)):
        p, n = worst("new+discpipe 3.2x", s), best("new 3.2x", s)
        check(p <= 1.35 * n + 200, "discpipe is no worse than the default at 3.2x on %s (35%% + 200 ms)" % s, "default %.0f ms, discpipe %.0f ms" % (n, p))

if failed:
    print("FAILED: %d claim(s) of the benchmark failed (the model is +-30%% per run: rerun once before believing a marginal one)" % failed)
    sys.exit(1)
print("benchmark: all claims hold")
