#include <doctest/doctest.h>

#include <initializer_list>
#include <vector>

#include "loom/proto/reassembly.hpp"

using loom::proto::reassembly::Counters;
using loom::proto::reassembly::Event;
using loom::proto::reassembly::Fragment;
using loom::proto::reassembly::Reassembler;

namespace {
struct D {
  std::int64_t t;
  std::uint32_t seq;
  std::uint16_t idx;
  std::uint16_t cnt;
  bool key;
};
std::vector<Event> run(std::initializer_list<D> trace, Counters& out) {
  Reassembler r;
  for (const auto& d : trace) r.push(d.t, Fragment{d.seq, d.idx, d.cnt, d.key});
  out = r.counters();
  return r.events();
}
Event deliver(std::int64_t t, std::uint32_t seq, bool key) {
  return Event{Event::Kind::Deliver, t, seq, key, 0};
}
Event idr(std::int64_t t, std::uint32_t last_good) {
  return Event{Event::Kind::IdrRequest, t, 0, false, last_good};
}
} // namespace

TEST_CASE("happy path delivers each frame in order, no drops") {
  Counters c;
  const auto ev = run({{0, 0, 0, 2, true}, {1, 0, 1, 2, true}, {14, 1, 0, 1, false},
                       {28, 2, 0, 1, false}},
                      c);
  CHECK(ev == std::vector<Event>{deliver(1, 0, true), deliver(14, 1, false), deliver(28, 2, false)});
  CHECK(c == Counters{0, 0, 0});
}

TEST_CASE("single loss -> gap discards + one IDR, keyframe recovers") {
  Counters c;
  const auto ev = run({{0, 0, 0, 1, true},
                       {14, 1, 0, 3, false},
                       {15, 1, 2, 3, false}, // frag 1 of frame 1 never arrives
                       {28, 2, 0, 1, false}, // completes with a gap -> discard + IDR
                       {42, 3, 0, 1, false}, // gap again, but an IDR is outstanding
                       {300, 4, 0, 1, true}, // host answers with a keyframe
                       {314, 5, 0, 1, false}},
                      c);
  CHECK(ev == std::vector<Event>{deliver(0, 0, true), idr(28, 0), deliver(300, 4, true),
                                 deliver(314, 5, false)});
  CHECK(c == Counters{1, 2, 0});
}

TEST_CASE("window holds at most two incomplete frames (rule 2 eviction)") {
  Counters c;
  const auto ev = run({{0, 0, 0, 1, true},
                       {10, 1, 0, 2, false},
                       {11, 2, 0, 2, false},
                       {12, 3, 0, 2, false}, // third incomplete evicts frame 1
                       {13, 2, 1, 2, false}, // frame 2 completes with a gap -> discard + IDR
                       {14, 3, 1, 2, false}},
                      c);
  CHECK(ev == std::vector<Event>{deliver(0, 0, true), idr(13, 0)});
  CHECK(c == Counters{1, 2, 0});
}

TEST_CASE("reorder + duplicate: dup is idempotent, late frame is stale") {
  Counters c;
  const auto ev = run({{0, 0, 0, 1, true},
                       {5, 2, 0, 1, false}, // gap -> discard + IDR
                       {6, 2, 0, 1, false}, // duplicate of a completed frame -> stale
                       {7, 1, 0, 1, false}, // arrives after 2 completed -> stale
                       {8, 3, 0, 1, false}},
                      c);
  CHECK(ev == std::vector<Event>{deliver(0, 0, true), idr(5, 0)});
  CHECK(c == Counters{0, 2, 2});
}
