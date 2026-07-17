#!/usr/bin/env bash
# Build libmsquic.so for Android arm64 and stage it for the Quest gradle build.
#
# msquic is not built inside the gradle/CMake native build the way the SDL client
# builds it: its quictls (OpenSSL) sub-build reads the NDK toolchain from the
# ENVIRONMENT (ANDROID_NDK_ROOT + the toolchain on PATH — see
# reviews/M3.0-spike-r3-msquic-ndk.md), which is awkward to guarantee inside
# AGP's CMake invocation, and building OpenSSL on every gradle run would wreck the
# wireless-adb iteration loop. So it is a one-time, explicit pre-step: run this,
# then `./gradlew assembleDebug` links the result as a prebuilt.
#
# Output (git-ignored, regenerated on demand):
#   quest/third_party/msquic/include/           msquic public headers
#   quest/third_party/msquic/lib/arm64-v8a/libmsquic.so
set -euo pipefail

QUEST_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$QUEST_DIR/third_party/msquic"
ABI=arm64-v8a
API=29                 # matches the SDL msquic build; Quest 3 runs far above it
MSQUIC_TAG=v2.5.9      # keep in lockstep with sdl/CMakeLists.txt

: "${ANDROID_NDK_ROOT:=$HOME/Library/Android/sdk/ndk/28.2.13676358}"
if [ ! -d "$ANDROID_NDK_ROOT" ]; then
  echo "build-msquic-android: NDK not found at $ANDROID_NDK_ROOT" >&2
  echo "  set ANDROID_NDK_ROOT to your NDK (r28c recommended)." >&2
  exit 1
fi

# Reuse the SDL client's msquic checkout if it exists; otherwise clone fresh.
SRC="$QUEST_DIR/../build/sdl/msquic/src/msquic_ext"
WORK="$QUEST_DIR/third_party/msquic-build"
if [ ! -f "$SRC/CMakeLists.txt" ]; then
  SRC="$WORK/msquic-src"
  if [ ! -f "$SRC/CMakeLists.txt" ]; then
    echo "== cloning msquic $MSQUIC_TAG =="
    git clone --depth 1 --branch "$MSQUIC_TAG" --recurse-submodules \
      https://github.com/microsoft/msquic.git "$SRC"
  fi
fi

# Both env requirements from the M3.0 spike: ANDROID_NDK_ROOT exported, and the
# NDK toolchain on PATH (quictls' Configure resolves the compiler via `which`).
export ANDROID_NDK_ROOT
export PATH="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/darwin-x86_64/bin:$PATH"

BUILD="$WORK/$ABI"
echo "== configuring msquic ($ABI, android-$API) =="
cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="android-$API" \
  -DANDROID_NDK="$ANDROID_NDK_ROOT" \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUIC_TLS_LIB=quictls \
  -DQUIC_BUILD_TOOLS=OFF -DQUIC_BUILD_TEST=OFF -DQUIC_BUILD_PERF=OFF \
  -DQUIC_ENABLE_LOGGING=OFF

echo "== building =="
cmake --build "$BUILD" -j

echo "== staging into third_party/msquic =="
mkdir -p "$OUT/include" "$OUT/lib/$ABI"
cp -R "$SRC/src/inc/." "$OUT/include/"
cp "$(find "$BUILD" -name libmsquic.so | head -1)" "$OUT/lib/$ABI/"

echo "done: $OUT/lib/$ABI/libmsquic.so"
