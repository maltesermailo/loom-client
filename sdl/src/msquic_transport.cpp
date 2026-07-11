#include "msquic_transport.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace loom::sdl {
namespace {

// ALPN token for protocol version 1 (PROTOCOL.md §2).
constexpr char kAlpn[] = "loom/1";

// Heap holder keeping a queued frame alive until msquic's SEND_COMPLETE.
struct SendBuf {
  std::vector<std::uint8_t> data;
  QUIC_BUFFER buffer;
};

}  // namespace

MsQuicTransport::MsQuicTransport() {
  if (QUIC_FAILED(MsQuicOpen2(&api_))) {
    api_ = nullptr;
    return;
  }
  const QUIC_REGISTRATION_CONFIG reg = {"loom-sdl", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
  if (QUIC_FAILED(api_->RegistrationOpen(&reg, &registration_))) {
    registration_ = nullptr;
  }
}

MsQuicTransport::~MsQuicTransport() {
  if (api_ != nullptr) {
    if (control_ != nullptr) api_->StreamClose(control_);
    if (connection_ != nullptr) api_->ConnectionClose(connection_);
    if (configuration_ != nullptr) api_->ConfigurationClose(configuration_);
    if (registration_ != nullptr) api_->RegistrationClose(registration_);
    MsQuicClose(api_);
  }
}

bool MsQuicTransport::open_configuration() {
  QUIC_BUFFER alpn;
  alpn.Length = static_cast<std::uint32_t>(sizeof(kAlpn) - 1);
  alpn.Buffer = reinterpret_cast<std::uint8_t*>(const_cast<char*>(kAlpn));

  // Keep-alive 5 s, idle timeout 15 s (PROTOCOL.md §2).
  QUIC_SETTINGS settings;
  std::memset(&settings, 0, sizeof(settings));
  settings.IdleTimeoutMs = 15000;
  settings.IsSet.IdleTimeoutMs = 1;
  settings.KeepAliveIntervalMs = 5000;
  settings.IsSet.KeepAliveIntervalMs = 1;

  if (QUIC_FAILED(api_->ConfigurationOpen(registration_, &alpn, 1, &settings, sizeof(settings),
                                          this, &configuration_))) {
    return false;
  }

  // TODO(M7): pin the host cert. Dev builds skip validation entirely.
  QUIC_CREDENTIAL_CONFIG cred;
  std::memset(&cred, 0, sizeof(cred));
  cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
  cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  return QUIC_SUCCEEDED(api_->ConfigurationLoadCredential(configuration_, &cred));
}

bool MsQuicTransport::start(const std::string& host, std::uint16_t port) {
  if (api_ == nullptr || registration_ == nullptr) return false;
  if (!open_configuration()) return false;

  if (QUIC_FAILED(api_->ConnectionOpen(registration_, conn_trampoline, this, &connection_))) {
    return false;
  }
  return QUIC_SUCCEEDED(api_->ConnectionStart(connection_, configuration_,
                                              QUIC_ADDRESS_FAMILY_UNSPEC, host.c_str(), port));
}

void MsQuicTransport::send_control(std::span<const std::uint8_t> frame) {
  if (control_ == nullptr) return;
  auto* h = new SendBuf{std::vector<std::uint8_t>(frame.begin(), frame.end()), {}};
  h->buffer.Length = static_cast<std::uint32_t>(h->data.size());
  h->buffer.Buffer = h->data.data();
  if (QUIC_FAILED(api_->StreamSend(control_, &h->buffer, 1, QUIC_SEND_FLAG_NONE, h))) {
    delete h;
  }
}

void MsQuicTransport::close(std::uint64_t code) {
  if (connection_ != nullptr) {
    api_->ConnectionShutdown(connection_, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, code);
  }
}

std::optional<TransportEvent> MsQuicTransport::next_event() {
  std::lock_guard<std::mutex> lock(mu_);
  if (events_.empty()) return std::nullopt;
  TransportEvent ev = std::move(events_.front());
  events_.pop();
  return ev;
}

void MsQuicTransport::push(TransportEvent ev) {
  std::lock_guard<std::mutex> lock(mu_);
  events_.push(std::move(ev));
}

QUIC_STATUS MsQuicTransport::on_connection(HQUIC conn, QUIC_CONNECTION_EVENT* ev) {
  switch (ev->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
      // Open the single client-initiated bidirectional control stream (§1).
      if (QUIC_SUCCEEDED(api_->StreamOpen(conn, QUIC_STREAM_OPEN_FLAG_NONE, stream_trampoline,
                                          this, &control_))) {
        api_->StreamStart(control_, QUIC_STREAM_START_FLAG_NONE);
      }
      push({TransportEvent::Kind::Connected, {}, 0});
      break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
      push({TransportEvent::Kind::Closed, {},
            ev->SHUTDOWN_INITIATED_BY_PEER.ErrorCode});
      break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
      push({TransportEvent::Kind::Closed, {}, 0});
      break;
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS MsQuicTransport::on_stream(HQUIC /*stream*/, QUIC_STREAM_EVENT* ev) {
  switch (ev->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
      std::vector<std::uint8_t> bytes;
      bytes.reserve(ev->RECEIVE.TotalBufferLength);
      for (std::uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
        const QUIC_BUFFER& b = ev->RECEIVE.Buffers[i];
        bytes.insert(bytes.end(), b.Buffer, b.Buffer + b.Length);
      }
      push({TransportEvent::Kind::ControlBytes, std::move(bytes), 0});
      break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
      delete static_cast<SendBuf*>(ev->SEND_COMPLETE.ClientContext);
      break;
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API MsQuicTransport::conn_trampoline(HQUIC conn, void* ctx,
                                                      QUIC_CONNECTION_EVENT* ev) {
  return static_cast<MsQuicTransport*>(ctx)->on_connection(conn, ev);
}

QUIC_STATUS QUIC_API MsQuicTransport::stream_trampoline(HQUIC stream, void* ctx,
                                                        QUIC_STREAM_EVENT* ev) {
  return static_cast<MsQuicTransport*>(ctx)->on_stream(stream, ev);
}

}  // namespace loom::sdl
