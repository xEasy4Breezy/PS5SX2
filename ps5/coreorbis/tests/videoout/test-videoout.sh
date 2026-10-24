#!/usr/bin/env bash
# PS5SX2 (2.11, AI-assisted): builds and runs the PC tests of the app's 119.88 Hz VideoOut switch (OrbisVideoOutMode.h/.cpp) and, with
# the frame generation tests (tests/fgpacing), the display rate and PAL 50 -> 100 cadence checks.
#   ps5/coreorbis/tests/videoout/test-videoout.sh          (about 40 s: the glue test waits out the 5 s settles for real)
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tests=$(cd -- "$here/.." && pwd)
coreorbis=$(cd -- "$here/../.." && pwd)
shims="$coreorbis/orbis-shims"
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
CXX=${CXX:-g++}
cxx() { # <out> <sources/flags...>
  local out=$1; shift
  "$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined "$@" -o "$out" 2> /dev/null ||
    "$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread "$@" -o "$out"
}
run() { # <source> <name>
  cxx "$obj/$2" -I"$shims" "$1"
  "$obj/$2" | tee -a "$obj/out.txt"
}
run "$here/test_videoout_mode.cpp" test_videoout_mode
run "$tests/fgpacing/test_display_hz.cpp" test_display_hz
run "$tests/fgpacing/test_fg_pacing.cpp" test_fg_pacing

# The glue (flags, thread, marker file) against a fake VideoOut: one process a scenario, the slow ones side by side.
cxx "$obj/glue" -I"$coreorbis/include-orbis" -I"$shims" "$here/test_videoout_glue.cpp" "$shims/OrbisVideoOutMode.cpp"
cxx "$obj/glue_api" -DTEST_DRIVER_API -I"$coreorbis/include-orbis" -I"$shims" "$here/test_videoout_glue.cpp" "$shims/OrbisVideoOutMode.cpp"
pids=()
scenarios=(default fg_off kill radv switch restore_early slow refused not_offered open_fail boot_marker boot_marker_fail probe)
for s in "${scenarios[@]}"; do
  mkdir -p "$obj/dir-$s"
  ("$obj/glue" "$s" "$obj/dir-$s" > "$obj/glue-$s.txt" 2>&1 || true) &
  pids+=($!)
done
mkdir -p "$obj/dir-driver_api"
"$obj/glue_api" driver_api "$obj/dir-driver_api" > "$obj/glue-driver_api.txt" 2>&1 || true
wait "${pids[@]}"
for s in "${scenarios[@]}" driver_api; do
  cat "$obj/glue-$s.txt" | tee -a "$obj/out.txt"
done
! grep -q '^FAIL' "$obj/out.txt"
test "$(grep -c '^PASS' "$obj/out.txt")" -eq $((3 + ${#scenarios[@]} + 1))
echo "ALL PASSED"
