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
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.meta.spatial.uiset.button.PrimaryCircleButton
import com.meta.spatial.uiset.button.SecondaryCircleButton
import com.meta.spatial.uiset.theme.LocalColorScheme
import com.meta.spatial.uiset.theme.LocalTypography
import com.meta.spatial.uiset.theme.SpatialTheme
import com.meta.spatial.uiset.theme.darkSpatialColorScheme
import com.meta.spatial.uiset.theme.icons.SpatialIcons
import com.meta.spatial.uiset.theme.icons.regular.FullScreen
import com.meta.spatial.uiset.theme.icons.regular.MultiBrowser
import kotlinx.coroutines.delay

/**
 * Loom windowed mode (ROADMAP M3.5 / M6.3): the desktop as 2D window(s) in Horizon
 * Home, composited by the OS — the sharp, shimmer-free path. Default launch
 * (com.oculus.intent.category.2D).
 *
 * Multi-display fan-in: one QUIC session negotiates multi-display, so the host
 * streams one video per display (§3.4). The native bridge publishes the stream
 * count; this Activity lays out that many [SurfaceView]s side-by-side, attaching
 * each by slot — the bridge binds slot k to stream k and decodes into its surface.
 *
 * Each [SurfaceView] is what a decoder renders into; a Compose overlay sits on top
 * (SurfaceViews keep their default below-window Z-order). The overlay is the
 * auto-hiding pill control (M3.5): "Match to window" reads the primary surface's
 * pixel size and asks the host to stream at exactly that resolution (VIEWPORT,
 * §3.10); it also enters the immersive many-monitor workspace (SpatialActivity).
 */
class PancakeActivity : ComponentActivity() {

  // The primary (slot 0) surface size in pixels, published from its surfaceChanged.
  // This is the resolution "Match to window" requests so the video maps ~1:1.
  private val surfaceWidth = mutableIntStateOf(0)
  private val surfaceHeight = mutableIntStateOf(0)

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)

    // Start the session immediately; surfaces attach per slot as they are created.
    nativeStart(HOST, PORT)

    setContent {
      SpatialTheme(colorScheme = darkSpatialColorScheme()) {
        // Poll the negotiated stream count; show at least one panel while connecting.
        var streamCount by remember { mutableIntStateOf(0) }
        LaunchedEffect(Unit) {
          while (true) {
            streamCount = nativeStreamCount()
            delay(200)
          }
        }
        val panels = if (streamCount > 0) streamCount else 1

        Box(Modifier.fillMaxSize()) {
          // One SurfaceView per streamed display, side-by-side.
          Row(Modifier.fillMaxSize()) {
            for (slot in 0 until panels) {
              VideoSurface(
                  slot = slot,
                  onPrimarySize = { w, h ->
                    if (slot == 0) {
                      surfaceWidth.intValue = w
                      surfaceHeight.intValue = h
                    }
                  },
                  modifier = Modifier.weight(1f).fillMaxHeight())
            }
          }

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

  override fun onDestroy() {
    nativeStop()
    super.onDestroy()
  }

  private fun launchImmersive() {
    startActivity(
        Intent(this, SpatialActivity::class.java).apply {
          action = Intent.ACTION_MAIN
          addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        })
  }

  private external fun nativeStart(host: String, port: Int)

  private external fun nativeStop()

  private external fun nativeSetViewport(width: Int, height: Int)

  private external fun nativeAttachSurface(slot: Int, surface: Surface)

  private external fun nativeDetachSurface(slot: Int)

  private external fun nativeStreamCount(): Int

  companion object {
    // Dev host; the config-file read (like the OpenXR client's loom_host.txt)
    // lands when discovery arrives (M7.2).
    private const val HOST = "192.168.178.72"
    private const val PORT = 47800

    init {
      System.loadLibrary("loom_panel")
    }
  }

  /**
   * One decoder target. Attaches its surface to native slot [slot] on create and
   * detaches on destroy; reports the primary slot's pixel size via [onPrimarySize].
   * Created without lockCanvas so the surface has a GPU producer (AMediaCodec).
   */
  @Composable
  private fun VideoSurface(
      slot: Int,
      onPrimarySize: (Int, Int) -> Unit,
      modifier: Modifier = Modifier,
  ) {
    AndroidView(
        factory = { ctx ->
          SurfaceView(ctx).apply {
            holder.addCallback(
                object : SurfaceHolder.Callback {
                  override fun surfaceCreated(holder: SurfaceHolder) {
                    nativeAttachSurface(slot, holder.surface)
                  }

                  override fun surfaceChanged(
                      holder: SurfaceHolder,
                      format: Int,
                      width: Int,
                      height: Int,
                  ) {
                    onPrimarySize(width, height)
                  }

                  override fun surfaceDestroyed(holder: SurfaceHolder) {
                    nativeDetachSurface(slot)
                  }
                })
          }
        },
        modifier = modifier)
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
            text = if (width > 0) "${width}×${height}" else "…",
            style =
                LocalTypography.current.body2.copy(
                    color = LocalColorScheme.current.primaryAlphaBackground))
        // Match the stream to the window size (VIEWPORT, §3.10).
        PrimaryCircleButton(
            icon = {
              Icon(SpatialIcons.Regular.FullScreen, contentDescription = "Match to window")
            },
            onClick = onMatch)
        // Enter the immersive many-monitor workspace.
        SecondaryCircleButton(
            icon = {
              Icon(SpatialIcons.Regular.MultiBrowser, contentDescription = "Immersive monitors")
            },
            onClick = onImmersive)
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
