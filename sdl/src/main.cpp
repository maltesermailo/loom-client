// loom-sdl — desktop debug client. M1.2: connect to loomd, run the mirror
// handshake, then open an SDL window and display the streamed HEVC video
// (datagrams → reassembly → libavcodec decode → YUV texture), requesting an IDR
// on loss. The on-screen latency/decode/loss overlay lands in M1.3.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "loom/core/session.hpp"
#include "loom/proto/control.hpp"
#include "loom/proto/errors.hpp"
#include "msquic_transport.hpp"
#include "renderer.hpp"
#include "video_pipeline.hpp"

using loom::core::Action;
using loom::core::Event;
using loom::core::HelloParams;
using loom::core::Session;
using loom::core::State;
using loom::proto::cbor::Value;
namespace control = loom::proto::control;
namespace errors = loom::proto::errors;

namespace {

std::vector<std::uint8_t> idr_request_frame(std::uint32_t last_good) {
  Value::Map body;
  body.emplace_back(Value::integer(0), Value::integer(static_cast<std::int64_t>(last_good)));
  return control::encode_frame(control::kIdrRequest, body);
}

}  // namespace

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

  std::unique_ptr<loom::sdl::Renderer> renderer;
  std::unique_ptr<loom::sdl::VideoPipeline> pipeline;
  std::vector<std::vector<std::uint8_t>> prebuffered;  // datagrams before STREAMING
  std::shared_ptr<const loom::sdl::DecodedFrame> current;
  bool running = true;
  int exit_code = 0;

  auto start_media = [&]() {
    if (pipeline) {
      return;
    }
    renderer = std::make_unique<loom::sdl::Renderer>("loom-sdl", 1280, 720);
    pipeline = std::make_unique<loom::sdl::VideoPipeline>(
        [&transport](std::uint32_t last_good) { transport.send_control(idr_request_frame(last_good)); });
    for (auto& dg : prebuffered) {
      pipeline->feed_datagram(dg);
    }
    prebuffered.clear();
    const auto& c = session.config();
    std::printf("loom-sdl: STREAMING %llux%llu@%llu codec %llu\n",
                static_cast<unsigned long long>(c ? c->width : 0),
                static_cast<unsigned long long>(c ? c->height : 0),
                static_cast<unsigned long long>(c ? c->refresh : 0),
                static_cast<unsigned long long>(c ? c->codec : 0));
  };

  while (running) {
    // Pump transport events.
    while (auto ev = transport.next_event()) {
      switch (ev->kind) {
        case loom::sdl::TransportEvent::Kind::Connected:
          session.on_event(Event::Connected);
          break;
        case loom::sdl::TransportEvent::Kind::ControlBytes:
          session.on_control_bytes(ev->bytes);
          break;
        case loom::sdl::TransportEvent::Kind::Datagram:
          if (pipeline) {
            pipeline->feed_datagram(ev->bytes);
          } else {
            prebuffered.push_back(std::move(ev->bytes));
          }
          break;
        case loom::sdl::TransportEvent::Kind::Closed:
          if (ev->code != errors::kNone) {
            std::fprintf(stderr, "loom-sdl: closed by host: %s (0x%02llx)\n", errors::name(ev->code),
                         static_cast<unsigned long long>(ev->code));
            exit_code = 1;
          }
          running = false;
          break;
      }
    }

    // Apply session actions.
    for (const auto& a : session.poll()) {
      switch (a.kind) {
        case Action::Kind::SendControl:
          transport.send_control(a.bytes);
          break;
        case Action::Kind::Established:
          std::printf("loom-sdl: WELCOME from \"%s\"\n",
                      session.host_name() ? session.host_name()->c_str() : "?");
          break;
        case Action::Kind::MediaExpected:
          start_media();
          break;
        case Action::Kind::Fatal:
          std::fprintf(stderr, "loom-sdl: fatal: %s (0x%02llx)\n", errors::name(a.code),
                       static_cast<unsigned long long>(a.code));
          exit_code = 1;
          running = false;
          break;
        case Action::Kind::Closed:
          running = false;
          break;
      }
    }

    // Render.
    if (renderer) {
      if (auto f = pipeline->take_frame()) {
        current = f;
      }
      if (current) {
        renderer->present(*current);  // vsync-paced
      }
      if (renderer->poll_quit()) {
        running = false;
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  std::printf("loom-sdl: exiting\n");
  return exit_code;
}
