package dev.ca79ng.shell

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageInstaller
import android.os.Build
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest

/**
 * Panel UI / APK update from USB memory (feature 86).
 *
 * Package layout on the stick:  <usb>/CA79NG/manifest.json  +  <usb>/CA79NG/app.apk
 *   manifest.json = {"version":"1.2.0", "versionCode":12, "sha256":"<64 hex>", "file":"app.apk"}
 * The phone build also looks in its app-specific external dir (…/Android/data/<pkg>/files/CA79NG/),
 * because scoped storage keeps ordinary apps from reading a USB stick directly.
 *
 * The original app used its own package format; here the package is plain and
 * integrity comes from the SHA-256 in the manifest plus Android's own signature check
 * (an update must be signed with the same key as the installed app).
 *
 * Install: panel (system app holding INSTALL_PACKAGES) -> PackageInstaller session, silent;
 *          phone -> ACTION_VIEW on a FileProvider URI (the user confirms in the system UI).
 */
class UpdateManager(
    private val activity: Activity,
    private val usbPath: () -> String,
    private val emit: (String) -> Unit,
) {
    private data class Pkg(val apk: File, val version: String, val versionCode: Long, val sha256: String)

    private fun candidates(): List<File> {
        val dirs = ArrayList<File>()
        val usb = usbPath()
        if (usb.isNotEmpty()) dirs += File(usb, "CA79NG")
        activity.getExternalFilesDir(null)?.let { dirs += File(it, "CA79NG") }
        return dirs
    }

    private fun find(): Pkg? {
        for (dir in candidates()) {
            val mf = File(dir, "manifest.json")
            if (!mf.isFile) continue
            val m = parseManifest(mf.readText()) ?: continue
            val apk = File(dir, m.optString("file", "app.apk"))
            if (!apk.isFile) continue
            return Pkg(apk, m.getString("version"), m.optLong("versionCode", 0), m.getString("sha256").lowercase())
        }
        return null
    }

    fun check(): String {
        val o = JSONObject().put("current", BuildConfig.VERSION_NAME)
        return try {
            val p = find() ?: return o.put("found", false).toString()
            o.put("found", true).put("version", p.version).put("newer", compareVersions(p.version, BuildConfig.VERSION_NAME) > 0).toString()
        } catch (e: Exception) {
            o.put("found", false).put("error", e.message ?: "error").toString()
        }
    }

    /** Verify and install on a worker thread; status goes to ca79ngNative.onUpdate. */
    fun install(): Boolean {
        val p = find() ?: return false
        Thread {
            try {
                status("verifying")
                val actual = sha256(p.apk)
                if (actual != p.sha256) return@Thread status("error", "sha256 mismatch")
                val info = activity.packageManager.getPackageArchiveInfo(p.apk.absolutePath, 0)
                    ?: return@Thread status("error", "not an APK")
                if (info.packageName != activity.packageName) return@Thread status("error", "package ${info.packageName} is not ${activity.packageName}")
                status("installing")
                if (BuildConfig.PANEL) installSession(p.apk) else installViaViewer(p.apk)
            } catch (e: Exception) {
                status("error", e.message ?: e.toString())
            }
        }.start()
        return true
    }

    private fun status(state: String, message: String? = null) {
        val o = JSONObject().put("state", state)
        if (message != null) o.put("message", message)
        emit(o.toString())
    }

    /** Silent install for the system-image panel app. */
    private fun installSession(apk: File) {
        val installer = activity.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
        params.setAppPackageName(activity.packageName)
        val id = installer.createSession(params)
        installer.openSession(id).use { session ->
            session.openWrite("app.apk", 0, apk.length()).use { out -> apk.inputStream().use { it.copyTo(out) }; session.fsync(out) }
            val action = "${activity.packageName}.INSTALL_STATUS"
            ContextCompat.registerReceiver(activity, object : BroadcastReceiver() {
                override fun onReceive(c: Context, i: Intent) {
                    when (i.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
                        PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                            // not privileged after all: let the system ask the user
                            @Suppress("DEPRECATION") val confirm = i.getParcelableExtra<Intent>(Intent.EXTRA_INTENT)
                            confirm?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)?.let { activity.startActivity(it) }
                        }
                        PackageInstaller.STATUS_SUCCESS -> { status("done"); activity.unregisterReceiver(this) }
                        else -> { status("error", i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: "install failed"); activity.unregisterReceiver(this) }
                    }
                }
            }, IntentFilter(action), ContextCompat.RECEIVER_NOT_EXPORTED)
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
            val pi = PendingIntent.getBroadcast(activity, id, Intent(action).setPackage(activity.packageName), flags)
            session.commit(pi.intentSender)
        }
    }

    /** Phone: hand the verified copy to the system package installer UI. */
    private fun installViaViewer(apk: File) {
        openSystemInstaller(activity, apk)
        status("done", "handed to the system installer")
    }

    companion object {
        /**
         * Copy a verified APK into the FileProvider's cache path and open the system installer on it (the user confirms;
         * Android refuses an APK signed by another key). Used by the USB update and the online self-update (SelfUpdate).
         * `apk` must not be the copy itself (cacheDir/updates/app.apk).
         */
        fun openSystemInstaller(activity: Activity, apk: File) {
            val dir = File(activity.cacheDir, "updates").apply { mkdirs() }
            val copy = File(dir, "app.apk")
            require(apk.canonicalPath != copy.canonicalPath) { "installer copy would overwrite its source" }
            apk.inputStream().use { input -> copy.outputStream().use { input.copyTo(it) } }
            val uri = FileProvider.getUriForFile(activity, "${activity.packageName}.files", copy)
            val view = Intent(Intent.ACTION_VIEW)
                .setDataAndType(uri, "application/vnd.android.package-archive")
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
            activity.runOnUiThread { activity.startActivity(view) }
        }

        private val VERSION = Regex("^\\d+(\\.\\d+)*$")
        private val SHA = Regex("^[0-9a-fA-F]{64}$")

        /** Same validation as src/features/update.ts parseManifest. */
        fun parseManifest(json: String): JSONObject? = try {
            val m = JSONObject(json)
            val file = m.optString("file", "app.apk")
            if (!VERSION.matches(m.optString("version")) || !SHA.matches(m.optString("sha256")) ||
                file.contains('/') || file.contains('\\') || file.contains("..")) null else m
        } catch (e: Exception) { null }

        fun compareVersions(a: String, b: String): Int {
            val pa = a.split('.', '-', '+').map { it.toIntOrNull() ?: 0 }
            val pb = b.split('.', '-', '+').map { it.toIntOrNull() ?: 0 }
            for (i in 0 until maxOf(pa.size, pb.size)) {
                val d = pa.getOrElse(i) { 0 } - pb.getOrElse(i) { 0 }
                if (d != 0) return if (d < 0) -1 else 1
            }
            return 0
        }

        fun sha256(f: File): String {
            val md = MessageDigest.getInstance("SHA-256")
            f.inputStream().use { input ->
                val buf = ByteArray(64 * 1024)
                while (true) { val n = input.read(buf); if (n < 0) break; md.update(buf, 0, n) }
            }
            return md.digest().joinToString("") { "%02x".format(it) }
        }
    }
}
