#pragma once
// OpenXR lifecycle and the frame loop.
//
// Structure follows the Meta XrCompositor_NativeActivity sample, trimmed to what
// Loom needs: a projection layer holding the floor grid, the cylinder holding
// the desktop, and (when toggled) the debug-overlay quad. No cube/equirect
// demos, no foveation.

// openxr_platform.h's Android structs are declared in terms of JNI and EGL
// types, so both must be included ahead of it.
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "egl_context.hpp"
#include "gl_scene.hpp"
#include "hevc_decoder.hpp"
#include "net_session.hpp"
#include "overlay.hpp"
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

// One streamed desktop: a cylinder layer with its own swapchain, the decode path
// feeding it, and where it sits in space. The primary display is window 0; each
// extra display (§3.4 multi-display) is another window, placed side-by-side.
// Non-movable (owns a SurfaceTexture + HevcDecoder), so held via unique_ptr.
struct DesktopWindow {
  std::uint16_t stream_id = 0;

  // Cylinder layer swapchain, sized to this stream, with its own mip chain.
  XrSwapchain swapchain = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageOpenGLESKHR> images;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mip_count = 1;
  bool painted = false;  // static test image painted at least once

  // Placement: yaw (radians) about the user, so windows fan out side-by-side.
  float yaw = 0.0f;

  // Decode path: the codec renders into the SurfaceTexture, whose OES texture the
  // render thread blits into the cylinder swapchain.
  SurfaceTexture surface;
  HevcDecoder decoder;
  bool decoder_active = false;

  // Newest e2e latency (capture→display, µs) for this stream (§4.5).
  std::uint64_t e2e_us = 0;
  bool have_e2e = false;
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

  // Drives the QUIC session/transport once per loop iteration, independent of
  // the OpenXR render state so the handshake progresses before rendering starts.
  // Brings the decoder up when streaming begins; sends STATS once per second.
  void pump_network();

  // Renders and submits one frame. No-op unless the session is running.
  void render_frame();

  bool session_running() const { return session_running_; }

  // Toggles the in-headset debug overlay (wired to the headset volume-up key).
  void toggle_overlay() { overlay_.toggle(); }

  // Toggles cylinder sharpening at runtime (volume-down) for A/B comparison; a
  // no-op when the runtime lacks the extension.
  void toggle_sharpening();

 private:
  std::vector<std::string> overlay_lines();

  bool create_instance(android_app* app);
  bool create_session();
  bool create_swapchains();
  bool create_cylinder_swapchain(DesktopWindow& window, uint32_t width, uint32_t height);
  void request_refresh_rate(float hz);
  void handle_session_state(XrSessionState state, bool* exit_requested);
  void render_eye(const XrView& view, EyeSwapchain& eye);

  // Reads host:port from the app's external files dir (loom_host.txt) and
  // connects; leaves the app on the static image if none is configured.
  void connect_from_config(android_app* app);
  // Brings up the decoder against the live receiver once streaming begins.
  void start_decoder();

  // Blits the current cylinder source into a window's next swapchain image. The
  // static-image path paints once; the video path repaints only on a new frame.
  void paint_cylinder_test_image(DesktopWindow& window);
  void paint_cylinder_from_decoder(DesktopWindow& window, const float transform[16]);
  void blit_into_cylinder(DesktopWindow& window, GLuint program, GLenum target, GLuint texture,
                          const float* transform);

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

  // One cylinder per streamed display (window 0 = primary). Built at create()
  // with just the primary (showing the test image); the extra displays are added
  // in start_decoder() once CONFIG (key 6) is known. Mip chains are regenerated
  // per blit so the compositor trilinear-filters the minified desktop instead of
  // aliasing the base level into edge shimmer.
  std::vector<std::unique_ptr<DesktopWindow>> windows_;
  JavaVM* vm_ = nullptr;  // for SurfaceTexture creation on extra windows

  // Whether the runtime advertised XR_FB_composition_layer_settings; gates the
  // cylinder layer's settings chain (§6.2), which carries both quality
  // super-sampling (anti-aliases the minified desktop — kills edge shimmer) and
  // sharpening. sharpening_on_ is the runtime A/B toggle (volume-down) for the
  // sharpening flag only; super-sampling stays on whenever the extension exists.
  bool layer_settings_supported_ = false;
  bool sharpening_on_ = true;

  FloorGrid grid_;
  GLuint test_texture_ = 0;
  GLuint blit_program_ = 0;      // samples a normal 2D texture (test image)
  GLuint oes_blit_program_ = 0;  // samples the decoder's OES external texture
  GLuint blit_vao_ = 0;

  // The transport + session pump. optional because it is non-movable and built
  // in create() once HELLO params are known. Absent when no host is configured.
  std::optional<NetSession> net_session_;
  std::int64_t last_stats_us_ = 0;

  // Debug overlay + the bitrate estimate it shows (bytes over a 1 s window).
  Overlay overlay_;
  std::uint64_t bitrate_kbps_ = 0;
  std::uint64_t bitrate_base_bytes_ = 0;
  std::int64_t bitrate_base_us_ = 0;
};

}  // namespace loom::quest
