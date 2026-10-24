#!/usr/bin/env bash
# PS5 port frontend (2.12, AI-assisted; swordpdf: "add an option to add a game from the shelf to the xmb (square - and then option on) it should utilize the existing
# mounting scheme we have, we should also be able to delete it, again, from the shelf"): the Home screen tile row, end to end on a PC.
#   1. gametile_test.cpp     fe_gametile.h: the request files, alive.txt and state.txt, the row's state machine in virtual time, the polling policy, the tile's own start
#                            (marker, launch check, own folder), the two notification texts in every language table (ASAN + UBSAN with FE_SANITIZE=address,undefined)
#   2. gametile_web_test.cpp the settings page's /api/tile against fe_web.cpp, the test playing the helper by hand over real files
#   3. gametile_page_test.js the page's own script in node (a fake DOM and a fake console)
#   4. fe_host --tiles       the shelf's sheet with a stand-in helper (FakeHelper in fe_host.cpp) in the shelf's virtual time: Square, then Cross on the row
# The host games: God of War (USA, SCUS-97399), Ratchet & Clank (Europe, SCES-50916), Gran Turismo 4 (USA, SCUS-97328), Castlevania - Lament of Innocence (USA,
# SLUS-20733), The Lord of the Rings - The Two Towers (USA, SLUS-20578). The row is the third selectable row of a game's sheet (Recommended, Hide from the shelf, Home
# screen tile): "press down 2". Time: `wait` is the shelf's own clock (and the stand-in helper's), 60 frames a second, much faster than real time.
# Pictures go to $FE_SHOTS when it is set.
#
#   ps5/frontend/host/test-gametile.sh
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
root=$(cd -- "$fe/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/gametile-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
cxx=${CXX:-$(command -v clang++-18 || command -v clang++)}
cc=${CC:-$(command -v clang-18 || command -v clang)}
san=${FE_SANITIZE:+-fsanitize=$FE_SANITIZE}
failed=0

# ---- 1. the pure logic
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function $san -I"$fe" "$here/gametile_test.cpp" "$fe/fe_i18n.cpp" -lpthread -o "$work/gametile_test"
"$work/gametile_test" "$work/pure" | grep -v '^\[i18n\]'

# ---- 2. the page's /api/tile
"$cc" -O1 -I"$root/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$work/chd.o"
"$cc" -O1 -c "$root/ps5/third_party/lz4/lz4.c" -o "$work/lz4.o"
"$cxx" -std=c++20 -O1 -g -Wall -Wno-unused-function $san -ffunction-sections -fdata-sections \
  -I"$root/3rdparty/libchdr/include" -I"$root/ps5/third_party/lz4" -I"$root/3rdparty/vulkan/include" \
  "$here/gametile_web_test.cpp" "$fe/fe_web.cpp" "$fe/fe_settings.cpp" "$fe/fe_games.cpp" "$fe/fe_covers.cpp" "$fe/fe_text.cpp" "$fe/fe_i18n.cpp" \
  "$work/chd.o" "$work/lz4.o" -Wl,--gc-sections -lz -lpthread -o "$work/gametile-web-test" 2> /dev/null
"$work/gametile-web-test" "$work/web" | grep -v '^\[web\]'

# ---- 3. the page's own script
if command -v node > /dev/null; then
  node "$here/gametile_page_test.js" "$fe/assets/web/index.html"
else
  echo "SKIP: no node, the page's steps weren't checked"
fi

# ---- 4. the shelf
"$here/build-host.sh" > /dev/null
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-$(ls /opt/pw-browsers/*/chrome-linux/vk_swiftshader_icd.json 2>/dev/null | head -1)}
shots=${FE_SHOTS:-$work/shots}
mkdir -p "$shots"
run() {
  local name=$1
  shift
  if out=$("$here/fe_host" --data "$work/$name" --out "$shots" --size 960x540 "$@" 2>&1) && ! grep -q "expect FAILED" <<<"$out"; then
    echo "ok: $name"
  else
    echo "FAILED: $name"
    grep "expect\|\[host\]\|tiles\]" <<<"$out" | grep -v "^\[host\] device" || true
    failed=1
  fi
}
# Like run, and the run's output must also match every "+regex" and none of the "-regex" arguments given before a "--" (extended regular expressions).
run_log() {
  local name=$1 pats=() good=1 p
  shift
  while [[ $1 != -- ]]; do
    pats+=("$1")
    shift
  done
  shift
  out=$("$here/fe_host" --data "$work/$name" --out "$shots" --size 960x540 "$@" 2>&1) || good=0
  grep -q "expect FAILED" <<<"$out" && good=0
  for p in "${pats[@]}"; do
    case $p in
      +*) grep -qE -- "${p:1}" <<<"$out" || good=0 ;;
      -*) ! grep -qE -- "${p:1}" <<<"$out" || good=0 ;;
    esac
  done
  if [[ $good == 1 ]]; then
    echo "ok: $name"
  else
    echo "FAILED: $name"
    grep "expect\|\[host\]\|tiles\]" <<<"$out" | grep -v "^\[host\] device" || true
    failed=1
  fi
}
# What the stand-in helper took, line by line (data/helper-took.txt).
took() { cat "$work/$1/helper-took.txt" 2> /dev/null || true; }
check_took() { # check_took <run> <expected text, \n \t escapes>
  local want got
  want=$(printf "$2")
  got=$(took "$1")
  if [[ $got == "$want" ]]; then
    echo "ok: $1: the helper took exactly the expected lines"
  else
    echo "FAILED: $1: the helper took [$got], wanted [$want]"
    failed=1
  fi
}
GOW='/data/PCSX2/games/God of War (USA).iso'
RC='/data/PCSX2/games/Ratchet & Clank (Europe).iso'

# Without --tiles there is no row at all: the sheet is as it was, and its third selectable row is the next group's.
run no-tiles "game 0" "wait 1" "press square" "wait 0.4" "press down 2" "wait 0.3" "expect sheet=1" "expect tilerow=0" "expect tilefocus=0" "expect tilephase=-1"
# The row is the third selectable row; Off at first; Cross asks (Asking at once), the helper takes the file (<= 1 s), builds (installing), and 4 s later it is ok: On.
# Cross again takes it off: Removing, then Off. The helper took exactly the add line, then the remove line.
run add-remove --tiles "game 0" "wait 1" "press square" "wait 0.4" "expect tilerow=1" "expect tilefocus=0" "press down 2" "wait 0.3" "expect tilefocus=1" "expect tilephase=0" \
  "shot tile-off" "press cross" "expect tilephase=1" "expect tilefile=1" "wait 2.5" "expect tilephase>=2" "expect tilephase<=3" "expect tilefile=0" "expect tilereq=1" \
  "shot tile-installing" "wait 4" "expect tilephase=4" "expect tileentries=1" "shot tile-on" \
  "press cross" "expect tilephase=1" "wait 2.5" "expect tilephase>=0" "expect tilephase<=6" "wait 3" "expect tilephase=0" "expect tileentries=0" "expect tilereq=2" "shot tile-off-again"
check_took add-remove "add\t$GOW\tSCUS-97399\tGod of War\nremove\t$GOW\n"
# Two quick presses are one request: the second finds the row busy. Left or Right adds from Off, but never takes a tile off; Triangle takes it off.
run busy-and-keys --tiles "game 0" "wait 1" "press square" "wait 0.4" "press down 2" "wait 0.3" "press cross" "press cross" "press right" "wait 3" "expect tilereq=1" \
  "wait 4" "expect tilephase=4" "press right" "wait 1.5" "expect tilereq=1" "expect tilephase=4" "press left" "wait 1.5" "expect tilereq=1" "press triangle" "expect tilephase=1" \
  "wait 5" "expect tilephase=0" "expect tilereq=2" "press left" "wait 1" "expect tilephase>=1" "expect tilephase<=3" "wait 6"
# A second press while the first is working finds the row busy (Installing: the helper is slow): nothing more is written.
run slow --tiles "game 0" "wait 1" "helper slow" "press square" "wait 0.4" "press down 2" "wait 0.3" "press cross" "wait 6" "expect tilephase=3" "expect tilereq=1" "press cross" "wait 1.5" \
  "expect tilephase=3" "expect tilereq=1" "press triangle" "wait 1.5" "expect tilereq=1" "wait 100" "expect tilephase=3" "wait 100" "expect tilephase=3" "shot tile-slow"
# The helper is not there (no alive.txt, or one two minutes old): phase 9, Cross writes nothing; the helper comes: the row follows within 3 s and Cross works.
run no-helper --tiles "game 0" "helper none" "wait 1" "press square" "wait 0.4" "press down 2" "wait 0.3" "expect tilephase=9" "shot tile-nohelper" "press cross" "wait 0.5" \
  "expect tilefile=0" "expect tilereq=0" "expect tilephase=9" "helper ok" "wait 3.5" "expect tilephase=0" "press cross" "wait 1" "expect tilephase>=1" "wait 6" "expect tilephase=4" "expect tilereq=1"
run stale-helper --tiles "game 0" "helper stale" "wait 1" "press square" "wait 0.4" "press down 2" "wait 0.3" "expect tilephase=9" "press cross" "wait 0.5" "expect tilefile=0" \
  "helper ok" "wait 3.5" "expect tilephase=0"
# The helper is alive but never takes the file: Asking for 15 s, then No answer, and the file is taken back; "helper ok" and a press again: it works.
run no-answer --tiles "game 0" "wait 1" "helper mute" "press square" "wait 0.4" "press down 2" "wait 0.3" "press cross" "wait 14" "expect tilephase=1" "expect tilefile=1" "wait 3" \
  "expect tilephase=7" "expect tilefile=0" "expect tilepending=0" "shot tile-noanswer" "helper ok" "press cross" "wait 7.5" "expect tilephase=4" "expect tilereq=1"
# The helper takes the file and never writes a line: Waiting, then Stuck 90 s after the take.
run stuck --tiles "game 0" "wait 1" "helper stuck" "press square" "wait 0.4" "press down 2" "wait 0.3" "press cross" "wait 80" "expect tilephase=2" "wait 15" "expect tilephase=8" \
  "expect tilereq=1" "shot tile-stuck" "helper ok" "press cross" "wait 7.5" "expect tilephase=4"
# The helper answers with a failure: Failed (the reason on the help line); a retry works.
run fail --tiles "game 0" "wait 1" "helper fail nogame" "press square" "wait 0.4" "press down 2" "wait 0.3" "press cross" "wait 3" "expect tilephase=5" "shot tile-failed" \
  "helper ok" "press cross" "expect tilephase=1" "wait 8" "expect tilephase=4" "expect tilereq=2"
# A limit of 10 tiles: the 11th game fails with the helper's word (the host has five games: the helper is told the limit is reached by seeding ten tiles first).
run limit --tiles "game 1" "wait 1" \
  "helper seed /data/a1.iso|PPSA99251|S1|One|ok" "helper seed /data/a2.iso|PPSA99252|S2|Two|ok" "helper seed /data/a3.iso|PPSA99253|S3|Three|ok" \
  "helper seed /data/a4.iso|PPSA99254|S4|Four|ok" "helper seed /data/a5.iso|PPSA99255|S5|Five|ok" "helper seed /data/a6.iso|PPSA99256|S6|Six|ok" \
  "helper seed /data/a7.iso|PPSA99257|S7|Seven|ok" "helper seed /data/a8.iso|PPSA99258|S8|Eight|ok" "helper seed /data/a9.iso|PPSA99259|S9|Nine|ok" \
  "helper seed /data/a10.iso|PPSA99260|S10|Ten|ok" "press square" "wait 0.4" "press down 2" "wait 0.3" "expect tilephase=0" "press cross" "wait 4" "expect tilephase=5" "shot tile-limit"
# A game whose tile is there already (the state file says ok) shows On without a request; the tile of the game this process runs from is not taken off from inside it.
run seeded --tiles "game 0" "wait 1" "helper seed $GOW|PPSA99251|SCUS-97399|God of War|ok" "wait 0.2" "press square" "wait 0.4" "press down 2" "wait 0.3" "expect tilephase=4" "expect tilereq=0"
run own-tile --tiles --own-tile "$GOW" "game 0" "wait 1" "helper seed $GOW|PPSA99251|SCUS-97399|God of War|ok" "wait 0.2" "press square" "wait 0.4" "press down 2" "wait 0.3" \
  "expect tilephase=4" "press cross" "wait 1.5" "expect tilereq=0" "expect tilefile=0" "expect tilephase=4" "press triangle" "wait 1.5" "expect tilereq=0" "shot tile-own"
# Another game's tile can be taken off from inside the tile of this one.
run own-tile-other --tiles --own-tile "$GOW" "game 1" "wait 1" "helper seed $RC|PPSA99252|SCES-50916|Ratchet & Clank|ok" "wait 0.2" "press square" "wait 0.4" "press down 2" "wait 0.3" \
  "expect tilephase=4" "press cross" "wait 1.5" "expect tilereq=1"
# The row is not on the sheet for all games, the disc being copied, or the disc in the drive (L1 inside their sheet gives a sheet of "this game" with no row).
run not-everywhere --tiles "game 0" "wait 1" "press square" "wait 0.4" "press r1" "wait 0.4" "expect tilerow=0" "expect tilephase=-1" "press l1" "wait 0.4" "expect tilerow=1" \
  "press circle" "wait 0.4" "drive SLUS-20312 Final Fantasy X" "wait 0.8" "press left 3" "wait 1" "expect selected=-2" "press square" "wait 0.6" "press l1" "wait 0.6" "expect sheet=1" \
  "expect tilerow=0" "press circle" "wait 0.4" "drive off" "wait 0.8" "dump 40" "wait 1.2" "expect selected=-1" "press square" "wait 0.6" "press l1" "wait 0.6" "expect sheet=1" "expect tilerow=0"
# Nothing is looked at unless the row is shown or an ask is pending: a closed sheet, 0 looks in 10 s; an open sheet 3 or 4; a pending ask polls once a second with the
# sheet closed, and stops when it is answered or given up.
run polls --tiles "game 0" "wait 1" "tilepollsreset" "wait 10" "expect tilepolls=0" "press square" "wait 0.4" "press down 2" "wait 0.3" "tilepollsreset" "wait 10" \
  "expect tilepolls>=3" "expect tilepolls<=4" "helper mute" "press cross" "press circle" "wait 0.4" "tilepollsreset" "wait 10" "expect tilepolls>=9" "expect tilepolls<=11" "expect tilepending=1" \
  "wait 7" "expect tilepending=0" "expect tilefile=0" "tilepollsreset" "wait 10" "expect tilepolls=0"
# An ask is followed (and given up, the file taken back) with no sheet showing its game: Cross on game 0, close, open game 1's sheet, then the all-games sheet.
run switch-while-pending --tiles "game 0" "wait 1" "helper mute" "press square" "wait 0.4" "press down 2" "wait 0.3" "press cross" "wait 0.3" "press circle" "wait 0.4" "press right" "wait 0.6" \
  "press square" "wait 0.4" "press r1" "wait 0.4" "expect tilerow=0" "expect tilepending=1" "expect tilefile=1" "wait 17" "expect tilepending=0" "expect tilefile=0"
# Two games asked while the helper does not take: two files (one per press), both followed.
run two-games --tiles "game 0" "wait 1" "helper mute" "press square" "wait 0.4" "press down 2" "wait 0.3" "press cross" "wait 0.3" "press circle" "wait 0.4" "press right" "wait 0.6" \
  "press square" "wait 0.4" "press down 2" "wait 0.3" "press cross" "wait 0.5" "expect tilefile=2" "expect tilepending=2" "helper ok" "wait 3" "press circle" "wait 10" "expect tilepending=0" \
  "expect tilefile=0" "expect tilereq=2"
# The shelf's cover for the game goes with the request as the fifth field (covers/<serial>.jpg here).
mkdir -p "$work/cover/covers"
printf 'JPEGISH' > "$work/cover/covers/SCUS-97399.jpg"
run cover --tiles "game 0" "wait 1" "press square" "wait 0.4" "press down 2" "wait 0.3" "press cross" "wait 3" "expect tilereq=1"
check_took cover "add\t$GOW\tSCUS-97399\tGod of War\t$work/cover/covers/SCUS-97399.jpg\n"
# What the shelf says in boot.log and in the settings log while a tile is made (the lines the testers send back).
run_log logs "+\[tiles\] God of War: off" "+\[tiles\] God of War: asked the helper to add it" "+\[tiles\] God of War: installing \(PPSA99251\)" "+\[tiles\] God of War: on \(PPSA99251\)" \
  "+\[settings.log\] shelf: home screen tile: God of War: on \(PPSA99251\)" "-popup" -- --tiles "game 0" "wait 1" "press square" "wait 0.4" "press down 2" "wait 0.3" "press cross" "wait 7" \
  "expect tilephase=4"
if [[ $failed != 0 ]]; then
  echo "game tiles: FAILED"
  exit 1
fi
echo "game tiles: all passed"
