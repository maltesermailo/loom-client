#include "xr_app.hpp"

#include <GLES2/gl2ext.h>  // GL_TEXTURE_EXTERNAL_OES
#include <android_native_app_glue.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "log.hpp"

namespace loom::quest {
namespace {

// Client monotonic clock in microseconds (§7 uses it for CLOCK_PING t0/PONG t3).
std::int64_t now_us() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

// Stream geometry from ARCHITECTURE §2; the cylinder swapchain is sized to the
// video it will carry from M3.2 on, so the layer does not change shape later.
constexpr uint32_t kCylinderWidth = 2560;
constexpr uint32_t kCylinderHeight = 1440;

// ARCHITECTURE §6.2 defaults. M3.4 makes these user-adjustable.
constexpr float kCylinderRadius = 1.8f;
constexpr float kCylinderCentralAngle = 55.0f * 3.14159265f / 180.0f;
constexpr float kCylinderAspect = 16.0f / 9.0f;

constexpr float kNearZ = 0.1f;
constexpr float kFarZ = 100.0f;

constexpr float kTargetRefreshHz = 72.0f;

bool xr_failed(XrResult result, const char* what) {
  if (XR_SUCCEEDED(result)) return false;

  LOOM_LOGE("%s failed: XrResult %d", what, static_cast<int>(result));
  return true;
}

// Two-call enumeration of the runtime's instance extensions. Used to gate
// optional extensions: an unsupported name passed to xrCreateInstance fails the
// whole call, so we probe first rather than assume availability.
bool instance_extension_supported(const char* name) {
  uint32_t count = 0;
  if (xr_failed(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr),
                "xrEnumerateInstanceExtensionProperties(count)")) {
    return false;
  }

  std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
  if (xr_failed(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data()),
                "xrEnumerateInstanceExtensionProperties")) {
    return false;
  }

  for (const auto& prop : props) {
    if (std::strcmp(prop.extensionName, name) == 0) return true;
  }
  return false;
}

// Both cylinder blits share this vertex shader. uTexTransform folds in the
// source's coordinate convention: a Y-flip for the top-down test image, or the
// SurfaceTexture matrix (crop + flip) for decoder output.
constexpr const char* kBlitVertexShader = R"(#version 300 es
uniform mat4 uTexTransform;
out vec2 vUv;
void main() {
  // Fullscreen triangle from gl_VertexID; no vertex buffer needed.
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  vUv = (uTexTransform * vec4(p, 0.0, 1.0)).xy;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kBlitFragmentShader = R"(#version 300 es
precision mediump float;
uniform sampler2D uTexture;
in vec2 vUv;
out vec4 outColor;
void main() {
  outColor = texture(uTexture, vUv);
}
)";

// The decoder renders into a GL_TEXTURE_EXTERNAL_OES texture, which needs the
// external-sampler extension and its own sampler type.
constexpr const char* kOesBlitFragmentShader = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES uTexture;
in vec2 vUv;
out vec4 outColor;
void main() {
  outColor = texture(uTexture, vUv);
}
)";

// Column-major (s,t) -> (s, 1-t): the test texture is uploaded top-down, but GL
// samples bottom-up. The OES path uses the SurfaceTexture matrix instead.
constexpr float kFlipY[16] = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1};

GLuint build_program(const char* vertex_src, const char* fragment_src) {
  const auto compile = [](GLenum type, const char* src) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    return shader;
  };

  const GLuint vs = compile(GL_VERTEX_SHADER, vertex_src);
  const GLuint fs = compile(GL_FRAGMENT_SHADER, fragment_src);

  const GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_FALSE) {
    char log[512];
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    LOOM_LOGE("program link failed: %s", log);
    glDeleteProgram(program);
    return 0;
  }

  return program;
}

