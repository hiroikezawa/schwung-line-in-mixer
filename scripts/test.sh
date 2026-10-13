#!/usr/bin/env bash
# Every test, then the Move build:
#   1. the .so through the real Schwung audio FX v2 ABI     tests/test_fx.c
#      (and again under AddressSanitizer + UBSan)
#   2. Schwung's own page planner / contract validator     tests/test_ui.mjs
#   3. ARM64 build + ABI check (glibc, both exports)
#   4. feedback guard with a fake /schwung-control (desktop only)
#
# SCHWUNG_ROOT: a Schwung source tree matching the device's host version
# (default ../schwung). `git archive v1.7.3 src` of the Schwung repo is enough.
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Hiro Ikezawa
set -euo pipefail
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
export SCHWUNG_ROOT="${SCHWUNG_ROOT:-$PROJECT/../schwung}"
cd "$PROJECT"
T=build/tests; mkdir -p "$T"

echo "== native build"
bash scripts/build.sh native

echo "== v2 ABI"
"${CC:-gcc}" -std=c11 -D_DEFAULT_SOURCE -O2 -g -Wall -Wextra -Werror -I"$SCHWUNG_ROOT/src" tests/test_fx.c -o $T/test_fx -ldl -lm
$T/test_fx build/native/line-in-mixer/line-in-mixer.so
$T/test_fx build/native/line-in-mixer/line-in-mixer.so cpu

echo "== feedback guard (fake /dev/shm/schwung-control)"
$T/test_fx build/native/line-in-mixer/line-in-mixer.so guard | tail -1

echo "== sanitizers (ASan + UBSan)"
SAN=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
LIM_EXTRA_CFLAGS="${SAN[*]} -g -O1" LIM_NATIVE_DIR="$PROJECT/build/asan" bash scripts/build.sh native >/dev/null
"${CC:-gcc}" -std=c11 -D_DEFAULT_SOURCE -O1 -g "${SAN[@]}" -I"$SCHWUNG_ROOT/src" tests/test_fx.c -o $T/test_fx_asan -ldl -lm
ASAN_OPTIONS=detect_leaks=1 $T/test_fx_asan build/asan/line-in-mixer/line-in-mixer.so | tail -1

echo "== Schwung page planner / contract validator"
if command -v node >/dev/null 2>&1; then
  node tests/test_ui.mjs "$SCHWUNG_ROOT" "$PROJECT"
elif [[ -x "/mnt/c/Program Files/nodejs/node.exe" ]]; then
  "/mnt/c/Program Files/nodejs/node.exe" "$(wslpath -w tests/test_ui.mjs)" "$(wslpath -w "$SCHWUNG_ROOT")" "$(wslpath -w "$PROJECT")"
else
  echo "SKIP: Node.js not found (UI contract not checked)" >&2
fi

echo "== ARM64"
if command -v "${CROSS_PREFIX:-aarch64-linux-gnu-}gcc" >/dev/null 2>&1; then
  bash scripts/build.sh arm64
  "${CROSS_PREFIX:-aarch64-linux-gnu-}gcc" -std=c11 -D_DEFAULT_SOURCE -O2 -I"$SCHWUNG_ROOT/src" tests/test_fx.c -o $T/test_fx_arm64 -ldl -lm
  echo "  device test: copy $T/test_fx_arm64 + dist/line-in-mixer/line-in-mixer.so to the Move, run: test_fx_arm64 line-in-mixer.so [cpu]"
else
  echo "SKIP: no aarch64 cross compiler" >&2
fi
echo "ALL TESTS PASSED"
