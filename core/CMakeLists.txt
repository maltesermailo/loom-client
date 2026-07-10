# loom_core — jitter buffer, session state machine, clock sync (client-side).
# Placeholder for now; populated once the protocol layer is in place.
add_library(loom_core STATIC src/placeholder.cpp)
target_include_directories(loom_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(loom_core PUBLIC cxx_std_20)
target_link_libraries(loom_core PUBLIC loom_proto)
