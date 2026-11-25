package dev.ca79ng.shell

import org.json.JSONObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.File
import java.math.BigInteger
import java.security.KeyPairGenerator
import java.security.Signature
import kotlin.random.Random

/** Online self-update: Ed25519 verifier, canonical form (shared fixture with the desktop shell), manifest checks. */
class SelfUpdateTest {
    // RFC 8032 section 7.1 TEST 1 (hex, allow-listed) and TESTs 2 / 3 (base64)
    private fun unhex(s: String) = ByteArray(s.length / 2) { s.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
    private val b64 = SelfUpdate.Companion::base64

    /** result/desktop/tests/fixtures/update-signed.json (tests run in the app module folder). */
    private val fixture: JSONObject by lazy {
        val f = listOf("../../../desktop/tests/fixtures/update-signed.json", "../../desktop/tests/fixtures/update-signed.json").map(::File).first { it.isFile }
        JSONObject(f.readText())
    }

    private fun code(text: String, pub: String): String = try {
        SelfUpdate.verifyManifest(text, pub); "ok"
    } catch (e: SelfUpdate.UpdateError) { e.code }

    @Test fun rfc8032Vectors() {
        assertTrue(Ed25519.verify(
            unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"), ByteArray(0),
            unhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")))
        assertTrue(Ed25519.verify(b64("PUAXw+hDiVqStwqnTRt+vJyYLM8uxJaMwM1V8Sr0Zgw="), b64("cg=="),
            b64("kqAJqfDUyrhyDoILX2QlQKKye1QWUD+Ps3YiI+vbadoIWsHkPhWZbkWPNhPQ8R2MOHsurrQwKu6wDSkWErsMAA==")))
        val pub3 = b64("/FHNjmIYoaONpH7QAjDwWAgW7RO6MwOsXeuRFUiQgCU=")
        val sig3 = b64("YpHWV97sJAJIJ+acOr4BowzlSKKEdDpEXjaA19taw6wY/5tTjRbykK5n92CYTcZZSnwV6XFu0o3AJ77O6h7ECg==")
        assertTrue(Ed25519.verify(pub3, b64("r4I="), sig3))
        assertFalse(Ed25519.verify(pub3, b64("r4M="), sig3)) // other message
        assertFalse(Ed25519.verify(pub3, b64("r4I="), sig3.copyOf().also { it[5] = (it[5] + 1).toByte() }))
        assertFalse(Ed25519.verify(pub3.copyOf(31), b64("r4I="), sig3))
    }

    @Test fun nonCanonicalSIsRejected() {
        val pub = b64("/FHNjmIYoaONpH7QAjDwWAgW7RO6MwOsXeuRFUiQgCU=")
        val sig = b64("YpHWV97sJAJIJ+acOr4BowzlSKKEdDpEXjaA19taw6wY/5tTjRbykK5n92CYTcZZSnwV6XFu0o3AJ77O6h7ECg==")
        // S + L encodes the same scalar mod L: a malleated signature must fail
        val l = BigInteger.ONE.shiftLeft(252).add(BigInteger("27742317777372353535851937790883648493"))
        val s = BigInteger(1, sig.copyOfRange(32, 64).reversedArray()).add(l)
        val sBytes = s.toByteArray().reversedArray().copyOf(32)
        assertFalse(Ed25519.verify(pub, b64("r4I="), sig.copyOfRange(0, 32) + sBytes))
    }

    /** Against the JDK's own Ed25519 (JDK 15+ on the build host): random keys and messages. */
    @Test fun matchesJdkEd25519() {
        val rnd = Random(79)
        repeat(12) {
            val kp = KeyPairGenerator.getInstance("Ed25519").generateKeyPair()
            val pub = kp.public.encoded.let { it.copyOfRange(it.size - 32, it.size) } // SubjectPublicKeyInfo ends with the raw key
            val msg = rnd.nextBytes(rnd.nextInt(0, 300))
            val sig = Signature.getInstance("Ed25519").run { initSign(kp.private); update(msg); sign() }
            assertTrue(Ed25519.verify(pub, msg, sig))
            val i = rnd.nextInt(64)
            val bad = sig.copyOf().also { it[i] = (it[i].toInt() xor (1 shl rnd.nextInt(8))).toByte() }
            assertFalse(Ed25519.verify(pub, msg, bad))
            if (msg.isNotEmpty()) assertFalse(Ed25519.verify(pub, msg.copyOf().also { it[0] = (it[0] + 1).toByte() }, sig))
        }
    }

    @Test fun sharedFixtureCanonicalAndSignature() {
        val m = JSONObject(fixture.getString("manifest"))
        m.remove("sig")
        assertEquals(fixture.getString("canonical"), SelfUpdate.canonical(m))
        val v = SelfUpdate.verifyManifest(fixture.getString("manifest"), fixture.getString("pub"))
        assertEquals(14L, v.android!!.versionCode)
        assertEquals("OpenGrand-1.2.0-phone.apk", v.android!!.file)
        assertEquals(3L, v.android!!.size)
    }

    @Test fun tamperedOrForeignManifestIsRefused() {
        val text = fixture.getString("manifest")
        val pub = fixture.getString("pub")
        fun tamper(f: (JSONObject) -> Unit) = JSONObject(text).also(f).toString()
        assertEquals("ok", code(text, pub))
        assertEquals("signature", code(tamper { it.getJSONObject("android").put("versionCode", 99) }, pub))
        assertEquals("signature", code(tamper { it.getJSONObject("android").put("sha256", "0".repeat(64)) }, pub))
        assertEquals("signature", code(tamper { it.getJSONObject("desktop").put("version", "9.9.9") }, pub))
        assertEquals("signature", code(tamper { it.put("extra", 1) }, pub))
        assertEquals("signature", code(tamper { it.remove("sig") }, pub))
        assertEquals("signature", code(text, SelfUpdate.PUBKEY)) // the test key is not the release key
        assertEquals("manifest", code("not json", pub))
        assertEquals("manifest", code(tamper { it.getJSONObject("android").put("size", 1.5) }, pub))
        assertEquals("manifest", code("x".repeat(SelfUpdate.MAX_MANIFEST + 1), pub))
    }

    @Test fun canonicalRules() {
        assertEquals("""{"a":{"c":2,"d":"x"},"b":1}""", SelfUpdate.canonical(JSONObject("""{"b":1,"a":{"d":"x","c":2}}""")))
        for (bad in listOf("""{"a":1.5}""", """{"a":-1}""", """{"a":[1]}""", """{"a":true}""", """{"a":null}""", """{"a":"q\""}""", """{"a":"é"}""", """{"a-b":1}""")) {
            try { SelfUpdate.canonical(JSONObject(bad)); fail(bad) } catch (e: SelfUpdate.UpdateError) { assertEquals("manifest", e.code) }
        }
    }

    @Test fun base64Decoder() {
        assertArrayEquals("abc".toByteArray(), b64("YWJj"))
        assertArrayEquals("ab".toByteArray(), b64("YWI="))
        assertArrayEquals("a".toByteArray(), b64("YQ=="))
        for (bad in listOf("YWJ", "Y===", "YQ==YWJj", "YW=j", "YW!j")) {
            try { b64(bad); fail(bad) } catch (e: SelfUpdate.UpdateError) { /* refused */ }
        }
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