Mat4 projection_from_fov(const XrFovf& fov) {
  const float tan_left = std::tan(fov.angleLeft);
  const float tan_right = std::tan(fov.angleRight);
  const float tan_down = std::tan(fov.angleDown);
  const float tan_up = std::tan(fov.angleUp);
  const float tan_width = tan_right - tan_left;
  const float tan_height = tan_up - tan_down;

  // Asymmetric-frustum perspective projection; the eye frusta are off-centre, so
  // this cannot be simplified to a symmetric fovy form.
  Mat4 m{};
  m.m[0] = 2.0f / tan_width;
  m.m[5] = 2.0f / tan_height;
  m.m[8] = (tan_right + tan_left) / tan_width;
  m.m[9] = (tan_up + tan_down) / tan_height;
  m.m[10] = -(kFarZ + kNearZ) / (kFarZ - kNearZ);
  m.m[11] = -1.0f;
  m.m[14] = -2.0f * kFarZ * kNearZ / (kFarZ - kNearZ);

  return m;
}

Mat4 view_from_pose(const XrPosef& pose) {
  const XrQuaternionf& q = pose.orientation;
  const float x = q.x, y = q.y, z = q.z, w = q.w;

  // Rotation matrix of the pose (column-major).
  Mat4 rotation{};
  rotation.m[0] = 1.0f - 2.0f * (y * y + z * z);
  rotation.m[1] = 2.0f * (x * y + z * w);
  rotation.m[2] = 2.0f * (x * z - y * w);
  rotation.m[4] = 2.0f * (x * y - z * w);
  rotation.m[5] = 1.0f - 2.0f * (x * x + z * z);
  rotation.m[6] = 2.0f * (y * z + x * w);
  rotation.m[8] = 2.0f * (x * z + y * w);
  rotation.m[9] = 2.0f * (y * z - x * w);
  rotation.m[10] = 1.0f - 2.0f * (x * x + y * y);
  rotation.m[15] = 1.0f;

  // The view matrix is the inverse of the head transform: transpose the
  // rotation (orthonormal) and translate by the negated position.
  Mat4 inverse_rotation{};
  for (int col = 0; col < 3; ++col) {
    for (int row = 0; row < 3; ++row) {
      inverse_rotation.m[col * 4 + row] = rotation.m[row * 4 + col];
    }
  }
  inverse_rotation.m[15] = 1.0f;

  Mat4 inverse_translation{};
  inverse_translation.m[0] = inverse_translation.m[5] = inverse_translation.m[10] = 1.0f;
  inverse_translation.m[15] = 1.0f;
  inverse_translation.m[12] = -pose.position.x;
  inverse_translation.m[13] = -pose.position.y;
  inverse_translation.m[14] = -pose.position.z;

  return multiply(inverse_rotation, inverse_translation);
}

}  // namespace

XrApp::~XrApp() {
  if (test_texture_ != 0) glDeleteTextures(1, &test_texture_);
  if (blit_program_ != 0) glDeleteProgram(blit_program_);
  if (blit_vao_ != 0) glDeleteVertexArrays(1, &blit_vao_);

  for (EyeSwapchain& eye : eyes_) {
    if (!eye.framebuffers.empty()) {
      glDeleteFramebuffers(static_cast<GLsizei>(eye.framebuffers.size()), eye.framebuffers.data());
    }
    if (eye.depth_buffer != 0) glDeleteRenderbuffers(1, &eye.depth_buffer);
    if (eye.handle != XR_NULL_HANDLE) xrDestroySwapchain(eye.handle);
  }

  if (cylinder_swapchain_ != XR_NULL_HANDLE) xrDestroySwapchain(cylinder_swapchain_);
  if (local_space_ != XR_NULL_HANDLE) xrDestroySpace(local_space_);
  if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
  if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
}

bool XrApp::init_loader(android_app* app) {
  PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
  if (xr_failed(
          xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                reinterpret_cast<PFN_xrVoidFunction*>(&xrInitializeLoaderKHR)),
          "xrGetInstanceProcAddr(xrInitializeLoaderKHR)")) {
    return false;
  }

  XrLoaderInitInfoAndroidKHR init_info = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
  init_info.applicationVM = app->activity->vm;
  init_info.applicationContext = app->activity->clazz;

  return !xr_failed(
      xrInitializeLoaderKHR(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&init_info)),
      "xrInitializeLoaderKHR");
}

