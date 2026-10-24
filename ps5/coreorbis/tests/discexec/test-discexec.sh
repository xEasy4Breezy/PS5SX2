#!/usr/bin/env bash
# PS5 port: when the app honours the disc helper's exec signal (orbis-shims/OrbisDiscExec.h) on a PC. 2.11 (dumpxmb, AI-assisted).
#   ps5/coreorbis/tests/discexec/test-discexec.sh           the test
#   ps5/coreorbis/tests/discexec/test-discexec.sh mutants   the test, then five hand-made wrong versions of the header, each of which must make it FAIL
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
shims="$here/../../orbis-shims"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
CXX=${CXX:-clang++-18}
FLAGS="-std=c++20 -O1 -g -Wall -Wextra -Werror"

# $1 = folder holding OrbisDiscExec.h, $2 = output
build() { "$CXX" $FLAGS -I"$1" "$here/test_discexec.cpp" -o "$2"; }

build "$shims" "$work/test_discexec"
"$work/test_discexec"

[ "${1:-}" = mutants ] || exit 0

# ---- mutants: the header is copied, one rule is broken in the copy (the repo file is never touched), the test must notice
mutate() { # $1 = name, $2 = what to break
	local dir="$work/m-$1"
	mkdir -p "$dir"
	cp "$shims/OrbisDiscExec.h" "$dir/OrbisDiscExec.h"
	python3 -I - "$dir/OrbisDiscExec.h" "$2" <<'PY'
import sys
path, kind = sys.argv[1], sys.argv[2]
text = open(path, encoding="utf-8").read()
old = text
starting = "\tif (in.game_starting)\n\t\treturn ExecVerdict::IgnoreGameStarting;\n"
tile = "\tif (in.is_tile)\n\t\treturn ExecVerdict::IgnoreTile;\n"
discexec = "\tif (in.flag_discexec)\n\t\treturn ExecVerdict::Honor;\n"
nodiscexec = "\tif (in.flag_nodiscexec)\n\t\treturn ExecVerdict::IgnoreFlag;\n"
if kind == "no_game_starting_branch":
    assert starting in text
    text = text.replace(starting, "")
elif kind == "tile_after_discexec":
    assert tile in text and discexec in text
    text = text.replace(tile, "")
    text = text.replace(discexec, discexec + tile)
elif kind == "stale_ge":
    assert "now - mtime > max_age" in text
    text = text.replace("now - mtime > max_age", "now - mtime >= max_age")
elif kind == "remove_only_exec":
    assert "\tunlink(launch_txt.c_str());\n" in text
    text = text.replace("\tunlink(launch_txt.c_str());\n", "\t(void)launch_txt;\n")
elif kind == "nodiscexec_before_discexec":
    assert discexec in text and nodiscexec in text
    text = text.replace(discexec, "").replace(nodiscexec, nodiscexec + discexec)
else:
    raise SystemExit("unknown mutant " + kind)
assert text != old, "the mutation changed nothing"
open(path, "w", encoding="utf-8").write(text)
PY
	local out rc
	# (the compile-time asserts are off here: the literal table has to catch the wrong version on its own)
	if ! out=$("$CXX" $FLAGS -DDISCEXEC_NO_STATIC_ASSERT -I"$dir" "$here/test_discexec.cpp" -o "$dir/t" 2>&1); then
		echo "mutant $1: did not compile:"; echo "$out" | head -10
		return 1
	fi
	# and with them on, the wrong version must not get through the compile either, or must fail the run
	local with_assert="compiles (the table decides)"
	if ! "$CXX" $FLAGS -I"$dir" "$here/test_discexec.cpp" -o "$dir/t2" >/dev/null 2>&1; then
		with_assert="also stopped by a static_assert"
	fi
	set +e
	out=$("$dir/t" 2>&1)
	rc=$?
	set -e
	if [ $rc -eq 0 ]; then
		echo "mutant $1: SURVIVED (the test passed with the rule broken)"
		return 1
	fi
	echo "mutant $1: killed by the table ($(echo "$out" | grep -c '^FAIL') failing checks); with the static_asserts on it $with_assert"
}

bad=0
for m in no_game_starting_branch tile_after_discexec stale_ge remove_only_exec nodiscexec_before_discexec; do
	mutate "$m" "$m" || bad=1
done
if [ $bad -ne 0 ]; then
	echo "mutant check FAILED"
	exit 1
fi
echo "mutant check: all five wrong versions were caught"
