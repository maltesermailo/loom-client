# loom-sdl — desktop debug client (ARCHITECTURE §7).
#
# Builds msquic from source as a pinned ExternalProject and links loom_core
# behind the ITransport seam, so the same session logic runs here and (later) on
# the Quest with msquic-on-Android. SDL2 + libavcodec decode + the overlay join
# in M1.2 / M1.3; M1.1 is connect + handshake only.
#
# This target is OFF by default and excluded from check.sh (it needs network to
# fetch msquic and a multi-minute first build); enable with the `sdl` preset.
include(ExternalProject)

set(MSQUIC_PREFIX ${CMAKE_BINARY_DIR}/msquic)
set(MSQUIC_INSTALL ${MSQUIC_PREFIX}/install)
if(APPLE)
  set(MSQUIC_LIB ${MSQUIC_INSTALL}/lib/libmsquic.dylib)
else()
  set(MSQUIC_LIB ${MSQUIC_INSTALL}/lib/libmsquic.so)
endif()

ExternalProject_Add(msquic_ext
  GIT_REPOSITORY https://github.com/microsoft/msquic.git
  GIT_TAG v2.5.9
  GIT_SHALLOW ON
  GIT_SUBMODULES_RECURSE ON
  PREFIX ${MSQUIC_PREFIX}
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=${MSQUIC_INSTALL}
    -DQUIC_BUILD_TOOLS=OFF
    -DQUIC_BUILD_TEST=OFF
    -DQUIC_BUILD_PERF=OFF
    -DQUIC_ENABLE_LOGGING=OFF
  BUILD_BYPRODUCTS ${MSQUIC_LIB}
  UPDATE_DISCONNECTED ON)

# msquic installs headers only at build time; create the dir so the imported
# target's include path exists at configure time.
file(MAKE_DIRECTORY ${MSQUIC_INSTALL}/include)

add_library(msquic SHARED IMPORTED GLOBAL)
set_target_properties(msquic PROPERTIES
  IMPORTED_LOCATION ${MSQUIC_LIB}
  INTERFACE_INCLUDE_DIRECTORIES ${MSQUIC_INSTALL}/include)
add_dependencies(msquic msquic_ext)

# SDL2 (+ttf) + libavcodec/util for display, overlay, and decode (Homebrew).
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)

# The media/instrumentation units, shared by loom-sdl and its tests.
add_library(loom_sdl_media STATIC
  src/msquic_transport.cpp
  src/decoder.cpp
  src/renderer.cpp
  src/video_pipeline.cpp)
target_include_directories(loom_sdl_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(loom_sdl_media PUBLIC cxx_std_20)
target_compile_options(loom_sdl_media PRIVATE -Wall -Wextra)
target_link_libraries(loom_sdl_media PUBLIC loom_core msquic PkgConfig::SDL2 PkgConfig::SDL2_TTF
                                            PkgConfig::FFMPEG)

add_executable(loom-sdl src/main.cpp)
target_compile_features(loom-sdl PRIVATE cxx_std_20)
target_compile_options(loom-sdl PRIVATE -Wall -Wextra)
target_link_libraries(loom-sdl PRIVATE loom_sdl_media)

# Latency-accuracy test (M1.3 accept) — needs the decoder, so it lives with the
# sdl target (off check.sh), run via the `sdl` preset's ctest.
if(LOOM_BUILD_TESTS)
  add_executable(sdl_tests tests/latency_test.cpp)
  target_link_libraries(sdl_tests PRIVATE loom_sdl_media doctest::doctest)
  add_test(NAME sdl_tests COMMAND sdl_tests)
endif()
