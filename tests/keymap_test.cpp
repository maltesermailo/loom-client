#include <doctest/doctest.h>

#include "loom/proto/keymap.hpp"

using loom::proto::keymap::Keymap;

TEST_CASE("keymap parses rows, ignoring comments and blank lines") {
  const auto km = Keymap::from_csv("# AKEYCODE,evdev header\n\n29,30\n7,11\n66,28\n");
  CHECK(km.size() == 3);
  CHECK(km.get(29) == 30);
  CHECK(km.get(7) == 11);
  CHECK(km.get(66) == 28);
}

TEST_CASE("keymap tolerates surrounding whitespace on fields") {
  const auto km = Keymap::from_csv("  59 , 42 \r\n117,125\n");
  CHECK(km.get(59) == 42);
  CHECK(km.get(117) == 125);
}

TEST_CASE("keymap returns nullopt for unmapped codes") {
  const auto km = Keymap::from_csv("30,0\n");
  CHECK(km.get(30) == 0);
  CHECK(km.get(999) == std::nullopt);
}

TEST_CASE("keymap skips malformed rows") {
  const auto km = Keymap::from_csv("30,0\nbroken\n40,x\n88,111\n");
  CHECK(km.size() == 2);
  CHECK(km.get(30) == 0);
  CHECK(km.get(88) == 111);
}
