#pragma once
// Minimal CBOR value model with a canonical encoder and a tolerant decoder,
// per PROTOCOL.md §3.2: senders MUST emit canonical CBOR (RFC 8949 §4.2.1 —
// definite lengths, shortest-form integers, bytewise-sorted map keys); receivers
// MUST accept any valid CBOR. Floats are added in step 4 (only STATS needs one).
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace loom::proto::cbor {

class Value {
public:
  using Array = std::vector<Value>;
  using Map = std::vector<std::pair<Value, Value>>; // order here is irrelevant; encode sorts
  enum class Type { Null, Bool, Int, Bytes, Text, Array, Map };

  // Named factories avoid bool/int overload ambiguity and read clearly.
  static Value null();
  static Value boolean(bool b);
  static Value integer(std::int64_t i);
  static Value bytes(std::vector<std::uint8_t> b);
  static Value text(std::string s);
  static Value array(Array a);
  static Value map(Map m);

  Type type() const;
  bool as_bool() const;
  std::int64_t as_int() const;
  const std::vector<std::uint8_t>& as_bytes() const;
  const std::string& as_text() const;
  const Array& as_array() const;
  const Map& as_map() const;

  bool operator==(const Value&) const = default;

private:
  using Variant = std::variant<std::monostate, bool, std::int64_t, std::vector<std::uint8_t>,
                               std::string, Array, Map>;
  Value() = default; // Null (monostate)
  explicit Value(Variant v) : v_(std::move(v)) {}
  Variant v_;
};

// Canonical CBOR (RFC 8949 §4.2.1).
std::vector<std::uint8_t> encode_canonical(const Value& v);

// Decode one CBOR item. Accepts any valid *definite-length* CBOR of the kinds
// the protocol uses; returns nullopt on malformed input. Trailing bytes after
// the item are ignored. Indefinite-length items are not used by Loom and are
// rejected.
std::optional<Value> decode(std::span<const std::uint8_t> bytes);

} // namespace loom::proto::cbor
