# Single unit-test executable (doctest, implement-with-main). New modules add
# their *_test.cpp here.
add_executable(proto_tests
  smoke_test.cpp
  datagram_test.cpp
  cbor_test.cpp
  control_test.cpp
  reassembly_test.cpp
  clocksync_test.cpp)
target_link_libraries(proto_tests PRIVATE loom_proto doctest::doctest)
add_test(NAME proto_tests COMMAND proto_tests)
