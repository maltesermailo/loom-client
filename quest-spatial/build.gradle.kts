// Only the base Android + Kotlin plugins are declared here (matching Meta's
// samples). The compose and meta.spatial plugins are applied solely in :app via
// the version-catalog alias — declaring them at the root pulls their bundled
// kotlin-compiler-embeddable onto the buildscript classpath and collides with
// the Kotlin Gradle plugin.
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.jetbrains.kotlin.android) apply false
}
