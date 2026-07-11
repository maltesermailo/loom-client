# loom_core — client session state machine, clock sync, jitter buffer.
# Transport-agnostic (sans-io): drives the same logic under SDL/msquic and,
# untouched, under the Quest client. Depends only on the wire layer.
add_library(loom_core STATIC src/session.cpp)
target_include_directories(loom_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(loom_core PUBLIC cxx_std_20)
target_compile_options(loom_core PRIVATE -Wall -Wextra)
target_link_libraries(loom_core PUBLIC loom_proto)
