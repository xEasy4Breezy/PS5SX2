#!/usr/bin/env bash
# PS5 port frontend: LaunchBox art in the cover service (launchbox_test.cpp). 2.11, AI-assisted. Needs Python's Pillow for the test pictures.
# Also the back covers' protocol (swordpdf: "back covers are not loading instantly"): who asks and when (files first, the network on permission), who
# answers (inside the painting pass), the neighbours' backs fetched ahead, the order of the downloads, the pauses that end, the card and its note.
#
#   ps5/frontend/host/test-launchbox.sh
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
pcsx2=$(cd -- "$fe/../.." && pwd)
work=${FE_TEST_DIR:-$here/obj/launchbox-test}
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
rm -rf "$work"
mkdir -p "$work/fix"
python3 - "$work/fix" <<'PY'
import sys
from PIL import Image, ImageDraw
d = sys.argv[1]
def front(path, colour):
    im = Image.new("RGB", (300, 420), colour)
    ImageDraw.Draw(im).rectangle([20, 20, 280, 120], fill=(250, 250, 250))
    im.save(path, "JPEG", quality=85)
front(d + "/front_a.jpg", (30, 90, 160))
front(d + "/front_b.jpg", (150, 40, 40))
front(d + "/back.jpg", (60, 60, 60))
Image.new("RGB", (60, 840), (20, 120, 60)).save(d + "/spine.jpg", "JPEG", quality=85)
Image.new("RGB", (5000, 50), (10, 10, 10)).save(d + "/huge.jpg", "JPEG", quality=70)   # wider than the 4096-pixel limit
disc = Image.new("RGBA", (700, 700), (0, 0, 0, 0))
dr = ImageDraw.Draw(disc)
dr.ellipse([0, 0, 699, 699], fill=(200, 200, 215, 255))
dr.ellipse([250, 250, 450, 450], fill=(0, 0, 0, 0))
disc.save(d + "/disc.png", "PNG")
PY
"$CC" -O1 -c "$pcsx2/ps5/third_party/lz4/lz4.c" -o "$work/lz4.o"
"$CC" -O1 -I"$pcsx2/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$work/chd_stub.o"
"$CXX" -std=c++20 -O1 -g -Wall -Wno-unused-function -I"$fe" -I"$pcsx2/3rdparty/vulkan/include" -I"$pcsx2/3rdparty/libchdr/include" \
  -I"$pcsx2/ps5/third_party/lz4" "$here/launchbox_test.cpp" "$fe/fe_games.cpp" "$fe/fe_covers.cpp" "$fe/fe_text.cpp" "$fe/fe_i18n.cpp" \
  "$fe/fe_settings.cpp" "$work/lz4.o" "$work/chd_stub.o" -o "$work/launchbox_test" -lz -lpthread
FE_RES="$pcsx2/bin/resources" "$work/launchbox_test" "$work/fix" "$work/run"
