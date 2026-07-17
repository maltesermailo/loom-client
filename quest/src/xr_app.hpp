#pragma once
// OpenXR lifecycle and the frame loop.
//
// Structure follows the Meta XrCompositor_NativeActivity sample, trimmed to what
// Loom needs: two layers (a projection layer holding the floor grid, and the
// cylinder holding the desktop), no cube/equirect/quad demos, no foveation.

// openxr_platform.h's Android structs are declared in terms of JNI and EGL
// types, so both must be included ahead of it.
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <vector>

#include "egl_context.hpp"
#include "gl_scene.hpp"
#include "hevc_decoder.hpp"
#include "surface_texture.hpp"

struct android_app;

namespace loom::quest {

// One eye's render target: an OpenXR swapchain plus the GL objects needed to
// render into whichever image the runtime hands us this frame.
struct EyeSwapchain {
  XrSwapchain handle = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageOpenGLESKHR> images;
  std::vector<GLuint> framebuffers;
  GLuint depth_buffer = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

class XrApp {
 public:
  ~XrApp();

  XrApp(const XrApp&) = delete;
  XrApp& operator=(const XrApp&) = delete;

  XrApp() = default;

  // Must run before any other OpenXR call on Android: the loader needs the VM
  // and Activity to find the runtime.
  static bool init_loader(android_app* app);

  bool create(android_app* app);

  // Drains the OpenXR event queue; drives xrBeginSession / xrEndSession.
  // Returns false once the runtime wants the app gone.
  bool poll_events(bool* exit_requested);

  // Renders and submits one frame. No-op unless the session is running.
  void render_frame();

  bool session_running() const { return session_running_; }

 private:
  bool create_instance(android_app* app);
  bool create_session();
  bool create_swapchains();
  bool create_cylinder_swapchain();
  void request_refresh_rate(float hz);
  void handle_session_state(XrSessionState state, bool* exit_requested);
  void render_eye(const XrView& view, EyeSwapchain& eye);

  // M3.2 decode path: load the looped test bitstream and start the decoder.
  // Falls back to the static test image if the file is absent (M3.1 behavior).
  void start_decoder(android_app* app);

  // Blits the current cylinder source into the layer's next swapchain image. The
  // static-image path paints once; the video path repaints only on a new frame.
  void paint_cylinder_test_image();
  void paint_cylinder_from_decoder(const float transform[16]);
  void blit_into_cylinder(GLuint program, GLenum target, GLuint texture, const float* transform);

  EglContext egl_;

  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId system_id_ = XR_NULL_SYSTEM_ID;
  XrSession session_ = XR_NULL_HANDLE;

  // The cylinder is posed in LOCAL space, so a runtime recenter re-origins the
  // space and the desktop follows the user for free (ROADMAP M3.1).
  XrSpace local_space_ = XR_NULL_HANDLE;

  XrSessionState session_state_ = XR_SESSION_STATE_UNKNOWN;
  bool session_running_ = false;

  std::vector<XrViewConfigurationView> view_configs_;
  std::vector<EyeSwapchain> eyes_;

  XrSwapchain cylinder_swapchain_ = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageOpenGLESKHR> cylinder_images_;
  uint32_t cylinder_width_ = 0;
  uint32_t cylinder_height_ = 0;
  bool cylinder_painted_ = false;

  FloorGrid grid_;
  GLuint test_texture_ = 0;
  GLuint blit_program_ = 0;      // samples a normal 2D texture (test image)
  GLuint oes_blit_program_ = 0;  // samples the decoder's OES external texture
  GLuint blit_vao_ = 0;

  // Video path (M3.2). When decoder_active_ is false the cylinder shows the
  // static test image (M3.1 fallback, e.g. the bitstream file is missing).
  bool decoder_active_ = false;
  SurfaceTexture surface_texture_;
  HevcDecoder decoder_;
};

}  // namespace loom::quest
