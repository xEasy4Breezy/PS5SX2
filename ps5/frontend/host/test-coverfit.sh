#!/usr/bin/env bash
# PS5 port frontend: the cover fit (fe_coverfit.h, CoverService::Decode with a CoverFit): the plans on a grid of sizes (coverfit_test.cpp, no
# pictures), Decode / DecodeWithAlpha on generated pictures and the service with the fit on and off (coverfit_decode_test.cpp). 2.11, AI-assisted.
# Needs Python's Pillow for the pictures (plain python3, as test-launchbox.sh).
#
#   bash ps5/frontend/host/test-coverfit.sh
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
pcsx2=$(cd -- "$fe/../.." && pwd)
work=${FE_TEST_DIR:-$here/obj/coverfit-test}
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}

# Every Decode that makes a front, a back or a spine texture must pass a fit (the parameter defaults to none, so a call site added later without one
# would silently keep stretching), and through FitFor, which honours the flag flags/nocoverfit. Prefetch's validity probe (it only asks whether the bytes are
# a picture) is the exception here, and the achievement badge lives in fe_app.cpp. Any call, whatever its arguments are called: the definition is skipped.
unfitted=$(grep -nE '(^|[^A-Za-z_])Decode\(' "$fe/fe_covers.cpp" | grep -v 'CoverService::Decode(' | grep -v 'FitFor(' | grep -v 'probe' || true)
if [ -n "$unfitted" ]; then
  echo "a cover Decode without a CoverFit:"
  echo "$unfitted"
  exit 1
fi

rm -rf "$work"
mkdir -p "$work/fix"
python3 "$here/make_fit_fixtures.py" "$work/fix" >/dev/null
"$CXX" -std=c++20 -O1 -g -Wall -I"$fe" "$here/coverfit_test.cpp" -o "$work/coverfit_test"
"$work/coverfit_test" | tail -1
"$CC" -O1 -c "$pcsx2/ps5/third_party/lz4/lz4.c" -o "$work/lz4.o"
"$CC" -O1 -I"$pcsx2/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$work/chd_stub.o"
"$CXX" -std=c++20 -O1 -g -Wall -Wno-unused-function -I"$fe" -I"$pcsx2/3rdparty/vulkan/include" -I"$pcsx2/3rdparty/libchdr/include" \
  -I"$pcsx2/ps5/third_party/lz4" "$here/coverfit_decode_test.cpp" "$fe/fe_games.cpp" "$fe/fe_covers.cpp" "$fe/fe_text.cpp" "$fe/fe_i18n.cpp" \
  "$fe/fe_settings.cpp" "$work/lz4.o" "$work/chd_stub.o" -o "$work/coverfit_decode_test" -lz -lpthread
FE_RES="$pcsx2/bin/resources" "$work/coverfit_decode_test" "$work/fix" "$work/run"
