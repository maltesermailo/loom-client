#pragma once
// Protocol error codes — PROTOCOL.md §10.
//
// Used both as the `code` in an ERROR (0x40) message body (§3.9) and as QUIC
// application close codes. They live in the wire layer so neither core/ nor the
// transports hand-roll them (the DRY rule: wire constants have one home).

#include <cstdint>

namespace loom::proto::errors {

inline constexpr std::uint64_t kNone = 0x00;                 // clean close (after BYE)
inline constexpr std::uint64_t kVersionUnsupported = 0x01;   // HELLO version not acceptable
inline constexpr std::uint64_t kBusy = 0x02;                 // host already has a session
inline constexpr std::uint64_t kNoCommonCodec = 0x03;        // no codec the host can encode
inline constexpr std::uint64_t kProtocolViolation = 0x04;    // framing / state-machine violation
inline constexpr std::uint64_t kDatagramUnsupported = 0x05;  // peer lacks QUIC datagrams
inline constexpr std::uint64_t kAuthFailed = 0x06;           // cert not pinned / pairing required
inline constexpr std::uint64_t kInternal = 0x07;             // unrecoverable local error

// Stable name for a code, for logs/UI. Unknown codes read as "INTERNAL" (§10).
inline const char* name(std::uint64_t code) {
  switch (code) {
    case kNone: return "NONE";
    case kVersionUnsupported: return "VERSION_UNSUPPORTED";
    case kBusy: return "BUSY";
    case kNoCommonCodec: return "NO_COMMON_CODEC";
    case kProtocolViolation: return "PROTOCOL_VIOLATION";
    case kDatagramUnsupported: return "DATAGRAM_UNSUPPORTED";
    case kAuthFailed: return "AUTH_FAILED";
    default: return "INTERNAL";
  }
}

}  // namespace loom::proto::errors
