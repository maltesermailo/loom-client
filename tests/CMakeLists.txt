# Single unit-test executable (doctest, implement-with-main). As modules land,
# additional *_test.cpp files are simply added to this target's sources.
add_executable(proto_tests smoke_test.cpp)
target_link_libraries(proto_tests PRIVATE loom_proto doctest::doctest)
add_test(NAME proto_tests COMMAND proto_tests)
