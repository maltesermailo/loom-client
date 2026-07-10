# loom_proto — independent wire-protocol implementation (no QUIC, no I/O).
# Real sources (datagram, control, reassembly, clocksync, keymap) land in
# steps 2+. For now it is a header-carrying static lib with a placeholder TU.
add_library(loom_proto STATIC src/placeholder.cpp)
target_include_directories(loom_proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(loom_proto PUBLIC cxx_std_20)
