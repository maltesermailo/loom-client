package com.loom.spatial

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.view.Gravity
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout

/**
 * Loom windowed mode (ROADMAP M3.5): the desktop as a 2D window in Horizon Home,
 * composited by the OS — the sharp, shimmer-free path. Default launch
 * (com.oculus.intent.category.2D); Home draws, resizes, and curves the window.
 *
 * The [SurfaceView] is what the decoder renders into. Phase 2.1 (here) stands the
 * surface up in the window; the next step hands its [android.view.Surface] to the
 * native bridge (client/core + msquic + AMediaCodec via JNI) for the live stream.
 * The button hands off to the immersive many-monitor workspace (SpatialActivity),
 * which the OS window model cannot provide (it caps concurrent windows).
 */
class PancakeActivity : Activity(), SurfaceHolder.Callback {

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)

    val root = FrameLayout(this)

    val surfaceView = SurfaceView(this)
    surfaceView.holder.addCallback(this)
    root.addView(
        surfaceView,
        FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

    val immersiveButton =
        Button(this).apply {
          text = "Enter immersive workspace (many monitors)"
          setOnClickListener { launchImmersive() }
        }
    root.addView(
        immersiveButton,
        FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            .apply { gravity = Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL })

    setContentView(root)
  }

  private fun launchImmersive() {
    startActivity(
        Intent(this, SpatialActivity::class.java).apply {
          action = Intent.ACTION_MAIN
          addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        })
  }

  override fun surfaceCreated(holder: SurfaceHolder) {
    // Hand the surface straight to the native decoder — no lockCanvas here, or the
    // surface gets a CPU producer and AMediaCodec_configure fails (-10000).
    nativeStart(HOST, PORT, holder.surface)
  }

  override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

  override fun surfaceDestroyed(holder: SurfaceHolder) {
    nativeStop()
  }

  private external fun nativeStart(host: String, port: Int, surface: Surface)

  private external fun nativeStop()

  companion object {
    // Dev host; the config-file read (like the OpenXR client's loom_host.txt)
    // lands when msquic is wired in Phase 2.2b.
    private const val HOST = "192.168.178.83"
    private const val PORT = 47800

    init {
      System.loadLibrary("loom_panel")
    }
  }
}
