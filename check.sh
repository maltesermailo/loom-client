#!/usr/bin/env bash
# CI-style gate for loom-client. Runs the checks that define "done":
#   1. cmake configure + build (host-tools preset: proto + core + tests + adapter)
#   2. ctest                                   (doctest unit tests)
#   3. vector-check <vector_adapter> spec/vectors  (all conformance vectors)
#
# The conformance harness and the vectors both come from the pinned spec
# submodule, so this script is self-contained within the client repo. The SDL
# and Quest targets need their SDKs (msquic/SDL2, NDK) and are built separately;
# this gate stays hermetic and toolchain-light so it runs anywhere.
set -euo pipefail
cd "$(dirname "$0")"

echo "== [1/3] cmake build (host-tools) =="
cmake --preset host-tools >/dev/null
cmake --build --preset host-tools

echo "== [2/3] ctest =="
ctest --preset host-tools

echo "== [3/3] conformance vectors =="
ADAPTER="$(pwd)/build/host-tools/proto/vector_adapter"

# Build the harness from the pinned spec submodule (its own standalone crate).
( cd spec/vector-check && cargo build --quiet )
spec/vector-check/target/debug/vector-check "$ADAPTER" spec/vectors

echo
echo "ALL CHECKS PASSED"
