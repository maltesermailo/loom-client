// JNI bridge between PancakeActivity (Kotlin) and the native streaming stack
// (ROADMAP M3.5 / M6.3). One QUIC session drives the shared loom::quest::NetSession
// (msquic + core::Session + per-stream core::VideoReceiver); the desktop is shown
// as N side-by-side 2D surfaces in the Home window, one AMediaCodec decoder per
// stream rendering straight into its SurfaceView — no OpenXR, no cylinder.
//
// Fan-in (M6.3): the session negotiates multi-display, so CONFIG carries one video
// stream per host display. Kotlin reads nativeStreamCount() and lays out that many
// SurfaceViews, attaching each by slot; the run loop binds slot k to stream k and
// stands up a decoder once both the surface and the stream are known.

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "hevc_decoder.hpp"
#include "log.hpp"
#include "net_session.hpp"

namespace {

std::int64_t now_us() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

// One live streaming session on its own thread. msquic invokes its callbacks on
// worker threads that only enqueue events; pump() drains them here, so the loop
// stays single-threaded (the M3.3 threading model). Surfaces are attached from the
// UI thread and consumed by the run loop under `mu`.
struct Bridge {
  std::thread thread;
  std::atomic<bool> stop{false};
  // VIEWPORT handoff (§3.10): the UI thread stores (width<<32 | height) on a
  // "Match to window" tap; the run loop forwards it to the session (primary).
  std::atomic<std::uint64_t> viewport{0};
  // Number of video streams the host is sending (0 until CONFIG). Kotlin polls it
  // to decide how many SurfaceViews to lay out.
  std::atomic<int> stream_count{0};

  // Surface (re)assignments from the UI thread, each carrying an ANativeWindow ref
  // whose ownership transfers to the run loop when it drains the queue. This keeps
  // the run loop the sole owner of every window ref — no cross-thread borrowing.
  std::mutex mu;
  struct SurfaceEvent {
    int slot;
    ANativeWindow* window;  // ref owned by this event until the run loop adopts it
  };
  std::vector<SurfaceEvent> surface_events;
};

Bridge* g_bridge = nullptr;  // accessed only on the UI thread (surface callbacks)

// Pixel size to configure slot `slot`'s decoder with: the primary (slot 0) from
// CONFIG keys 2-3, each extra from CONFIG key 6 in order. A hint only — the codec
// adapts to the stream's actual SPS dimensions.
void decoder_size(const std::optional<loom::core::SessionConfig>& cfg, std::size_t slot, int* w,
                  int* h) {
  *w = 2560;
  *h = 1440;
  if (!cfg) return;
  if (slot == 0) {
    *w = static_cast<int>(cfg->width);
    *h = static_cast<int>(cfg->height);
  } else if (slot - 1 < cfg->extra_streams.size()) {
    *w = static_cast<int>(cfg->extra_streams[slot - 1].width);
    *h = static_cast<int>(cfg->extra_streams[slot - 1].height);
  }
}

void run(std::string host, std::uint16_t port, Bridge* b) {
  loom::core::HelloParams params;
  params.client_name = "loom-panel";
  // Advertise multi-display fan-in (§3.4): the host streams one video per display.
  params.features |= loom::core::kFeatureMultiDisplay;

  loom::quest::NetSession session(params);
  if (!session.start(host, port)) {
    LOOM_LOGE("failed to start QUIC connection to %s:%u", host.c_str(), port);
    return;
  }
  LOOM_LOGI("connecting to %s:%u", host.c_str(), port);

  // One decoder per panel slot; the window it renders into is owned here (acquired
  // in nativeAttachSurface, released when it changes or on teardown).
  struct Panel {
    ANativeWindow* window = nullptr;
    std::optional<loom::quest::HevcDecoder> decoder;
  };
  // unique_ptr because Panel owns a non-movable HevcDecoder (vector::resize needs
  // a movable element otherwise).
  std::vector<std::unique_ptr<Panel>> panels;

  const auto interval = std::chrono::microseconds(1000000 / 72);
  while (!b->stop.load() && !session.finished()) {
    session.pump(now_us());
    b->stream_count.store(static_cast<int>(session.stream_ids().size()), std::memory_order_relaxed);

    // Forward any pending "Match to window" request (§3.10, primary stream).
    if (const std::uint64_t vp = b->viewport.load(std::memory_order_relaxed); vp != 0) {
      session.request_viewport(static_cast<std::uint32_t>(vp >> 32),
                               static_cast<std::uint32_t>(vp & 0xffffffffu));
    }

    const auto ids = session.stream_ids();  // primary first, then extras in order

    // Apply any surface (re)assignments, adopting each event's window ref.
    std::vector<Bridge::SurfaceEvent> events;
    {
      std::lock_guard<std::mutex> lk(b->mu);
      events.swap(b->surface_events);
    }
    for (const auto& ev : events) {
      if (ev.slot < 0) {
        if (ev.window != nullptr) ANativeWindow_release(ev.window);
        continue;
      }
      const auto slot = static_cast<std::size_t>(ev.slot);
      if (panels.size() <= slot) panels.resize(slot + 1);
      if (!panels[slot]) panels[slot] = std::make_unique<Panel>();
      Panel& p = *panels[slot];
      // Tear the old decoder down and release the old window before adopting the new.
      if (p.decoder) {
        p.decoder->stop();
        p.decoder.reset();
        if (auto* rx = session.receiver(slot < ids.size() ? ids[slot] : 0)) rx->resume();
      }
      if (p.window != nullptr) ANativeWindow_release(p.window);
      p.window = ev.window;  // ownership transferred from the event
    }

    for (std::size_t slot = 0; slot < panels.size(); ++slot) {
      if (!panels[slot]) continue;
      Panel& p = *panels[slot];
      // Bring a decoder up once we have this slot's surface and its stream.
      if (session.streaming() && p.window != nullptr && !p.decoder && slot < ids.size()) {
        if (auto* rx = session.receiver(ids[slot])) {
          int w = 0, h = 0;
          decoder_size(session.config(), slot, &w, &h);
          p.decoder.emplace();
          if (p.decoder->create(p.window, w, h)) {
            p.decoder->start(rx);
            LOOM_LOGI("decoder started: slot %zu, stream %u (%dx%d)", slot, ids[slot], w, h);
          } else {
            LOOM_LOGE("decoder create failed: slot %zu, stream %u", slot, ids[slot]);
            p.decoder.reset();
          }
        }
      }
    }

    std::this_thread::sleep_for(interval);
  }

  // Stop decoders before their receivers (in `session`) go away, then release
  // every window ref — the panels' and any surface events never drained.
  for (auto& pp : panels) {
    if (!pp) continue;
    if (pp->decoder) pp->decoder->stop();
    if (pp->window != nullptr) ANativeWindow_release(pp->window);
  }
  {
    std::lock_guard<std::mutex> lk(b->mu);
    for (auto& ev : b->surface_events) {
      if (ev.window != nullptr) ANativeWindow_release(ev.window);
    }
    b->surface_events.clear();
  }
  LOOM_LOGI("session loop ended (streaming=%d)", session.streaming() ? 1 : 0);
}

}  // namespace

