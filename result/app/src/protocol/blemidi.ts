// BLE-MIDI packet codec (Bluetooth LE MIDI specification 1.0).
//
// The original Android app let the OS stack frame BLE-MIDI
// (MidiManager.openBluetoothDevice) and only hand-built packets to rename the
// module. The new app also runs
// in browsers via Web Bluetooth, where no OS framing exists, so we implement
// the codec ourselves.
//
// Packet: header (1 0 t12..t7) then for each message a timestamp byte
// (1 t6..t0) followed by MIDI bytes. Running status is allowed between
// messages of one packet. SysEx may span packets: a continuation packet starts
// with the header byte and continues the data without a timestamp; the
// terminating F7 is preceded by its own timestamp byte.

export const BLE_MIDI_SERVICE = "03b80e5a-ede8-4b33-a751-6ce34ec4c700";
export const BLE_MIDI_CHARACTERISTIC = "7772e5db-3868-4112-a1a9-f2669d106bf3";

/** Encode one or more complete MIDI messages into BLE packets of at most
 * `mtu - 3` bytes (ATT header). `timeMs` is the 13-bit millisecond clock. */
export function encodeBlePackets(messages: Uint8Array[], timeMs: number, mtu = 23): Uint8Array[] {
  const max = Math.max(5, mtu - 3);
  const ts = timeMs & 0x1fff;
  const header = 0x80 | ((ts >> 7) & 0x3f);
  const tsByte = 0x80 | (ts & 0x7f);
  const packets: Uint8Array[] = [];
  let cur: number[] = [header];

  const flush = () => {
    if (cur.length > 1) packets.push(Uint8Array.from(cur));
    cur = [header];
  };

  for (const msg of messages) {
    if (msg.length === 0) continue;
    if (msg[0] === 0xf0) {
      // SysEx: timestamp + F0 + data... ; split across packets; F7 gets its own timestamp
      const data = msg[msg.length - 1] === 0xf7 ? msg.subarray(0, msg.length - 1) : msg;
      if (cur.length + 2 > max) flush();
      cur.push(tsByte);
      for (let i = 0; i < data.length; i++) {
        if (cur.length >= max) {
          flush(); // continuation packet: header only, no timestamp
        }
        cur.push(data[i]);
      }
      if (cur.length + 2 > max) flush();
      cur.push(tsByte, 0xf7);
    } else {
      if (cur.length + 1 + msg.length > max) flush();
      cur.push(tsByte, ...msg);
    }
  }
  flush();
  return packets;
}

/** Stateful decoder: feed packets, receive complete MIDI messages with their
 * 13-bit timestamps. Handles running status and SysEx continuation. */
export class BleMidiDecoder {
  private sysex: number[] | null = null;
  private running = 0;
  private readonly onMessage: (msg: Uint8Array, timestamp: number) => void;
  constructor(onMessage: (msg: Uint8Array, timestamp: number) => void) {
    this.onMessage = onMessage;
  }

  feed(packet: ArrayLike<number>): void {
    if (packet.length < 2 || (packet[0] & 0x80) === 0) return;
    const high = packet[0] & 0x3f;
    let ts = high << 7;
    let i = 1;
    // continuation of a SysEx: data bytes directly after the header
    if (this.sysex && (packet[1] & 0x80) === 0) {
      while (i < packet.length && (packet[i] & 0x80) === 0) this.sysex.push(packet[i++]);
    }
    while (i < packet.length) {
      const b = packet[i];
      if (b & 0x80) {
        // timestamp byte (a status byte never appears without a timestamp before it)
        ts = (high << 7) | (b & 0x7f);
        i++;
        if (i >= packet.length) break;
      }
      const s = packet[i];
      // data bytes while a SysEx is open belong to it: after an interleaved real-time byte, or after a
      // timestamp byte some modules put at the start of a continuation packet. Treating them as running-
      // status channel messages turned a file DataPacket into `80 xx yy` note-offs (field 2026-11-16).
      if (this.sysex && (s & 0x80) === 0) {
        while (i < packet.length && (packet[i] & 0x80) === 0) this.sysex.push(packet[i++]);
        continue;
      }
      if (s === 0xf7 && this.sysex) {
        this.sysex.push(0xf7);
        this.onMessage(Uint8Array.from(this.sysex), ts);
        this.sysex = null;
        i++;
        continue;
      }
      if (s === 0xf0) {
        this.running = 0; // SysEx cancels running status (MIDI 1.0)
        this.sysex = [0xf0];
        i++;
        while (i < packet.length && (packet[i] & 0x80) === 0) this.sysex.push(packet[i++]);
        continue;
      }
      if (s >= 0xf8) {
        this.onMessage(Uint8Array.of(s), ts);
        i++;
        continue;
      }
      let status = this.running;
      if (s & 0x80) {
        status = s;
        i++;
      }
      if (!status) {
        i++;
        continue;
      }
      if (status < 0xf0) this.running = status;
      const n = dataLength(status);
      const msg = [status];
      for (let k = 0; k < n && i < packet.length && (packet[i] & 0x80) === 0; k++) msg.push(packet[i++]);
      if (msg.length === n + 1) this.onMessage(Uint8Array.from(msg), ts);
    }
  }
}

export function dataLength(status: number): number {
  if (status < 0xf0) {
    const t = status & 0xf0;
    return t === 0xc0 || t === 0xd0 ? 1 : 2;
  }
  if (status === 0xf1 || status === 0xf3) return 1;
  if (status === 0xf2) return 2;
  return 0;
}

// ---- QS BLE-MIDI module private SysEx (manufacturer 00 02 08) ----
// Observed in bluetoothmidi.js and BleMidi.renameDevice / DFU_execute.
const MODULE_HDR = [0xf0, 0x00, 0x02, 0x08, 0x10, 0x55];

export const BleModule = {
  setName(name: string): Uint8Array {
    const bytes = Array.from(new TextEncoder().encode(name.slice(0, 16)), (b) => b & 0x7f);
    while (bytes.length < 16) bytes.push(0x00);
    return Uint8Array.from([...MODULE_HDR, 0x00, 0x01, ...bytes, 0xf7]);
  },
  timestampOffset(t: number): Uint8Array {
    return Uint8Array.from([...MODULE_HDR, 0x00, 0x02, t & 0x7f, 0x00, 0xf7]);
  },
  requestFirmware(): Uint8Array {
    return Uint8Array.from([...MODULE_HDR, 0x10, 0x04, 0x00, 0xf7]);
  },
  enterDfu(): Uint8Array {
    return Uint8Array.from([...MODULE_HDR, 0x7f, 0x00, 0x01, 0xf7]);
  },
  disconnect(): Uint8Array {
    return Uint8Array.from([...MODULE_HDR, 0x7f, 0x01, 0x01, 0xf7]);
  },
  /** Parse a firmware version reply (20 04 + ASCII). */
  parseFirmwareReply(msg: ArrayLike<number>): string | null {
    for (let i = 0; i < MODULE_HDR.length; i++) if (msg[i] !== MODULE_HDR[i]) return null;
    if (msg[6] !== 0x20 || msg[7] !== 0x04) return null;
    let s = "";
    for (let i = 8; i < msg.length - 1; i++) if (msg[i]) s += String.fromCharCode(msg[i]);
    return s;
  },
};
