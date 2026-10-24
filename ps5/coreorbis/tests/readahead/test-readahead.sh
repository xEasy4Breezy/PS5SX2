#!/usr/bin/env bash
# PS5SX2 (2.11, AI-assisted): builds and runs the PC test of the disc read-ahead (OrbisDiscReadAhead.h).
#   ps5/coreorbis/tests/readahead/test-readahead.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
CXX=${CXX:-g++}
"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$root/ps5/coreorbis/orbis-shims" "$here/test_readahead.cpp" -o "$obj/test_readahead" ||
  "$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -I"$root/ps5/coreorbis/orbis-shims" "$here/test_readahead.cpp" -o "$obj/test_readahead"
"$obj/test_readahead" | tee "$obj/out.txt"
grep -q "^ALL PASSED" "$obj/out.txt"
