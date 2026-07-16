# loom-quest — Quest 3 client

The NDK/OpenXR app. As of M3.1 it brings up an OpenXR session and shows a floor
grid in a projection layer plus a cylinder layer holding a static test image.
Transport, decode and real video arrive in M3.2/M3.3 (`spec/ROADMAP.md`).

## Why this is a Gradle build and not a CMake preset

The OpenXR loader is distributed only as a Maven AAR
(`org.khronos.openxr:openxr_loader_for_android`), consumed through AGP's
**prefab** feature — which is what turns it into the `OpenXR::openxr_loader`
CMake target that `CMakeLists.txt` expects. Gradle therefore has to own the
dependency graph; it drives our `CMakeLists.txt` via `externalNativeBuild`.
There is no `quest` CMake preset: it was removed in M3.1 rather than kept as a
placeholder that could not actually build the app.

`client/check.sh` still format-checks `quest/src`, but does not build it — the
hermetic gate stays toolchain-light.

## Toolchain

Pinned deliberately; see `reviews/M3.0-spike-r3-msquic-ndk.md` for the NDK choice.

| Component | Version | Note |
|---|---|---|
| NDK | `28.2.13676358` (r28c) | stable; msquic/quictls verified against it |
| AGP / Gradle | 8.13.2 / 8.14.3 | |
| Kotlin | 2.3.21 | the WiFi shim only |
| JDK | **21** | AGP does not support the JDK 26 that is `java` on this Mac |
| compileSdk / targetSdk | 36 | `platforms;android-36` |
| minSdk | **29** | msquic is built for `android-29`; `WIFI_MODE_FULL_LOW_LATENCY` is API 29+ |
| OpenXR loader | 1.1.61 | Maven AAR, via prefab |

`local.properties` (git-ignored) must point at the SDK:

```properties
sdk.dir=/Users/<you>/Library/Android/sdk
```

## Build and deploy

JDK 21 must be selected — the default JDK on this machine is 26, which AGP
rejects:

```sh
export JAVA_HOME=$(/usr/libexec/java_home -v 21)

cd client/quest
./gradlew assembleDebug
adb install -r build/outputs/apk/debug/loom-quest-debug.apk
adb shell am start -n com.loom.quest/.LoomActivity
```

`./gradlew installDebug` also works and skips the explicit `adb install`.

### Wireless adb

Preferred: the app is developed with the headset untethered. Once over USB:

```sh
adb tcpip 5555
adb connect <quest-ip>:5555      # e.g. 192.168.178.162:5555
adb devices -l                   # should list ...:5555  device  model:Quest_3
```

The headset's IP is in Settings → Wi-Fi → (network) → Advanced. Note the host
must reach the headset on the same LAN — the same path `loomd` streams over.

### Logs

Everything the app logs uses the `loom` tag:

```sh
adb logcat -s loom
```

Runtime frame statistics (FPS, stale frames, app render time) come from the
compositor, not from us:

```sh
adb logcat -s VrApi
```

## Layout

| Path | What |
|---|---|
| `AndroidManifest.xml` | NativeActivity + VR intent category + WiFi permissions |
| `kotlin/…/LoomActivity.kt` | the only JVM code: holds `WIFI_MODE_FULL_LOW_LATENCY` |
| `CMakeLists.txt` | native library, driven by Gradle |
| `src/main.cpp` | `android_main`, lifecycle, frame loop |
| `src/xr_app.{hpp,cpp}` | OpenXR instance/session/swapchains, layer submission |
| `src/egl_context.{hpp,cpp}` | EGL context (no window surface) |
| `src/gl_scene.{hpp,cpp}` | floor grid + test texture |
