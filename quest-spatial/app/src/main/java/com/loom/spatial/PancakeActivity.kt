package com.loom.spatial

import android.content.Intent
import android.os.Bundle
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.meta.spatial.uiset.button.PrimaryButton
import com.meta.spatial.uiset.button.SecondaryButton
import com.meta.spatial.uiset.theme.LocalColorScheme
import com.meta.spatial.uiset.theme.LocalTypography
import com.meta.spatial.uiset.theme.SpatialTheme
import com.meta.spatial.uiset.theme.darkSpatialColorScheme
import com.meta.spatial.uiset.theme.icons.SpatialIcons
import com.meta.spatial.uiset.theme.icons.regular.FullScreen
import kotlinx.coroutines.delay

/**
 * Loom windowed mode (ROADMAP M3.5): the desktop as a 2D window in Horizon Home,
 * composited by the OS — the sharp, shimmer-free path. Default launch
 * (com.oculus.intent.category.2D); Home draws, resizes, and curves the window.
 *
 * The [SurfaceView] is what the decoder renders into; a Compose overlay sits on
 * top of it (the SurfaceView keeps its default below-window Z-order, so the
 * overlay draws over the video and the video shows through the overlay's
 * transparent regions). The overlay is the auto-hiding pill control (M3.5): its
 * "Match to window" action reads the surface's pixel size and asks the host to
 * stream at exactly that resolution (VIEWPORT, §3.10), which clears minification
 * shimmer. It also enters the immersive many-monitor workspace (SpatialActivity),
 * which the OS window model cannot provide (it caps concurrent windows).
 */
class PancakeActivity : ComponentActivity(), SurfaceHolder.Callback {

  // The live surface size in pixels, published from surfaceChanged. This is the
  // resolution "Match to window" requests, so the decoded video maps ~1:1.
  private val surfaceWidth = mutableIntStateOf(0)
  private val surfaceHeight = mutableIntStateOf(0)

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)

    setContent {
      SpatialTheme(colorScheme = darkSpatialColorScheme()) {
        Box(Modifier.fillMaxSize()) {
          // The decoder target. Created here so the native bridge gets the
          // Surface straight from the holder — no lockCanvas, or the surface
          // gets a CPU producer and AMediaCodec_configure fails (-10000).
          AndroidView(
              factory = { ctx ->
                SurfaceView(ctx).apply { holder.addCallback(this@PancakeActivity) }
              },
              modifier = Modifier.fillMaxSize())

          PillControl(
              width = surfaceWidth.intValue,
              height = surfaceHeight.intValue,
              onMatch = { w, h -> nativeSetViewport(w, h) },
              onImmersive = { launchImmersive() },
              modifier = Modifier.align(Alignment.TopCenter))
        }
      }
    }
  }

  private fun launchImmersive() {
    startActivity(
        Intent(this, SpatialActivity::class.java).apply {
          action = Intent.ACTION_MAIN
          addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        })
  }

  override fun surfaceCreated(holder: SurfaceHolder) {
    nativeStart(HOST, PORT, holder.surface)
  }

  override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
    surfaceWidth.intValue = width
    surfaceHeight.intValue = height
  }

  override fun surfaceDestroyed(holder: SurfaceHolder) {
    nativeStop()
  }

  private external fun nativeStart(host: String, port: Int, surface: Surface)

  private external fun nativeStop()

  private external fun nativeSetViewport(width: Int, height: Int)

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

/** Milliseconds of no interaction before the pill collapses to a thin line. */
private const val AUTO_HIDE_MS = 4000L

/**
 * The windowed control surface: an expanded pill with a "Match to window" action
 * and the immersive entry, collapsing to a thin line after [AUTO_HIDE_MS] of no
 * interaction. Any tap re-expands it and restarts the idle timer.
 */
@Composable
private fun PillControl(
    width: Int,
    height: Int,
    onMatch: (Int, Int) -> Unit,
    onImmersive: () -> Unit,
    modifier: Modifier = Modifier,
) {
  var expanded by remember { mutableStateOf(true) }
  // Bumped on every interaction so the auto-hide timer restarts even when we are
  // already expanded (toggling `expanded` alone wouldn't re-key the effect).
  var interaction by remember { mutableIntStateOf(0) }

  LaunchedEffect(expanded, interaction) {
    if (expanded) {
      delay(AUTO_HIDE_MS)
      expanded = false
    }
  }

  Box(modifier.fillMaxWidth().padding(top = 20.dp), contentAlignment = Alignment.TopCenter) {
    if (expanded) {
      ExpandedPill(
          width = width,
          height = height,
          onMatch = {
            if (width > 0 && height > 0) onMatch(width, height)
            interaction++
          },
          onImmersive = {
            onImmersive()
            interaction++
          })
    } else {
      CollapsedPill(
          onClick = {
            expanded = true
            interaction++
          })
    }
  }
}

@Composable
private fun ExpandedPill(
    width: Int,
    height: Int,
    onMatch: () -> Unit,
    onImmersive: () -> Unit,
) {
  Row(
      modifier =
          Modifier.clip(RoundedCornerShape(28.dp))
              .background(brush = LocalColorScheme.current.panel)
              .padding(horizontal = 20.dp, vertical = 12.dp),
      verticalAlignment = Alignment.CenterVertically,
      horizontalArrangement = Arrangement.spacedBy(16.dp)) {
        Text(
            text = if (width > 0) "Window ${width}×${height}" else "Window …",
            style =
                LocalTypography.current.body2.copy(
                    color = LocalColorScheme.current.primaryAlphaBackground))
        PrimaryButton(
            label = "Match to window",
            leading = { Icon(SpatialIcons.Regular.FullScreen, contentDescription = null) },
            onClick = onMatch)
        SecondaryButton(label = "Monitors", onClick = onImmersive)
      }
}

@Composable
private fun CollapsedPill(onClick: () -> Unit) {
  // A generous, easy-to-hit tap target around a thin visible line.
  Box(
      modifier = Modifier.height(28.dp).width(140.dp).clickable(onClick = onClick),
      contentAlignment = Alignment.Center) {
        Box(
            Modifier.height(8.dp)
                .width(120.dp)
                .clip(RoundedCornerShape(50))
                .background(brush = LocalColorScheme.current.panel))
      }
}
