package dev.ca79ng.shell

import android.annotation.SuppressLint
import android.app.Activity
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import no.nordicsemi.android.dfu.DfuProgressListenerAdapter
import no.nordicsemi.android.dfu.DfuServiceController
import no.nordicsemi.android.dfu.DfuServiceInitiator
import no.nordicsemi.android.dfu.DfuServiceListenerHelper
import org.json.JSONObject
import java.io.File
import java.util.UUID

/**
 * BLE-MIDI module firmware update (feature 87) with Nordic DFU, following the
 * original app:
 *   the web side reads the module version (SysEx 10 04 -> 20 04) and sends the
 *   buttonless enter-DFU SysEx (F0 00 02 08 10 55 7F 00 01 F7) over the MIDI link;
 *   this class then scans for the bootloader by name — "DfuTarg" (legacy DFU,
 *   service 00001530-1212-EFDE-1523-785FEABCD123) or "DFUMI1P" (secure DFU,
 *   service 0000FE59-…) — with a 10 s timeout and starts DfuServiceInitiator on
 *   the first hit. Progress goes to ca79ngNative.onDfu as {"state":"scan"},
 *   {"progress":N}, {"progress":100,"complete":1} or {"error":"…"}.
 *
 * Package search order: <usb>/CA79NG/blemidi.zip (+ optional blemidi.json {"version"}),
 * <usb>/BleFirm/blemidi.zip (the original panel layout), then app storage
 * files/firm/blemidi.zip and <external files>/CA79NG/blemidi.zip. A USB package is
 * copied into app storage first so the DFU service never reads from a stick that
 * may be pulled. (The original copied it to files/firm/blemidi.zip but then
 * looked for files/firm/<type>_blemidi.zip — a mismatch we avoid by passing the path.)
 */
class BleDfu(private val activity: Activity, private val usbPath: () -> String, private val emit: (String) -> Unit) {
    private val main = Handler(Looper.getMainLooper())
    private var controller: DfuServiceController? = null
    private var scanCb: ScanCallback? = null

    private val listener = object : DfuProgressListenerAdapter() {
        override fun onDeviceConnecting(deviceAddress: String) = send(JSONObject().put("state", "connect"))
        override fun onDfuProcessStarting(deviceAddress: String) = send(JSONObject().put("progress", 0))
        override fun onProgressChanged(deviceAddress: String, percent: Int, speed: Float, avgSpeed: Float, currentPart: Int, partsTotal: Int) =
            send(JSONObject().put("progress", percent))
        override fun onDfuCompleted(deviceAddress: String) { send(JSONObject().put("progress", 100).put("complete", 1)); finish() }
        override fun onDfuAborted(deviceAddress: String) { send(JSONObject().put("error", "aborted")); finish() }
        override fun onError(deviceAddress: String, error: Int, errorType: Int, message: String?) {
            send(JSONObject().put("error", message ?: "DFU error $error")); finish()
        }
    }

    private fun send(o: JSONObject) = emit(o.toString())

    fun findPackage(): String {
        val usb = usbPath()
        val firmDir = File(activity.filesDir, "firm").apply { mkdirs() }
        val usbCandidates = if (usb.isEmpty()) emptyList() else listOf(File(usb, "CA79NG/blemidi.zip"), File(usb, "BleFirm/blemidi.zip"))
        for (src in usbCandidates) {
            if (!src.isFile) continue
            val dst = File(firmDir, "blemidi.zip")
            src.inputStream().use { i -> dst.outputStream().use { i.copyTo(it) } }
            return describe(dst, File(src.parentFile, "blemidi.json"), "usb")
        }
        val local = listOfNotNull(File(firmDir, "blemidi.zip"), activity.getExternalFilesDir(null)?.let { File(it, "CA79NG/blemidi.zip") })
        for (f in local) if (f.isFile) return describe(f, File(f.parentFile, "blemidi.json"), "app")
        return "{}"
    }

