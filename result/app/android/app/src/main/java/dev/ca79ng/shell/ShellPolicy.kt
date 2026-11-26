package dev.ca79ng.shell

/** Pure decisions of the shell, kept free of Android classes so they are unit-tested (ShellPolicyTest). */
object ShellPolicy {
    /**
     * Runtime permissions BLE-MIDI scan / connect / module DFU need on this SDK level.
     * Android 12+: Nearby devices (SCAN is declared neverForLocation).
     * Android 6–11: a BLE scan returns nothing without fine location.
     */
    fun blePermissions(sdk: Int): List<String> = when {
        sdk >= 31 -> listOf("android.permission.BLUETOOTH_SCAN", "android.permission.BLUETOOTH_CONNECT")
        sdk >= 23 -> listOf("android.permission.ACCESS_FINE_LOCATION")
        else -> emptyList()
    }

    /**
     * MIME types for the system file picker from an <input accept> list. The web
     * side filters by extension (".c79" OTA payload, ".mid" songs, the OTA key file),
     * which Android pickers do not understand, and MIDI files often carry no MIME
     * type — so anything but a pure MIME list opens the picker on every file.
     */
    fun chooserMimeTypes(accept: Array<String>?): Array<String> {
        val types = accept.orEmpty().flatMap { it.split(',') }.map { it.trim() }.filter { it.isNotEmpty() }
        if (types.isEmpty() || types.any { !it.contains('/') }) return arrayOf("*/*")
        return types.distinct().toTypedArray()
    }
}

/**
 * When the phone shell lets the piano go and takes it back.
 * The piano's BLE-MIDI module serves one central at a time, so an app left in the background must
 * not hold it: after [graceMs] stopped the page is told "background" (it sends DISCONNECT `7E 01 00` —
 * the stock hands control back to the panel — and closes the link), back in front "foreground" (it
 * reconnects to the same piano). A short stop (file chooser, a quick app switch) keeps the link.
 * Pure, so it is unit-tested (ShellPolicyTest); MainActivity feeds it the lifecycle and the clock.
 */
class LinkLifecycle(val graceMs: Long = GRACE_MS) {
    private var stoppedAt: Long? = null
    var released = false
        private set

    /** Activity stopped (home, another app, the file chooser). */
    fun onStop(now: Long) {
        if (stoppedAt == null) stoppedAt = now
    }

    /** Checked [graceMs] after onStop: true once → tell the page "background". */
    fun releaseDue(now: Long): Boolean {
        val t = stoppedAt ?: return false
        if (released || now - t < graceMs) return false
        released = true
        return true
    }

    /**
     * Checked [FORCE_CLOSE_MS] after "background": close what the page left open only if the app is still away.
     * Back in front meanwhile, the page is reconnecting - the force close posted earlier must not cut that link.
     */
    fun forceCloseDue(): Boolean = released

    /** Activity back in front: true → the link was let go, tell the page "foreground". */
    fun onStart(): Boolean {
        stoppedAt = null
        val was = released
        released = false
        return was
    }

    companion object {
        const val GRACE_MS = 30_000L
        /** after "background", devices the page did not close itself are closed by the shell */
        const val FORCE_CLOSE_MS = 2_000L
    }
}
