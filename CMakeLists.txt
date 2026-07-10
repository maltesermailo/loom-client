cmake_minimum_required(VERSION 3.24)
project(loom_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(LOOM_BUILD_TESTS "Build loom-client unit tests + the vector adapter" ON)

# Test/tooling dependencies are fetched before the subdirectories that use them.
if(LOOM_BUILD_TESTS)
  include(FetchContent)
  # doctest's own CMakeLists declares a pre-4.0 cmake_minimum_required; raise the
  # policy floor to doctest's real minimum (3.14) so CMake 4 accepts it.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.14)
  FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11)
  FetchContent_MakeAvailable(doctest)

  # nlohmann/json single header — used only by the vector adapter binary (a
  # conformance tool), so the loom_proto library stays dependency-free.
  FetchContent_Declare(
    nlohmann_json
    URL https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
    DOWNLOAD_NO_EXTRACT TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
  add_library(nlohmann_json_header INTERFACE)
  target_include_directories(nlohmann_json_header INTERFACE ${nlohmann_json_SOURCE_DIR})
endif()

# Independent protocol implementation + client core (jitter/state machine/clock).
add_subdirectory(proto)
add_subdirectory(core)

# The Quest (OpenXR/NDK) and SDL debug clients are added in later sessions;
# their preset placeholders live in CMakePresets.json.

if(LOOM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
