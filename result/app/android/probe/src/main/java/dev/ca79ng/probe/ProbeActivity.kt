package dev.ca79ng.probe

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.Gravity
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.webkit.WebSettings
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * One screen of read-only facts about the panel shell's environment (install probe).
 * Nothing here changes the device; the only buttons hand the user to system screens
 * (uninstall dialog, app info) or open the Kawai app by its launcher intent.
 */
class ProbeActivity : Activity() {
    private lateinit var info: TextView
    private lateinit var link: TextView

    private val live = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            LinkReceiver.record(context, "live", intent)
        }
    }
    private val prefsListener = SharedPreferences.OnSharedPreferenceChangeListener { _, _ ->
        runOnUiThread { link.text = linkText() }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = dp(16)
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
            setBackgroundColor(Color.rgb(16, 24, 32))
        }
        col.addView(TextView(this).apply {
            text = "OpenGrand install probe — installed OK"
            textSize = 28f
            setTextColor(Color.rgb(120, 230, 140))
            setTypeface(typeface, Typeface.BOLD)
        })
        info = mono(); link = mono()
        col.addView(info)
        col.addView(link)

        // the panel is 480x800 portrait: one full-width uninstall button, two smaller ones under it
        col.addView(button("Uninstall me", Color.rgb(200, 60, 60), 28f) {
            // ACTION_DELETE on API 23 needs no permission; the system asks the user to confirm
            startSafely(Intent(Intent.ACTION_DELETE, Uri.parse("package:$packageName")))
        }, LinearLayout.LayoutParams(MATCH_PARENT, dp(120)).apply { topMargin = dp(16) })
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER }
        row.addView(button("App info", Color.rgb(70, 90, 120), 18f) {
            startSafely(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$packageName")))
        }, LinearLayout.LayoutParams(0, dp(72), 1f).apply { rightMargin = dp(8) })
        row.addView(button("Open Kawai app", Color.rgb(70, 90, 120), 18f) {
            packageManager.getLaunchIntentForPackage(KAWAI)?.let { startSafely(it) }
                ?: toastLine("Kawai app has no launcher intent")
        }, LinearLayout.LayoutParams(0, dp(72), 1f))
        col.addView(row, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { topMargin = dp(8) })

        setContentView(ScrollView(this).apply { addView(col) })
        registerReceiver(live, IntentFilter(LinkReceiver.ACTION))
        prefs().registerOnSharedPreferenceChangeListener(prefsListener)
    }

    override fun onResume() {
        super.onResume()
        info.text = infoText()
        link.text = linkText()
    }

    override fun onDestroy() {
        prefs().unregisterOnSharedPreferenceChangeListener(prefsListener)
        unregisterReceiver(live)
        super.onDestroy()
    }

    private fun infoText(): String = buildString {
        line("Device", "${Build.BRAND} / ${Build.MODEL} (${Build.MANUFACTURER}, ${Build.DEVICE})")
        line("Android", "${Build.VERSION.RELEASE}  SDK_INT=${Build.VERSION.SDK_INT}")
        line("ABIs", Build.SUPPORTED_ABIS.joinToString(", "))
        line("Build", Build.DISPLAY)
        line("Probe", "$packageName ${pkgVersion(packageName) ?: "?"}, installer=${installerOf(packageName)}")
        line("Unknown sources", unknownSources())
        append('\n')
        line("WebView", webView())
        line("WebView UA", try { WebSettings.getDefaultUserAgent(this@ProbeActivity) } catch (e: Throwable) { "error: ${e.javaClass.simpleName}" })
        append('\n')
        line("Kawai app", pkgVersion(KAWAI)?.let { "$KAWAI installed, versionName=$it${systemFlag(KAWAI)}" } ?: "$KAWAI NOT installed")
        line("Default HOME", defaultHome())
        line("HOME candidates", homeCandidates())
        line("APK viewer", resolveName(Intent(Intent.ACTION_VIEW).setDataAndType(Uri.parse("file:///x.apk"), "application/vnd.android.package-archive")))
        line("Uninstaller", resolveName(Intent(Intent.ACTION_DELETE, Uri.parse("package:$packageName"))))
    }

    private fun linkText(): String {
        val p = prefs()
        val m = p.getInt("count_manifest", 0)
        val l = p.getInt("count_live", 0)
        val last = p.getLong("last_time", 0L)
        return buildString {
            append('\n')
            line("Link broadcast", "${LinkReceiver.ACTION}  (recorded only, no action taken)")
            line("  received", "manifest receiver ${m}x, this screen ${l}x")
            if (last == 0L) line("  last", "never")
            else line("  last", "${SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date(last))} via ${p.getString("last_via", "?")}: ${p.getString("last_extras", "")}")
        }
    }

    /** API 26+ has WebView.getCurrentWebViewPackage(); on API 23 read the known provider packages. */
    private fun webView(): String {
        if (Build.VERSION.SDK_INT >= 26) {
            android.webkit.WebView.getCurrentWebViewPackage()?.let { return "${it.packageName} ${it.versionName} (current)" }
        }
        val found = WEBVIEW_PKGS.mapNotNull { p -> pkgVersion(p)?.let { "$p $it" } }
        return if (found.isEmpty()) "none of ${WEBVIEW_PKGS.joinToString("/")}" else found.joinToString("; ")
    }

    private fun unknownSources(): String = try {
        @Suppress("DEPRECATION")
        val on = Settings.Secure.getInt(contentResolver, Settings.Secure.INSTALL_NON_MARKET_APPS, 0) == 1
        if (on) "ON  <- switch it back OFF when done (Settings > Security)" else "off"
    } catch (e: Throwable) { "? (${e.javaClass.simpleName})" }

    private fun defaultHome(): String {
        val ri = packageManager.resolveActivity(Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME), PackageManager.MATCH_DEFAULT_ONLY)
            ?: return "none"
        val ai = ri.activityInfo
        // "android/...ResolverActivity" = no default chosen (more than one candidate)
        return "${ai.packageName}/${ai.name}"
    }

    private fun homeCandidates(): String =
        packageManager.queryIntentActivities(Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME), 0)
            .joinToString(", ") { it.activityInfo.packageName }.ifEmpty { "none" }

    private fun resolveName(i: Intent): String =
        packageManager.resolveActivity(i, PackageManager.MATCH_DEFAULT_ONLY)?.activityInfo?.let { "${it.packageName}/${it.name}" } ?: "none"

    private fun pkgVersion(pkg: String): String? = try {
        val pi = packageManager.getPackageInfo(pkg, 0)
        @Suppress("DEPRECATION") "${pi.versionName} (code ${pi.versionCode})"
    } catch (e: PackageManager.NameNotFoundException) { null }

    private fun systemFlag(pkg: String): String = try {
        val ai = packageManager.getApplicationInfo(pkg, 0)
        if (ai.flags and android.content.pm.ApplicationInfo.FLAG_SYSTEM != 0) ", system app, ${ai.sourceDir}" else ", user app, ${ai.sourceDir}"
    } catch (e: PackageManager.NameNotFoundException) { "" }

    private fun installerOf(pkg: String): String = try {
        @Suppress("DEPRECATION") packageManager.getInstallerPackageName(pkg) ?: "(none)"
    } catch (e: Throwable) { "?" }

    private fun startSafely(i: Intent) {
        try { startActivity(i) } catch (e: Throwable) { toastLine("cannot open: ${e.javaClass.simpleName}") }
    }

    private fun toastLine(msg: String) { link.text = linkText() + "\n! $msg" }

    private fun prefs() = getSharedPreferences(LinkReceiver.PREFS, Context.MODE_PRIVATE)
    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()
    private fun StringBuilder.line(k: String, v: String) { append(k).append(": ").append(v).append('\n') }

    private fun mono() = TextView(this).apply {
        textSize = 15f
        typeface = Typeface.MONOSPACE
        setTextColor(Color.rgb(230, 235, 240))
        setTextIsSelectable(true)
    }

    private fun button(label: String, bg: Int, size: Float, onClick: () -> Unit) = Button(this).apply {
        text = label
        textSize = size
        setTextColor(Color.WHITE)
        setBackgroundColor(bg)
        setOnClickListener { onClick() }
    }

    companion object {
        const val KAWAI = "jp.co.kawai.denshi.kwmapp2"
        val WEBVIEW_PKGS = listOf("com.google.android.webview", "com.android.webview", "com.android.chrome")
    }
}
