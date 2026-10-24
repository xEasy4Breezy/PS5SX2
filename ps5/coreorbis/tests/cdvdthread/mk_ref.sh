#!/usr/bin/env bash
# PS5SX2 (2.11, AI-assisted): how ref/CDVDdiscThread_174.cpp was made: the thread file of the commit the 2.11 work started from (HEAD 8846f93, which is also
# what build 174 ran: its read-ahead windows live in OrbisDiscReadAhead.h, not here), frozen so that the benchmark and the test of the test can compare the
# new thread with it after the real file has changed. Run once; not part of the test.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
mkdir -p "$here/ref"
{
  echo "// FROZEN COPY (tests/cdvdthread/mk_ref.sh): pcsx2/CDVD/CDVDdiscThread.cpp as of git 8846f93 = the thread of build 174, before the 2.11 disc lag work."
  echo "// Only the benchmark (bench_cdvdthread.cpp) and the check of the test itself (test_cdvdthread.cpp -DREF_THREAD) build this."
  git -C "$root" show HEAD:pcsx2/CDVD/CDVDdiscThread.cpp
} > "$here/ref/CDVDdiscThread_174.cpp"
