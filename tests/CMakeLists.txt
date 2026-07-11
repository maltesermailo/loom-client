# Single unit-test executable (doctest, implement-with-main). New modules add
# their *_test.cpp here.
add_executable(proto_tests
  smoke_test.cpp
  datagram_test.cpp
  cbor_test.cpp
  control_test.cpp
  reassembly_test.cpp
  clocksync_test.cpp
  keymap_test.cpp
  session_test.cpp)
# loom_core PUBLIC-links loom_proto, so linking core alone covers both.
target_link_libraries(proto_tests PRIVATE loom_core doctest::doctest)
add_test(NAME proto_tests COMMAND proto_tests)