    private fun describe(zip: File, meta: File, source: String): String {
        val o = JSONObject().put("path", zip.absolutePath).put("source", source)
        try { if (meta.isFile) JSONObject(meta.readText()).optString("version").takeIf { it.isNotEmpty() }?.let { o.put("version", it) } } catch (_: Exception) {}
        return o.toString()
    }

    private fun hasPermissions(): Boolean {
        // Android 6–11 need fine location for the bootloader scan, 12+ Nearby devices
        val need = ShellPolicy.blePermissions(Build.VERSION.SDK_INT)
        val missing = need.filter { activity.checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isEmpty()) return true
        activity.requestPermissions(missing.toTypedArray(), 87)
        return false
    }

    /** Scan for the bootloader, then flash `zipPath`. Returns false if it cannot start. */
    @SuppressLint("MissingPermission")
    fun start(zipPath: String): Boolean {
        if (!File(zipPath).isFile) return false
        if (!hasPermissions()) { main.post { send(JSONObject().put("error", "Bluetooth permission needed — allow it and retry")) }; return true }
        val adapter = (activity.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
        val scanner = adapter?.takeIf { it.isEnabled }?.bluetoothLeScanner ?: return false
        DfuServiceListenerHelper.registerProgressListener(activity, listener)
        val filters = BOOTLOADERS.map { ScanFilter.Builder().setDeviceName(it.key).build() }
        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                // results already posted to the main thread still arrive after stopScan(): only the first hit of the
                // scan that is still running may start the DFU (a second hit queued a second DfuServiceInitiator)
                if (scanCb !== this) return
                val name = result.scanRecord?.deviceName ?: result.device.name ?: return
                val service = BOOTLOADERS[name] ?: return
                val uuids = result.scanRecord?.serviceUuids
                // bootloaders normally advertise their DFU service; accept a bare name match otherwise
                if (!uuids.isNullOrEmpty() && uuids.none { it.uuid == service }) return
                stopScan()
                send(JSONObject().put("state", "found").put("name", name).put("secure", service == SECURE_DFU))
                if (Build.VERSION.SDK_INT >= 26) DfuServiceInitiator.createDfuNotificationChannel(activity)
                controller = DfuServiceInitiator(result.device.address)
                    .setDeviceName(name)
                    .setKeepBond(false)
                    .setForeground(false) // the UI stays open during the update
                    .setUnsafeExperimentalButtonlessServiceInSecureDfuEnabled(true)
                    .setZip(zipPath)
                    .start(activity, DfuService::class.java)
            }
            override fun onScanFailed(errorCode: Int) { if (scanCb !== this) return; scanCb = null; send(JSONObject().put("error", "scan failed $errorCode")); finish() }
        }
        scanCb = cb
        send(JSONObject().put("state", "scan"))
        scanner.startScan(filters, ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), cb)
        main.postDelayed({
            if (scanCb === cb) { stopScan(); send(JSONObject().put("error", "bootloader DfuTarg/DFUMI1P not found")); finish() }
        }, 10_000)
        return true
    }

    @SuppressLint("MissingPermission")
    private fun stopScan() {
        val cb = scanCb ?: return
        scanCb = null
        (activity.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter?.bluetoothLeScanner?.stopScan(cb)
    }

    fun abort() {
        stopScan()
        controller?.abort()
    }

    private fun finish() {
        controller = null
        DfuServiceListenerHelper.unregisterProgressListener(activity, listener)
    }

    companion object {
        val LEGACY_DFU: UUID = UUID.fromString("00001530-1212-EFDE-1523-785FEABCD123")
        val SECURE_DFU: UUID = UUID.fromString("0000FE59-0000-1000-8000-00805F9B34FB")
        /** bootloader advertising name -> DFU service (original BLE model map) */
        val BOOTLOADERS = mapOf("DfuTarg" to LEGACY_DFU, "DFUMI1P" to SECURE_DFU)
    }
}
