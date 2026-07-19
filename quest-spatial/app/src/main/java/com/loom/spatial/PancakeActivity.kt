package com.loom.spatial

import android.content.Intent
import android.os.Bundle
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
 * Loom windowed mode (ROADMAP M3.5 / M6.3): the primary desktop as a 2D window in
 * Horizon Home, composited by the OS — the sharp, shimmer-free path. Default launch
 * (com.oculus.intent.category.2D).
 *
 * Multi-display fan-in: one QUIC session negotiates multi-display, so the host
 * streams one video per display (§3.4). This window shows the primary display
 * (slot 0); each **additional** display gets its **own** Home window — a separate
 * [DisplayWindowActivity] task — so Home draws, moves, and resizes them
 * independently. This Activity owns the session lifetime (start/stop).
 */
class PancakeActivity : ComponentActivity() {

  // The primary (slot 0) surface size in pixels; the resolution "Match to window"
  // requests so the video maps ~1:1.
  private val surfaceWidth = mutableIntStateOf(0)
  private val surfaceHeight = mutableIntStateOf(0)

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)

    NativeBridge.start(HOST, PORT)

    setContent {
      SpatialTheme(colorScheme = darkSpatialColorScheme()) {
        // Open one extra window per additional display as the host reports them.
        var streamCount by remember { mutableIntStateOf(0) }
        val opened = remember { mutableSetOf<Int>() }
        LaunchedEffect(Unit) {
          while (true) {
            streamCount = NativeBridge.streamCount()
            for (slot in 1 until streamCount) {
              if (opened.add(slot)) openDisplayWindow(slot)
            }
            delay(300)
          }
        }

        Box(Modifier.fillMaxSize()) {
          DisplaySurface(
              slot = 0,
              onSize = { w, h ->
                surfaceWidth.intValue = w
                surfaceHeight.intValue = h
              },
              modifier = Modifier.fillMaxSize())

          PillControl(
              width = surfaceWidth.intValue,
              height = surfaceHeight.intValue,
              onMatch = { w, h -> NativeBridge.setViewport(w, h) },
              onImmersive = { launchImmersive() },
              modifier = Modifier.align(Alignment.TopCenter))
        }
      }
    }
  }

  override fun onDestroy() {
    NativeBridge.stop()
    super.onDestroy()
  }

  /** Launch display `slot` as its own 2D Home window (a separate task). */
  private fun openDisplayWindow(slot: Int) {
    startActivity(
        Intent(this, DisplayWindowActivity::class.java).apply {
          putExtra(DisplayWindowActivity.EXTRA_SLOT, slot)
          addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_MULTIPLE_TASK)
        })
  }

  private fun launchImmersive() {
    startActivity(
        Intent(this, SpatialActivity::class.java).apply {
          action = Intent.ACTION_MAIN
          addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        })
  }

  companion object {
    // Dev host; discovery replaces this in M7.2.
    private const val HOST = "192.168.178.72"
    private const val PORT = 47800
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
        PrimaryCircleButton(
            icon = {
              Icon(SpatialIcons.Regular.FullScreen, contentDescription = "Match to window")
            },
            onClick = onMatch)
        SecondaryCircleButton(
            icon = {
              Icon(SpatialIcons.Regular.MultiBrowser, contentDescription = "Immersive monitors")
            },
            onClick = onImmersive)
      }
}

@Composable
private fun CollapsedPill(onClick: () -> Unit) {
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
