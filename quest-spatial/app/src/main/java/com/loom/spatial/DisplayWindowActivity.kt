package com.loom.spatial

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.ui.Modifier
import com.meta.spatial.uiset.theme.SpatialTheme
import com.meta.spatial.uiset.theme.darkSpatialColorScheme

/**
 * One additional display as its own 2D Home window (ROADMAP M6.3). Launched by
 * [PancakeActivity] in a separate task, one per host display beyond the primary,
 * so Horizon Home draws, moves, and resizes each window independently. It shows a
 * single [DisplaySurface] for its slot and does not touch the session lifetime —
 * the primary window owns [NativeBridge.start]/[NativeBridge.stop].
 */
class DisplayWindowActivity : ComponentActivity() {

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)

    val slot = intent.getIntExtra(EXTRA_SLOT, 0)
    setContent {
      SpatialTheme(colorScheme = darkSpatialColorScheme()) {
        Box(Modifier.fillMaxSize()) { DisplaySurface(slot = slot, modifier = Modifier.fillMaxSize()) }
      }
    }
  }

  companion object {
    const val EXTRA_SLOT = "com.loom.spatial.SLOT"
  }
}
