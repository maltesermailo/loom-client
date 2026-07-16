#pragma once
// Logging for the Quest client. Everything lands under the `loom` logcat tag:
//   adb logcat -s loom

#include <android/log.h>

#define LOOM_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "loom", __VA_ARGS__)
#define LOOM_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "loom", __VA_ARGS__)
