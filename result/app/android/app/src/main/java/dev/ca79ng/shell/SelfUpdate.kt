package dev.ca79ng.shell

import android.app.Activity
import android.content.pm.PackageInfo
import android.os.Build
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * Online self-update of the phone app (phone flavor only; the panel updates from USB, UpdateManager).
 *
 * Source: the latest GitHub release of int11/opengrand, asset update.json (format: result/desktop/src/updatepolicy.cjs;
 * both read result/desktop/tests/fixtures/update.json in their tests). No signature: trust is HTTPS to GitHub; a "sig"
 * that older releases carry is ignored. Order of checks: contents -> versionCode strictly greater than the installed one
 * -> download capped at `size` -> sha256 -> the APK's package name and versionCode -> the system installer (the user
 * confirms; Android refuses an APK signed by another certificate than the installed app).
 *
 * The page (src/features/selfupdate.ts) decides when to check (setting, startup, every 24 h, online) and shows the
 * state; this class reports it through `emit` (ca79ngNative.onSelfUpdate).
 */
class SelfUpdate(private val activity: Activity, private val emit: (String) -> Unit) {
    private val dir = File(activity.filesDir, "selfupdate")
    private val stagedFile = File(dir, "staged.json")
    @Volatile private var state = JSONObject().put("phase", "idle")
    @Volatile private var busy = false
    private var stagedApk: File? = null

    init {
        base()
        try {
            val s = JSONObject(stagedFile.readText())
            val apk = File(dir, s.getString("file"))
            if (s.getLong("versionCode") > BuildConfig.VERSION_CODE.toLong() && apk.isFile) {
                stagedApk = apk
                state.put("phase", "ready").put("available", s.getString("version")).put("action", "install")
            } else {
                dir.deleteRecursively() // installed (or older than what runs): spent
            }
        } catch (e: Exception) { /* nothing staged */ }
    }

    private fun base() {
        state.put("current", BuildConfig.VERSION_NAME).put("currentCode", BuildConfig.VERSION_CODE)
    }

    fun state(): String = state.toString()

    private fun set(vararg kv: Pair<String, Any?>) {
        val o = JSONObject(state.toString())
        for ((k, v) in kv) if (v == null) o.remove(k) else o.put(k, v)
        state = o
        emit(o.toString())
    }

    /** Check, and download when newer. Returns false when a check is already running. */
    fun check(): Boolean {
        synchronized(this) { if (busy) return false; busy = true }
        Thread {
            try {
                run()
            } catch (e: UpdateError) {
                set("phase" to "error", "error" to e.code, "message" to e.message, "progress" to null, "checkedAt" to System.currentTimeMillis())
            } catch (e: Exception) {
                set("phase" to "error", "error" to "error", "message" to (e.message ?: e.toString()), "progress" to null, "checkedAt" to System.currentTimeMillis())
            } finally {
                busy = false
            }
        }.start()
        return true
    }

    private fun run() {
        if (state.optString("phase") == "ready" && stagedApk?.isFile == true) return set()
        set("phase" to "checking", "error" to null, "message" to null, "progress" to null)
        val rel = Release.parse(String(fetch(LATEST_URL, MAX_RELEASE_JSON), Charsets.UTF_8))
        val mfUrl = rel.assets[MANIFEST_NAME] ?: throw UpdateError("norelease", "the latest release has no update.json")
        val m = verifyManifest(String(fetch(mfUrl, MAX_MANIFEST.toLong()), Charsets.UTF_8))
        val a = m.android
        if (a == null || a.versionCode <= BuildConfig.VERSION_CODE.toLong()) {
            return set("phase" to "uptodate", "available" to null, "action" to null, "page" to rel.page, "checkedAt" to System.currentTimeMillis())
        }
        set("available" to a.version, "page" to rel.page)
        val url = rel.assets[a.file] ?: throw UpdateError("manifest", "the release has no ${a.file}")
        dir.mkdirs()
        val part = File(dir, "${a.file}.part")
        val apk = File(dir, a.file)
        set("phase" to "downloading", "progress" to 0)
        download(url, part, a)
        checkApk(part, a)
        apk.delete()
        if (!part.renameTo(apk)) throw UpdateError("write", "cannot keep the download")
        stagedFile.writeText(JSONObject().put("version", a.version).put("versionCode", a.versionCode).put("file", a.file).put("sha256", a.sha256).toString())
        stagedApk = apk
        set("phase" to "ready", "action" to "install", "progress" to null, "checkedAt" to System.currentTimeMillis())
    }

