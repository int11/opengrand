package dev.ca79ng.shell

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.File

/** Feature 86: the shell-side checks that gate an APK install. */
class UpdateManagerTest {
    private val sha = "0123456789abcdef".repeat(4)

    @Test fun manifestValidation() {
        assertNotNull(UpdateManager.parseManifest("""{"version":"1.2.0","sha256":"$sha"}"""))
        assertNotNull(UpdateManager.parseManifest("""{"version":"2","sha256":"${sha.uppercase()}","file":"app.apk"}"""))
        assertNull(UpdateManager.parseManifest("""{"version":"1.2.0","sha256":"abc"}"""))
        assertNull(UpdateManager.parseManifest("""{"version":"v1","sha256":"$sha"}"""))
        assertNull(UpdateManager.parseManifest("""{"version":"1.0","sha256":"$sha","file":"../x.apk"}"""))
        assertNull(UpdateManager.parseManifest("""{"version":"1.0","sha256":"$sha","file":"sub/x.apk"}"""))
        assertNull(UpdateManager.parseManifest("not json"))
    }

    @Test fun versionOrder() {
        assertEquals(1, UpdateManager.compareVersions("1.10.0", "1.9.3"))
        assertEquals(0, UpdateManager.compareVersions("1.1", "1.1.0"))
        assertEquals(-1, UpdateManager.compareVersions("1.0.9", "1.1.0"))
    }

    @Test fun sha256OfFile() {
        val f = File.createTempFile("apk", ".bin")
        f.writeText("abc")
        assertEquals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", UpdateManager.sha256(f))
        f.delete()
    }
}
