#pragma once
// The in-headset debug overlay: the M1.3 instrumentation fields (e2e latency,
// rtt, decode ms, loss %, bitrate) drawn as text on an OpenXR quad composition
// layer floating below the cylinder.
//
// It is its own layer, not painted onto the cylinder, so it refreshes every
// frame regardless of video — the stats keep ticking even when the stream
// freezes. Text is rasterized on the CPU with a tiny embedded bitmap font (no
// font library) and uploaded to the quad swapchain each frame it is visible.

// openxr_platform.h's GLES swapchain-image struct is declared in terms of EGL
// and JNI types, so those precede it (matching xr_app.hpp).
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <string>
#include <vector>

namespace loom::quest {

class Overlay {
 public:
  ~Overlay();

  bool create(XrSession session);

  bool visible() const { return visible_; }
  void toggle() { visible_ = !visible_; }

  // Rasterizes `lines` into the quad swapchain and fills `out` with the quad
  // layer to submit in `space`. Call each frame while visible(). Returns false
  // on a swapchain error (skip the layer that frame).
  bool build_layer(XrSpace space, const std::vector<std::string>& lines,
                   XrCompositionLayerQuad* out);

 private:
  XrSwapchain swapchain_ = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageOpenGLESKHR> images_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::vector<std::uint8_t> pixels_;  // RGBA scratch, width_*height_*4
  bool visible_ = false;
};

}  // namespace loom::quest
