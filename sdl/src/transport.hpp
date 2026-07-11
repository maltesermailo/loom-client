#pragma once
// ITransport — the QUIC seam that keeps loom::core transport-agnostic.
//
// core::Session speaks only bytes and events; something has to move those over
// a real QUIC connection. That "something" is an ITransport. The SDL client
// implements it with msquic-on-desktop (MsQuicTransport); the Quest client will
// implement the same interface with msquic-on-Android, reusing core/ untouched.
//
// It is intentionally poll-based to match core::Session: msquic's callbacks run
// on its own threads and enqueue TransportEvents; the single-threaded client
// loop drains them with next_event() and feeds core, then pushes core's
// SendControl actions back through send_control(). No protocol logic lives here.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace loom::sdl {

struct TransportEvent {
  enum class Kind {
    Connected,     // QUIC handshake done + control stream open
    ControlBytes,  // control-stream bytes arrived (in `bytes`)
    Datagram,      // one media datagram arrived (in `bytes`) — §4
    Closed,        // connection shut down; `code` is the app close code (§10)
  };
  Kind kind;
  std::vector<std::uint8_t> bytes;
  std::uint64_t code = 0;
};

class ITransport {
public:
  virtual ~ITransport() = default;

  // Begin connecting to host:port (ALPN loom/1). Returns false on immediate
  // setup failure; success/failure of the handshake itself arrives as a
  // Connected / Closed event.
  virtual bool start(const std::string& host, std::uint16_t port) = 0;

  // Queue a complete length-prefixed control frame for the control stream.
  virtual void send_control(std::span<const std::uint8_t> frame) = 0;

  // Pop the next transport event, or nullopt if none is pending.
  virtual std::optional<TransportEvent> next_event() = 0;

  // Begin an application-initiated shutdown with the given close code.
  virtual void close(std::uint64_t code) = 0;
};

}  // namespace loom::sdl
