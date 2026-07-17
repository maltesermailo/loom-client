#pragma once
// The decode output target: a GL_TEXTURE_EXTERNAL_OES texture fed by an Android
// SurfaceTexture, which the HEVC decoder renders into in surface mode.
//
// There is no pure-NDK way to create a SurfaceTexture — ASurfaceTexture wraps a
// Java android.graphics.SurfaceTexture — so we construct one Java object via JNI
// and wrap it. Everything after that (acquire window, update, transform) is NDK.
//
// Threading: construct and update() on the render thread (they touch the GL
// context). The ANativeWindow handed to the decoder is a producer and is used
// from the decode thread; that split is exactly what SurfaceTexture is for.

#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <android/surface_texture.h>
#include <jni.h>

namespace loom::quest {

class SurfaceTexture {
 public:
  SurfaceTexture() = default;
  ~SurfaceTexture();

  SurfaceTexture(const SurfaceTexture&) = delete;
  SurfaceTexture& operator=(const SurfaceTexture&) = delete;

  // Creates the OES texture and the backing SurfaceTexture. Must run on the
  // render thread with the GL context current; `vm` is app->activity->vm.
  bool create(JavaVM* vm);

  // The decoder's output surface (surface mode). Valid after create().
  ANativeWindow* window() const { return window_; }

  GLuint texture() const { return texture_; }

  // Latches the most recently decoded frame into the OES texture. Render thread
  // only. Returns false if no new frame was pending. On true, `transform` is the
  // 4x4 UV transform and `timestamp_ns` is the frame's presentation timestamp —
  // the decoder set it to the host capture_ts (µs) so it comes back as capture_ts
  // × 1000, feeding the e2e latency overlay (§4.5).
  bool update(float transform[16], int64_t* timestamp_ns);

 private:
  GLuint texture_ = 0;
  jobject surface_texture_ = nullptr;  // global ref
  ASurfaceTexture* ast_ = nullptr;
  ANativeWindow* window_ = nullptr;
  JavaVM* vm_ = nullptr;
};

}  // namespace loom::quest
