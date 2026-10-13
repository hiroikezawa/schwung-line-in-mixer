#!/usr/bin/env bash
# Build the Line In Mixer audio FX.
#
#   scripts/build.sh [arm64|native]
#
#   arm64   (default) Move .so + install tarball in dist/
#   native  desktop .so in build/native/ for the tests
#
# SCHWUNG_ROOT must point at a Schwung checkout (only its headers are used);
# default ../schwung. Build against the headers of the host version the Move runs.
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Hiro Ikezawa
set -euo pipefail
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
SCHWUNG_ROOT="${SCHWUNG_ROOT:-$PROJECT/../schwung}"
cd "$PROJECT"
ID=line-in-mixer

ARCH="${1:-arm64}"
case "$ARCH" in
  arm64)  CC="${CROSS_PREFIX:-aarch64-linux-gnu-}gcc"; TUNE=(-mcpu=cortex-a72); OUT="$PROJECT/dist/$ID" ;;
  native) CC="${CC:-gcc}"; TUNE=(); OUT="${LIM_NATIVE_DIR:-$PROJECT/build/native}/$ID" ;;
  *) echo "usage: $0 [arm64|native]" >&2; exit 2 ;;
esac
[[ -f "$SCHWUNG_ROOT/src/host/audio_fx_api_v2.h" ]] || {
  echo "Schwung source not found at $SCHWUNG_ROOT (set SCHWUNG_ROOT)" >&2; exit 1; }

mkdir -p build/gen
python3 scripts/metadata.py build/gen/line_in_mixer_meta.h
rm -rf "$OUT"; mkdir -p "$OUT"
read -r -a EXTRA <<< "${LIM_EXTRA_CFLAGS:-}"
# No -ffast-math: the isfinite() guards in the parser must survive.
"$CC" -std=c11 -O2 -fPIC -shared -fno-math-errno -fvisibility=hidden "${TUNE[@]}" "${EXTRA[@]}" \
  -Wall -Wextra -Werror -Wl,-z,defs \
  -I"$SCHWUNG_ROOT/src" -Ibuild/gen src/line_in_mixer.c -o "$OUT/$ID.so" -lm
cp module.json help.json "$OUT/"
if [[ "$ARCH" == arm64 ]]; then
  python3 scripts/check_abi.py "$OUT/$ID.so"
  cp README.md LICENSE THIRD_PARTY_LICENSES.md "$OUT/"
  tar -czf "dist/$ID-module.tar.gz" -C dist "$ID"
  echo "-> dist/$ID-module.tar.gz"
else
  echo "-> ${OUT#$PROJECT/}/$ID.so"
fi
