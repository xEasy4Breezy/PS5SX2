#!/usr/bin/env bash
# 2.11 (AI-assisted; swordpdf: "add a search section to the shelf (make it sleek)" and "moving the right stick would allow you to slickly
# flip it and see the back in the shelf"): the shelf's search and the case turning over, in fe_host (SwiftShader or lavapipe stands in for the
# GPU). The 5 host games: God of War (USA, SCUS-97399), Ratchet & Clank (Europe, SCES-50916), Gran Turismo 4 (USA, SCUS-97328),
# Castlevania - Lament of Innocence (USA, SLUS-20733), The Lord of the Rings - The Two Towers (USA, SLUS-20578).
#   - R2 opens the panel's keyboard (the host has no PS5 one); each key narrows the shelf; Circle closes the keyboard (the search stays) and,
#     on the shelf, clears the search; Triangle clears it on the keyboard;
#   - the settings page's box sets and clears it ("remote"); every word must match (title, serial, region, file name, any case);
#   - the right stick turns the picked case over -- hard to turn on purpose (swordpdf: "small stick movements make the rotation go fast. make it
#     harder"): nothing below a 0.35 push, a gentle curve above it, the whole way only near a full push (0.95), a soft spring, and a cap of 2.6
#     flips a second so a flick cannot spin it; the click holds it, moving the shelf puts it back, a push the other way swings it through the front;
#   - the picked case's back cover (swordpdf: "back covers are not loading instantly"): asked for after 0.20 s on disk, after 0.35 s or at once when
#     the player starts to turn the case with the network too, never while scrolling; the neighbours' backs are handed over to be fetched ahead.
#   - the case the selection leaves (R3 of build 175: "viewing the back cover and moving to the next game, the cover gets deloaded before it turns back to
#     the front, a black spot for a second") keeps its back cover for as long as the flip still turns it over on screen, and not much longer.
# Pictures go to $FE_SHOTS when it is set.
#
#   ps5/frontend/host/test-search.sh
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
"$here/build-host.sh" >/dev/null
work=$(mktemp -d "${TMPDIR:-/tmp}/search-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-$(ls /opt/pw-browsers/*/chrome-linux/vk_swiftshader_icd.json 2>/dev/null | head -1)}
shots=${FE_SHOTS:-$work/shots}
failed=0
run() {
  local name=$1
  shift
  if out=$("$here/fe_host" --data "$work/$name" --out "$shots" --size 960x540 "$@" 2>&1) && ! grep -q "expect FAILED" <<<"$out"; then
    echo "ok: $name"
  else
    echo "FAILED: $name"
    grep "expect\|\[host\]\|search" <<<"$out" | grep -v "^\[host\] device" || true
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
    grep "expect\|\[host\]\|search\|back cover" <<<"$out" | grep -v "^\[host\] device" || true
    failed=1
  fi
}
# The panel's keyboard starts on "q"; "down" goes to the asdf row (the key nearest: "a"), then right x7 is "k": only "Ratchet & Clank" has one.
run keyboard-types "game 0" "wait 1" "expect shelf=5" "press r2" "wait 0.5" "expect typing=1" "shot search-keyboard" \
  "press down" "press right 7" "press cross" "wait 0.3" "expect shelf=1" "shot search-typed" \
  "press triangle" "wait 0.2" "expect shelf=5" "expect searchlen=0"
# "war": w (row 2, col 1 of qwertyuiop), a (asdf row), r (qwerty row col 3): Cross each; God of War is the only one.
run keyboard-word "game 3" "wait 1" "press r2" "wait 0.5" "press right" "press cross" "press down" "press left" "press cross" "press up" "press right 3" \
  "press cross" "wait 0.3" "expect searchlen=3" "expect shelf=1" "expect selected=0" "press circle" "wait 0.3" "expect typing=0" "expect shelf=1" \
  "shot search-war" "press circle" "wait 0.3" "expect shelf=5" "expect selected=0" "expect searchlen=0"
# Words: all of them must match. "usa" 4, "usa slus" 2, "of" 3 (title text), "zzz" none (the shelf says so).
run remote-words "game 0" "wait 1" "remote usa" "wait 0.3" "expect shelf=4" "remote USA SLUS" "wait 0.3" "expect shelf=2" "remote of" "wait 0.3" "expect shelf=3" \
  "remote scus-97" "wait 0.3" "expect shelf=2" "remote scus97" "wait 0.3" "expect shelf=2" "remote zzz" "wait 0.3" "expect shelf=0" "shot search-none" \
  "remote" "wait 0.3" "expect shelf=5" "expect searchlen=0"
# The case turns over: the stick pushed right all the way is the back; let go and it comes back; the click holds it; a move puts it back.
run flip "game 2" "wait 1" "expect flip100=0" "stick 1 0" "wait 1.2" "expect flip100=100" "shot flip-back" "stick 0.8 0" "wait 1.5" "expect flip100>=45" \
  "expect flip100<=62" "shot flip-half" "stick 0 0" "wait 1.5" "expect flip100=0" "shot flip-front" "press r3" "wait 1.2" "expect flip100=100" \
  "press right" "wait 1.5" "expect flip100=0" "expect selected=3" "stick -1 0" "wait 1.5" "expect flip100=100" "stick 0 0" "wait 1.5" "expect flip100=0"
# Hard to turn (swordpdf): a push of 0.3 (even 0.35, the edge of the dead zone) does not move it, either way, with a sideways wobble or not.
run flip-dead-zone "game 2" "wait 1" "stick 0.3 0" "wait 1.5" "expect flip100=0" "stick 0.35 0" "wait 1.5" "expect flip100=0" "stick -0.34 0" "wait 1.5" \
  "expect flip100=0" "stick 0.2 0.9" "wait 1" "expect flip100=0" "stick 0 0" "wait 0.3"
# ... a half push (0.5) turns it a little (5 %), 0.65 a fifth of the way (22 %), 0.8 half way (53 %), and the whole way only from 0.95.
run flip-gentle "game 2" "wait 1" "stick 0.5 0" "wait 1.5" "expect flip100>=2" "expect flip100<=12" "stick 0.65 0" "wait 1.5" "expect flip100>=15" "expect flip100<=30" \
  "stick 0.8 0" "wait 1.5" "expect flip100>=45" "expect flip100<=62" "stick 0.9 0" "wait 1.5" "expect flip100>=75" "expect flip100<=95" "stick 0.95 0" "wait 1.5" \
  "expect flip100=100" "stick 0 0" "wait 1.5" "expect flip100=0"
# ... but a full push reaches the back in under 0.8 s (and the click holds it there about as fast), and lets go as fast.
run flip-full-push "game 2" "wait 1" "stick 1 0" "wait 0.8" "expect flip100>=98" "stick 0 0" "wait 1.5" "expect flip100=0" "press r3" "wait 0.8" "expect flip100>=98" \
  "press r3" "wait 1.5" "expect flip100=0"
# The turning is capped at 2.6 flips a second -- the full push reaches the cap and no more -- and a flick of the stick (a tenth of a second) turns it a third
# of the way at most; a flick right and left again stays small and never makes it jump.
run flip-speed-cap "game 2" "wait 1" "flipreset" "stick 1 0" "wait 2" "expect flip100=100" "expect flipspeed100<=265" "expect flipspeed100>=200" "stick 0 0" "wait 2" \
  "flipreset" "stick 1 0" "wait 0.1" "stick 0 0" "wait 2" "expect flippeak100<=45" "expect flippeak100>=20" "expect flip100=0" \
  "flipreset" "stick 1 0" "wait 0.1" "stick 0 0" "wait 0.1" "stick -1 0" "wait 0.1" "stick 0 0" "wait 2" "expect flipspeed100<=265" "expect flippeak100<=45" "expect flip100=0"
# A push the other way while it is over never makes it jump across or spin round (the pose repeats every two flips, and it takes the short way):
# from the back, a push to the other side leaves it there; from well over (0.9: 83 %) it goes on to the back, not round through the front; from a
# little over (0.7: 30 %) or with a small push (0.65) the other way it goes through the front to that side, as the player pushes.
run flip-swing "game 2" "wait 1" \
  "stick 1 0" "wait 1.5" "expect flip100>=98" "flipreset" "stick -1 0" "wait 2" "expect flip100>=98" "expect flipbottom100>=95" "expect flipspeed100<=60" \
  "stick 0 0" "wait 2" "expect flip100=0" \
  "stick 0.9 0" "wait 1.5" "expect flipangle100>=78" "expect flipangle100<=88" "flipreset" "stick -1 0" "wait 2" "expect flip100>=98" "expect flipbottom100>=75" \
  "expect flipspeed100<=265" "stick 0 0" "wait 2" "expect flip100=0" \
  "stick 0.7 0" "wait 1.5" "expect flipangle100>=26" "expect flipangle100<=34" "flipreset" "stick -1 0" "wait 2" "expect flip100>=98" "expect flipbottom100<=6" \
  "expect flipspeed100<=265" "stick 0 0" "wait 2" "expect flip100=0" \
  "stick 0.8 0" "wait 1.5" "flipreset" "stick -0.65 0" "wait 2" "expect flipbottom100<=6" "expect flipangle100>=-26" "expect flipangle100<=-18" "expect flipspeed100<=265" \
  "stick 0 0" "wait 2" "expect flip100=0"
# A back cover the player put in covers/ (SCUS-97328_back.png): found once the case has been picked for a moment, and shown on the back.
# The picture has a red square at its top left: seen from behind it is at the viewer's top left, upright.
# (2.11: made once, and put in the covers/ of every run that wants it with `backs`: for Gran Turismo 4 (host game 2) and Castlevania (game 3).)
mkdir -p "$work/back-fixture"
python3 - "$work/back-fixture/SCUS-97328_back.png" <<'PY'
import sys, zlib, struct
w, h = 360, 504
rows = []
for y in range(h):
    row = bytearray([0])
    for x in range(w):
        r = 40 + (x * 200 // w)
        g = 60 + (y * 160 // h)
        b = 180 if (x // 30 + y // 30) % 2 == 0 else 90
        if x < 6 or y < 6 or x >= w - 6 or y >= h - 6:
            r, g, b = 255, 255, 255
        if x < 60 and y < 60:
            r, g, b = 255, 40, 40
        row += bytes([r, g, b])
    rows.append(bytes(row))
raw = b"".join(rows)
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                              chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
PY
cp "$work/back-fixture/SCUS-97328_back.png" "$work/back-fixture/SLUS-20733_back.png"
backs() { # backs <run name>: the two back covers in that run's covers/
  mkdir -p "$work/$1/covers"
  cp "$work/back-fixture/"*.png "$work/$1/covers/"
}
backs flip-back-real
run flip-back-real "game 2" "wait 2" "stick 1 0" "wait 1.3" "expect flip100=100" "shot flip-back-real" "stick 0.8 0" "wait 1.0" "shot flip-back-real-turning" "stick 0 0" "wait 1"
# 2.11 (swordpdf: "back covers are not loading instantly"). Time in these runs: `wait` is the shelf's own clock (the 0.20 s and 0.35 s of being picked),
# `sleep` real time for the covers' thread (one frame every 50 ms), so each step uses the one it needs. The shelf says when it asks for a back (the log).
# The back is there without any flip, for the picked game and for the next one after a step.
backs back-loaded-without-flip
run back-loaded-without-flip "game 2" "wait 0.5" "sleep 0.6" "expect backreal=1" "press right" "wait 0.3" "sleep 0.8" "expect backreal=1" "expect selected=3"
# Not asked for before 0.20 s: a game just stepped to has no back asked for, and gets its back (the files) at 0.20 s, the download too at 0.35 s.
backs back-asked-late
run_log back-asked-late "+back cover asked for Castlevania.*\(files only\) (19[0-9]|2[0-9][0-9]|3[0-4][0-9]) ms after it was picked" \
  "+back cover asked for Castlevania.*\(download too\) 3[4-9][0-9] ms after it was picked" -- \
  "game 2" "wait 1" "press right" "wait 0.1" "expect backreal=0" "wait 0.45" "sleep 0.6" "expect backreal=1"
# ... but when the player starts to turn the case it is asked for at once, the download included (nothing else could have made the back come before 0.20 s).
backs back-asked-by-flip
run_log back-asked-by-flip "+back cover asked for Castlevania.*\(files and download\) ([0-9]|[0-9][0-9]|1[0-9][0-9]) ms after it was picked" \
  "-back cover asked for Castlevania.*\(files only\)" -- "game 2" "wait 1" "press right" "stick 1 0" "wait 0.1" "sleep 0.6" "expect backreal=1" "stick 0 0" "wait 1"
# ... and a push inside the dead zone is no sign of that.
backs back-not-asked-by-wobble
run_log back-not-asked-by-wobble "+back cover asked for Castlevania.*\(files only\) (19[0-9]|2[0-9][0-9]|3[0-4][0-9]) ms" -- \
  "game 2" "wait 1" "press right" "stick 0.3 0" "wait 0.25" "stick 0 0" "wait 0.3"
# Scrolling asks for nothing: held right from the first game, the games passed over (Gran Turismo, Castlevania: 0.11 s each) have no back asked for or
# arrived, the one landed on does.
backs back-held-scroll
run_log back-held-scroll "+back cover asked for The Lord of the Rings.*\(files only\)" "-back cover asked for Gran Turismo" "-back cover asked for Castlevania" \
  "-back cover for Gran Turismo" "-back cover for Castlevania" -- "game 0" "wait 1" "hold right 0.7" "wait 0.5" "expect selected=4"
# The neighbours' backs are handed over to be fetched ahead: three a side, nearest first, the side the selection is moving towards first (the host has
# five games: two a side at most).
run_log back-ring "+back cover asked for Gran Turismo" -- "game 2" "wait 1" "expect ringcount=4" "expect ringfirst=3" "press left" "wait 1" "expect selected=1" \
  "expect ringcount=4" "expect ringfirst=0" "press right" "press right" "wait 1" "expect selected=3" "expect ringcount=4" "expect ringfirst=4"
# Nothing is handed over while the case is only passing by (0.45 s of being picked: above the 0.36 s a held button waits to repeat).
run back-ring-late "game 0" "wait 1" "expect ringfirst=1" "press right" "wait 0.3" "press right" "wait 0.2" "expect selected=2" "expect ringfirst=1"
# R3 (build 175): the case the selection leaves is still turned over -- the flip turns every case near the scroll position by what its place allows (App::FlipTurn,
# the pose's own number) -- so its back cover stays while turn2 (the degrees the flip adds to Gran Turismo 4's pose, host game 2) is 10 or more, and goes soon
# after (0.2 s): backat2 is 1 at every frame of that, then 0, and nothing is held. Before the fix it was 0 from the first frame of the move (the painted back,
# a dark card, over the real one). `expectwhile <condition> <must> [<must> ...]` runs frame by frame while the condition holds and checks the others at each of them (setat2: the case is drawn at all).
backs back-kept-turning
run back-kept-turning "game 2" "wait 0.5" "sleep 0.6" "expect backat2=1" "stick 1 0" "wait 1.3" "expect flip100=100" "expect turn2>=170" \
  "stick 0 0" "press right" "expect selected=3" "expectwhile turn2>=10 backat2=1 setat2=1" "wait 1" "expect backat2=0" "expect backheld=0" "expect setat2=1"
# ... with the stick held the flip stays: the case slides aside and turns to the front by its place alone, and its back stays the whole way.
backs back-kept-stick-held
run back-kept-stick-held "game 2" "wait 0.5" "sleep 0.6" "expect backat2=1" "stick 1 0" "wait 1.3" "expect turn2>=170" \
  "press right" "expect selected=3" "expectwhile turn2>=10 backat2=1 setat2=1" "wait 1" "expect backat2=0" "expect backheld=0" "expect setat2=1" "stick 0 0" "wait 1"
# ... the click's hold (R3) is let go by the move itself, the other way round: the same.
backs back-kept-latched
run back-kept-latched "game 2" "wait 0.5" "sleep 0.6" "expect backat2=1" "press r3" "wait 1.3" "expect turn2>=170" \
  "press left" "expect selected=1" "expectwhile turn2>=10 backat2=1 setat2=1" "wait 1" "expect backat2=0" "expect backheld=0" "expect setat2=1"
# Back to the game just left while it is still turned: its back is there at once (nothing is asked for again), and nothing is held for it any more.
backs back-kept-return
run back-kept-return "game 2" "wait 0.5" "sleep 0.6" "expect backat2=1" "stick 1 0" "wait 1.3" "stick 0 0" "press right" "wait 0.05" "expect backheld=1" \
  "press left" "expect selected=2" "expect backreal=1" "expect backheld=0" "wait 1" "expect backreal=1"
# A game that is not turned over (the flip at rest) lets its back go at once, as before: nothing is held, nothing waits.
backs back-not-held-at-rest
run back-not-held-at-rest "game 2" "wait 0.5" "sleep 0.6" "expect backat2=1" "press right" "expect backheld=0" "expect backat2=0"
# The hold is small and does not leak. These two runs give all five host games a back cover (backs_all). The stick held, the shelf walked through to its end with a
# pause for each back to arrive: at most 3 backs are held at a moment (heldpeak), at most 4 are loaded (countpeak: the picked one and the ones still held),
# and once the stick is let go only the picked case still has one (backcount counts the cases off the shelf too).
backs_all() { # backs_all <run name>: the back cover of every host game in that run's covers/
  backs "$1"
  for serial in SCUS-97399 SCES-50916 SLUS-20578; do cp "$work/back-fixture/SCUS-97328_back.png" "$work/$1/covers/${serial}_back.png"; done
}
backs_all back-held-walk
run back-held-walk "game 0" "wait 0.5" "sleep 0.6" "stick 1 0" "wait 1.3" "flipreset" \
  "press right" "wait 0.05" "sleep 0.3" "press right" "wait 0.05" "sleep 0.3" "press right" "wait 0.05" "sleep 0.3" "press right" "wait 0.05" "sleep 0.3" \
  "expect selected=4" "expect heldpeak<=3" "expect countpeak<=4" "stick 0 0" "wait 1.5" "expect backheld=0" "expect backcount<=1"
# ... the search narrowing the shelf while a back is held: the held case goes off the shelf with the rest, and nothing stays loaded but the picked case's.
backs_all back-held-search
run back-held-search "game 2" "wait 0.5" "sleep 0.6" "press r3" "wait 1.3" "press right" "wait 0.05" "press r2" "wait 0.5" "press down" "press right 7" "press cross" \
  "wait 0.5" "expect shelf=1" "expect backheld=0" "wait 1" "expect backcount<=1"
# (review of R3) The stick let go a moment before the move: the flip is on its way back (122 degrees at the move) and the hold is the same.
backs back-kept-midflip
run back-kept-midflip "game 2" "wait 0.5" "sleep 0.6" "expect backat2=1" "stick 1 0" "wait 1.3" "expect turn2>=170" "stick 0 0" "wait 0.12" "expect turn2>=90" \
  "press right" "expect selected=3" "expectwhile turn2>=10 backat2=1 setat2=1" "wait 1" "expect backat2=0" "expect backheld=0" "expect setat2=1"
# The log line of the hold (boot.log; the tester note names it): once, 0.3 to 0.6 s after the move, when the flip has brought the case round -- and not when the
# flip was at rest at the move (nothing was held), nor when the player came back to the case before that (it was never let go).
backs back-held-log
run_log back-held-log "+back cover of Gran Turismo 4 let go [3-5][0-9][0-9] ms after the selection left it: its case is turned back to the front" -- \
  "game 2" "wait 0.5" "sleep 0.6" "press r3" "wait 1.3" "press right" "wait 1.5" "expect backat2=0" "expect setat2=1"
backs back-held-log-at-rest
run_log back-held-log-at-rest "-back cover of .* let go" -- "game 2" "wait 0.5" "sleep 0.6" "expect backat2=1" "press right" "wait 1.5" "expect backat2=0"
backs back-held-log-return
run_log back-held-log-return "-back cover of .* let go" -- "game 2" "wait 0.5" "sleep 0.6" "press r3" "wait 1.3" "press right" "wait 0.05" "press left" "wait 1.5" \
  "expect selected=2" "expect backreal=1"
# Stepping to and fro inside the turn (the click's hold, three times each way): at most 2 cases hold a back at a moment, and when it is over only the picked one does.
backs_all back-held-pingpong
run back-held-pingpong "game 2" "wait 0.5" "sleep 0.6" "press r3" "wait 1.3" "flipreset" \
  "press right" "wait 0.1" "press left" "wait 0.05" "press right" "wait 0.1" "press left" "wait 0.05" "press right" "wait 0.1" "press left" \
  "expect heldpeak<=2" "expect selected=2" "wait 1.5" "expect backheld=0" "expect backcount<=1"
# A jump of five places (R1) with the case turned: the case left is held while it is turned over, not longer.
backs_all back-held-jump
run back-held-jump "game 0" "wait 0.5" "sleep 0.6" "press r3" "wait 1.3" "expect backat0=1" "flipreset" "press r1" "expect selected=4" \
  "expect backat0=1" "expect setat0=1" "wait 1.5" "expect backat0=0" "expect backheld=0" "expect backcount<=1"
# The second move goes to the disc in the drive (an entry with no covers of its own, picked by the next step): the case held for the first move is still let go
# once its flip has come round (the other half of PollCovers, where no game is picked, runs the holds too). Without it the back stayed until the next step to a game.
backs_all back-held-drive-entry
run back-held-drive-entry "game 1" "wait 0.5" "sleep 0.6" "drive SLUS-99999 Host Disc" "wait 0.3" "expect driveentry=1" "expect backat1=1" "press r3" "wait 1.3" "expect turn1>=170" \
  "press left" "expect selected=0" "wait 0.05" "press left" "expect selected=-2" "expect backheld=1" "wait 1.5" "expect backheld=0" "expect backat1=0" "expect setat1=1" \
  "expect backcount<=1"
# While the keyboard is up (or the sheet) the stick does nothing.
run flip-not-while-typing "game 2" "wait 1" "press r2" "wait 0.5" "stick 1 0" "wait 1.2" "expect flip100=0" "stick 0 0" "press circle" "wait 0.3"
if [[ $failed != 0 ]]; then
  echo "search and flip: FAILED"
  exit 1
fi
echo "search and flip: all passed"
