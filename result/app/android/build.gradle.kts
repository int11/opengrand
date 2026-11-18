plugins {
    id("com.android.application") version "8.9.0" apply false
    id("org.jetbrains.kotlin.android") version "1.9.23" apply false
}

// install probe (separate module :probe)
tasks.register("assembleProbeDebug") { dependsOn(":probe:assembleDebug") }
