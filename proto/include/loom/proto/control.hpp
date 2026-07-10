#pragma once
// Control-stream framing + CBOR envelope — PROTOCOL.md §3. Each frame is a
// big-endian u32 body length (MUST be <= 65536) followed by a CBOR message: a
// 2-element array [msg_type, body-map] with integer body keys. On decode,
// unknown body keys are stripped and unknown msg_types are ignored (§3.2);
// framing/envelope violations are PROTOCOL_VIOLATION.
//
// This step registers message types 0x01-0x05; the rest arrive in step 4.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "loom/proto/cbor.hpp"
#include "loom/proto/result.hpp"

namespace loom::proto::control {

inline constexpr std::size_t kMaxFrameBody = 65536;

// Message registry (§3.3).
inline constexpr std::uint64_t kHello = 0x01;
inline constexpr std::uint64_t kWelcome = 0x02;
inline constexpr std::uint64_t kConfig = 0x03;
inline constexpr std::uint64_t kConfigAck = 0x04;
inline constexpr std::uint64_t kStart = 0x05;

// The body-map keys defined for a message type, or nullopt if unregistered
// (an unregistered message is ignored on receipt). Keys outside the set are
// dropped on decode.
std::optional<std::span<const std::int64_t>> known_keys(std::uint64_t msg_type);

struct Decoded {
  enum class Kind { Message, Ignored };
  Kind kind;
  std::uint64_t msg_type;  // valid when kind == Message
  cbor::Value::Map body;   // filtered body, when kind == Message
};

enum class ControlError { protocol_violation };
const char* to_string(ControlError e);

// Encode [msg_type, body] as canonical CBOR, prefixed with its u32 length.
std::vector<std::uint8_t> encode_frame(std::uint64_t msg_type, const cbor::Value::Map& body);

// Decode one length-prefixed control frame.
Result<Decoded, ControlError> decode_frame(std::span<const std::uint8_t> bytes);

} // namespace loom::proto::control
