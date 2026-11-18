plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.ca79ng.shell"
    compileSdk = 35
    defaultConfig {
        applicationId = "dev.ca79ng.shell"
        minSdk = 23 // same floor as the original app
        targetSdk = 35
        versionCode = 13
        versionName = "1.1.11"
    }
    // "panel": built-in LCD (system app, HOME launcher, USB-MIDI to the main board)
    // "phone": PianoRemote-style companion (BLE-MIDI / USB-MIDI)
    flavorDimensions += "target"
    productFlavors {
        create("panel") { dimension = "target"; buildConfigField("boolean", "PANEL", "true"); applicationIdSuffix = ".panel" }
        create("phone") { dimension = "target"; buildConfigField("boolean", "PANEL", "false") }
    }
    buildFeatures { buildConfig = true }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    // the web UI bundle (../dist, from `npm run build`) is packaged as assets/www
    sourceSets["main"].assets.srcDirs("build/generated/www")
}

// Sync, not Copy: hashed bundle names change per build and Copy would keep the old ones in the APK
val copyWeb by tasks.registering(Sync::class) {
    from("../../dist")
    into("build/generated/www/www")
}
tasks.named("preBuild") { dependsOn(copyWeb) }

dependencies {
    implementation("androidx.webkit:webkit:1.12.1")
    implementation("androidx.core:core-ktx:1.13.1") // FileProvider, registerReceiver flags (feature 86)
    // BLE-MIDI module DFU, the same library the original app used (feature 87).
    // 2.5.0: newer releases pull kotlin-stdlib 2.3, which this project's Kotlin 1.9 plugin cannot read.
    implementation("no.nordicsemi.android:dfu:2.5.0")
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303") // real org.json for JVM tests (android.jar's is a stub)
}
