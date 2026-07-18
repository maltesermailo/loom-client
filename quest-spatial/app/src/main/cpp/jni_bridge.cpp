// JNI bridge between PancakeActivity (Kotlin) and the native streaming stack
// (ROADMAP M3.5). Phase 2.2b: connect to loomd and drive the shared
// loom::quest::NetSession (msquic + core::Session + core::VideoReceiver) on a
// background thread, reused verbatim from the OpenXR client. No decode yet — 2.2c
// pops access units from the receiver and feeds AMediaCodec into `window_`.

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <thread>

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
// stays single-threaded (the M3.3 threading model).
struct Bridge {
  std::thread thread;
  std::atomic<bool> stop{false};
  ANativeWindow* window = nullptr;
  // VIEWPORT handoff (§3.10): the UI thread stores (width<<32 | height) on a
  // "Match to window" tap; the run loop forwards it to the session.
  std::atomic<std::uint64_t> viewport{0};
};

Bridge* g_bridge = nullptr;  // accessed only on the UI thread (surface callbacks)

void run(std::string host, std::uint16_t port, ANativeWindow* window, std::atomic<bool>* stop,
         std::atomic<std::uint64_t>* viewport) {
  loom::core::HelloParams params;
  params.client_name = "loom-panel";

  loom::quest::NetSession session(params);
  if (!session.start(host, port)) {
    LOOM_LOGE("failed to start QUIC connection to %s:%u", host.c_str(), port);
    if (window != nullptr) ANativeWindow_release(window);
    return;
  }
  LOOM_LOGI("connecting to %s:%u", host.c_str(), port);

  // Scoped so the decoder (and its AMediaCodec) tears down before we drop our
  // reference to the window it renders into.
  {
    // Held in an optional so a mid-session resolution change (§8) can tear the
    // old decoder down and stand a new one up at the new size, on the same window.
    std::optional<loom::quest::HevcDecoder> decoder;
    auto bring_up_decoder = [&](int width, int height) {
      if (decoder) {
        // HevcDecoder::stop() stops the *shared* receiver to unblock the old
        // decode thread; revive it so the new decoder's pop_au() blocks for
        // frames again instead of exiting immediately. frame_seq continues (§8).
        decoder->stop();
        decoder.reset();
        session.receiver().resume();
      }
      decoder.emplace();
      if (decoder->create(window, width, height)) {
        decoder->start(&session.receiver());
        LOOM_LOGI("decoder started (%dx%d) into panel window", width, height);
      } else {
        LOOM_LOGE("decoder create failed (%dx%d)", width, height);
        decoder.reset();
      }
    };

    const auto interval = std::chrono::microseconds(1000000 / 72);
    while (!stop->load() && !session.finished()) {
      session.pump(now_us());

      // Forward any pending "Match to window" request; the session rate-limits
      // it (§3.10) and dedups a settled size, so calling every tick is cheap.
      if (const std::uint64_t vp = viewport->load(std::memory_order_relaxed); vp != 0) {
        session.request_viewport(static_cast<std::uint32_t>(vp >> 32),
                                 static_cast<std::uint32_t>(vp & 0xffffffffu));
      }

      // On the first START (§3), bring the decoder up at the negotiated
      // resolution, rendering straight into the window. The decode thread then
      // pulls access units from the receiver — no OpenXR, no cylinder.
      if (session.just_started_streaming()) {
        const auto& cfg = session.config();
        bring_up_decoder(cfg ? static_cast<int>(cfg->width) : 2560,
                         cfg ? static_cast<int>(cfg->height) : 1440);
      } else if (session.config_changed()) {
        // Mid-session reconfiguration (§8): the host renegotiated resolution.
        // Recreate the decoder so it is configured for the new IDR's parameter
        // sets; frame_seq (and the receiver) continue underneath.
        const auto& cfg = session.config();
        if (cfg) {
          LOOM_LOGI("config changed → recreating decoder (%dx%d)",
                    static_cast<int>(cfg->width), static_cast<int>(cfg->height));
          bring_up_decoder(static_cast<int>(cfg->width), static_cast<int>(cfg->height));
        }
      }

      std::this_thread::sleep_for(interval);
    }

    if (decoder) decoder->stop();
    LOOM_LOGI("session loop ended (streaming=%d)", session.streaming() ? 1 : 0);
  }

  if (window != nullptr) ANativeWindow_release(window);
}

}  // namespace

extern "C" JNIEXPORT void JNICALL Java_com_loom_spatial_PancakeActivity_nativeStart(
    JNIEnv* env, jobject /*thiz*/, jstring host, jint port, jobject surface) {
  if (g_bridge != nullptr) return;  // already running

  const char* host_c = env->GetStringUTFChars(host, nullptr);
  std::string host_s(host_c);
  env->ReleaseStringUTFChars(host, host_c);

  ANativeWindow* window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
  LOOM_LOGI("native bridge start: host=%s port=%d surface=%s", host_s.c_str(),
            static_cast<int>(port), window != nullptr ? "valid" : "null");

  g_bridge = new Bridge();
  g_bridge->window = window;
  g_bridge->thread = std::thread(run, host_s, static_cast<std::uint16_t>(port), window,
                                 &g_bridge->stop, &g_bridge->viewport);
}

extern "C" JNIEXPORT void JNICALL Java_com_loom_spatial_PancakeActivity_nativeSetViewport(
    JNIEnv* /*env*/, jobject /*thiz*/, jint width, jint height) {
  if (g_bridge == nullptr || width <= 0 || height <= 0) return;

  g_bridge->viewport.store((static_cast<std::uint64_t>(width) << 32) |
                               static_cast<std::uint64_t>(height),
                           std::memory_order_relaxed);
  LOOM_LOGI("viewport request: %dx%d", static_cast<int>(width), static_cast<int>(height));
}

extern "C" JNIEXPORT void JNICALL
Java_com_loom_spatial_PancakeActivity_nativeStop(JNIEnv* /*env*/, jobject /*thiz*/) {
  if (g_bridge == nullptr) return;

  g_bridge->stop.store(true);
  if (g_bridge->thread.joinable()) g_bridge->thread.join();  // the thread releases the window

  delete g_bridge;
  g_bridge = nullptr;
  LOOM_LOGI("native bridge stop");
}
