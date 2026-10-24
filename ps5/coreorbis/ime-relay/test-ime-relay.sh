#!/bin/bash
# Host test of the keyboard relay's app side (ProsperoImeRelay.cpp) against a fake ELF loader on 127.0.0.1:9021.
# Needs the payload built once (make -f Makefile.vk obj-vk/ime-relay/ime_relay.elf, in ps5/coreorbis); any file that holds the
# request marker would do for the embedded payload.
set -e
here=$(cd "$(dirname "$0")" && pwd)
elf=${IME_RELAY_ELF:-$here/../obj-vk/ime-relay/ime_relay.elf}
[ -f "$elf" ] || { echo "no payload at $elf (build it first)"; exit 2; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
${CXX:-clang++} -std=c++20 -O1 -g ${IME_SAN:+-fsanitize=$IME_SAN} -Wall -Wextra -I"$here/../include-orbis" \
	-DORBIS_IME_RELAY_ELF="\"$(realpath "$elf")\"" -o "$work/ime_relay_test" "$here/ime_relay_test.cpp" -pthread
IME_TEST_DIR="$work" "$work/ime_relay_test"
