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
};

Bridge* g_bridge = nullptr;  // accessed only on the UI thread (surface callbacks)

void run(std::string host, std::uint16_t port, ANativeWindow* window, std::atomic<bool>* stop) {
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
    loom::quest::HevcDecoder decoder;

    const auto interval = std::chrono::microseconds(1000000 / 72);
    while (!stop->load() && !session.finished()) {
      session.pump(now_us());

      // On the first START (§3), bring the decoder up at the negotiated
      // resolution, rendering straight into the window. The decode thread then
      // pulls access units from the receiver — no OpenXR, no cylinder.
      if (session.just_started_streaming()) {
        const auto& cfg = session.config();
        const int width = cfg ? static_cast<int>(cfg->width) : 2560;
        const int height = cfg ? static_cast<int>(cfg->height) : 1440;
        if (decoder.create(window, width, height)) {
          decoder.start(&session.receiver());
          LOOM_LOGI("decoder started (%dx%d) into panel window", width, height);
        } else {
          LOOM_LOGE("decoder create failed");
        }
      }

      std::this_thread::sleep_for(interval);
    }

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
  g_bridge->thread =
      std::thread(run, host_s, static_cast<std::uint16_t>(port), window, &g_bridge->stop);
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
