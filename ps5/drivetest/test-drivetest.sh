#!/bin/bash
# PC test of the drive speed test (drive_speed.c built with -DHOST_TEST): runs it against folders standing in for the drives, with every phase shortened,
# and checks the log it writes, that it cleans up after itself, that a folder it can't write to is read only, a missing drive, and the lock.
#   ps5/drivetest/test-drivetest.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -u
here=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-clang-18}
work=$(mktemp -d)
trap 'chmod -R u+w "$work" 2>/dev/null; rm -rf "$work"' EXIT
fails=0
check() { # <description> <command...>
  local what=$1
  shift
  if "$@"; then echo "PASS  $what"; else echo "FAIL  $what"; fails=$((fails + 1)); fi
}
build() { # <scope> <name>
  "$CC" -std=gnu11 -O1 -g -Wall -Wextra -Werror -DHOST_TEST -DSCOPE="$1" -o "$work/$2" "$here/drive_speed.c" || { echo "FAIL  build $2"; exit 1; }
}
build 1 internal
build 2 external
build 3 all
mkdir -p "$work/in" "$work/ext0" "$work/ext1" "$work/ro" "$work/games"
# an "existing" big file on the internal folder (300 MB, sparse is fine for a read test)
dd if=/dev/zero of="$work/in/games/big.iso" bs=1M count=300 status=none 2>/dev/null || { mkdir -p "$work/in/games"; dd if=/dev/zero of="$work/in/games/big.iso" bs=1M count=300 status=none; }
export DRIVETEST_SCALE=0.15
# 1. all: two external folders and the internal one
DRIVETEST_INTERNAL="$work/in" DRIVETEST_EXTERNAL="$work/ext0,$work/ext1" DRIVETEST_LOG="$work/all.log" "$work/all" > "$work/all.out" 2>&1
rc=$?
check "all: exit 0" test $rc -eq 0
check "all: three drives found" grep -q "3 drives to test" "$work/all.log"
check "all: write, block sizes, read, cold read, random, small files all reported" bash -c "grep -q 'write 8 MiB' '$work/all.log' && grep -q 'write 64 KiB' '$work/all.log' && grep -q 'write 32 MiB' '$work/all.log' && grep -q 'read, buffered' '$work/all.log' && grep -q 'cold read' '$work/all.log' && grep -q 'random 4 KiB' '$work/all.log' && grep -q 'created' '$work/all.log'"
check "all: a summary line per drive" bash -c "[ \$(grep -c '^RESULT | .* | .* | ' '$work/all.log') -eq 3 ]"
check "all: the popups say the headline numbers" grep -q "\[popup\] PS5SX2 internal" "$work/all.out"
check "all: the second at a time lines for the main write" grep -q "write second  1" "$work/all.log"
check "all: nothing left in the folders" bash -c "[ ! -e '$work/in/PS5SX2-speedtest' ] && [ ! -e '$work/ext0/PS5SX2-speedtest' ] && [ ! -e '$work/ext1/PS5SX2-speedtest' ]"
check "all: the existing file is untouched" bash -c "[ \$(stat -c %s '$work/in/games/big.iso') -eq 314572800 ]"
check "all: the lock is gone" bash -c "[ ! -e '$work/all.log.lock' ]"
# 2. internal only: the external folders are not looked at
DRIVETEST_INTERNAL="$work/in" DRIVETEST_EXTERNAL="$work/ext0" DRIVETEST_LOG="$work/int.log" "$work/internal" > /dev/null 2>&1
check "internal: one drive" grep -q "1 drive to test" "$work/int.log"
check "internal: the external folder was not used" bash -c "! grep -q 'external 0' '$work/int.log'"
# 3. external with nothing plugged in
DRIVETEST_EXTERNAL="" DRIVETEST_LOG="$work/none.log" "$work/external" > "$work/none.out" 2>&1
rc=$?
check "external with no drive: exit 1 and says so" bash -c "[ $rc -eq 1 ] && grep -q 'no drive found' '$work/none.log' && grep -q 'popup' '$work/none.out'"
# 4. a folder it can't write to: read only, no crash, nothing created
if [ "$(id -u)" != 0 ]; then
  cp "$work/in/games/big.iso" "$work/ro/big.iso"
  chmod 555 "$work/ro"
  DRIVETEST_EXTERNAL="$work/ro" DRIVETEST_LOG="$work/ro.log" "$work/external" > /dev/null 2>&1
  rc=$?
  check "read-only drive: finished" test $rc -eq 0
  check "read-only drive: says so and still reads the existing file" bash -c "grep -q \"can't make\" '$work/ro.log' && grep -q 'cold read' '$work/ro.log'"
  chmod 755 "$work/ro"
fi
# 5. the lock: a fresh one stops a second run, an old one is taken over
echo "$(date +%s)" > "$work/lock.log.lock"
DRIVETEST_INTERNAL="$work/in" DRIVETEST_LOG="$work/lock.log" "$work/internal" > "$work/lock.out" 2>&1
rc=$?
check "a fresh lock: refused" bash -c "[ $rc -eq 1 ] && grep -q 'already running' '$work/lock.out'"
echo "$(( $(date +%s) - 3600 ))" > "$work/lock.log.lock"
DRIVETEST_INTERNAL="$work/in" DRIVETEST_LOG="$work/lock.log" "$work/internal" > /dev/null 2>&1
check "an hour-old lock: taken over" grep -q "1 drive to test" "$work/lock.log"
echo
[ $fails -eq 0 ] && echo "drivetest: all passed" || echo "drivetest: $fails FAILED"
sed -n 1,60p "$work/all.log" | head -70
exit $fails