    private fun open(url: String): HttpURLConnection {
        val c = try {
            (URL(url).openConnection() as HttpURLConnection).apply {
                connectTimeout = 15000
                readTimeout = 30000
                instanceFollowRedirects = true
                setRequestProperty("Accept", if (url == LATEST_URL) "application/vnd.github+json" else "application/octet-stream")
                setRequestProperty("User-Agent", "OpenGrand-android-updater")
            }
        } catch (e: IOException) { throw UpdateError("offline", e.message) }
        val code = try { c.responseCode } catch (e: IOException) { c.disconnect(); throw UpdateError("offline", e.message) }
        httpProblem(code)?.let { c.disconnect(); throw UpdateError(it, "HTTP $code") }
        return c
    }

    private fun fetch(url: String, max: Long): ByteArray {
        val c = open(url)
        try {
            c.inputStream.use { input ->
                val out = java.io.ByteArrayOutputStream()
                val buf = ByteArray(16 * 1024)
                while (true) {
                    val n = input.read(buf); if (n < 0) break
                    out.write(buf, 0, n)
                    if (out.size() > max) throw UpdateError("manifest", "reply too large")
                }
                return out.toByteArray()
            }
        } catch (e: IOException) { throw UpdateError("offline", e.message) } finally { c.disconnect() }
    }

    private fun download(url: String, part: File, a: Entry) {
        val c = open(url)
        val md = MessageDigest.getInstance("SHA-256")
        var got = 0L
        try {
            c.inputStream.use { input ->
                part.outputStream().use { out ->
                    val buf = ByteArray(64 * 1024)
                    var last = -1
                    while (true) {
                        val n = input.read(buf); if (n < 0) break
                        got += n
                        if (got > a.size) throw UpdateError("size", "download larger than update.json says")
                        md.update(buf, 0, n)
                        out.write(buf, 0, n)
                        val pct = (got * 100 / a.size).toInt()
                        if (pct != last) { last = pct; set("progress" to pct) }
                    }
                }
            }
        } catch (e: IOException) {
            part.delete(); throw UpdateError("offline", e.message)
        } catch (e: UpdateError) {
            part.delete(); throw e
        } finally { c.disconnect() }
        if (got != a.size) { part.delete(); throw UpdateError("size", "got $got of ${a.size} bytes") }
        if (hex(md.digest()) != a.sha256) { part.delete(); throw UpdateError("sha256", "download does not match update.json") }
    }

    /** The APK is this app (package name) at the version update.json promises. */
    private fun checkApk(f: File, a: Entry) {
        val info: PackageInfo = activity.packageManager.getPackageArchiveInfo(f.absolutePath, 0)
            ?: run { f.delete(); throw UpdateError("apk", "not an APK") }
        @Suppress("DEPRECATION") val code = if (Build.VERSION.SDK_INT >= 28) info.longVersionCode else info.versionCode.toLong()
        if (info.packageName != activity.packageName || code != a.versionCode) {
            f.delete(); throw UpdateError("apk", "APK is ${info.packageName} $code, expected ${activity.packageName} ${a.versionCode}")
        }
    }

