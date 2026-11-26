package dev.ca79ng.shell

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiInputPort
import android.media.midi.MidiManager
import android.media.midi.MidiOutputPort
import android.media.midi.MidiReceiver
import android.os.Handler
import android.os.HandlerThread
import android.os.ParcelUuid
import org.json.JSONArray
import org.json.JSONObject
import java.util.Collections
import java.util.concurrent.ConcurrentHashMap
import java.util.UUID

/**
 * USB-MIDI and BLE-MIDI through android.media.midi — the same API the original
 * app used (KWMMidiManager), minus its quirks:
 *  - devices are identified by "usb:<id>" / "ble:<mac>" instead of the literal
 *    "USB" (the original supported only one USB device);
 *  - incoming bytes are re-framed into complete messages with a SysEx buffer
 *    large enough for any message the firmware sends (the original dropped
 *    SysEx longer than 511 bytes);
 *  - timestamps are never scheduled (sent immediately), which is what the
 *    original ended up doing for Kawai modules anyway ("denyTimeStampModel").
 */
class MidiBridge(private val context: Context, private val emit: (String, String) -> Unit, private val devicesChanged: () -> Unit) {
    private val midi = context.getSystemService(Context.MIDI_SERVICE) as MidiManager
    private val thread = HandlerThread("ca79ng-midi").apply { start() }
    private val handler = Handler(thread.looper)
    /** An open device with both its ports: all three are closed together (the output port feeds this id's Framer). */
    private class Conn(val device: MidiDevice, val input: MidiInputPort, val output: MidiOutputPort?) {
        fun close() {
            try { output?.close() } catch (_: Exception) {}
            try { input.close() } catch (_: Exception) {}
            try { device.close() } catch (_: Exception) {}
        }
    }
    private val open = ConcurrentHashMap<String, Conn>() // JS bridge thread + MIDI thread
    private val bleSeen: MutableMap<String, String> = Collections.synchronizedMap(LinkedHashMap()) // mac -> name

    private val deviceCallback = object : MidiManager.DeviceCallback() {
        override fun onDeviceAdded(device: MidiDeviceInfo) = devicesChanged()
        override fun onDeviceRemoved(device: MidiDeviceInfo) {
            val id = idOf(device)
            open.remove(id)?.let { it.close(); emit("__detached", id) }
            devicesChanged()
        }
    }

    init {
        midi.registerDeviceCallback(deviceCallback, handler)
    }

    private fun idOf(info: MidiDeviceInfo): String = when (info.type) {
        MidiDeviceInfo.TYPE_BLUETOOTH -> {
            @Suppress("DEPRECATION")
            val dev = info.properties.getParcelable<android.bluetooth.BluetoothDevice>(MidiDeviceInfo.PROPERTY_BLUETOOTH_DEVICE)
            "ble:" + (dev?.address ?: info.id)
        }
        else -> "usb:" + info.id
    }

    private fun nameOf(info: MidiDeviceInfo): String =
        info.properties.getString(MidiDeviceInfo.PROPERTY_NAME)
            ?: info.properties.getString(MidiDeviceInfo.PROPERTY_PRODUCT) ?: "MIDI device"

    @Suppress("DEPRECATION")
    fun listDevices(): String {
        val arr = JSONArray()
        for (info in midi.devices) {
            if (info.inputPortCount == 0 || info.outputPortCount == 0) continue
            arr.put(JSONObject().put("id", idOf(info)).put("name", nameOf(info))
                .put("driver", if (info.type == MidiDeviceInfo.TYPE_BLUETOOTH) "BLE-MIDI" else "USB-MIDI"))
        }
        for ((mac, name) in synchronized(bleSeen) { bleSeen.toList() }) {
            val id = "ble:$mac"
            if ((0 until arr.length()).none { arr.getJSONObject(it).getString("id") == id })
                arr.put(JSONObject().put("id", id).put("name", name).put("driver", "BLE-MIDI"))
        }
        return arr.toString()
    }

