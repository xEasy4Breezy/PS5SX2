#!/bin/bash
# Host test of the relaunch payload's app side (ProsperoRelaunch.cpp) against a fake ELF loader that RUNS what it receives: the real request, written
# over the marker of the host build of relaunch.c (with the stand-ins of relaunch_stub.c), goes through the real payload from end to end. The loader's
# port is picked at random (a free one), so this does not collide with test-ime-relay.sh or another run. Takes about 40 s. 2.11 (AI-assisted).
#   CC=clang-18 CXX=clang++-18 ps5/coreorbis/relaunch/test-relaunch.sh
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
CC=${CC:-clang}
CXX=${CXX:-clang++}
${CC} -std=gnu11 -O1 -g -Wall -Wextra -Werror -DRELAUNCH_HOST_TEST -o "$work/relaunch_host.elf" "$here/relaunch.c" "$here/relaunch_stub.c"
# a free port (the console's loader is 9021; the test's own is another so that nothing else on this machine is in the way)
port=""
for try in 1 2 3 4 5 6 7 8; do
	cand=$((20000 + RANDOM % 20000))
	if python3 -I - "$cand" <<'PY'
import socket, sys
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    s.bind(("127.0.0.1", int(sys.argv[1])))
except OSError:
    sys.exit(1)
PY
	then
		port=$cand
		break
	fi
done
[ -n "$port" ] || { echo "no free port found"; exit 2; }
common=(-std=c++20 -O1 -g -Wall -Wextra -I"$here/../include-orbis" -DORBIS_RELAUNCH_LOADER_PORT="$port" -pthread)
${CXX} "${common[@]}" -DORBIS_RELAUNCH_ELF="\"$work/relaunch_host.elf\"" -o "$work/relaunch_test" "$here/relaunch_test.cpp"
${CXX} "${common[@]}" -Wno-unused-function -DRELAUNCH_TEST_NOMARKER -DORBIS_RELAUNCH_ELF="\"$(readlink -f /bin/true)\"" -o "$work/relaunch_test_nomarker" "$here/relaunch_test.cpp"
# the same payload padded to 24 MB: the send to a loader that never reads really fills the buffers
cp "$work/relaunch_host.elf" "$work/relaunch_big.elf"
head -c 25165824 /dev/zero >> "$work/relaunch_big.elf"
${CXX} "${common[@]}" -Wno-unused-function -DRELAUNCH_TEST_STALL -DORBIS_RELAUNCH_ELF="\"$work/relaunch_big.elf\"" -o "$work/relaunch_test_stall" "$here/relaunch_test.cpp"
mkdir -p "$work/run"
RELAUNCH_TEST_DIR="$work/run" "$work/relaunch_test_nomarker"
RELAUNCH_TEST_DIR="$work/run" "$work/relaunch_test_stall"
RELAUNCH_TEST_DIR="$work/run" "$work/relaunch_test"
