# loom_proto — independent wire-protocol implementation (no QUIC, no I/O).
add_library(loom_proto STATIC
  src/datagram.cpp
  src/cbor.cpp)
target_include_directories(loom_proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(loom_proto PUBLIC cxx_std_20)
target_compile_options(loom_proto PRIVATE -Wall -Wextra)
