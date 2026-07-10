# loom_proto — independent wire-protocol implementation (no QUIC, no I/O).
add_library(loom_proto STATIC
  src/datagram.cpp
  src/cbor.cpp
  src/control.cpp
  src/reassembly.cpp
  src/clocksync.cpp
  src/keymap.cpp)
target_include_directories(loom_proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(loom_proto PUBLIC cxx_std_20)
target_compile_options(loom_proto PRIVATE -Wall -Wextra)

# vector_adapter — conformance adapter bin (VECTORS.md §3). Depends on
# nlohmann/json (bin-only), so the library above stays dependency-free.
if(LOOM_BUILD_TESTS)
  add_executable(vector_adapter src/vector_adapter.cpp)
  target_link_libraries(vector_adapter PRIVATE loom_proto nlohmann_json_header)
  target_compile_features(vector_adapter PRIVATE cxx_std_20)
endif()
