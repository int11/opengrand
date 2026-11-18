package dev.ca79ng.shell

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

/** The shell's MIDI re-framing and hex text form — the contract with src/transport/android.ts. */
class MidiFramerTest {
    private fun frame(vararg chunks: String, max: Int = 64 * 1024): List<String> {
        val got = ArrayList<String>()
        val f = MidiFramer(max) { got.add(MidiFramer.hex(it)) }
        for (c in chunks) f.feed(MidiFramer.unhex(c))
        return got
    }

    @Test fun hexMatchesWebSide() {
        // src/protocol/sysex.ts toHex: upper case, single spaces
        assertEquals("F0 40 7F 07 08 02 5F 7F 7F F7", MidiFramer.hex(byteArrayOf(0xF0.toByte(), 0x40, 0x7F, 7, 8, 2, 0x5F, 0x7F, 0x7F, 0xF7.toByte())))
        assertArrayEquals(byteArrayOf(0xF0.toByte(), 0x0A, 0xF7.toByte()), MidiFramer.unhex("  f0  0a\nF7 "))
    }

    @Test fun sysexSplitAcrossBlePackets() {
        // BLE-MIDI delivers one SysEx in several onSend calls
        assertEquals(listOf("F0 40 7F 10 04 1E 01 F7"), frame("F0 40 7F", "10 04", "1E 01 F7"))
    }

    @Test fun longSysexKept() {
        // OTA / dump replies are longer than the 511 bytes the original app accepted
        val body = (0 until 600).joinToString(" ") { "%02X".format(it and 0x7F) }
        val got = frame("F0 $body F7")
        assertEquals(1, got.size)
        assertEquals(602, MidiFramer.unhex(got[0]).size)
    }

    @Test fun overlongSysexDroppedWhole() {
        assertEquals(listOf("90 3C 40"), frame("F0 01 02 03 04 F7 90 3C 40", max = 4))
    }

    @Test fun realtimeInsideSysex() {
        assertEquals(listOf("F8", "F0 01 02 F7"), frame("F0 01 F8 02 F7"))
    }

    @Test fun runningStatusAndAbortedSysex() {
        assertEquals(listOf("90 3C 40", "90 3E 40", "B0 07 64"), frame("90 3C 40 3E 40", "F0 01 02 B0 07 64"))
    }

    @Test fun strayEndIgnored() {
        assertEquals(listOf("C0 05"), frame("F7 C0 05"))
    }
}
