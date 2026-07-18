// loom-quest — NativeActivity entry point.
//
// OpenXR session lifecycle, the cylinder video layer, and the QUIC session are
// wired in xr_app / net_session; this file is just the android_main loop and the
// Android lifecycle/input callbacks (spec/ROADMAP.md M3.1–M3.3).

#include <android/input.h>
#include <android/keycodes.h>
#include <android_native_app_glue.h>

#include "log.hpp"
#include "xr_app.hpp"

namespace {

struct AppState {
  bool resumed = false;
  loom::quest::XrApp* xr = nullptr;
};

// native_app_glue delivers lifecycle transitions here. Session state itself
// comes from OpenXR events, not from these: the runtime only signals READY once
// the activity is resumed, so sleep/wake needs no special handling beyond
// letting xrWaitFrame throttle us while invisible.
void on_app_cmd(android_app* app, int32_t cmd) {
  auto* state = static_cast<AppState*>(app->userData);

  switch (cmd) {
    case APP_CMD_RESUME:
      state->resumed = true;
      break;
    case APP_CMD_PAUSE:
      state->resumed = false;
      break;
    default:
      break;
  }
}

// The headset's volume keys drive dev toggles: volume-up the debug overlay,
// volume-down the cylinder sharpening A/B. Controller buttons come through
// OpenXR (M4); the volume keys reach us here via the Android input queue, which
// is enough for a dev toggle without the OpenXR action system.
int32_t on_input(android_app* app, AInputEvent* event) {
  auto* state = static_cast<AppState*>(app->userData);
  if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_KEY ||
      AKeyEvent_getAction(event) != AKEY_EVENT_ACTION_DOWN) {
    return 0;
  }

  const int32_t key = AKeyEvent_getKeyCode(event);
  if (key == AKEYCODE_VOLUME_UP) {
    if (state->xr != nullptr) state->xr->toggle_overlay();
    return 1;  // consume
  }
  if (key == AKEYCODE_VOLUME_DOWN) {
    if (state->xr != nullptr) state->xr->toggle_sharpening();
    return 1;  // consume
  }

  return 0;
}

}  // namespace

void android_main(android_app* app) {
  AppState state;
  app->userData = &state;
  app->onAppCmd = on_app_cmd;
  app->onInputEvent = on_input;

  if (!loom::quest::XrApp::init_loader(app)) {
    LOOM_LOGE("OpenXR loader init failed");
    return;
  }

  loom::quest::XrApp xr;
  state.xr = &xr;
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
          (!state.resumed && !xr.session_running() && app->destroyRequested == 0) ? -1 : 0;
      if (ALooper_pollOnce(timeout_ms, nullptr, nullptr, reinterpret_cast<void**>(&source)) < 0) {
        break;
      }
      if (source != nullptr) source->process(app, source);
    }

    xr.poll_events(&exit_requested);
    xr.pump_network();  // drive the QUIC session every iteration, render or not
    xr.render_frame();
  }

  LOOM_LOGI("loom-quest exiting");
}
