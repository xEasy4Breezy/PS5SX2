#!/usr/bin/env bash
# PS5 port: the folders the BIOS lookup searches (orbis-shims/OrbisBiosRoots.h) on a PC. 2026-10-16 (AI-assisted).
#   ps5/coreorbis/tests/biosroots/test-biosroots.sh
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
shims=$(cd -- "$here/../../orbis-shims" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
CXX=${CXX:-clang++-18}
"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -I"$shims" "$here/biosroots_test.cpp" -o "$work/biosroots_test"
"$work/biosroots_test" "$work/tree"
