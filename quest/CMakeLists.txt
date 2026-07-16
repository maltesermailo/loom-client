cmake_minimum_required(VERSION 3.24)
project(loom_quest LANGUAGES C CXX)

# Invoked by Gradle (AGP externalNativeBuild), never standalone: find_package
# below resolves against the prefab-exported OpenXR AAR, which only exists inside
# a Gradle build. See README.md for the build/deploy commands.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenXR REQUIRED CONFIG)

# NativeActivity's glue lives in the NDK sources, not in a prebuilt library.
add_library(native_app_glue STATIC
  ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
  ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(loom_quest SHARED
  src/egl_context.cpp
  src/gl_scene.cpp
  src/main.cpp
  src/xr_app.cpp)

target_link_libraries(loom_quest PRIVATE
  OpenXR::openxr_loader
  native_app_glue
  android
  log
  EGL
  GLESv3)

# Without this the linker drops ANativeActivity_onCreate as unused and the app
# dies at launch with "Unable to find native library entry point".
target_link_options(loom_quest PRIVATE -u ANativeActivity_onCreate)
