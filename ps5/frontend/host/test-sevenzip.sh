#!/usr/bin/env bash
# PS5 port frontend: the 7z reader's tests (fe_sevenzip.c) on a PC (2026-10-16, AI-assisted). Builds the LZMA SDK files it needs (3rdparty/lzma, 14 of them:
# this harness links no libchdr, so LzmaDec.c is built here too) and fe_sevenzip.c with the sanitizers, makes the archives with Python's lzma module
# (make_7z_fixtures.py), and runs sevenzip_check.py: the corpus, cancel, memory bounds, a mutation fuzz, and (SZ7_REAL_PROBES) real 7-Zip headers.
#
#   ps5/frontend/host/test-sevenzip.sh
#   SEVENZIP_FUZZ=2500 SEVENZIP_SEED=4242 ps5/frontend/host/test-sevenzip.sh      a longer fuzz
#   SEVENZIP_BIG=1 ps5/frontend/host/test-sevenzip.sh                              also a ~600 MiB solid archive
#   SZ7_REAL_PROBES=<dir of <label>.sig/.end/.packed/.json> ps5/frontend/host/test-sevenzip.sh
#   SEVENZIP_SDK_FUZZ=1000 ps5/frontend/host/test-sevenzip.sh                      more damaged-stream runs on the archives made with the LZMA SDK's own encoder (default 300)
#   SEVENZIP_LIBFUZZER=300 ps5/frontend/host/test-sevenzip.sh                      afterwards, a coverage-guided libFuzzer run of that many seconds (fuzz7z.cpp;
#                                                                                  SEVENZIP_LIBFUZZER_JOBS=4 workers; needs clang's libFuzzer runtime)
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
work=$(mktemp -d "${TMPDIR:-/tmp}/sevenzip.XXXXXX")
trap 'rm -rf "$work"' EXIT
# SEVENZIP_SAN="-fno-omit-frame-pointer" builds without the sanitizers; "-fsanitize=thread -g" for ThreadSanitizer
san=(${SEVENZIP_SAN:--fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer})

srcs=(7zArcIn 7zBuf 7zCrc 7zCrcOpt 7zDec 7zStream Bcj2 Bra Bra86 BraIA64 CpuArch Delta Lzma2Dec LzmaDec)
mkdir -p "$work/obj" "$work/run"
sobjs=(); pobjs=()
for s in "${srcs[@]}"; do
  # the SDK reads its headers with unaligned 64-bit loads (GetUi64): the alignment check stays off for its objects, as in the other harnesses
  "$CC" -O1 -g -w -DZ7_ST -I"$sdk/include" "${san[@]}" -fno-sanitize=alignment -c "$sdk/src/$s.c" -o "$work/obj/$s.san.o"
  "$CC" -O2 -w -DZ7_ST -I"$sdk/include" -c "$sdk/src/$s.c" -o "$work/obj/$s.plain.o"
  sobjs+=("$work/obj/$s.san.o"); pobjs+=("$work/obj/$s.plain.o")
done
"$CC" -O1 -g -Wall -Wextra -Wno-unused-parameter -DZ7_ST -I"$sdk/include" "${san[@]}" -c "$fe/fe_sevenzip.c" -o "$work/obj/fe_sevenzip.san.o"
"$CC" -O2 -Wall -Wextra -Wno-unused-parameter -DZ7_ST -I"$sdk/include" -c "$fe/fe_sevenzip.c" -o "$work/obj/fe_sevenzip.plain.o"
"$CXX" -std=c++20 -O1 -g -Wall -I"$fe" "${san[@]}" "$here/sevenzip_test.cpp" "$work/obj/fe_sevenzip.san.o" "${sobjs[@]}" -lz -lpthread -o "$work/sevenzip_test.san"
"$CXX" -std=c++20 -O2 -Wall -I"$fe" "$here/sevenzip_test.cpp" "$work/obj/fe_sevenzip.plain.o" "${pobjs[@]}" -lz -lpthread -o "$work/sevenzip_test.plain"

extra=()
[[ -n ${SEVENZIP_BIG:-} ]] && extra+=(--big)
python3 -I -X utf8 "$here/sevenzip_check.py" "$work/sevenzip_test.san" "$work/sevenzip_test.plain" "$work/run" \
  --fuzz "${SEVENZIP_FUZZ:-600}" --seed "${SEVENZIP_SEED:-1}" "${extra[@]}"

