package dev.ca79ng.shell

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ShellPolicyTest {
    @Test fun blePermissionsBySdk() {
        assertEquals(listOf("android.permission.BLUETOOTH_SCAN", "android.permission.BLUETOOTH_CONNECT"), ShellPolicy.blePermissions(34))
        assertEquals(listOf("android.permission.ACCESS_FINE_LOCATION"), ShellPolicy.blePermissions(30))
        assertEquals(listOf("android.permission.ACCESS_FINE_LOCATION"), ShellPolicy.blePermissions(23))
    }

    @Test fun chooserTypes() {
        // the app's inputs: OTA payload ".c79", MIDI upload ".mid,.MID,.smf,audio/midi", key file (no accept)
        assertArrayEquals(arrayOf("*/*"), ShellPolicy.chooserMimeTypes(arrayOf(".c79")))
        assertArrayEquals(arrayOf("*/*"), ShellPolicy.chooserMimeTypes(arrayOf(".mid", ".MID", ".smf", "audio/midi")))
        assertArrayEquals(arrayOf("*/*"), ShellPolicy.chooserMimeTypes(arrayOf(".mid,.MID,audio/midi")))
        assertArrayEquals(arrayOf("*/*"), ShellPolicy.chooserMimeTypes(arrayOf("")))
        assertArrayEquals(arrayOf("*/*"), ShellPolicy.chooserMimeTypes(null))
        assertArrayEquals(arrayOf("audio/midi", "audio/x-midi"), ShellPolicy.chooserMimeTypes(arrayOf("audio/midi, audio/x-midi")))
    }

    @Test fun shortStopKeepsTheLink() {
        val l = LinkLifecycle(graceMs = 30_000)
        l.onStop(1_000)
        assertFalse(l.releaseDue(20_000)) // file chooser, quick switch
        assertFalse(l.onStart()) // nothing to reconnect
        assertFalse(l.releaseDue(40_000)) // a check posted before onStart does nothing
    }

    @Test fun longStopReleasesOnceAndForegroundReconnects() {
        val l = LinkLifecycle(graceMs = 30_000)
        l.onStop(1_000)
        l.onStop(5_000) // a second onStop does not restart the grace
        assertFalse(l.releaseDue(30_000))
        assertTrue(l.releaseDue(31_000))
        assertTrue(l.released)
        assertFalse(l.releaseDue(60_000)) // told once
        assertTrue(l.onStart())
        assertFalse(l.released)
        assertFalse(l.onStart())
    }

    @Test fun forceCloseOnlyWhileStillReleased() {
        val l = LinkLifecycle(graceMs = 30_000)
        l.onStop(1_000)
        assertTrue(l.releaseDue(31_000))
        assertTrue(l.forceCloseDue()) // still away when the force close comes: free the module
        // back in front within FORCE_CLOSE_MS: the page reconnects, the force close posted earlier must not cut it
        assertTrue(l.onStart())
        assertFalse(l.forceCloseDue())
        l.onStop(32_500) // away again right after: still inside the new grace
        assertFalse(l.forceCloseDue())
    }
}
