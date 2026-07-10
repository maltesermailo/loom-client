// vector_adapter — the loom-client conformance adapter (VECTORS.md §2/§3).
//
// Invoked as `vector_adapter <category>` with a vector file's JSON on stdin; it
// runs each case's `op` against loom_proto and prints {"results": [...]} (one
// per case, in order) on stdout. This is the only loom-client component that
// does I/O; the library stays pure.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <json.hpp>

#include "loom/proto/cbor.hpp"
#include "loom/proto/clocksync.hpp"
#include "loom/proto/control.hpp"
#include "loom/proto/datagram.hpp"
#include "loom/proto/hex.hpp"
#include "loom/proto/keymap.hpp"
#include "loom/proto/reassembly.hpp"

using json = nlohmann::json;
namespace proto = loom::proto;
namespace cbor = loom::proto::cbor;
namespace control = loom::proto::control;
namespace reasm = loom::proto::reassembly;
namespace clocksync = loom::proto::clocksync;
namespace keymap = loom::proto::keymap;
namespace hex = loom::proto::hex;

// ---------------------------------------------------------------- JSON <-> CBOR

static cbor::Value json_to_cbor(const json& j) {
  if (j.is_null()) return cbor::Value::null();
  if (j.is_boolean()) return cbor::Value::boolean(j.get<bool>());
  if (j.is_number_integer() || j.is_number_unsigned()) return cbor::Value::integer(j.get<std::int64_t>());
  if (j.is_number_float()) return cbor::Value::floating(j.get<double>());
  if (j.is_string()) return cbor::Value::text(j.get<std::string>());
  if (j.is_array()) {
    cbor::Value::Array a;
    for (const auto& e : j) a.push_back(json_to_cbor(e));
    return cbor::Value::array(std::move(a));
  }
  // object: either a {"$hex": ...} byte string, or a map with integer keys.
  if (j.size() == 1 && j.contains("$hex")) return cbor::Value::bytes(hex::from_hex(j.at("$hex").get<std::string>()));
  cbor::Value::Map m;
  for (auto it = j.begin(); it != j.end(); ++it)
    m.emplace_back(cbor::Value::integer(std::stoll(it.key())), json_to_cbor(it.value()));
  return cbor::Value::map(std::move(m));
}

static json cbor_to_json(const cbor::Value& v) {
  using T = cbor::Value::Type;
  switch (v.type()) {
  case T::Null:
    return nullptr;
  case T::Bool:
    return v.as_bool();
  case T::Int:
    return v.as_int();
  case T::Float:
    return v.as_float();
  case T::Text:
    return v.as_text();
  case T::Bytes: {
    json o;
    o["$hex"] = hex::to_hex(v.as_bytes());
    return o;
  }
  case T::Array: {
    json a = json::array();
    for (const auto& e : v.as_array()) a.push_back(cbor_to_json(e));
    return a;
  }
  case T::Map: {
    json o = json::object();
    for (const auto& [k, val] : v.as_map()) o[std::to_string(k.as_int())] = cbor_to_json(val);
    return o;
  }
  }
  return nullptr;
}

// ---------------------------------------------------------------- ops

static json datagram_encode(const json& in) {
  const auto h = proto::make_header(in.at("flags_keyframe").get<bool>(), in.at("stream_id").get<std::uint16_t>(),
                                    in.at("frame_seq").get<std::uint32_t>(), in.at("frag_index").get<std::uint16_t>(),
                                    in.at("frag_count").get<std::uint16_t>());
  const auto payload = hex::from_hex(in.at("payload").get<std::string>());
  return json{{"hex", hex::to_hex(proto::encode_datagram(h, payload))}};
}

static json datagram_decode(const json& in) {
  const auto bytes = hex::from_hex(in.at("hex").get<std::string>());
  const auto r = proto::decode(bytes);
  if (!r.has_value()) return json{{"ok", false}, {"reason", proto::to_string(r.error())}};
  const auto& d = r.value();
  json header;
  header["flags_keyframe"] = d.header.keyframe;
  header["flags_last"] = d.header.last_fragment;
  header["stream_id"] = d.header.stream_id;
  header["frame_seq"] = d.header.frame_seq;
  header["frag_index"] = d.header.frag_index;
  header["frag_count"] = d.header.frag_count;
  header["payload_len"] = d.payload_len;
  return json{{"ok", true}, {"header", header}};
}

static json control_encode(const json& in) {
  const auto msg_type = in.at("msg_type").get<std::uint64_t>();
  const cbor::Value body = json_to_cbor(in.at("body"));
  return json{{"hex", hex::to_hex(control::encode_frame(msg_type, body.as_map()))}};
}