bool XrApp::create(android_app* app) {
  if (!egl_.create()) return false;
  if (!create_instance(app)) return false;
  if (!create_session()) return false;
  if (!create_swapchains()) return false;
  if (!create_cylinder_swapchain()) return false;

  if (!grid_.create()) return false;
  test_texture_ = create_test_texture(kCylinderWidth, kCylinderHeight);
  blit_program_ = build_program(kBlitVertexShader, kBlitFragmentShader);
  oes_blit_program_ = build_program(kBlitVertexShader, kOesBlitFragmentShader);
  if (blit_program_ == 0 || oes_blit_program_ == 0) return false;
  glGenVertexArrays(1, &blit_vao_);

  // The SurfaceTexture must be created on the render thread with GL current
  // (here); the decoder attaches to its window later, when streaming begins.
  if (!surface_texture_.create(app->activity->vm)) {
    LOOM_LOGE("SurfaceTexture setup failed — video unavailable");
  }

  overlay_.create(session_);

  connect_from_config(app);
  request_refresh_rate(kTargetRefreshHz);

  return true;
}

void XrApp::connect_from_config(android_app* app) {
  // Dev config (M7 replaces this with mDNS discovery): host or host:port in
  // <externalDataPath>/loom_host.txt, adb-pushable without a rebuild. Absent →
  // no connection, the cylinder stays on the static test image.
  const std::string path = std::string(app->activity->externalDataPath) + "/loom_host.txt";
  std::ifstream file(path);
  if (!file) {
    LOOM_LOGI("no %s — not connecting (static image)", path.c_str());
    return;
  }

  std::string line;
  std::getline(file, line);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
    line.pop_back();
  }
  std::string host = line;
  std::uint16_t port = 47800;
  if (const auto colon = line.rfind(':'); colon != std::string::npos) {
    host = line.substr(0, colon);
    port = static_cast<std::uint16_t>(std::stoi(line.substr(colon + 1)));
  }
  if (host.empty()) return;

  loom::core::HelloParams params;
  params.client_name = "loom-quest";
  net_session_.emplace(params);
  if (!net_session_->start(host, port)) {
    LOOM_LOGE("failed to start QUIC connection to %s:%u", host.c_str(), port);
    net_session_.reset();
    return;
  }
  LOOM_LOGI("connecting to %s:%u", host.c_str(), port);
}

void XrApp::start_decoder() {
  if (!decoder_.create(surface_texture_.window(), static_cast<int>(cylinder_width_),
                       static_cast<int>(cylinder_height_))) {
    return;
  }
  decoder_.start(&net_session_->receiver());
  decoder_active_ = true;
  LOOM_LOGI("decoder started (streaming)");
}

void XrApp::pump_network() {
  if (!net_session_) return;

  const std::int64_t now = now_us();
  net_session_->pump(now);

  if (net_session_->just_started_streaming()) {
    start_decoder();
    last_stats_us_ = now;
    bitrate_base_us_ = now;
    bitrate_base_bytes_ = 0;
  }

  // STATS every second (§3.7). Frame/loss counts come from the receiver; decode
  // and rtt/e2e from here. The bitrate estimate (bytes over the window) feeds the
  // overlay only — it is not a §3.7 field.
  if (decoder_active_ && now - last_stats_us_ >= 1'000'000) {
    const auto c = net_session_->receiver().counters();

    const double secs = static_cast<double>(now - bitrate_base_us_) / 1e6;
    if (secs > 0.0) {
      bitrate_kbps_ = static_cast<std::uint64_t>(
          static_cast<double>(c.bytes - bitrate_base_bytes_) * 8.0 / 1000.0 / secs);
    }
    bitrate_base_bytes_ = c.bytes;
    bitrate_base_us_ = now;

    loom::core::StatsInput in;
    in.frames_received = c.frames_received;
    in.frames_dropped = c.frames_dropped;
    in.datagrams = c.datagrams;
    in.decode_us = static_cast<std::uint64_t>(decoder_.metrics().latest_ms * 1000.0f);
    if (const auto clk = net_session_->clock()) {
      in.rtt_us = static_cast<std::uint64_t>(std::max<std::int64_t>(0, clk->rtt));
    }
    if (have_e2e_) in.e2e_us = e2e_us_;
    net_session_->send_stats(in);
    last_stats_us_ = now;
  }
}

