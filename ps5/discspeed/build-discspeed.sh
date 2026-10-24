#!/bin/bash
# Builds the three disc drive speed test ELFs from disc_speed.c (2.11, AI-assisted):
#   PS5SX2 Disc Drive Test - internal.elf    the PS5's own disc drive
#   PS5SX2 Disc Drive Test - external.elf    any other DVD/CD/BD drive (USB)
#   PS5SX2 Disc Drive Test - all.elf         both
# Needs the PS5 payload SDK (prospero-clang) and LLVM for its lld, as the disc helper does (tile6/build_disc.sh). Paths can be given in the environment:
#   PS5_PAYLOAD_SDK  (default ~/rps5/toolchain/ps5-payload-sdk)    LLVM_BIN (default ~/rps5/toolchain/LLVM-22.1.8-Linux-X64/bin)
#   OUT_DIR          (default ~/rps5/discspeed-build)
# (With this toolchain the ELF header's entry point is 0x0, as in the disc helper ELFs that run on consoles: the loader finds the start another way. The
# ELFs of the app's own build, with another toolchain, need the check.)
set -e
LLVM_BIN=${LLVM_BIN:-$HOME/rps5/toolchain/LLVM-22.1.8-Linux-X64/bin}
export PATH=$LLVM_BIN:$HOME/.local/bin:$PATH
export LLVM_CONFIG=$LLVM_BIN/llvm-config
export LD_LIBRARY_PATH=$HOME/rps5/shimlib:${LD_LIBRARY_PATH:-}
SDK=${PS5_PAYLOAD_SDK:-$HOME/rps5/toolchain/ps5-payload-sdk}
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT=${OUT_DIR:-$HOME/rps5/discspeed-build}
mkdir -p "$OUT"
sed 's/\r$//' "$HERE/disc_speed.c" > "$OUT/disc_speed.c"
cd "$OUT"
build() { # <scope> <name>
  "$SDK/bin/prospero-clang" -std=gnu11 -O2 -Wall -Wextra -Werror -DSCOPE="$1" -o "$2" disc_speed.c 2>&1 | grep -v 'no version information' || true
  [ -f "$2" ] || { echo "build of $2 failed"; exit 1; }
  echo "built $2 ($(stat -c %s "$2") bytes)"
}
build 1 "PS5SX2 Disc Drive Test - internal.elf"
build 2 "PS5SX2 Disc Drive Test - external.elf"
build 3 "PS5SX2 Disc Drive Test - all.elf"
ls -la "$OUT"/*.elf