    /** Hand the staged APK (sha256 checked again) to the system installer. */
    fun apply(): Boolean {
        val apk = stagedApk ?: return false
        if (state.optString("phase") != "ready") return false
        Thread {
            try {
                val s = JSONObject(stagedFile.readText())
                if (UpdateManager.sha256(apk) != s.getString("sha256")) {
                    apk.delete(); stagedFile.delete(); stagedApk = null
                    return@Thread set("phase" to "error", "error" to "sha256", "message" to "staged APK changed", "action" to null)
                }
                set("phase" to "installing", "hint" to null)
                if (!UpdateManager.openSystemInstaller(activity, apk)) {
                    // 「Install unknown apps」 is off: its switch is open now; the user comes back and taps install again
                    return@Thread set("phase" to "ready", "hint" to "source")
                }
                set("phase" to "ready") // the user may cancel the system confirm: the button stays
            } catch (e: Exception) {
                set("phase" to "error", "error" to "install", "message" to (e.message ?: e.toString()))
            }
        }.start()
        return true
    }

    class UpdateError(val code: String, message: String?) : Exception(message ?: code)

    data class Entry(val version: String, val versionCode: Long, val file: String, val sha256: String, val size: Long)
    data class Manifest(val android: Entry?)
    data class Release(val page: String, val assets: Map<String, String>) {
        companion object {
            fun parse(json: String): Release {
                val o = try { JSONObject(json) } catch (e: Exception) { throw UpdateError("manifest", "release is not JSON") }
                val list = o.optJSONArray("assets") ?: throw UpdateError("manifest", "release has no assets")
                val assets = HashMap<String, String>()
                for (i in 0 until list.length()) {
                    val a = list.optJSONObject(i) ?: continue
                    val name = a.optString("name"); val url = a.optString("browser_download_url")
                    if (name.isNotEmpty() && url.startsWith("https://")) assets[name] = url
                }
                val page = o.optString("html_url").takeIf { it.startsWith("https://github.com/") } ?: RELEASES_PAGE
                return Release(page, assets)
            }
        }
    }

    companion object {
        const val LATEST_URL = "https://api.github.com/repos/int11/opengrand/releases/latest"
        const val RELEASES_PAGE = "https://github.com/int11/opengrand/releases/latest"
        const val MANIFEST_NAME = "update.json"
        const val MAX_MANIFEST = 64 * 1024
        private const val MAX_RELEASE_JSON = 1024L * 1024
        private const val MAX_ASSET = 1024L * 1024 * 1024
        private val RE_VERSION = Regex("^\\d+(\\.\\d+){0,3}$")
        private val RE_FILE = Regex("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
        private val RE_SHA = Regex("^[0-9a-f]{64}$")

        /** Same as updatepolicy.cjs httpProblem: offline / ratelimit / norelease are quiet. */
        fun httpProblem(status: Int): String? = when {
            status == 403 || status == 429 -> "ratelimit"
            status == 404 -> "norelease"
            status in 200..299 -> null
            else -> "http"
        }

        /** Parse update.json and check its android entry. Throws UpdateError("manifest"). */
        fun verifyManifest(text: String): Manifest {
            if (text.length > MAX_MANIFEST) throw UpdateError("manifest", "update.json too large")
            val o = try { JSONObject(text) } catch (e: Exception) { throw UpdateError("manifest", "update.json is not JSON") }
            if (o.opt("schema") != 1) throw UpdateError("manifest", "update.json: bad schema")
            val a = o.optJSONObject("android") ?: return Manifest(null)
            val version = a.opt("version") as? String
            val code = intOf(a.opt("versionCode"))
            val file = a.opt("file") as? String
            val sha = a.opt("sha256") as? String
            val size = intOf(a.opt("size"))
            if (version == null || !RE_VERSION.matches(version) || code == null || code <= 0 ||
                file == null || !RE_FILE.matches(file) || file.contains("..") || file == MANIFEST_NAME ||
                sha == null || !RE_SHA.matches(sha) || size == null || size <= 0 || size > MAX_ASSET) {
                throw UpdateError("manifest", "update.json: bad android entry")
            }
            return Manifest(Entry(version, code, file, sha, size))
        }

        /** A JSON integer (not 1.5, not "1"), as Long. */
        private fun intOf(v: Any?): Long? = if (v is Int || v is Long) (v as Number).toLong() else null

        fun hex(b: ByteArray): String = b.joinToString("") { "%02x".format(it) }
    }
}
