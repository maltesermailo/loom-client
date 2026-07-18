package com.loom.spatial

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Placeholder panel content; a VideoSurfacePanel fed by the decoder replaces it in Phase 2. */
@Composable
fun DesktopPanel() {
  Column(
      modifier = Modifier.fillMaxSize().background(Color(0xFF101418)).padding(32.dp),
      verticalArrangement = Arrangement.Center,
      horizontalAlignment = Alignment.CenterHorizontally,
  ) {
    Text(
        text = "Loom — Spatial SDK client",
        color = Color(0xFFE6EDF3),
        fontSize = 34.sp,
        textAlign = TextAlign.Center,
    )
    Text(
        text = "M3.5 · Phase 1 · curved video panel next",
        color = Color(0xFF7D8590),
        fontSize = 20.sp,
        textAlign = TextAlign.Center,
    )
  }
}