    /** Scan 5 s for BLE-MIDI peripherals (service UUID filter only, as the original). */
    @SuppressLint("MissingPermission")
    fun scanBle() {
        val bt = (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter ?: return
        if (!bt.isEnabled) return
        val scanner = bt.bluetoothLeScanner ?: return
        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val name = result.scanRecord?.deviceName ?: result.device.name ?: "BLE-MIDI"
                if (bleSeen.put(result.device.address, name) == null) devicesChanged()
            }
        }
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(BLE_MIDI_SERVICE)).build()
        try {
            scanner.startScan(listOf(filter), ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_BALANCED).build(), cb)
        } catch (e: SecurityException) {
            return // Nearby devices / location permission denied: nothing found, the list stays as is
        }
        handler.postDelayed({ try { scanner.stopScan(cb) } catch (_: Exception) {} }, 5000)
    }

    @SuppressLint("MissingPermission")
    @Suppress("DEPRECATION")
    fun open(id: String): Boolean {
        if (open.containsKey(id)) return true
        val lock = Object()
        var ok = false
        var gaveUp = false
        val onOpened = MidiManager.OnDeviceOpenedListener { device ->
            synchronized(lock) {
                if (device != null && gaveUp) {
                    device.close() // opened after the 8 s timeout: the web side already reported failure
                } else if (device != null) {
                    val input = device.openInputPort(0)
                    if (input == null) {
                        // no input port (held by another app): open() reports failure, so let the device go too -
                        // kept in `open`, it held the BLE link and the next open() returned true with nothing to send on
                        device.close()
                    } else {
                        val output = device.openOutputPort(0)
                        output?.connect(Framer { msg -> emit(id, hex(msg)) })
                        open[id] = Conn(device, input, output)
                        ok = true
                    }
                }
                lock.notifyAll()
            }
        }
        try {
            synchronized(lock) {
                if (id.startsWith("ble:")) {
                    val bt = (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter ?: return false
                    if (!bt.isEnabled) return false
                    midi.openBluetoothDevice(bt.getRemoteDevice(id.removePrefix("ble:")), onOpened, handler)
                } else {
                    val info = midi.devices.firstOrNull { idOf(it) == id } ?: return false
                    midi.openDevice(info, onOpened, handler)
                }
                lock.wait(8000)
                if (!ok) gaveUp = true
            }
        } catch (e: Exception) {
            return false // SecurityException (Nearby devices denied), bad MAC, …
        }
        return ok
    }

    fun send(id: String, hexStr: String) {
        try {
            val bytes = unhex(hexStr)
            open[id]?.input?.send(bytes, 0, bytes.size)
        } catch (e: Exception) {
            // port closed underneath us (BLE link lost): onDeviceRemoved reports the detach
        }
    }

    fun close(id: String) {
        open.remove(id)?.close()
    }

    /** Close every open device (BLE: GATT disconnect) and tell the page, which sees the link close. */
    fun closeOpen() {
        for (id in open.keys.toList()) {
            close(id)
            emit("__detached", id)
        }
    }

    fun closeAll() {
        // the MidiManager keeps the callback (and through it this bridge and the activity) until it is unregistered
        midi.unregisterDeviceCallback(deviceCallback)
        for (id in open.keys.toList()) close(id)
        thread.quitSafely()
    }

    /** Adapts the pure [MidiFramer] to android.media.midi. */
    private class Framer(out: (ByteArray) -> Unit) : MidiReceiver() {
        private val f = MidiFramer(out = out)
        override fun onSend(data: ByteArray, offset: Int, count: Int, timestamp: Long) = f.feed(data, offset, count)
    }

    companion object {
        val BLE_MIDI_SERVICE: UUID = UUID.fromString("03B80E5A-EDE8-4B33-A751-6CE34EC4C700")
        fun hex(b: ByteArray) = MidiFramer.hex(b)
        fun unhex(s: String) = MidiFramer.unhex(s)
    }
}
