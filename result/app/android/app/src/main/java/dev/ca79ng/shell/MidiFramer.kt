package dev.ca79ng.shell

import java.io.ByteArrayOutputStream

/**
 * Re-frames a MIDI byte stream (as android.media.midi delivers it: BLE-MIDI
 * packets split long SysEx across many onSend calls) into complete messages.
 * Pure JVM so it is unit-tested without a device (MidiFramerTest).
 *  - SysEx is buffered across chunks up to [maxSysex] bytes (the original app
 *    dropped SysEx longer than 511 bytes); an over-long one is dropped whole,
 *    never delivered truncated;
 *  - realtime bytes (F8..FF) pass through immediately, even inside SysEx;
 *  - running status is kept for channel messages;
 *  - a status byte inside SysEx (other than F7 / realtime) aborts the SysEx.
 */
class MidiFramer(private val maxSysex: Int = 64 * 1024, private val out: (ByteArray) -> Unit) {
    private val sysex = ByteArrayOutputStream()
    private var inSysex = false
    private var overflow = false
    private var status = 0
    private val msg = ByteArray(3)
    private var have = 0
    private var need = 0

    fun feed(data: ByteArray, offset: Int = 0, count: Int = data.size) {
        for (i in offset until offset + count) feed(data[i].toInt() and 0xFF)
    }

    fun feed(b: Int) {
        if (b >= 0xF8) { out(byteArrayOf(b.toByte())); return }
        if (inSysex) {
            if (b == 0xF7) {
                if (!overflow) { sysex.write(b); out(sysex.toByteArray()) }
                sysex.reset(); inSysex = false; overflow = false; return
            }
            if (b < 0x80) {
                if (sysex.size() < maxSysex - 1) sysex.write(b) else overflow = true
                return
            }
            sysex.reset(); inSysex = false; overflow = false
        }
        if (b == 0xF0) { inSysex = true; sysex.write(b); status = 0; return }
        if (b == 0xF7) return // stray end of SysEx
        if (b >= 0x80) {
            status = b; have = 0
            need = when { b < 0xC0 || b in 0xE0..0xEF -> 2; b < 0xE0 -> 1; b == 0xF2 -> 2; b == 0xF1 || b == 0xF3 -> 1; else -> 0 }
            if (need == 0) { out(byteArrayOf(b.toByte())); status = 0 }
            return
        }
        if (status == 0) return
        msg[0] = status.toByte(); msg[1 + have] = b.toByte(); have++
        if (have == need) { out(msg.copyOf(1 + need)); have = 0; if (status >= 0xF0) status = 0 }
    }

    companion object {
        /** Same text form as the web side's toHex / fromHex (src/protocol/sysex.ts): "F0 7F … F7". */
        fun hex(b: ByteArray) = b.joinToString(" ") { "%02X".format(it.toInt() and 0xFF) }
        fun unhex(s: String) = s.trim().split(Regex("\\s+")).filter { it.isNotEmpty() }.map { it.toInt(16).toByte() }.toByteArray()
    }
}