std::vector<std::string> XrApp::overlay_lines() {
  char buf[64];
  std::vector<std::string> lines;

  const auto clk = net_session_ ? net_session_->clock() : std::nullopt;
  if (clk && have_e2e_) {
    std::snprintf(buf, sizeof buf, "e2e %llu ms  rtt %llu ms",
                  static_cast<unsigned long long>(e2e_us_ / 1000),
                  static_cast<unsigned long long>(std::max<std::int64_t>(0, clk->rtt) / 1000));
  } else {
    std::snprintf(buf, sizeof buf, "e2e --  rtt --");
  }
  lines.emplace_back(buf);

  double loss_pct = 0.0;
  if (net_session_) {
    const auto c = net_session_->receiver().counters();
    const std::uint64_t total = c.frames_received + c.frames_dropped;
    if (total > 0) loss_pct = 100.0 * static_cast<double>(c.frames_dropped) / total;
  }
  std::snprintf(buf, sizeof buf, "decode %.1f ms  loss %.2f%%",
                decoder_active_ ? decoder_.metrics().latest_ms : 0.0f, loss_pct);
  lines.emplace_back(buf);

  std::snprintf(buf, sizeof buf, "bitrate %llu kbps",
                static_cast<unsigned long long>(bitrate_kbps_));
  lines.emplace_back(buf);

  return lines;
}

