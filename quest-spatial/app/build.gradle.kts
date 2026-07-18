import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.jetbrains.kotlin.android)
    alias(libs.plugins.meta.spatial.plugin)
    alias(libs.plugins.jetbrains.kotlin.plugin.compose)
}

android {
    namespace = "com.loom.spatial"
    // Meta's samples use 34; we compile against the installed 36 (backward
    // compatible) and keep target/min at 34 (Horizon OS = Android 14).
    compileSdk = 36

    defaultConfig {
        applicationId = "com.loom.spatial"
        // HorizonOS is Android 14 (API level 34), matching Meta's samples.
        minSdk = 34
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"

        // Quest is arm64 only; matches the msquic prebuilt in ../quest.
        ndk { abiFilters += "arm64-v8a" }
    }

    // Same NDK the OpenXR client + the msquic prebuilt were built with.
    ndkVersion = "28.2.13676358"
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    sourceSets {
        getByName("main") {
            // Prebuilt libmsquic.so, shared with the OpenXR client; AGP packages
            // it and CMake links it as the IMPORTED msquic target.
            jniLibs.srcDir("../../quest/third_party/msquic/lib")
        }
    }

    buildFeatures {
        buildConfig = true
        compose = true
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    lint {
        abortOnError = false
        checkReleaseBuilds = false
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)

    // Meta Spatial SDK (M3.5). Only the packages this client needs.
    implementation(libs.meta.spatial.sdk.base)
    implementation(libs.meta.spatial.sdk.toolkit)
    implementation(libs.meta.spatial.sdk.vr)
    implementation(libs.meta.spatial.sdk.compose)

    // Compose — panels render Android Compose UI.
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation("androidx.compose.material3:material3")
}
