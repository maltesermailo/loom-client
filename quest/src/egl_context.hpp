#pragma once
// The EGL context OpenXR renders through. There is no window surface: the
// compositor owns the display, so we render into swapchain images and only need
// a context to be current on this thread.

#include <EGL/egl.h>

namespace loom::quest {

class EglContext {
 public:
  EglContext() = default;
  ~EglContext();

  EglContext(const EglContext&) = delete;
  EglContext& operator=(const EglContext&) = delete;

  bool create();

  EGLDisplay display() const { return display_; }
  EGLConfig config() const { return config_; }
  EGLContext context() const { return context_; }

 private:
  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLConfig config_ = nullptr;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLSurface surface_ = EGL_NO_SURFACE;
};

}  // namespace loom::quest
