// loom-sdl — desktop debug client. M1.1 scope: connect to loomd over QUIC
// (msquic) and run the mirror handshake to STREAMING. The SDL2 window,
// libavcodec decode, and the on-screen overlay arrive in M1.2 / M1.3; this
// binary is deliberately headless for now so the session/transport bring-up is
// verifiable on its own (spec/ROADMAP.md M1.1).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "loom/core/session.hpp"
#include "loom/proto/errors.hpp"
#include "msquic_transport.hpp"

using loom::core::Action;
using loom::core::Event;
using loom::core::HelloParams;
using loom::core::Session;
using loom::core::State;
namespace errors = loom::proto::errors;

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  std::uint16_t port = 47800;
  if (argc > 1) host = argv[1];
  if (argc > 2) port = static_cast<std::uint16_t>(std::atoi(argv[2]));

  HelloParams params;
  params.client_name = "loom-sdl";
  loom::sdl::MsQuicTransport transport;
  Session session{params};

  std::printf("loom-sdl: connecting to %s:%u (ALPN loom/1)\n", host.c_str(), port);
  if (!transport.start(host, port)) {
    std::fprintf(stderr, "loom-sdl: failed to start QUIC connection\n");
    return 1;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  int exit_code = 1;
  bool running = true;

  while (running && std::chrono::steady_clock::now() < deadline) {
    while (auto ev = transport.next_event()) {
      switch (ev->kind) {
        case loom::sdl::TransportEvent::Kind::Connected:
          session.on_event(Event::Connected);
          break;
        case loom::sdl::TransportEvent::Kind::ControlBytes:
          session.on_control_bytes(ev->bytes);
          break;
        case loom::sdl::TransportEvent::Kind::Closed:
          if (ev->code == errors::kNone) {
            std::printf("loom-sdl: connection closed cleanly\n");
            exit_code = 0;
          } else {
            std::fprintf(stderr, "loom-sdl: closed by host: %s (0x%02llx)\n",
                         errors::name(ev->code), static_cast<unsigned long long>(ev->code));
          }
          running = false;
          break;
      }
    }

    for (const auto& a : session.poll()) {
      switch (a.kind) {
        case Action::Kind::SendControl:
          transport.send_control(a.bytes);
          break;
        case Action::Kind::Established:
          std::printf("loom-sdl: WELCOME from host \"%s\"\n",
                      session.host_name() ? session.host_name()->c_str() : "?");
          break;
        case Action::Kind::MediaExpected: {
          const auto& c = session.config();
          std::printf("loom-sdl: STREAMING — handshake complete (%llux%llu@%llu, codec %llu)\n",
                      static_cast<unsigned long long>(c ? c->width : 0),
                      static_cast<unsigned long long>(c ? c->height : 0),
                      static_cast<unsigned long long>(c ? c->refresh : 0),
                      static_cast<unsigned long long>(c ? c->codec : 0));
          std::printf("loom-sdl: media path lands in M1.2; exiting.\n");
          exit_code = 0;
          running = false;
          break;
        }
        case Action::Kind::Fatal:
          std::fprintf(stderr, "loom-sdl: fatal: %s (0x%02llx)\n", errors::name(a.code),
                       static_cast<unsigned long long>(a.code));
          running = false;
          break;
        case Action::Kind::Closed:
          exit_code = 0;
          running = false;
          break;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  if (running) std::fprintf(stderr, "loom-sdl: timed out before STREAMING\n");
  return exit_code;
}