extern "C" JNIEXPORT void JNICALL Java_com_loom_spatial_NativeBridge_start(JNIEnv* env,
                                                                           jobject /*thiz*/,
                                                                           jstring host,
                                                                           jint port) {
  if (g_bridge != nullptr) return;  // already running

  const char* host_c = env->GetStringUTFChars(host, nullptr);
  std::string host_s(host_c);
  env->ReleaseStringUTFChars(host, host_c);

  LOOM_LOGI("native bridge start: host=%s port=%d", host_s.c_str(), static_cast<int>(port));

  g_bridge = new Bridge();
  g_bridge->thread = std::thread(run, host_s, static_cast<std::uint16_t>(port), g_bridge);
}

// The number of video streams the host is sending (0 until CONFIG). Kotlin polls
// this to lay out that many panels.
extern "C" JNIEXPORT jint JNICALL Java_com_loom_spatial_NativeBridge_streamCount(JNIEnv* /*env*/,
                                                                                 jobject /*thiz*/) {
  return g_bridge != nullptr ? g_bridge->stream_count.load(std::memory_order_relaxed) : 0;
}

// Attach (or replace) the surface for panel `slot`. The run loop adopts it and
// stands a decoder up for stream `slot`.
extern "C" JNIEXPORT void JNICALL Java_com_loom_spatial_NativeBridge_attachSurface(
    JNIEnv* env, jobject /*thiz*/, jint slot, jobject surface) {
  if (g_bridge == nullptr || slot < 0) return;
  // The fromSurface ref transfers to the queued event (adopted by the run loop).
  ANativeWindow* window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
  {
    std::lock_guard<std::mutex> lk(g_bridge->mu);
    g_bridge->surface_events.push_back({slot, window});
  }
  LOOM_LOGI("attach surface: slot %d (%s)", static_cast<int>(slot),
            window != nullptr ? "valid" : "null");
}

extern "C" JNIEXPORT void JNICALL Java_com_loom_spatial_NativeBridge_detachSurface(JNIEnv* /*env*/,
                                                                                   jobject /*thiz*/,
                                                                                   jint slot) {
  if (g_bridge == nullptr || slot < 0) return;
  std::lock_guard<std::mutex> lk(g_bridge->mu);
  g_bridge->surface_events.push_back({slot, nullptr});
}

extern "C" JNIEXPORT void JNICALL Java_com_loom_spatial_NativeBridge_setViewport(JNIEnv* /*env*/,
                                                                                 jobject /*thiz*/,
                                                                                 jint width,
                                                                                 jint height) {
  if (g_bridge == nullptr || width <= 0 || height <= 0) return;
  g_bridge->viewport.store(
      (static_cast<std::uint64_t>(width) << 32) | static_cast<std::uint64_t>(height),
      std::memory_order_relaxed);
  LOOM_LOGI("viewport request: %dx%d", static_cast<int>(width), static_cast<int>(height));
}

extern "C" JNIEXPORT void JNICALL Java_com_loom_spatial_NativeBridge_stop(JNIEnv* /*env*/,
                                                                          jobject /*thiz*/) {
  if (g_bridge == nullptr) return;

  g_bridge->stop.store(true);
  if (g_bridge->thread.joinable()) g_bridge->thread.join();  // the thread releases the windows

  delete g_bridge;
  g_bridge = nullptr;
  LOOM_LOGI("native bridge stop");
}
