// loom-quest — NativeActivity entry point.
//
// M3.1 scope: OpenXR session lifecycle, a projection layer with a floor grid, and
// the cylinder layer showing a static test image. Transport, decode and the real
// video arrive in M3.2/M3.3 (spec/ROADMAP.md).

#include <android_native_app_glue.h>

#include "log.hpp"
#include "xr_app.hpp"

namespace {

// native_app_glue delivers lifecycle transitions here. Session state itself
// comes from OpenXR events, not from these: the runtime only signals READY once
// the activity is resumed, so sleep/wake needs no special handling beyond
// letting xrWaitFrame throttle us while invisible.
void on_app_cmd(android_app* app, int32_t cmd) {
  auto* resumed = static_cast<bool*>(app->userData);

  switch (cmd) {
    case APP_CMD_RESUME:
      *resumed = true;
      break;
    case APP_CMD_PAUSE:
      *resumed = false;
      break;
    default:
      break;
  }
}

}  // namespace

void android_main(android_app* app) {
  bool resumed = false;
  app->userData = &resumed;
  app->onAppCmd = on_app_cmd;

  if (!loom::quest::XrApp::init_loader(app)) {
    LOOM_LOGE("OpenXR loader init failed");
    return;
  }

  loom::quest::XrApp xr;
  if (!xr.create(app)) {
    LOOM_LOGE("OpenXR setup failed");
    return;
  }
  LOOM_LOGI("loom-quest ready");

  bool exit_requested = false;
  while (app->destroyRequested == 0 && !exit_requested) {
    // Block only while there is genuinely nothing to do — paused, no session,
    // not shutting down. Once resumed we must poll non-blocking, because OpenXR
    // session-state events arrive through xrPollEvent rather than the Android
    // looper: blocking here would wait for an Android event that never comes and
    // the session would never start.
    for (;;) {
      android_poll_source* source = nullptr;
      const int timeout_ms =
          (!resumed && !xr.session_running() && app->destroyRequested == 0) ? -1 : 0;
      if (ALooper_pollOnce(timeout_ms, nullptr, nullptr, reinterpret_cast<void**>(&source)) < 0) {
        break;
      }
      if (source != nullptr) source->process(app, source);
    }

    xr.poll_events(&exit_requested);
    xr.render_frame();
  }

  LOOM_LOGI("loom-quest exiting");
}
