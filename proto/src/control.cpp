#include "loom/proto/control.hpp"

namespace loom::proto::control {

std::optional<std::span<const std::int64_t>> known_keys(std::uint64_t msg_type) {
  static constexpr std::int64_t k0[] = {0};
  static constexpr std::int64_t k01[] = {0, 1};
  static constexpr std::int64_t k012[] = {0, 1, 2};
  static constexpr std::int64_t k0_5[] = {0, 1, 2, 3, 4, 5};
  static constexpr std::int64_t k0_6[] = {0, 1, 2, 3, 4, 5, 6};
  using S = std::span<const std::int64_t>;
  switch (msg_type) {
  case kHello:
  case kConfig:
    return S(k0_5);
  case kWelcome:
  case kClockPong:
    return S(k012);
  case kStats:
    return S(k0_6);
  case kError:
  case kPairB:
  case kPairResult:
    return S(k01);
  case kConfigAck:
  case kInput:
  case kIdrRequest:
  case kClockPing:
  case kBye:
  case kPairA:
  case kPairC:
    return S(k0);
  case kStart:
    return S{}; // START has an empty body
  default:
    return std::nullopt;
  }
}

const char* to_string(ControlError) { return "PROTOCOL_VIOLATION"; }

std::vector<std::uint8_t> encode_frame(std::uint64_t msg_type, const cbor::Value::Map& body) {
  cbor::Value::Array env;
  env.push_back(cbor::Value::integer(static_cast<std::int64_t>(msg_type)));
  env.push_back(cbor::Value::map(body));
  const auto payload = cbor::encode_canonical(cbor::Value::array(std::move(env)));

  const auto len = static_cast<std::uint32_t>(payload.size());
  std::vector<std::uint8_t> out;
  out.reserve(4 + payload.size());
  out.push_back(static_cast<std::uint8_t>(len >> 24));
  out.push_back(static_cast<std::uint8_t>(len >> 16));
  out.push_back(static_cast<std::uint8_t>(len >> 8));
  out.push_back(static_cast<std::uint8_t>(len));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

static bool contains(std::span<const std::int64_t> keys, std::int64_t k) {
  for (const auto x : keys)
    if (x == k) return true;
  return false;
}

Result<Decoded, ControlError> decode_frame(std::span<const std::uint8_t> bytes) {
  using E = ControlError;
  if (bytes.size() < 4) return Err(E::protocol_violation);
  const std::uint32_t len = (static_cast<std::uint32_t>(bytes[0]) << 24) |
                            (static_cast<std::uint32_t>(bytes[1]) << 16) |
                            (static_cast<std::uint32_t>(bytes[2]) << 8) |
                            static_cast<std::uint32_t>(bytes[3]);
  // The length field alone is sufficient to reject an over-limit frame (§3.1).
  if (len > kMaxFrameBody) return Err(E::protocol_violation);
  if (bytes.size() - 4 < len) return Err(E::protocol_violation);

  const auto item = cbor::decode(bytes.subspan(4, len));
  if (!item || item->type() != cbor::Value::Type::Array) return Err(E::protocol_violation);
  const auto& arr = item->as_array();
  if (arr.size() != 2) return Err(E::protocol_violation);
  if (arr[0].type() != cbor::Value::Type::Int || arr[0].as_int() < 0)
    return Err(E::protocol_violation);
  if (arr[1].type() != cbor::Value::Type::Map) return Err(E::protocol_violation);

  const auto msg_type = static_cast<std::uint64_t>(arr[0].as_int());
  const auto allowed = known_keys(msg_type);
  if (!allowed) return Ok(Decoded{Decoded::Kind::Ignored, 0, {}});

  cbor::Value::Map filtered;
  for (const auto& [k, v] : arr[1].as_map()) {
    if (k.type() == cbor::Value::Type::Int && contains(*allowed, k.as_int()))
      filtered.emplace_back(k, v);
  }
  return Ok(Decoded{Decoded::Kind::Message, msg_type, std::move(filtered)});
}

} // namespace loom::proto::control
