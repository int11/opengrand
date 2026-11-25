package dev.ca79ng.shell

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.os.storage.StorageManager
import android.provider.Settings
import android.util.DisplayMetrics
import android.view.WindowInsets
import android.widget.FrameLayout
import android.view.WindowManager
import android.webkit.JavascriptInterface
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebView
import androidx.webkit.WebViewAssetLoader
import androidx.webkit.WebViewClientCompat
import org.json.JSONObject

/**
 * Thin shell: a WebView running the OpenGrand UI (assets/www) plus the `ca79ng`
 * bridge declared in src/transport/android.ts. Compared with the original
 * 122-command `kwmcore` bridge, playback/recording run in the firmware and the
 * shell only provides MIDI transport and a few panel services.
 */
class MainActivity : Activity() {
    private lateinit var web: WebView
    private lateinit var midi: MidiBridge
    private lateinit var updates: UpdateManager
    private lateinit var dfu: BleDfu
    private var selfUpdate: SelfUpdate? = null
    private var fileCallback: ValueCallback<Array<Uri>>? = null

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (BuildConfig.PANEL) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        web = WebView(this)
        // targetSdk 35 forces edge-to-edge on Android 15: keep the page out from under the
        // status and navigation bars (the header's 연결 button sat under the status bar).
        // WebView ignores its own padding, so the inset goes on a wrapping frame.
        val frame = FrameLayout(this)
        frame.addView(web, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        setContentView(frame)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) frame.setOnApplyWindowInsetsListener { v, insets ->
            val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            WindowInsets.CONSUMED
        }
        midi = MidiBridge(this, ::emit) { js("window.ca79ngNative && ca79ngNative.onDevices && ca79ngNative.onDevices()") }
        updates = UpdateManager(this, ::usbPath) { json -> callback("onUpdate", json) }
        dfu = BleDfu(this, ::usbPath) { json -> callback("onDfu", json) }
        // online self-update from GitHub releases: phone only (the panel updates from USB)
        if (!BuildConfig.PANEL) selfUpdate = SelfUpdate(this) { json -> callback("onSelfUpdate", json) }
        if (!BuildConfig.PANEL) ensureBlePermissions()
        // debug APK: chrome://inspect on the PC sees the page (console, network) over adb
        if (BuildConfig.DEBUG) WebView.setWebContentsDebuggingEnabled(true)

