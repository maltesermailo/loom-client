package com.loom.spatial

import com.meta.spatial.compose.ComposeFeature
import com.meta.spatial.compose.composePanel
import com.meta.spatial.core.Entity
import com.meta.spatial.core.Pose
import com.meta.spatial.core.Quaternion
import com.meta.spatial.core.SpatialFeature
import com.meta.spatial.core.Vector3
import com.meta.spatial.runtime.ReferenceSpace
import com.meta.spatial.toolkit.AppSystemActivity
import com.meta.spatial.toolkit.PanelRegistration
import com.meta.spatial.toolkit.Transform
import com.meta.spatial.toolkit.createPanelEntity
import com.meta.spatial.vr.VRFeature

/**
 * Loom Spatial SDK client (ROADMAP M3.5) — Phase 1 scaffold.
 *
 * A single desktop panel created and placed in code (no Meta Spatial Editor
 * scene). The panel is flat for this first build (verified Spatial SDK 0.13.2
 * API); the curved shape (PanelShapeType.CYLINDER) and the live VideoSurfacePanel
 * replace the placeholder in the next iterations. The whole point of this client
 * is to reach anisotropic filtering the OpenXR cylinder-layer client cannot —
 * see ARCHITECTURE §6.5.
 */
class SpatialActivity : AppSystemActivity() {

  override fun registerFeatures(): List<SpatialFeature> =
      listOf(VRFeature(this), ComposeFeature())

  override fun onSceneReady() {
    super.onSceneReady()

    // Recenter-able floor-relative space, matching the OpenXR client's LOCAL.
    scene.setReferenceSpace(ReferenceSpace.LOCAL_FLOOR)

    // Instantiate the registered panel and place it in front of the user. Without
    // a GLXF scene the entity must be created here; registerPanels() only says
    // how to render this id.
    Entity.createPanelEntity(
        R.id.desktop_panel,
        Transform(Pose(Vector3(0.0f, 0.5f, 1.0f), Quaternion(0.0f, 180.0f, 0.0f))),
    )
  }

  override fun registerPanels(): List<PanelRegistration> =
      listOf(
          PanelRegistration(R.id.desktop_panel) {
            config {
              // Physical size in metres (16:9). Curvature comes next once the
              // exact PanelConfigOptions field is confirmed.
              width = 1.6f
              height = 0.9f
              themeResourceId = R.style.PanelAppTheme
            }
            composePanel { setContent { DesktopPanel() } }
          }
      )
}
