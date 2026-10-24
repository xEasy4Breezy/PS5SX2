#!/usr/bin/env bash
# PS5SX2 (2.11, AI-assisted): builds and runs the PC tests of the CDVD thread (the disc lag round):
#   ps5/coreorbis/tests/cdvdthread/test-cdvdthread.sh            functional test + check of the test itself + benchmark (about 2 minutes)
#   CDVDTHREAD_QUICK=1 ...test-cdvdthread.sh                     the benchmark's loader and stream+hops only (about 40 s)
#   CDVDTHREAD_NOBENCH=1 ...                                     the functional tests only
#   CDVDTHREAD_TSAN=1 ...                                        also the functional test under ThreadSanitizer (g++)
#
# 1. test_cdvdthread: the REAL pcsx2/CDVD/CDVDdiscThread.cpp on a scripted fake drive (no timing guesses: gates and long limits).
# 2. the same test built against the frozen build 174 thread (ref/): its two checks (the CPU thread reads the drive itself; a request pushed during the last
#    prefetch read waits 250 ms) must FAIL there: the test would notice a regression.
# 3. bench_cdvdthread: the CPU thread's freeze (the time it waits in cdvdGetSector) for upstream, build 174, and the new design, on a model of the PS5's
#    drive (seek 200 ms, 2.78 MB/s at 2.0x, 4.43 MB/s at 3.2x), six scenarios; asserts only relative claims (the model is a ranking, not a promise: +-30%).
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
CXX=${CXX:-clang++-18}
inc=(-I"$root" -I"$root/pcsx2" -I"$root/3rdparty/include" -I"$root/3rdparty/fmt/include" -I"$root/common" -I"$root/3rdparty/fast_float/include"
  -I"$root/ps5/coreorbis/orbis-shims" -I"$root/pcsx2/CDVD" -I"$here")
defs=(-D_M_X86=1 -D__POSIX__=1 -D__linux__=1)
build() { # build <out> <extra flags...> -- <sources...>
  local out=$1
  shift
  "$CXX" -std=c++20 -O1 -g "${defs[@]}" "${inc[@]}" "$@" -pthread -o "$out"
}

fail=0

echo "== 1. functional test of the real thread"
build "$obj/test_cdvdthread" "$here/test_cdvdthread.cpp" "$root/pcsx2/CDVD/CDVDdiscThread.cpp"
"$obj/test_cdvdthread" > "$obj/functional_full.txt" || true
grep -E "^(PASS|FAIL|ALL PASSED|FAILED)" "$obj/functional_full.txt" | tee "$obj/functional.txt"
grep -q "^ALL PASSED" "$obj/functional.txt" || fail=1
# the line every 10 s in boot.log, with its figures, while the game reads
if grep -Eq '^\[disc\] 10 s: cpu thread asked [0-9]+ sectors, missed [0-9]+ blocks, waited [0-9]+ ms \(max [0-9]+ ms, over 33 ms: [0-9]+.*\) \| cdvd thread read [0-9]+ blocks in [0-9]+ ms \| .* \| cpu-wait on' "$obj/functional_full.txt"; then
  echo "PASS  the '[disc] 10 s:' line is printed while the disc is read"
else
  echo "FAIL  no '[disc] 10 s:' line in the output"
  fail=1
fi
grep -q '^\[disc\] read path: cpu-wait=on (cap 3000 ms)' "$obj/functional_full.txt" && echo "PASS  the '[disc] read path:' line is printed at open" || { echo "FAIL  no '[disc] read path:' line"; fail=1; }

echo "== 2. the same test against the frozen build 174 thread: the two checks of the changes must FAIL"
build "$obj/test_cdvdthread_ref" -DREF_THREAD "$here/test_cdvdthread.cpp" "$here/ref/CDVDdiscThread_174.cpp"
"$obj/test_cdvdthread_ref" | grep -E "^(PASS|FAIL|ALL PASSED|FAILED)" | tee "$obj/ref.txt" || true
if grep -q "^FAIL  slow drive: no drive read was made on the CPU thread" "$obj/ref.txt" &&
   grep -q "^FAIL  wake-up: a request pushed during the last prefetch read" "$obj/ref.txt"; then
  echo "PASS  the test notices the build 174 thread (both checks fail on it, as they must)"
else
  echo "FAIL  the test did not fail on the build 174 thread: it would not catch a regression"
  fail=1
fi

if [[ ${CDVDTHREAD_TSAN:-0} == 1 ]]; then
  echo "== 1b. functional test under ThreadSanitizer (g++)"
  g++ -std=c++20 -O1 -g -fsanitize=thread "${defs[@]}" "${inc[@]}" "$here/test_cdvdthread.cpp" "$root/pcsx2/CDVD/CDVDdiscThread.cpp" -pthread -o "$obj/test_cdvdthread_tsan"
  TSAN_OPTIONS="halt_on_error=0 exitcode=66" "$obj/test_cdvdthread_tsan" 2>"$obj/tsan.txt" | grep -E "^(PASS|FAIL|ALL PASSED|FAILED)" | tee "$obj/tsan-functional.txt"
  grep -q "^ALL PASSED" "$obj/tsan-functional.txt" || fail=1
  if grep -q "WARNING: ThreadSanitizer" "$obj/tsan.txt"; then
    echo "FAIL  ThreadSanitizer reported:"
    head -60 "$obj/tsan.txt"
    fail=1
  else
    echo "PASS  ThreadSanitizer: no report"
  fi
fi

if [[ ${CDVDTHREAD_NOBENCH:-0} != 1 ]]; then
  echo "== 3. benchmark: the CPU thread's freeze on a model of the PS5's drive"
  build "$obj/bench_new" "$here/bench_cdvdthread.cpp" "$root/pcsx2/CDVD/CDVDdiscThread.cpp"
  build "$obj/bench_ref" -DREF_THREAD "$here/bench_cdvdthread.cpp" "$here/ref/CDVDdiscThread_174.cpp"
  quick=${CDVDTHREAD_QUICK:-0}
  jobs=${CDVDTHREAD_JOBS:-8}
  python3 -I "$here/run_bench.py" "$obj" "$quick" "$jobs" | tee "$obj/bench.txt" || fail=1
fi

if [[ $fail != 0 ]]; then
  echo "FAILED"
  exit 1
fi
echo "ALL PASSED"
