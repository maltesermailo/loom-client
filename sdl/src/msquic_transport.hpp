#pragma once
// MsQuicTransport — ITransport backed by Microsoft's msquic (C API).
//
// Owns the msquic registration, configuration (ALPN loom/1, dev credentials),
// the connection, and the single client-initiated bidirectional control stream.
// msquic invokes our callbacks on its worker threads; those callbacks only push
// TransportEvents onto a mutex-guarded queue, so the client loop stays single-
// threaded. TODO(M7): this uses NO_CERTIFICATE_VALIDATION — no cert pinning yet.

#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <vector>

#include "msquic.h"
#include "transport.hpp"

namespace loom::sdl {

class MsQuicTransport : public ITransport {
public:
  MsQuicTransport();
  ~MsQuicTransport() override;

  MsQuicTransport(const MsQuicTransport&) = delete;
  MsQuicTransport& operator=(const MsQuicTransport&) = delete;

  bool start(const std::string& host, std::uint16_t port) override;
  void send_control(std::span<const std::uint8_t> frame) override;
  std::optional<TransportEvent> next_event() override;
  void close(std::uint64_t code) override;

private:
  // msquic C callbacks trampoline into these.
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

}  // namespace loom::sdl
