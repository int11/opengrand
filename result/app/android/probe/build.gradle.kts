// Install probe: a throwaway APK that answers
// "can the built-in panel (RK312x, Android 6.0.1) install a normal APK from a USB stick at all?"
// before the non-privileged panel shell (`panelapp`) is built.
// Separate module, not a flavor of :app, so none of :app's manifest permissions merge in.
// Build: ./gradlew assembleProbeDebug  (alias of :probe:assembleDebug, root build.gradle.kts)
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.ca79ng.probe"
    compileSdk = 35
    defaultConfig {
        applicationId = "dev.ca79ng.probe" // distinct from dev.ca79ng.shell(.panel) and the Kawai app
        minSdk = 21 // the panel is API 23
        // Android 6 installs any targetSdk; 23 = the panel's own level, so the probe sees exactly
        // API 23 runtime behaviour (no export-flag rules for registerReceiver, no package-visibility
        // filtering). Android 14+ phones still accept 23 (their floor is 23).
        targetSdk = 23
        versionCode = 1
        versionName = "0.1.0"
    }
    buildTypes {
        // debug-signed, but shrunk: without R8 the Kotlin stdlib alone is ~1.5 MB
        getByName("debug") {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
        }
    }
    // kotlin/*.kotlin_builtins only serve kotlin-reflect, which the probe does not use
    packaging { resources { excludes += "kotlin/**" } }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    lint {
        // targetSdk 23 is deliberate (see above); this APK is never published to a store
        disable += setOf("ExpiredTargetSdkVersion", "OldTargetApi")
    }
}
