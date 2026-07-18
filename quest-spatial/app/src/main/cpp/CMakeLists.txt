cmake_minimum_required(VERSION 3.22.1)
project(loom_panel LANGUAGES C CXX)

# Invoked by Gradle (AGP externalNativeBuild). The panel client reuses the same
# vector-proven client layers and transport as the OpenXR client (ROADMAP M3.5),
# minus anything OpenXR — it decodes into a plain ANativeWindow, not a cylinder.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# cpp/ -> main -> src -> app -> quest-spatial -> client
get_filename_component(CLIENT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ABSOLUTE)

# Shared client layers: proto (wire) + core (session, clock, VideoReceiver).
set(LOOM_BUILD_TESTS OFF)
add_subdirectory(${CLIENT_ROOT}/proto ${CMAKE_BINARY_DIR}/loom_proto)
add_subdirectory(${CLIENT_ROOT}/core ${CMAKE_BINARY_DIR}/loom_core)

# msquic prebuilt, shared with the OpenXR client (built by
# quest/scripts/build-msquic-android.sh).
set(MSQUIC_DIR ${CLIENT_ROOT}/quest/third_party/msquic)
if(NOT EXISTS ${MSQUIC_DIR}/lib/${ANDROID_ABI}/libmsquic.so)
  message(FATAL_ERROR "libmsquic.so missing — run quest/scripts/build-msquic-android.sh first.")
endif()
add_library(msquic SHARED IMPORTED)
set_target_properties(msquic PROPERTIES
  IMPORTED_LOCATION ${MSQUIC_DIR}/lib/${ANDROID_ABI}/libmsquic.so
  INTERFACE_INCLUDE_DIRECTORIES ${MSQUIC_DIR}/include)

# Reuse the Quest client's transport + session pump verbatim (no OpenXR deps).
set(QUEST_SRC ${CLIENT_ROOT}/quest/src)

add_library(loom_panel SHARED
  jni_bridge.cpp
  ${QUEST_SRC}/quic_transport.cpp
  ${QUEST_SRC}/net_session.cpp
  ${QUEST_SRC}/hevc_decoder.cpp)

target_include_directories(loom_panel PRIVATE ${QUEST_SRC})

target_link_libraries(loom_panel PRIVATE
  loom_core   # brings loom_proto (session, clock sync, VideoReceiver)
  msquic
  android     # ANativeWindow_fromSurface
  log         # __android_log_print
  mediandk)   # AMediaCodec (surface-mode HEVC decode) — used in 2.2c