# the audit tool (ps5/tools/audit_7z_headers.py) against a local server that serves generated packs with Range support
python3 -I -X utf8 "$here/audit_check.py" "$work/sevenzip_test.plain" "$work/audit"

# (review) archives whose LZMA2 / LZMA streams come from the LZMA SDK's own encoder, the one 7-Zip uses (multithreaded blocks with dictionary resets, uncompressed
# chunks): sdk_encode.c is built with the SDK's encoder files; sdk_corpus_check.py wraps its streams in 7z containers, reads them back with the sanitized reader, and
# damages the packed streams (SEVENZIP_SDK_FUZZ runs, default 300; SEVENZIP_BIG=1 adds a 180 MiB archive with a 64 MiB dictionary)
"$CC" -O2 -w -D_GNU_SOURCE -I"$sdk/include" "$here/sdk_encode.c" "$sdk/src/Lzma2Enc.c" "$sdk/src/LzmaEnc.c" "$sdk/src/LzFind.c" "$sdk/src/LzFindMt.c" \
  "$sdk/src/LzFindOpt.c" "$sdk/src/MtCoder.c" "$sdk/src/MtDec.c" "$sdk/src/7zStream.c" "$sdk/src/Threads.c" "$sdk/src/CpuArch.c" -lpthread -o "$work/sdk_encode"
python3 -I -X utf8 "$here/sdk_corpus_check.py" "$work/sevenzip_test.san" "$work/sdk_encode" "$work/sdk" --fuzz "${SEVENZIP_SDK_FUZZ:-300}" --seed "${SEVENZIP_SEED:-1}" "${extra[@]}"

# libFuzzer: the whole reader (open, scan, extract) on mutated archives whose CRCs are repaired first; any report, crash or leak ends the run with a failure
if [[ -n ${SEVENZIP_LIBFUZZER:-} ]]; then
  fz="$work/libfuzzer"
  mkdir -p "$fz/obj" "$fz/corpus" "$fz/seeds" "$fz/artifacts"
  fl=(-O1 -g -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  fobjs=()
  for s in "${srcs[@]}"; do
    "$CC" "${fl[@]}" -fno-sanitize=alignment -w -DZ7_ST -I"$sdk/include" -c "$sdk/src/$s.c" -o "$fz/obj/$s.o"
    fobjs+=("$fz/obj/$s.o")
  done
  "$CC" "${fl[@]}" -w -DZ7_ST -I"$sdk/include" -c "$fe/fe_sevenzip.c" -o "$fz/obj/fe_sevenzip.o"
  "$CXX" -std=c++20 "${fl[@]}" -fsanitize=fuzzer -I"$fe" "$here/fuzz7z.cpp" "$fz/obj/fe_sevenzip.o" "${fobjs[@]}" -lz -o "$fz/fuzz7z"
  python3 -I -X utf8 "$here/make_7z_fixtures.py" seeds "$fz/seeds" > /dev/null
  jobs=${SEVENZIP_LIBFUZZER_JOBS:-4}
  (cd "$fz" && ./fuzz7z -max_total_time="$SEVENZIP_LIBFUZZER" -jobs="$jobs" -workers="$jobs" -max_len=16384 -timeout=30 -rss_limit_mb=3000 \
    -artifact_prefix="$fz/artifacts/" "$fz/corpus" "$fz/seeds" > "$fz/fuzz.log" 2>&1) || true
  if compgen -G "$fz/artifacts/*" > /dev/null; then
    echo "LIBFUZZER FOUND SOMETHING:"; ls -la "$fz/artifacts"; tail -30 "$fz"/fuzz-*.log | head -80
    cp -r "$fz/artifacts" "${TMPDIR:-/tmp}/sevenzip-libfuzzer-artifacts" && echo "(kept in ${TMPDIR:-/tmp}/sevenzip-libfuzzer-artifacts)"
    exit 1
  fi
  echo "LIBFUZZER: no finding in ${SEVENZIP_LIBFUZZER} s with $jobs workers ($(grep -h -o '^Done [0-9]* runs' "$fz"/fuzz-*.log 2>/dev/null | awk '{s+=$2} END{print s+0}') executions, $(grep -h -o 'cov: [0-9]*' "$fz"/fuzz-*.log 2>/dev/null | awk '{if ($2>m) m=$2} END{print m+0}') edges)"
fi
