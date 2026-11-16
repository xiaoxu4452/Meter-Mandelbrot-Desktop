#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# One-shot Linux build of the `mset` command-line engine.
#
#   ./build-linux.sh              AVX2 on, g++
#   AVX2=0 ./build-linux.sh       portable build (no AVX2 requirement)
#   CXX=clang++ ./build-linux.sh  use Clang
#
# Optional runtime dependency: GMP's shared library (Debian/Ubuntu
# `libgmp10`, dev headers in `libgmp-dev`).  It is loaded with dlopen at
# runtime, so the binary builds and runs without it via the built-in BigFloat
# backend (set MB_GMP=0 to force that path).
# ---------------------------------------------------------------------------
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-g++}"
AVX2="${AVX2:-1}"
OUTDIR=build-linux
mkdir -p "$OUTDIR"

flags=(-O3 -std=c++20 -Wall -Wextra -pthread -Isrc)
if [[ "$AVX2" == "1" ]]; then
  flags+=(-mavx2 -mfma)
fi

sources=(
  src/bigfloat.cpp
  src/mathx/bigmath.cpp
  src/mathx/expr.cpp
  src/mathx/families.cpp
  src/gmp_bridge.cpp
  src/dive.cpp
  src/reference.cpp
  src/deep.cpp
  src/render.cpp
  src/palette.cpp
  src/image.cpp
  src/workspace.cpp
  src/port/registry.cpp
  src/main.cpp
)

echo "[build] $CXX  AVX2=$AVX2"
"$CXX" "${flags[@]}" "${sources[@]}" -o "$OUTDIR/mset" -ldl -lpthread
echo "[build] -> $OUTDIR/mset"
"$OUTDIR/mset" info