static json control_decode(const json& in) {
  const auto bytes = hex::from_hex(in.at("hex").get<std::string>());
  const auto r = control::decode_frame(bytes);
  if (!r.has_value()) return json{{"ok", false}, {"error", control::to_string(r.error())}};
  const auto& d = r.value();
  if (d.kind == control::Decoded::Kind::Ignored) return json{{"ok", true}, {"ignored", true}};
  return json{{"ok", true}, {"msg_type", d.msg_type}, {"body", cbor_to_json(cbor::Value::map(d.body))}};
}

static json reassembly_trace(const json& in) {
  reasm::Reassembler r;
  for (const auto& d : in.at("trace")) {
    const reasm::Fragment f{d.at("frame_seq").get<std::uint32_t>(), d.at("frag_index").get<std::uint16_t>(),
                            d.at("frag_count").get<std::uint16_t>(), d.at("keyframe").get<bool>()};
    r.push(d.at("t_ms").get<std::int64_t>(), f);
  }
  json events = json::array();
  for (const auto& e : r.events()) {
    json ev;
    ev["t_ms"] = e.t_ms;
    if (e.kind == reasm::Event::Kind::Deliver) {
      ev["ev"] = "deliver";
      ev["frame_seq"] = e.frame_seq;
      ev["keyframe"] = e.keyframe;
    } else {
      ev["ev"] = "idr_request";
      ev["last_good"] = e.last_good;
    }
    events.push_back(ev);
  }
  const auto& c = r.counters();
  json counters;
  counters["dropped_incomplete"] = c.dropped_incomplete;
  counters["discarded_gap"] = c.discarded_gap;
  counters["stale_fragments"] = c.stale_fragments;
  return json{{"events", events}, {"counters", counters}};
}

static json clocksync_series(const json& in) {
  clocksync::ClockFilter f;
  json estimates = json::array();
  for (const auto& s : in.at("samples")) {
    const auto e = f.push(s.at(0).get<std::int64_t>(), s.at(1).get<std::int64_t>(),
                          s.at(2).get<std::int64_t>(), s.at(3).get<std::int64_t>());
    estimates.push_back(json{{"rtt", e.rtt}, {"offset", e.offset}});
  }
  return json{{"estimates", estimates}};
}

static json keymap_lookup(const json& in, const keymap::Keymap& table) {
  const auto v = table.get(in.at("code").get<std::int64_t>());
  if (v) return json{{"code", *v}};
  return json{{"code", nullptr}};
}

// ---------------------------------------------------------------- keymap files

static std::string read_file(const std::filesystem::path& p) {
  std::ifstream f(p);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Locate the spec keymaps/ dir. Prefers $LOOM_KEYMAPS_DIR, else common cwd-
// relative locations (the harness runs the adapter from the client repo root).
static std::filesystem::path find_keymaps_dir() {
  std::vector<std::filesystem::path> candidates;
  if (const char* e = std::getenv("LOOM_KEYMAPS_DIR")) candidates.emplace_back(e);
  for (const char* c : {"spec/keymaps", "../spec/keymaps", "../../spec/keymaps"}) candidates.emplace_back(c);
  for (const auto& c : candidates)
    if (std::filesystem::is_regular_file(c / "akeycode_to_evdev.csv")) return c;
  return {};
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: vector_adapter <category>\n";
    return 2;
  }
  const std::string category = argv[1];
  const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  const json doc = json::parse(input);

  std::optional<keymap::Keymap> ak2ev, ev2cg;
  if (category == "keymap") {
    const auto dir = find_keymaps_dir();
    if (dir.empty()) {
      std::cerr << "vector_adapter: could not locate spec keymaps/ (set LOOM_KEYMAPS_DIR)\n";
      return 2;
    }
    ak2ev = keymap::Keymap::from_csv(read_file(dir / "akeycode_to_evdev.csv"));
    ev2cg = keymap::Keymap::from_csv(read_file(dir / "evdev_to_cgkeycode.csv"));
  }

  json results = json::array();
  for (const auto& c : doc.at("cases")) {
    const std::string op = c.at("op").get<std::string>();
    const json& in = c.at("input");
    if (category == "datagram")
      results.push_back(op == "encode" ? datagram_encode(in) : datagram_decode(in));
    else if (category == "control")
      results.push_back(op == "encode" ? control_encode(in) : control_decode(in));
    else if (category == "reassembly")
      results.push_back(reassembly_trace(in));
    else if (category == "clocksync")
      results.push_back(clocksync_series(in));
    else if (category == "keymap")
      results.push_back(keymap_lookup(in, op == "akeycode_to_evdev" ? *ak2ev : *ev2cg));
    else {
      std::cerr << "vector_adapter: unknown category " << category << "\n";
      return 2;
    }
  }

  std::cout << json{{"results", results}}.dump() << "\n";
  return 0;
}
