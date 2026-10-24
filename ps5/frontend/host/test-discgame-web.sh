#!/usr/bin/env bash
# PS5 port frontend (2.11, AI-assisted): the settings page for a game that plays from a disc drive (discgame_web_test.cpp): the
# page lists it as the running game, and its settings, patches and cheats are that game's, not the first game of the list's.
#
#   ps5/frontend/host/test-discgame-web.sh
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cxx=${CXX:-$(command -v clang++-18 || command -v clang++)}
cc=${CC:-$(command -v clang-18 || command -v clang)}
"$cc" -O1 -I"$root/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$work/chd.o"
"$cc" -O1 -c "$root/ps5/third_party/lz4/lz4.c" -o "$work/lz4.o"
"$cxx" -std=c++20 -O1 -g -Wall -Wno-unused-function ${FE_SANITIZE:+-fsanitize=$FE_SANITIZE} -ffunction-sections -fdata-sections \
  -I"$root/3rdparty/libchdr/include" -I"$root/ps5/third_party/lz4" \
  -I"$root/3rdparty/vulkan/include" \
  "$here/discgame_web_test.cpp" "$root/ps5/frontend/fe_web.cpp" \
  "$root/ps5/frontend/fe_settings.cpp" "$root/ps5/frontend/fe_games.cpp" \
  "$root/ps5/frontend/fe_covers.cpp" "$root/ps5/frontend/fe_text.cpp" "$root/ps5/frontend/fe_i18n.cpp" \
  "$work/chd.o" "$work/lz4.o" -Wl,--gc-sections -lz -lpthread -o "$work/discgame-test"
"$work/discgame-test" "$work/data"
echo "PASS: a game played from a disc drive is the page's running game, with its own settings file"
