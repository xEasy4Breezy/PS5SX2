#!/usr/bin/env bash
# PS5 port frontend: builds host/sevenzip_test, the 7z reader's command-line driver (fe_sevenzip.c + the LZMA SDK files it needs + sevenzip_test.cpp), WITHOUT the sanitizers, for
# ps5/tools/audit_7z_headers.py (which asks it what the app will make of a pack) and for trying a .7z by hand:
#
#   ps5/frontend/host/build-sevenzip-driver.sh [<output file>]          default: ps5/frontend/host/sevenzip_test
#   ps5/frontend/host/sevenzip_test <a.7z> --scan-only                  what the header says; without --scan-only it also decodes everything and checks every CRC (--out DIR to write the files)
#
# Needs clang (18 or later) and zlib's headers. test-sevenzip.sh is the test (with the sanitizers).
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
pcsx2=$(cd -- "$fe/../.." && pwd)
sdk="$pcsx2/3rdparty/lzma"
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
out=${1:-$here/sevenzip_test}
work=$(mktemp -d "${TMPDIR:-/tmp}/sevenzip-driver.XXXXXX")
trap 'rm -rf "$work"' EXIT
objs=()
for s in 7zArcIn 7zBuf 7zCrc 7zCrcOpt 7zDec 7zStream Bcj2 Bra Bra86 BraIA64 CpuArch Delta Lzma2Dec LzmaDec; do
  "$CC" -O2 -w -DZ7_ST -I"$sdk/include" -c "$sdk/src/$s.c" -o "$work/$s.o"
  objs+=("$work/$s.o")
done
"$CC" -O2 -Wall -Wextra -Wno-unused-parameter -DZ7_ST -I"$sdk/include" -c "$fe/fe_sevenzip.c" -o "$work/fe_sevenzip.o"
"$CXX" -std=c++20 -O2 -Wall -I"$fe" "$here/sevenzip_test.cpp" "$work/fe_sevenzip.o" "${objs[@]}" -lz -lpthread -o "$out"
echo "built $out"
