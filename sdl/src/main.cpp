// loom-sdl — desktop debug client. M1.3: streamed video plus a clock-synced
// instrumentation overlay (e2e latency, decode ms, loss %, bitrate, RTT), and
// STATS reports back to the host every second.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "clock.hpp"
#include "loom/core/session.hpp"
#include "loom/proto/control.hpp"
#include "loom/proto/errors.hpp"
#include "metrics.hpp"
#include "msquic_transport.hpp"
#include "renderer.hpp"
#include "video_pipeline.hpp"

using loom::core::Action;
using loom::core::Event;
using loom::core::HelloParams;
using loom::core::Session;
using loom::proto::cbor::Value;
namespace control = loom::proto::control;
namespace errors = loom::proto::errors;

namespace {

std::vector<std::uint8_t> idr_request_frame(std::uint32_t last_good) {
  Value::Map body;
  body.emplace_back(Value::integer(0), Value::integer(static_cast<std::int64_t>(last_good)));
  return control::encode_frame(control::kIdrRequest, body);
}

std::vector<std::string> overlay_lines(const loom::sdl::OverlayStats& s) {
  char buf[128];
  std::vector<std::string> lines;
  if (s.have_clock) {
    std::snprintf(buf, sizeof buf, "e2e %llu ms   rtt %llu ms",
                  static_cast<unsigned long long>(s.e2e_us / 1000),
                  static_cast<unsigned long long>(s.rtt_us / 1000));
  } else {
    std::snprintf(buf, sizeof buf, "e2e --   rtt -- (waiting for clock)");
  }
  lines.emplace_back(buf);
  std::snprintf(buf, sizeof buf, "decode %.1f ms   loss %.2f%%", s.decode_us / 1000.0, s.loss_pct);
  lines.emplace_back(buf);
  std::snprintf(buf, sizeof buf, "bitrate %llu kbps",
                static_cast<unsigned long long>(s.bitrate_kbps));
  lines.emplace_back(buf);
  return lines;
}

std::uint64_t rtt_of(const std::optional<loom::proto::clocksync::Estimate>& clk) {
  return clk ? static_cast<std::uint64_t>(std::max<std::int64_t>(0, clk->rtt)) : 0;
}

// Print the current overlay values as one line to stdout (the demo captures it).
void print_overlay(const loom::sdl::OverlayStats& s) {
  if (s.have_clock) {
    std::printf(
        "overlay: e2e %llu ms  rtt %llu ms  decode %.1f ms  loss %.2f%%  bitrate %llu kbps\n",
        static_cast<unsigned long long>(s.e2e_us / 1000),
        static_cast<unsigned long long>(s.rtt_us / 1000), s.decode_us / 1000.0, s.loss_pct,
        static_cast<unsigned long long>(s.bitrate_kbps));
  } else {
    std::printf("overlay: e2e --  rtt --  decode %.1f ms  loss %.2f%%  bitrate %llu kbps\n",
                s.decode_us / 1000.0, s.loss_pct, static_cast<unsigned long long>(s.bitrate_kbps));
  }

  std::fflush(stdout);
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
  std::vector<std::vector<std::uint8_t>> prebuffered;
  std::shared_ptr<const loom::sdl::DecodedFrame> current;
  loom::sdl::Metrics metrics;
  std::int64_t last_stats_us = 0;
  std::int64_t bitrate_base_us = 0;
  loom::sdl::VideoPipeline::Counters bitrate_base;
  std::uint64_t bitrate_kbps = 0;
  bool running = true;
  int exit_code = 0;

  auto start_media = [&]() {
    if (pipeline) return;
    renderer = std::make_unique<loom::sdl::Renderer>("loom-sdl", 1280, 720);
    pipeline = std::make_unique<loom::sdl::VideoPipeline>([&transport](std::uint32_t last_good) {
      transport.send_control(idr_request_frame(last_good));
    });
    for (auto& dg : prebuffered) pipeline->feed_datagram(dg);
    prebuffered.clear();
    last_stats_us = loom::sdl::now_us();
    bitrate_base_us = last_stats_us;
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
          session.on_control_bytes(ev->bytes, loom::sdl::now_us());
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
            std::fprintf(stderr, "loom-sdl: closed by host: %s (0x%02llx)\n",
                         errors::name(ev->code), static_cast<unsigned long long>(ev->code));
            exit_code = 1;
          }
          running = false;
          break;
      }
    }

    session.on_tick(loom::sdl::now_us());  // queues a CLOCK_PING when due

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

    // STATS + bitrate once per second (§3.7 cadence).
    const std::int64_t t = loom::sdl::now_us();
    if (pipeline && t - last_stats_us >= 1'000'000) {
      const auto cur = pipeline->counters();
      const double secs = static_cast<double>(t - bitrate_base_us) / 1e6;
      bitrate_kbps =
          secs > 0.0
              ? static_cast<std::uint64_t>(static_cast<double>(cur.bytes - bitrate_base.bytes) *
                                           8.0 / 1000.0 / secs)
              : 0;
      bitrate_base = cur;
      bitrate_base_us = t;

      const auto clk = session.clock();
      print_overlay(metrics.overlay(cur, rtt_of(clk), clk.has_value(), bitrate_kbps));

      transport.send_control(
          session.encode_stats(metrics.take_window(cur, rtt_of(clk), clk.has_value())));
      last_stats_us = t;
    }

    // Render newest frame + overlay.
    if (renderer) {
      if (auto f = pipeline->take_frame()) {
        current = f;
        if (const auto clk = session.clock()) {
          // client-clock capture instant = capture_ts − offset (host ≈ client + offset, §7).
          const std::int64_t e2e =
              loom::sdl::now_us() - (static_cast<std::int64_t>(f->capture_ts) - clk->offset);
          metrics.on_frame(f->capture_ts, loom::sdl::now_us(), f->decode_us,
                           static_cast<std::uint64_t>(std::max<std::int64_t>(0, e2e)));
        }
      }
      const auto clk = session.clock();
      const auto ov =
          metrics.overlay(pipeline->counters(), rtt_of(clk), clk.has_value(), bitrate_kbps);
      if (current) {
        renderer->present(*current, overlay_lines(ov));
      }
      if (renderer->poll_quit()) running = false;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  std::printf("loom-sdl: exiting\n");
  return exit_code;
}
