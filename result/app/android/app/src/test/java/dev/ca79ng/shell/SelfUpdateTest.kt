package dev.ca79ng.shell

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.fail
import org.junit.Test
import java.io.File

/** Online self-update: manifest checks on the fixture shared with the desktop shell. */
class SelfUpdateTest {
    /** result/desktop/tests/fixtures/update.json (tests run in the app module folder). */
    private val fixture: JSONObject by lazy {
        val f = listOf("../../../desktop/tests/fixtures/update.json", "../../desktop/tests/fixtures/update.json").map(::File).first { it.isFile }
        JSONObject(f.readText())
    }

    private fun code(text: String): String = try {
        SelfUpdate.verifyManifest(text); "ok"
    } catch (e: SelfUpdate.UpdateError) { e.code }

    @Test fun sharedFixtureParsesAndSigIsIgnored() {
        val text = fixture.getString("manifest")
        val v = SelfUpdate.verifyManifest(text)
        assertEquals(14L, v.android!!.versionCode)
        assertEquals("OpenGrand-1.2.0-phone.apk", v.android!!.file)
        assertEquals(3L, v.android!!.size)
        assertEquals("ok", code(JSONObject(text).also { it.remove("sig") }.toString()))
    }

    @Test fun badContentsAreRefused() {
        val text = fixture.getString("manifest")
        fun tamper(f: (JSONObject) -> Unit) = JSONObject(text).also(f).toString()
        assertEquals("manifest", code("not json"))
        assertEquals("manifest", code("x".repeat(SelfUpdate.MAX_MANIFEST + 1)))
        assertEquals("manifest", code(tamper { it.put("schema", 2) }))
        assertEquals("manifest", code(tamper { it.getJSONObject("android").put("size", 1.5) }))
        assertEquals("manifest", code(tamper { it.getJSONObject("android").put("size", 0) }))
        assertEquals("manifest", code(tamper { it.getJSONObject("android").put("file", "../a.apk") }))
        assertEquals("manifest", code(tamper { it.getJSONObject("android").put("sha256", "A".repeat(64)) }))
        assertEquals("manifest", code(tamper { it.getJSONObject("android").remove("versionCode") }))
        assertNull(SelfUpdate.verifyManifest(tamper { it.remove("android") }).android)
    }

    @Test fun httpOutcomes() {
        assertEquals("ratelimit", SelfUpdate.httpProblem(403))
        assertEquals("ratelimit", SelfUpdate.httpProblem(429))
        assertEquals("norelease", SelfUpdate.httpProblem(404))
        assertEquals("http", SelfUpdate.httpProblem(500))
        assertNull(SelfUpdate.httpProblem(200))
    }

    @Test fun releaseAssets() {
        val r = SelfUpdate.Release.parse("""{"html_url":"https://github.com/int11/opengrand/releases/tag/r1","assets":[
            {"name":"update.json","browser_download_url":"https://x/u"},{"name":"evil","browser_download_url":"http://x/e"}]}""")
        assertEquals("https://x/u", r.assets["update.json"])
        assertNull(r.assets["evil"])
        assertEquals(SelfUpdate.RELEASES_PAGE, SelfUpdate.Release.parse("""{"assets":[],"html_url":"https://evil.example/"}""").page)
        try { SelfUpdate.Release.parse("""{"message":"Not Found"}"""); fail() } catch (e: SelfUpdate.UpdateError) { assertNotNull(e.code) }
    }
}
