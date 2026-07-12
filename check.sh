#!/usr/bin/env bash
# CI-style gate for loom-client. Runs the checks that define "done":
#   1. clang-format --dry-run --Werror        (formatting; skipped if absent)
#   2. cmake configure + build (host-tools preset: proto + core + tests + adapter)
#   3. ctest                                   (doctest unit tests)
#   4. vector-check <vector_adapter> spec/vectors  (all conformance vectors)
#
# The conformance harness and the vectors both come from the pinned spec
# submodule, so this script is self-contained within the client repo. The SDL
# and Quest targets need their SDKs (msquic/SDL2, NDK) and are built separately;
# this gate stays hermetic and toolchain-light so it runs anywhere.
set -euo pipefail
cd "$(dirname "$0")"

echo "== [1/4] clang-format check =="
clang_format="$(command -v clang-format || true)"
[ -n "$clang_format" ] || clang_format="$(ls /opt/homebrew/opt/llvm/bin/clang-format 2>/dev/null || true)"
[ -n "$clang_format" ] || clang_format="$(xcrun --find clang-format 2>/dev/null || true)"
if [ -n "$clang_format" ]; then
  # The generated fixture is one machine-emitted line; clang-format would reflow it.
  find proto core sdl tests -type f \( -name '*.hpp' -o -name '*.cpp' \) \
    ! -name 'idr_fixture.hpp' -print0 |
    xargs -0 "$clang_format" --dry-run --Werror --style=file
else
  echo "   clang-format not found — skipping (install llvm to enforce)"
fi

echo "== [2/4] cmake build (host-tools) =="
cmake --preset host-tools >/dev/null
cmake --build --preset host-tools

echo "== [3/4] ctest =="
ctest --preset host-tools

echo "== [4/4] conformance vectors =="
ADAPTER="$(pwd)/build/host-tools/proto/vector_adapter"

# Build the harness from the pinned spec submodule (its own standalone crate).
( cd spec/vector-check && cargo build --quiet )
spec/vector-check/target/debug/vector-check "$ADAPTER" spec/vectors

echo
echo "ALL CHECKS PASSED"
