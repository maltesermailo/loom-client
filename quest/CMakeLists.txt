cmake_minimum_required(VERSION 3.24)
project(loom_quest LANGUAGES C CXX)

# Invoked by Gradle (AGP externalNativeBuild), never standalone: find_package
# below resolves against the prefab-exported OpenXR AAR, which only exists inside
# a Gradle build. See README.md for the build/deploy commands.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenXR REQUIRED CONFIG)

# The shared, vector-proven client layers: proto (wire) + core (session, clock,
# VideoReceiver). Built here as subdirectories of the Quest native build so the
# Quest reuses them untouched. LOOM_BUILD_TESTS stays off (no doctest here).
set(LOOM_BUILD_TESTS OFF)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../proto ${CMAKE_BINARY_DIR}/loom_proto)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_BINARY_DIR}/loom_core)

# msquic, prebuilt for Android by scripts/build-msquic-android.sh (see README).
# Linked as a prebuilt rather than built here — its quictls sub-build needs NDK
# env that is awkward inside gradle's CMake, and rebuilding it every run would
# ruin the wireless-adb loop.
set(MSQUIC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/msquic)
if(NOT EXISTS ${MSQUIC_DIR}/lib/${ANDROID_ABI}/libmsquic.so)
  message(FATAL_ERROR
    "libmsquic.so missing — run quest/scripts/build-msquic-android.sh first.")
endif()
add_library(msquic SHARED IMPORTED)
set_target_properties(msquic PROPERTIES
  IMPORTED_LOCATION ${MSQUIC_DIR}/lib/${ANDROID_ABI}/libmsquic.so
  INTERFACE_INCLUDE_DIRECTORIES ${MSQUIC_DIR}/include)

# NativeActivity's glue lives in the NDK sources, not in a prebuilt library.
add_library(native_app_glue STATIC
  ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
  ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(loom_quest SHARED
  src/egl_context.cpp
  src/gl_scene.cpp
  src/hevc_decoder.cpp
  src/main.cpp
  src/net_session.cpp
  src/overlay.cpp
  src/quic_transport.cpp
  src/surface_texture.cpp
  src/xr_app.cpp)

target_link_libraries(loom_quest PRIVATE
  loom_core  # brings loom_proto (session, clock sync, VideoReceiver)
  msquic
  OpenXR::openxr_loader
  native_app_glue
  android
  log
  EGL
  GLESv3
  mediandk)  # AMediaCodec / AMediaFormat (surface-mode HEVC decode)

# Without this the linker drops ANativeActivity_onCreate as unused and the app
# dies at launch with "Unable to find native library entry point".
target_link_options(loom_quest PRIVATE -u ANativeActivity_onCreate)
