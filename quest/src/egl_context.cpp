#include "egl_context.hpp"

#include <EGL/eglext.h>  // EGL_OPENGL_ES3_BIT_KHR

#include "log.hpp"

namespace loom::quest {
namespace {

// Attributes the config must match exactly. Alpha is required: the compositor
// blends our layers in multiple passes. Depth/stencil/samples are zero because
// swapchain images carry their own depth and MSAA would be wasted on a surface
// the compositor immediately resamples.
constexpr EGLint kConfigAttribs[] = {EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE,    8,
                                     EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
                                     EGL_SAMPLES,    0, EGL_NONE};

}  // namespace

EglContext::~EglContext() {
  if (display_ == EGL_NO_DISPLAY) return;

  eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
  if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
  eglTerminate(display_);
}

bool EglContext::create() {
  display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display_ == EGL_NO_DISPLAY) {
    LOOM_LOGE("eglGetDisplay failed");
    return false;
  }
  eglInitialize(display_, nullptr, nullptr);

  // Deliberately not eglChooseConfig: Android's implementation silently injects
  // multisample flags when the user has "force 4x MSAA" enabled in developer
  // settings, which we would pay for and the compositor would throw away.
  // Enumerating and matching ourselves is the only way to refuse that.
  constexpr int kMaxConfigs = 256;
  EGLConfig configs[kMaxConfigs];
  EGLint num_configs = 0;
  if (eglGetConfigs(display_, configs, kMaxConfigs, &num_configs) == EGL_FALSE) {
    LOOM_LOGE("eglGetConfigs failed");
    return false;
  }

  for (EGLint i = 0; i < num_configs && config_ == nullptr; ++i) {
    EGLint value = 0;

    eglGetConfigAttrib(display_, configs[i], EGL_RENDERABLE_TYPE, &value);
    if ((value & EGL_OPENGL_ES3_BIT_KHR) == 0) continue;

    // Must also be window-compatible so the context can share textures with a
    // normal window context (the decode path in M3.2 needs this).
    eglGetConfigAttrib(display_, configs[i], EGL_SURFACE_TYPE, &value);
    if ((value & (EGL_WINDOW_BIT | EGL_PBUFFER_BIT)) != (EGL_WINDOW_BIT | EGL_PBUFFER_BIT)) {
      continue;
    }

    bool matches = true;
    for (int j = 0; kConfigAttribs[j] != EGL_NONE && matches; j += 2) {
      eglGetConfigAttrib(display_, configs[i], kConfigAttribs[j], &value);
      matches = (value == kConfigAttribs[j + 1]);
    }
    if (matches) config_ = configs[i];
  }

  if (config_ == nullptr) {
    LOOM_LOGE("no matching EGL config among %d", num_configs);
    return false;
  }

  const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attribs);
  if (context_ == EGL_NO_CONTEXT) {
    LOOM_LOGE("eglCreateContext failed: 0x%x", eglGetError());
    return false;
  }

  // A 16x16 pbuffer exists only to have something to make current against.
  const EGLint surface_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
  surface_ = eglCreatePbufferSurface(display_, config_, surface_attribs);
  if (surface_ == EGL_NO_SURFACE) {
    LOOM_LOGE("eglCreatePbufferSurface failed: 0x%x", eglGetError());
    return false;
  }

  if (eglMakeCurrent(display_, surface_, surface_, context_) == EGL_FALSE) {
    LOOM_LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
    return false;
  }

  return true;
}

}  // namespace loom::quest
