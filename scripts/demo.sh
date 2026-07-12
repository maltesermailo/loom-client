#!/usr/bin/env bash
# Build and launch loom-sdl for the loopback demo. Args: [host] [port]. Runs in
# the foreground; the superrepo scripts/demo.sh backgrounds it and captures its
# stdout overlay lines. Needs the sdl toolchain (msquic/SDL2/ffmpeg), so it is
# separate from the hermetic check.sh.
set -euo pipefail

cd "$(dirname "$0")/.."  # client repo root

cmake --preset sdl >/dev/null
cmake --build --preset sdl

# msquic is built as an ExternalProject; put its dylib on the loader path.
msquic_dylib="$(find build/sdl/msquic/install -name 'libmsquic*.dylib' | head -1)"
if [ -z "$msquic_dylib" ]; then
  echo "demo: libmsquic not found under build/sdl/msquic/install" >&2
  exit 1
fi

exec env DYLD_LIBRARY_PATH="$(dirname "$msquic_dylib")" build/sdl/sdl/loom-sdl "$@"