bool XrApp::create_instance(android_app* app) {
  XrInstanceCreateInfoAndroidKHR android_info = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
  android_info.applicationVM = app->activity->vm;
  android_info.applicationActivity = app->activity->clazz;

  // Core extensions: the client cannot run without any of these, so a missing
  // one should (and will) fail xrCreateInstance loudly.
  std::vector<const char*> extensions = {
      XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
      XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
      XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME,
      XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
  };

  // XR_FB_composition_layer_settings (§6.2) drives super-sampling and sharpening
  // on the cylinder layer — an optional quality win, so probe for it and enable
  // it only when the runtime advertises it (confirmed here, not assumed). When
  // absent the layer is simply submitted without the settings chain.
  layer_settings_supported_ =
      instance_extension_supported(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
  if (layer_settings_supported_) {
    extensions.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    LOOM_LOGI("XR_FB_composition_layer_settings present: super-sampling + sharpening enabled");
  } else {
    LOOM_LOGI("XR_FB_composition_layer_settings absent: super-sampling + sharpening off");
  }

  XrInstanceCreateInfo create_info = {XR_TYPE_INSTANCE_CREATE_INFO};
  create_info.next = &android_info;
  create_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  std::strcpy(create_info.applicationInfo.applicationName, "loom");
  create_info.applicationInfo.applicationVersion = 1;
  std::strcpy(create_info.applicationInfo.engineName, "loom");
  create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  create_info.enabledExtensionNames = extensions.data();

  if (xr_failed(xrCreateInstance(&create_info, &instance_), "xrCreateInstance")) return false;

  XrSystemGetInfo system_info = {XR_TYPE_SYSTEM_GET_INFO};
  system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

  return !xr_failed(xrGetSystem(instance_, &system_info, &system_id_), "xrGetSystem");
}

void XrApp::toggle_sharpening() {
  if (!layer_settings_supported_) {
    LOOM_LOGI("sharpening toggle ignored: extension not supported");
    return;
  }

  sharpening_on_ = !sharpening_on_;
  LOOM_LOGI("cylinder sharpening %s", sharpening_on_ ? "on" : "off");
}

bool XrApp::create_session() {
  // Required by the spec before xrCreateSession, even though we ignore the
  // result: the runtime uses the call to validate our GL version.
  PFN_xrGetOpenGLESGraphicsRequirementsKHR get_requirements = nullptr;
  xrGetInstanceProcAddr(instance_, "xrGetOpenGLESGraphicsRequirementsKHR",
                        reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
  XrGraphicsRequirementsOpenGLESKHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
  if (get_requirements != nullptr) get_requirements(instance_, system_id_, &requirements);

  XrGraphicsBindingOpenGLESAndroidKHR binding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
  binding.display = egl_.display();
  binding.config = egl_.config();
  binding.context = egl_.context();

  XrSessionCreateInfo create_info = {XR_TYPE_SESSION_CREATE_INFO};
  create_info.next = &binding;
  create_info.systemId = system_id_;
  if (xr_failed(xrCreateSession(instance_, &create_info, &session_), "xrCreateSession")) {
    return false;
  }

  XrReferenceSpaceCreateInfo space_info = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  space_info.poseInReferenceSpace.orientation.w = 1.0f;

  return !xr_failed(xrCreateReferenceSpace(session_, &space_info, &local_space_),
                    "xrCreateReferenceSpace(LOCAL)");
}

bool XrApp::create_swapchains() {
  uint32_t view_count = 0;
  xrEnumerateViewConfigurationViews(
      instance_, system_id_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
  view_configs_.resize(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  if (xr_failed(xrEnumerateViewConfigurationViews(instance_, system_id_,
                                                  XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                  view_count, &view_count, view_configs_.data()),
                "xrEnumerateViewConfigurationViews")) {
    return false;
  }

  eyes_.resize(view_count);
  for (uint32_t i = 0; i < view_count; ++i) {
    EyeSwapchain& eye = eyes_[i];
    eye.width = view_configs_[i].recommendedImageRectWidth;
    eye.height = view_configs_[i].recommendedImageRectHeight;

    XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = GL_RGBA8;
    info.sampleCount = 1;
    info.width = eye.width;
    info.height = eye.height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (xr_failed(xrCreateSwapchain(session_, &info, &eye.handle), "xrCreateSwapchain(eye))")) {
      return false;
    }

    uint32_t image_count = 0;
    xrEnumerateSwapchainImages(eye.handle, 0, &image_count, nullptr);
    eye.images.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(eye.handle, image_count, &image_count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data()));

    // One framebuffer per swapchain image, plus a shared depth buffer: the
    // runtime hands back a different image each frame and we must not rebind
    // attachments mid-flight.
    glGenRenderbuffers(1, &eye.depth_buffer);
    glBindRenderbuffer(GL_RENDERBUFFER, eye.depth_buffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, static_cast<GLsizei>(eye.width),
                          static_cast<GLsizei>(eye.height));

    eye.framebuffers.resize(image_count);
    glGenFramebuffers(static_cast<GLsizei>(image_count), eye.framebuffers.data());
    for (uint32_t j = 0; j < image_count; ++j) {
      glBindFramebuffer(GL_FRAMEBUFFER, eye.framebuffers[j]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             eye.images[j].image, 0);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                eye.depth_buffer);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOOM_LOGE("eye framebuffer %u incomplete", j);
        return false;
      }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  LOOM_LOGI("swapchains: %u eyes at %ux%u", view_count, eyes_[0].width, eyes_[0].height);

  return true;
}

bool XrApp::create_cylinder_swapchain() {
  cylinder_width_ = kCylinderWidth;
  cylinder_height_ = kCylinderHeight;

  // Full mip chain so the compositor can trilinear-filter the desktop, which it
  // minifies heavily onto the cylinder (2560 source px across ~1000 display px);
  // sampling only the base level aliases high-contrast edges into shimmer.
  uint32_t mip_count = 1;
  for (uint32_t dim = std::max(cylinder_width_, cylinder_height_); dim > 1; dim >>= 1) {
    ++mip_count;
  }

  XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  info.format = GL_RGBA8;
  info.sampleCount = 1;
  info.width = cylinder_width_;
  info.height = cylinder_height_;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = mip_count;

  // Not every runtime allocates mipmapped swapchains; fall back to a single level
  // rather than fail to bring the cylinder up at all.
  if (XR_FAILED(xrCreateSwapchain(session_, &info, &cylinder_swapchain_))) {
    LOOM_LOGI("mipmapped cylinder swapchain rejected; falling back to mipCount 1");
    info.mipCount = mip_count = 1;
    if (xr_failed(xrCreateSwapchain(session_, &info, &cylinder_swapchain_),
                  "xrCreateSwapchain(cylinder)")) {
      return false;
    }
  }
  cylinder_mip_count_ = mip_count;
  LOOM_LOGI("cylinder swapchain: %ux%u, %u mip level(s)", cylinder_width_, cylinder_height_,
            cylinder_mip_count_);

  uint32_t image_count = 0;
  xrEnumerateSwapchainImages(cylinder_swapchain_, 0, &image_count, nullptr);
  cylinder_images_.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
  xrEnumerateSwapchainImages(
      cylinder_swapchain_, image_count, &image_count,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(cylinder_images_.data()));

  return true;
}

void XrApp::request_refresh_rate(float hz) {
  PFN_xrEnumerateDisplayRefreshRatesFB enumerate_rates = nullptr;
  xrGetInstanceProcAddr(instance_, "xrEnumerateDisplayRefreshRatesFB",
                        reinterpret_cast<PFN_xrVoidFunction*>(&enumerate_rates));
  if (enumerate_rates != nullptr) {
    uint32_t count = 0;
    enumerate_rates(session_, 0, &count, nullptr);
    std::vector<float> rates(count);
    enumerate_rates(session_, count, &count, rates.data());

    for (float rate : rates) LOOM_LOGI("supported refresh rate: %.1f Hz", rate);
  }

  PFN_xrRequestDisplayRefreshRateFB request_rate = nullptr;
  xrGetInstanceProcAddr(instance_, "xrRequestDisplayRefreshRateFB",
                        reinterpret_cast<PFN_xrVoidFunction*>(&request_rate));
  if (request_rate == nullptr) return;

  if (!xr_failed(request_rate(session_, hz), "xrRequestDisplayRefreshRateFB")) {
    LOOM_LOGI("requested %.1f Hz", hz);
  }
}

void XrApp::handle_session_state(XrSessionState state, bool* exit_requested) {
  session_state_ = state;

  if (state == XR_SESSION_STATE_READY) {
    XrSessionBeginInfo begin_info = {XR_TYPE_SESSION_BEGIN_INFO};
    begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    session_running_ = !xr_failed(xrBeginSession(session_, &begin_info), "xrBeginSession");
    LOOM_LOGI("session began");
    return;
  }

  if (state == XR_SESSION_STATE_STOPPING) {
    xrEndSession(session_);
    session_running_ = false;
    LOOM_LOGI("session ended");
    return;
  }

  if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
    *exit_requested = true;
  }
}

bool XrApp::poll_events(bool* exit_requested) {
  for (;;) {
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    const XrResult result = xrPollEvent(instance_, &event);
    if (result != XR_SUCCESS) break;

    switch (event.type) {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
        const auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
        handle_session_state(changed.state, exit_requested);
        break;
      }

      // The runtime re-origins LOCAL space on recenter. Nothing to do: the
      // cylinder is posed in that space, so it follows automatically.
      case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
        LOOM_LOGI("reference space changed (recenter)");
        break;

      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
        *exit_requested = true;
        break;

      default:
        break;
    }
  }

  return true;
}

void XrApp::blit_into_cylinder(GLuint program, GLenum target, GLuint texture,
                               const float* transform) {
  uint32_t index = 0;
  XrSwapchainImageAcquireInfo acquire_info = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (xr_failed(xrAcquireSwapchainImage(cylinder_swapchain_, &acquire_info, &index),
                "xrAcquireSwapchainImage(cylinder)")) {
    return;
  }

  XrSwapchainImageWaitInfo wait_info = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;
  xrWaitSwapchainImage(cylinder_swapchain_, &wait_info);

  GLuint framebuffer = 0;
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         cylinder_images_[index].image, 0);

  glViewport(0, 0, static_cast<GLsizei>(cylinder_width_), static_cast<GLsizei>(cylinder_height_));
  glDisable(GL_DEPTH_TEST);
  glUseProgram(program);
  glUniformMatrix4fv(glGetUniformLocation(program, "uTexTransform"), 1, GL_FALSE, transform);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(target, texture);
  glUniform1i(glGetUniformLocation(program, "uTexture"), 0);
  glBindVertexArray(blit_vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  glBindVertexArray(0);
  glBindTexture(target, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Rebuild the mip chain from the base level we just rendered, so the compositor
  // has prefiltered levels to sample under minification.
  if (cylinder_mip_count_ > 1) {
    glBindTexture(GL_TEXTURE_2D, cylinder_images_[index].image);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  glDeleteFramebuffers(1, &framebuffer);

  XrSwapchainImageReleaseInfo release_info = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(cylinder_swapchain_, &release_info);
}

void XrApp::paint_cylinder_test_image() {
  blit_into_cylinder(blit_program_, GL_TEXTURE_2D, test_texture_, kFlipY);
  cylinder_painted_ = true;
}

void XrApp::paint_cylinder_from_decoder(const float transform[16]) {
  blit_into_cylinder(oes_blit_program_, GL_TEXTURE_EXTERNAL_OES, surface_texture_.texture(),
                     transform);
}

void XrApp::render_eye(const XrView& view, EyeSwapchain& eye) {
  uint32_t index = 0;
  XrSwapchainImageAcquireInfo acquire_info = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (xr_failed(xrAcquireSwapchainImage(eye.handle, &acquire_info, &index),
                "xrAcquireSwapchainImage(eye)")) {
    return;
  }

  XrSwapchainImageWaitInfo wait_info = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;
  xrWaitSwapchainImage(eye.handle, &wait_info);

  glBindFramebuffer(GL_FRAMEBUFFER, eye.framebuffers[index]);
  glViewport(0, 0, static_cast<GLsizei>(eye.width), static_cast<GLsizei>(eye.height));

  glEnable(GL_DEPTH_TEST);
  glClearColor(0.02f, 0.02f, 0.04f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  const Mat4 view_proj = multiply(projection_from_fov(view.fov), view_from_pose(view.pose));
  grid_.draw(view_proj);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  XrSwapchainImageReleaseInfo release_info = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(eye.handle, &release_info);
}

void XrApp::render_frame() {
  if (!session_running_) return;

  XrFrameWaitInfo wait_info = {XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState frame_state = {XR_TYPE_FRAME_STATE};
  if (xr_failed(xrWaitFrame(session_, &wait_info, &frame_state), "xrWaitFrame")) return;

  XrFrameBeginInfo begin_info = {XR_TYPE_FRAME_BEGIN_INFO};
  if (xr_failed(xrBeginFrame(session_, &begin_info), "xrBeginFrame")) return;

  std::vector<XrView> views(view_configs_.size(), {XR_TYPE_VIEW});
  XrViewState view_state = {XR_TYPE_VIEW_STATE};
  uint32_t view_count = 0;

  XrViewLocateInfo locate_info = {XR_TYPE_VIEW_LOCATE_INFO};
  locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate_info.displayTime = frame_state.predictedDisplayTime;
  locate_info.space = local_space_;
  xrLocateViews(session_, &locate_info, &view_state, static_cast<uint32_t>(views.size()),
                &view_count, views.data());

  const bool poses_valid = (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0 &&
                           (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;

  std::vector<XrCompositionLayerProjectionView> projection_views(view_count);
  XrCompositionLayerProjection projection_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerCylinderKHR cylinder_layer = {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
  // Chained onto cylinder_layer.next below; declared here so it outlives the
  // render block and stays alive for the xrEndFrame read at function end.
  XrCompositionLayerSettingsFB cylinder_settings = {XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
  XrCompositionLayerQuad overlay_layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  std::vector<const XrCompositionLayerBaseHeader*> layers;

  if (frame_state.shouldRender == XR_TRUE && poses_valid) {
    // Freshness over completeness (§6.4): repaint the cylinder only when the
    // decoder has produced a new frame; otherwise the compositor keeps
    // resampling the last image, so head motion stays smooth through a stall.
    // The static fallback (no decoder) is painted once.
    if (decoder_active_) {
      float transform[16];
      std::int64_t ts_ns = 0;
      if (surface_texture_.update(transform, &ts_ns)) {
        paint_cylinder_from_decoder(transform);

        // The frame timestamp is the host capture_ts (µs) the decoder passed
        // through, ×1000. e2e = now − (capture_ts − offset) (§4.5, §7).
        if (const auto clk = net_session_ ? net_session_->clock() : std::nullopt) {
          const std::int64_t capture_ts = ts_ns / 1000;
          const std::int64_t e2e = now_us() - (capture_ts - clk->offset);
          e2e_us_ = static_cast<std::uint64_t>(std::max<std::int64_t>(0, e2e));
          have_e2e_ = true;
        }
      }
    } else if (!cylinder_painted_) {
      paint_cylinder_test_image();
    }

    for (uint32_t i = 0; i < view_count; ++i) {
      render_eye(views[i], eyes_[i]);

      projection_views[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
      projection_views[i].pose = views[i].pose;
      projection_views[i].fov = views[i].fov;
      projection_views[i].subImage.swapchain = eyes_[i].handle;
      projection_views[i].subImage.imageRect.offset = {0, 0};
      projection_views[i].subImage.imageRect.extent = {static_cast<int32_t>(eyes_[i].width),
                                                       static_cast<int32_t>(eyes_[i].height)};
      projection_views[i].subImage.imageArrayIndex = 0;
    }

    projection_layer.space = local_space_;
    projection_layer.viewCount = view_count;
    projection_layer.views = projection_views.data();
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection_layer));

    cylinder_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    cylinder_layer.space = local_space_;
    cylinder_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    cylinder_layer.subImage.swapchain = cylinder_swapchain_;
    cylinder_layer.subImage.imageRect.offset = {0, 0};
    cylinder_layer.subImage.imageRect.extent = {static_cast<int32_t>(cylinder_width_),
                                                static_cast<int32_t>(cylinder_height_)};
    cylinder_layer.subImage.imageArrayIndex = 0;
    cylinder_layer.pose.orientation.w = 1.0f;
    cylinder_layer.radius = kCylinderRadius;
    cylinder_layer.centralAngle = kCylinderCentralAngle;
    cylinder_layer.aspectRatio = kCylinderAspect;

    // §6.2 layer settings. Super-sampling anti-aliases the desktop, which the
    // compositor minifies ~3:1 onto the cylinder (2560 px source across ~55° of a
    // ~110° FOV); without it, high-contrast edges alias and shimmer as the layer
    // reprojects under head motion. Sharpening rides the volume-down A/B toggle.
    if (layer_settings_supported_) {
      cylinder_settings.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB;
      if (sharpening_on_) {
        cylinder_settings.layerFlags |= XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
      }
      cylinder_layer.next = &cylinder_settings;
    }

    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder_layer));

    // Debug overlay on top, refreshed every frame so its stats keep ticking even
    // when the video is frozen.
    if (overlay_.visible() && overlay_.build_layer(local_space_, overlay_lines(), &overlay_layer)) {
      layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&overlay_layer));
    }
  }

  XrFrameEndInfo end_info = {XR_TYPE_FRAME_END_INFO};
  end_info.displayTime = frame_state.predictedDisplayTime;
  end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end_info.layerCount = static_cast<uint32_t>(layers.size());
  end_info.layers = layers.data();
  xrEndFrame(session_, &end_info);
}

}  // namespace loom::quest
