#include "surface_texture.hpp"

#include <GLES2/gl2ext.h>  // GL_TEXTURE_EXTERNAL_OES
#include <android/surface_texture.h>
#include <android/surface_texture_jni.h>

#include "log.hpp"

namespace loom::quest {

SurfaceTexture::~SurfaceTexture() {
  if (window_ != nullptr) ANativeWindow_release(window_);
  if (ast_ != nullptr) ASurfaceTexture_release(ast_);

  if (surface_texture_ != nullptr && vm_ != nullptr) {
    JNIEnv* env = nullptr;
    if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
      env->DeleteGlobalRef(surface_texture_);
    }
  }

  if (texture_ != 0) glDeleteTextures(1, &texture_);
}

bool SurfaceTexture::create(JavaVM* vm) {
  vm_ = vm;

  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

  // android_main runs on a native-glue thread that is not attached to the JVM;
  // attach it so we can construct the SurfaceTexture. It stays attached for the
  // life of the render thread.
  JNIEnv* env = nullptr;
  if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) {
    LOOM_LOGE("AttachCurrentThread failed");
    return false;
  }

  jclass cls = env->FindClass("android/graphics/SurfaceTexture");
  jmethodID ctor = env->GetMethodID(cls, "<init>", "(I)V");
  jobject local = env->NewObject(cls, ctor, static_cast<jint>(texture_));
  if (local == nullptr) {
    LOOM_LOGE("SurfaceTexture construction failed");
    return false;
  }
  surface_texture_ = env->NewGlobalRef(local);
  env->DeleteLocalRef(local);
  env->DeleteLocalRef(cls);

  ast_ = ASurfaceTexture_fromSurfaceTexture(env, surface_texture_);
  if (ast_ == nullptr) {
    LOOM_LOGE("ASurfaceTexture_fromSurfaceTexture failed");
    return false;
  }

  window_ = ASurfaceTexture_acquireANativeWindow(ast_);
  if (window_ == nullptr) {
    LOOM_LOGE("acquireANativeWindow failed");
    return false;
  }

  return true;
}

bool SurfaceTexture::update(float transform[16]) {
  // updateTexImage always latches the newest available buffer (or keeps the
  // current one when none arrived); the timestamp only advances on a genuinely
  // new frame, which is how we know whether to re-blit — the compositor keeps
  // re-sampling the cylinder swapchain for free when we don't.
  const int64_t before = ASurfaceTexture_getTimestamp(ast_);
  if (ASurfaceTexture_updateTexImage(ast_) != 0) return false;
  const int64_t after = ASurfaceTexture_getTimestamp(ast_);

  ASurfaceTexture_getTransformMatrix(ast_, transform);

  return after != before;
}

}  // namespace loom::quest
