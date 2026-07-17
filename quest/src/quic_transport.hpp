#pragma once
// QuicTransport — the Quest client's QUIC transport over msquic (C API).
//
// The msquic C API is identical on desktop and Android, so this mirrors the SDL
// client's MsQuicTransport (sdl/src/msquic_transport.*). It is deliberately a
// quest-local copy rather than shared code: this is where the platform
// differences concentrate (the Android libmsquic build, callback threading), and
// keeping it thin and local is the seam the session prompt asks for. The wire
// behavior is identical — same ALPN loom/1, same dev credentials.
//
// msquic invokes our callbacks on its worker threads; they only push
// TransportEvents onto a mutex-guarded queue, so the render-thread pump drains
// them single-threaded. TODO(M7): NO_CERTIFICATE_VALIDATION — no pinning yet.

#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <vector>

#include "msquic.h"

namespace loom::quest {

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

class QuicTransport {
 public:
  QuicTransport();
  ~QuicTransport();

  QuicTransport(const QuicTransport&) = delete;
  QuicTransport& operator=(const QuicTransport&) = delete;

  // Begin connecting to host:port (ALPN loom/1). Returns false on immediate
  // setup failure; handshake success/failure arrives as a Connected/Closed event.
  bool start(const std::string& host, std::uint16_t port);

  // Queue a complete length-prefixed control frame for the control stream.
  void send_control(std::span<const std::uint8_t> frame);

  // Pop the next transport event, or nullopt if none is pending.
  std::optional<TransportEvent> next_event();

  // Begin an application-initiated shutdown with the given close code.
  void close(std::uint64_t code);

 private:
  QUIC_STATUS on_connection(HQUIC conn, QUIC_CONNECTION_EVENT* ev);
  QUIC_STATUS on_stream(HQUIC stream, QUIC_STREAM_EVENT* ev);
  static QUIC_STATUS QUIC_API conn_trampoline(HQUIC, void*, QUIC_CONNECTION_EVENT*);
  static QUIC_STATUS QUIC_API stream_trampoline(HQUIC, void*, QUIC_STREAM_EVENT*);

  void push(TransportEvent ev);
  bool open_configuration();

  const QUIC_API_TABLE* api_ = nullptr;
  HQUIC registration_ = nullptr;
  HQUIC configuration_ = nullptr;
  HQUIC connection_ = nullptr;
  HQUIC control_ = nullptr;  // the bidirectional control stream

  std::mutex mu_;
  std::queue<TransportEvent> events_;
};

}  // namespace loom::quest