        // Serve the bundle from a https origin (WebView blocks module scripts from file://)
        val loader = WebViewAssetLoader.Builder().addPathHandler("/assets/", WebViewAssetLoader.AssetsPathHandler(this)).build()
        web.webViewClient = object : WebViewClientCompat() {
            override fun shouldInterceptRequest(view: WebView, request: android.webkit.WebResourceRequest) =
                loader.shouldInterceptRequest(request.url)
        }
        // Without a WebChromeClient WebView cancels confirm()/prompt() (delete, format, factory
        // reset, rename silently did nothing) and ignores <input type=file> (OTA key / .c79
        // payload, MIDI upload). The default client shows the JS dialogs; files go to the picker.
        web.webChromeClient = object : WebChromeClient() {
            override fun onShowFileChooser(view: WebView, cb: ValueCallback<Array<Uri>>, params: FileChooserParams): Boolean {
                fileCallback?.onReceiveValue(null)
                fileCallback = cb
                val types = ShellPolicy.chooserMimeTypes(params.acceptTypes)
                val pick = Intent(Intent.ACTION_GET_CONTENT).addCategory(Intent.CATEGORY_OPENABLE)
                    .setType(if (types.size == 1) types[0] else "*/*")
                    .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, params.mode == FileChooserParams.MODE_OPEN_MULTIPLE)
                if (types.size > 1) pick.putExtra(Intent.EXTRA_MIME_TYPES, types)
                return try {
                    startActivityForResult(Intent.createChooser(pick, null), PICK_FILE)
                    true
                } catch (e: Exception) {
                    fileCallback = null
                    false
                }
            }
        }
        web.settings.javaScriptEnabled = true
        web.settings.domStorageEnabled = true
        web.addJavascriptInterface(Bridge(), "ca79ng")
        web.loadUrl("https://appassets.androidplatform.net/assets/www/index.html")
    }

    private fun js(code: String) = runOnUiThread { web.evaluateJavascript(code, null) }

    private fun callback(name: String, json: String) =
        js("window.ca79ngNative && ca79ngNative.$name && ca79ngNative.$name(${JSONObject.quote(json)})")

    /** First removable volume (USB memory), "" if none. Replaces libMRJNI getUSBPath(). */
    private fun usbPath(): String {
        val sm = getSystemService(Context.STORAGE_SERVICE) as StorageManager
        if (Build.VERSION.SDK_INT >= 30) {
            return sm.storageVolumes.firstOrNull { it.isRemovable && it.state == "mounted" }?.directory?.absolutePath ?: ""
        }
        return ""
    }

    private fun emit(id: String, hex: String) {
        if (id == "__detached") js("ca79ngNative.onDetached('$hex')")
        else js("ca79ngNative.onMidi('$id','$hex')")
    }

    @Deprecated("Activity result API needs androidx.activity; the shell stays on plain Activity")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode == PICK_FILE) {
            fileCallback?.onReceiveValue(WebChromeClient.FileChooserParams.parseResult(resultCode, data))
            fileCallback = null
            return
        }
        @Suppress("DEPRECATION") super.onActivityResult(requestCode, resultCode, data)
    }

    /** Asks for whatever BLE-MIDI still lacks on this Android version (ShellPolicy.blePermissions). */
    private fun ensureBlePermissions() {
        val need = ShellPolicy.blePermissions(Build.VERSION.SDK_INT)
            .filter { checkSelfPermission(it) != android.content.pm.PackageManager.PERMISSION_GRANTED }
        if (need.isNotEmpty()) requestPermissions(need.toTypedArray(), 1)
    }

    // Hand the piano over when the app is away (LinkLifecycle): the page sends DISCONNECT and closes the
    // link (GATT disconnect, the module is free for the PC / another phone); back in front it reconnects.
    // The panel build never lets go (it is the piano's own screen).
    private val linkLife = LinkLifecycle()
    private val ui = Handler(Looper.getMainLooper())
    private val releaseCheck = Runnable {
        if (linkLife.releaseDue(SystemClock.elapsedRealtime())) {
            callback("onLifecycle", "background")
            ui.postDelayed({ midi.closeOpen() }, LinkLifecycle.FORCE_CLOSE_MS) // a throttled page: free the module anyway
        }
    }

    override fun onStop() {
        super.onStop()
        if (BuildConfig.PANEL) return
        linkLife.onStop(SystemClock.elapsedRealtime())
        ui.postDelayed(releaseCheck, linkLife.graceMs)
    }

    override fun onStart() {
        super.onStart()
        ui.removeCallbacks(releaseCheck)
        if (linkLife.onStart()) callback("onLifecycle", "foreground")
    }

    override fun onDestroy() {
        ui.removeCallbacksAndMessages(null)
        midi.closeAll()
        web.destroy()
        super.onDestroy()
    }

    inner class Bridge {
        @JavascriptInterface fun listDevices(): String = midi.listDevices()
        @JavascriptInterface fun scanBle() {
            if (!BuildConfig.PANEL) runOnUiThread { ensureBlePermissions() } // denied before: ask again on "Connect"
            midi.scanBle()
        }
        @JavascriptInterface fun open(id: String): Boolean = midi.open(id)
        @JavascriptInterface fun send(id: String, hex: String) = midi.send(id, hex)
        @JavascriptInterface fun close(id: String) = midi.close(id)

        /** Reported by the shell instead of being guessed by the UI (the original
         *  app never detected the panel itself). */
        @JavascriptInterface fun panel(): String {
            val o = JSONObject().put("embedded", BuildConfig.PANEL)
            if (BuildConfig.PANEL) {
                val dm = DisplayMetrics()
                @Suppress("DEPRECATION") windowManager.defaultDisplay.getRealMetrics(dm)
                o.put("lcd", JSONObject().put("w", dm.widthPixels).put("h", dm.heightPixels).put("dpi", dm.densityDpi))
            }
            return o.toString()
        }

        /** LCD backlight 0..255 (panel build is a system app holding WRITE_SETTINGS). */
        @JavascriptInterface fun setBacklight(level: Int) {
            if (!BuildConfig.PANEL) return
            if (Build.VERSION.SDK_INT >= 23 && !Settings.System.canWrite(this@MainActivity)) return
            Settings.System.putInt(contentResolver, Settings.System.SCREEN_BRIGHTNESS, level.coerceIn(0, 255))
        }

        @JavascriptInterface fun usbPath(): String = this@MainActivity.usbPath()

        /** Shell version for the Information screen. */
        @JavascriptInterface fun appInfo(): String = JSONObject()
            .put("version", BuildConfig.VERSION_NAME).put("versionCode", BuildConfig.VERSION_CODE)
            .put("flavor", if (BuildConfig.PANEL) "panel" else "phone").toString()

        /** Feature 86: <usb>/CA79NG/manifest.json + app.apk (sha256-verified) */
        @JavascriptInterface fun checkUpdate(): String = updates.check()
        @JavascriptInterface fun installUpdate(): Boolean = updates.install()

        /** Online self-update (phone): state JSON ("" on the panel), check + download, open the installer */
        @JavascriptInterface fun selfUpdateState(): String = selfUpdate?.state() ?: ""
        @JavascriptInterface fun selfUpdateCheck(): Boolean = selfUpdate?.check() ?: false
        @JavascriptInterface fun selfUpdateApply(): Boolean = selfUpdate?.apply() ?: false

        /** Feature 87: BLE-MIDI module DFU (Nordic) */
        @JavascriptInterface fun findDfuPackage(): String = try { dfu.findPackage() } catch (e: Exception) { "{}" }
        @JavascriptInterface fun startDfu(path: String): Boolean = runOnUiThreadBlocking { dfu.start(path) }
        @JavascriptInterface fun abortDfu() = runOnUiThread { dfu.abort() }
    }

    companion object {
        private const val PICK_FILE = 2
    }

    /** JavascriptInterface calls arrive on a binder thread; BLE scanning wants the main thread. */
    private fun <T> runOnUiThreadBlocking(f: () -> T): T {
        val latch = java.util.concurrent.CountDownLatch(1)
        var result: Result<T>? = null
        runOnUiThread { result = runCatching(f); latch.countDown() }
        latch.await()
        return result!!.getOrThrow()
    }
}
