package com.loom.spatial

import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView

/**
 * The one process-wide handle to the native streaming stack (jni_bridge.cpp). A
 * single QUIC session fans out to one decoder per host display; each display is
 * shown in its own 2D Home window (a separate Activity/task), and every window
 * talks to the same session through this object.
 *
 * Lifecycle: the primary window ([PancakeActivity]) calls [start]/[stop]; every
 * window (primary + [DisplayWindowActivity]) attaches its own surface by slot.
 */
object NativeBridge {
  init {
    System.loadLibrary("loom_panel")
  }

  /** Start the QUIC session (idempotent — only the first call has effect). */
  external fun start(host: String, port: Int)

  /** Stop the session and tear every decoder down. */
  external fun stop()

  /** Ask the host to stream the primary display at this size (VIEWPORT, §3.10). */
  external fun setViewport(width: Int, height: Int)

  /** Attach (or replace) the surface a decoder for stream `slot` renders into. */
  external fun attachSurface(slot: Int, surface: Surface)

  /** Detach the surface for `slot` (its decoder stops). */
  external fun detachSurface(slot: Int)

  /** Number of video streams the host is sending (0 until CONFIG). */
  external fun streamCount(): Int
}

/**
 * One decoder target: a [SurfaceView] that attaches to native [slot] on create
 * and detaches on destroy, reporting its pixel size via [onSize]. Created without
 * lockCanvas so the surface has a GPU producer (AMediaCodec).
 */
@Composable
fun DisplaySurface(
    slot: Int,
    onSize: (Int, Int) -> Unit = { _, _ -> },
    modifier: Modifier = Modifier,
) {
  AndroidView(
      factory = { ctx ->
        SurfaceView(ctx).apply {
          holder.addCallback(
              object : SurfaceHolder.Callback {
                override fun surfaceCreated(holder: SurfaceHolder) {
                  NativeBridge.attachSurface(slot, holder.surface)
                }

                override fun surfaceChanged(
                    holder: SurfaceHolder,
                    format: Int,
                    width: Int,
                    height: Int,
                ) {
                  onSize(width, height)
                }

                override fun surfaceDestroyed(holder: SurfaceHolder) {
                  NativeBridge.detachSurface(slot)
                }
              })
        }
      },
      modifier = modifier)
}
