#!/bin/bash
# PC test of the disc drive speed test (disc_speed.c built with -DHOST_TEST): fake drives (image files and a fake SCSI layer: a Sony drive that takes only
# DB 00 00 20 / 32 and a PC drive that takes SET CD SPEED), every phase shortened. Checks the log it writes, the scopes, a missing drive, a missing disc,
# a bad place on the disc, the lock, and that it only ever reads the images.
#   ps5/discspeed/test-discspeed.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -u
here=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-clang-18}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fails=0
check() { # <description> <command...>
  local what=$1
  shift
  if "$@"; then echo "PASS  $what"; else echo "FAIL  $what"; fails=$((fails + 1)); fi
}
build() { # <scope> <name>
  "$CC" -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread -DHOST_TEST -DSCOPE="$1" -o "$work/$2" "$here/disc_speed.c" || { echo "FAIL  build $2"; exit 1; }
}
build 1 internal
build 2 external
build 3 all
truncate -s 700M "$work/a.img"
truncate -s 700M "$work/b.img"
export DISCTEST_SCALE=0.05
export DISCTEST_FAKE="cd0=$work/a.img:sony,cd1=$work/b.img:pc"
sum_a=$(stat -c '%s %Y' "$work/a.img")
# 1. all: the PS5's drive and an external one
DISCTEST_LOG="$work/all.log" "$work/all" > "$work/all.out" 2>&1
rc=$?
check "all: exit 0" test $rc -eq 0
check "all: both drives tested, named, and told apart" bash -c "grep -q '^=== PS5 drive SONY PS-SYSTEM 503R 2305 (cd0)' '$work/all.log' && grep -q '^=== external HL-DT-ST DVDRAM GP65NB60 1.00 (cd1)' '$work/all.log'"
check "all: a RESULT line per drive" bash -c "[ \$(grep -c '^RESULT | PS5 drive\|^RESULT | external' '$work/all.log') -eq 2 ]"
check "all: identity (profile, MODE SENSE 2A, GET PERFORMANCE)" bash -c "grep -q 'disc: DVD-ROM' '$work/all.log' && grep -q 'MODE SENSE page 2A: max read 4432' '$work/all.log' && grep -q 'GET PERFORMANCE read 0' '$work/all.log'"
check "all: the speed map has five places" bash -c "[ \$(grep -c 'at [0-9]*% of the disc' '$work/all.log') -eq 10 ]"
check "all: read sizes tried" bash -c "grep -q '32 KiB reads' '$work/all.log' && grep -q '4 MiB reads' '$work/all.log'"
check "all: the stutter comparison runs on both drives" bash -c "[ \$(grep -c 'PS5SX2 now: 32 KiB reads' '$work/all.log') -eq 2 ] && [ \$(grep -c '128 KiB reads (the 5S2CD fork)' '$work/all.log') -eq 2 ] && grep -q 'slowest read' '$work/all.log'"
check "all: sustained second by second" grep -q "second  *[0-9.]*:.*MB/s" "$work/all.log"
check "all: seeks: random reads and the two-stream runs on both drives" bash -c "[ \$(grep -c 'random 32 KiB reads, one at a time' '$work/all.log') -eq 2 ] && [ \$(grep -c 'two streams in turn, ' '$work/all.log') -eq 6 ] && [ \$(grep -c '^RESULT |   seeks:' '$work/all.log') -eq 2 ]"
check "all: reading more per seek is faster (128 KiB beats 32 KiB, 512 KiB beats 128 KiB)" bash -c "grep '^RESULT |   seeks:' '$work/all.log' | sed 's/.*32 KiB at a time \\([0-9.]*\\) MB.s, 128 KiB \\([0-9.]*\\) MB.s, 512 KiB \\([0-9.]*\\) MB.s/\\1 \\2 \\3/' | awk '{ if (!(\$2 > \$1 && \$3 > \$2)) bad = 1 } END { exit bad }'"
check "all: a finished run says so" bash -c "[ \$(grep -c '^=== run finished' '$work/all.log') -eq 1 ]"
check "all: Sony drive: DB 20 and 32 taken, the other 42 refused" bash -c "grep -q 'Sony DB 00 00 20: TAKEN' '$work/all.log' && grep -q 'Sony DB 00 00 32: TAKEN' '$work/all.log' && grep -q 'Sony DB: 2 taken, 42 refused' '$work/all.log'"
check "all: Sony drive's best is the 3.2 setting" bash -c "grep '^RESULT | PS5 drive' '$work/all.log' | grep -q 'with Sony DB 00 00 32'"
check "all: PC drive: SET CD SPEED taken, SET STREAMING refused, best is SET CD SPEED" bash -c "grep -q 'SET CD SPEED max: TAKEN' '$work/all.log' && grep -q 'SET STREAMING max: refused' '$work/all.log' && grep '^RESULT | external' '$work/all.log' | grep -q 'with SET CD SPEED max'"
check "all: the drive claims are in the RESULT lines" bash -c "grep '^RESULT | external' '$work/all.log' | grep -q 'claims up to'"
check "all: the second RESULT line compares the read patterns" bash -c "[ \$(grep -c '^RESULT |   reads:' '$work/all.log') -eq 2 ]"
check "all: a popup per drive and a done popup" bash -c "[ \$(grep -c '^\[popup\] PS5SX2 \(PS5 drive\|external\)' '$work/all.out') -eq 2 ] && grep -q 'disc drive test done' '$work/all.out'"
check "all: Sony put back to its default (DB 00 00 20)" grep -q "Sony drive back to its default" "$work/all.log"
check "all: the lock is gone" bash -c "[ ! -e '$work/all.log.lock' ]"
check "all: the image was only read" bash -c "[ \"\$(stat -c '%s %Y' '$work/a.img')\" = '$sum_a' ]"
# 2. internal / external scopes
DISCTEST_LOG="$work/int.log" "$work/internal" > "$work/int.out" 2>&1
check "internal: only the PS5's drive" bash -c "grep -q '^=== PS5 drive' '$work/int.log' && ! grep -q '^=== external' '$work/int.log' && grep -q 'cd1 .*not tested by this ELF' '$work/int.log'"
DISCTEST_LOG="$work/ext.log" "$work/external" > "$work/ext.out" 2>&1
check "external: only the external drive" bash -c "grep -q '^=== external' '$work/ext.log' && ! grep -q '^=== PS5 drive' '$work/ext.log' && grep -q 'cd0 .*not tested by this ELF' '$work/ext.log'"
# 3. a CD in the external drive: the speeds are in CD x
DISCTEST_FAKE="cd0=$work/b.img:pc:08" DISCTEST_LOG="$work/cd.log" "$work/external" > /dev/null 2>&1
check "a CD: profile named, speeds in CD x" bash -c "grep -q 'disc: CD-ROM' '$work/cd.log' && grep -q 'x CD' '$work/cd.log'"
# 4. no drive at all
DISCTEST_FAKE="" DISCTEST_LOG="$work/none.log" "$work/all" > "$work/none.out" 2>&1
rc=$?
check "no drive: exit 1, says so, popup" bash -c "[ $rc -eq 1 ] && grep -q 'no optical drive to test' '$work/none.log' && grep -q 'popup' '$work/none.out'"
# 5. a drive with no disc: it waits (a minute, scaled), then skips it
DISCTEST_FAKE="cd0=-:sony" DISCTEST_LOG="$work/nodisc.log" "$work/internal" > "$work/nodisc.out" 2>&1
rc=$?
check "no disc: waits, skips, exit 1, popup" bash -c "[ $rc -eq 1 ] && grep -q 'Waiting up to a minute' '$work/nodisc.log' && grep -q 'no disc, skipped' '$work/nodisc.log' && grep -q 'nothing was tested' '$work/nodisc.out'"
# 6. a bad place on the disc (100 to 200 MiB): reads fail there, the test still finishes and says so
DISCTEST_FAKE="cd0=$work/a.img:sony" DISCTEST_FAKE_BAD="30-700" DISCTEST_LOG="$work/bad.log" "$work/internal" > /dev/null 2>&1
rc=$?
check "bad disc: finishes, read errors reported" bash -c "[ $rc -eq 0 ] && grep -q 'read errors' '$work/bad.log' && grep -q '^RESULT | PS5 drive' '$work/bad.log'"
# 7. the lock: a fresh one stops a second run, an old one is taken over
echo "$(date +%s)" > "$work/lock.log.lock"
DISCTEST_LOG="$work/lock.log" "$work/internal" > "$work/lock.out" 2>&1
rc=$?
check "a fresh lock: refused" bash -c "[ $rc -eq 1 ] && grep -q 'already running' '$work/lock.out'"
echo "$(( $(date +%s) - 3600 ))" > "$work/lock.log.lock"
DISCTEST_LOG="$work/lock.log" "$work/internal" > /dev/null 2>&1
check "an hour-old lock: taken over" grep -q "^=== PS5 drive" "$work/lock.log"
# 7b. a lock whose process is gone is taken over at once (a run that was killed), one whose process is alive is respected
echo "$(date +%s) 999999" > "$work/dead.log.lock"
DISCTEST_FAKE="cd0=$work/a.img:sony" DISCTEST_LOG="$work/dead.log" "$work/internal" > "$work/dead.out" 2>&1
check "a fresh lock of a dead process: taken over, and the log says the earlier run did not finish" bash -c "grep -q '^=== PS5 drive' '$work/dead.log' && grep -q 'the lock of an earlier run was found' '$work/dead.log'"
echo "$(date +%s) $$" > "$work/live.log.lock"
DISCTEST_LOG="$work/live.log" "$work/internal" > "$work/live.out" 2>&1
rc=$?
check "a fresh lock of a live process: refused" bash -c "[ $rc -eq 1 ] && grep -q 'already running' '$work/live.out'"
# 7c. the watchdog: a read that never comes back (from 100 MiB on) is reported, and the run is given up so the next load is not refused
DISCTEST_FAKE="cd0=$work/a.img:sony" DISCTEST_FAKE_HANG=100 DISCTEST_WD_WARN=1 DISCTEST_WD_GIVEUP=3 DISCTEST_LOG="$work/hang.log" "$work/internal" > "$work/hang.out" 2>&1
rc=$?
check "a stuck drive: reported where, given up with exit 2, lock gone, popup" bash -c "[ $rc -eq 2 ] && grep -q 'nothing came back' '$work/hang.log' && grep -q 'giving up' '$work/hang.log' && [ ! -e '$work/hang.log.lock' ] && grep -q 'stopped answering' '$work/hang.out'"
check "a stuck drive: the log has no finished line" bash -c "! grep -q '^=== run finished' '$work/hang.log'"
# 8. the log is appended
DISCTEST_LOG="$work/lock.log" "$work/internal" > /dev/null 2>&1
check "a second run is added to the log" bash -c "[ \$(grep -c '^#####' '$work/lock.log') -eq 2 ]"
echo
[ $fails -eq 0 ] && echo "discspeed: all passed" || echo "discspeed: $fails FAILED"
sed -n 1,70p "$work/all.log"
exit $fails
