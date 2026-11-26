// Web Bluetooth BLE-MIDI transport with our own packet codec.
// GATT: service 03B80E5A-…, I/O characteristic 7772E5DB-… (observed in BleMidi/BleScanner).
// The original app requested MTU 247 and HIGH connection priority; browsers
// negotiate MTU themselves, so we assume the conservative 23 unless told.
import { BLE_MIDI_CHARACTERISTIC, BLE_MIDI_SERVICE, BleMidiDecoder, encodeBlePackets } from "../protocol/blemidi.ts";
import { BaseLink } from "./link.ts";

// Minimal structural types so this file compiles without DOM Bluetooth typings.
export interface GattChar {
  value?: DataView;
  startNotifications(): Promise<unknown>;
  writeValueWithoutResponse(data: Uint8Array): Promise<void>;
  addEventListener(type: string, cb: (e: Event) => void): void;
  removeEventListener(type: string, cb: (e: Event) => void): void;
}

/**
 * Per characteristic: ONE write chain for every link on it, and the link that listens.
 * Chrome rejects a GATT write while another is still in progress ("GATT operation already in
 * progress"). Re-CONNECT on the same piano opened a second link with its own chain, so the old
 * session's `7E 01 00` and the new `7E 01 01` were written at once and the CONNECT was dropped
 * (silently, by a catch that swallowed the rejection) — field 2026-10-31
 * The old link also kept listening, so every notification was handled once more per re-CONNECT.
 */
interface CharState {
  writer: GattWriter;
  owner: BleLink | null;
}
const shared = new WeakMap<GattChar, CharState>();
export interface BtDevice {
  id?: string;
  name?: string;
  gatt?: { connect(): Promise<{ getPrimaryService(uuid: string): Promise<{ getCharacteristic(uuid: string): Promise<GattChar> }>; disconnect(): void }> };
  addEventListener(type: string, cb: () => void): void;
  removeEventListener?(type: string, cb: () => void): void;
}

export interface GattWriterOptions {
  /** Average rate in BLE packet bytes per second (default 2500 — below the module's 31.25 kbaud UART, 3125 B/s). */
  bytesPerSec?: number;
  /** Bytes that may go out back to back before pacing starts (default 160 = 8 full packets at MTU 23). */
  burstBytes?: number;
  /** Extra attempts for a write that rejects (default 2). */
  retries?: number;
  retryDelayMs?: number;
  now?: () => number;
  sleep?: (ms: number) => Promise<void>;
}

/**
 * Serialises GATT write-without-response packets (one outstanding operation), paces them with a
 * token bucket and retries a rejected write.
 *
 * Why: the BLE-MIDI module forwards to the piano at
 * 31.25 kbaud ≈ 3125 B/s but accepts BLE packets faster; whatever it cannot forward piles up in
 * its buffer, which loses data somewhere above 256 B (04_sysex_size). Keeping the average below
 * the UART rate keeps that buffer near empty. A rejected write used to be dropped silently, so a
 * lost CONNECT surfaced only as "timeout waiting for piano reply". If a packet still fails after
 * the retries, the rest of that message is skipped (a SysEx without its head is worthless).
 */
export class GattWriter {
  readonly stats = { packets: 0, bytes: 0, retried: 0, dropped: 0, pacedMs: 0 };
  private readonly write: (p: Uint8Array) => Promise<void>;
  private readonly rate: number;
  private readonly burst: number;
  private readonly retries: number;
  private readonly retryDelayMs: number;
  private readonly now: () => number;
  private readonly sleep: (ms: number) => Promise<void>;
  private tail: Promise<unknown> = Promise.resolve();
  private tokens: number;
  private last: number;

  constructor(write: (p: Uint8Array) => Promise<void>, opt: GattWriterOptions = {}) {
    this.write = write;
    this.rate = opt.bytesPerSec ?? 2500;
    this.burst = opt.burstBytes ?? 160;
    this.retries = opt.retries ?? 2;
    this.retryDelayMs = opt.retryDelayMs ?? 30;
    this.now = opt.now ?? (() => performance.now());
    this.sleep = opt.sleep ?? ((ms) => new Promise((r) => setTimeout(r, ms)));
    this.tokens = this.burst;
    this.last = this.now();
  }

  /** Queue the packets of one message. Resolves true when all of them were written. */
  send(packets: Uint8Array[]): Promise<boolean> {
    const job = this.tail.then(async () => {
      for (let i = 0; i < packets.length; i++) {
        if (!(await this.one(packets[i]))) {
          this.stats.dropped += packets.length - i;
          return false;
        }
      }
      return true;
    });
    this.tail = job.catch(() => false);
    return job;
  }

  /** Resolves when everything queued so far has been written (or dropped). */
  idle(): Promise<void> {
    return this.tail.then(() => undefined);
  }

  private refill(): void {
    const t = this.now();
    this.tokens = Math.min(this.burst, this.tokens + ((t - this.last) * this.rate) / 1000);
    this.last = t;
  }

  private async one(p: Uint8Array): Promise<boolean> {
    this.refill();
    if (this.tokens < p.length) {
      const wait = Math.ceil(((p.length - this.tokens) * 1000) / this.rate);
      this.stats.pacedMs += wait;
      await this.sleep(wait);
      this.refill();
    }
    this.tokens -= p.length;
    for (let attempt = 0; ; attempt++) {
      try {
        await this.write(p);
        this.stats.packets++;
        this.stats.bytes += p.length;
        return true;
      } catch {
        if (attempt >= this.retries) return false;
        this.stats.retried++;
        await this.sleep(this.retryDelayMs * (attempt + 1));
      }
    }
  }
}

export class BleLink extends BaseLink {
  readonly kind = "ble" as const;
  readonly name: string;
  private readonly disconnect: () => void;
  private readonly ch: GattChar;
  private readonly st: CharState;
  private readonly listener: () => void;
  private readonly mtu: number;
  /** The characteristic's one paced write chain, shared by every link on it. */
  readonly writer: GattWriter;
  readonly key: string;
  reopen?: () => Promise<BleLink>;
  private open = true;
  /** Resolves when close() has let the GATT connection go (after the queued writes went out). */
  released: Promise<void> = Promise.resolve();
  constructor(name: string, ch: GattChar, disconnect: () => void, mtu: number, key = `ble:${name}`) {
    super();
    this.name = name;
    this.key = key;
    this.ch = ch;
    this.disconnect = disconnect;
    this.mtu = mtu;
    const dec = new BleMidiDecoder((m) => this.emit(m));
    this.listener = () => {
      const v = ch.value;
      if (!v) return;
      const pk = new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
      // raw packet ring for field debugging over CDP (cdp.sh: window.__bleRaw), 2026-11-16
      const g = globalThis as unknown as { __bleRaw?: string[] };
      if (g.__bleRaw) { g.__bleRaw.push(Array.from(pk, (x) => x.toString(16).padStart(2, "0")).join(" ")); if (g.__bleRaw.length > 600) g.__bleRaw.shift(); }
      dec.feed(pk);
    };
    let st = shared.get(ch);
    if (!st) shared.set(ch, (st = { writer: new GattWriter((p) => ch.writeValueWithoutResponse(p)), owner: null }));
    st.owner?.retire();
    st.owner = this;
    this.st = st;
    this.writer = st.writer;
    ch.addEventListener("characteristicvaluechanged", this.listener);
  }
  /** A newer link took the characteristic: stop listening. Writes still go out, in order, on the shared chain. */
  retire(): void {
    this.ch.removeEventListener("characteristicvaluechanged", this.listener);
    if (this.st.owner === this) this.st.owner = null;
  }
  send(msg: Uint8Array): void {
    // one paced chain per characteristic: GATT allows one outstanding operation, and the
    // old link's 7E 01 00 must not race the new link's 7E 01 01
    void this.writer.send(encodeBlePackets([msg], Math.floor(performance.now()), this.mtu));
  }
  get isOpen(): boolean {
    return this.open;
  }
  /**
   * Stop listening and let the GATT connection go once the queued writes (a DISCONNECT `7E 01 00`)
   * went out — a released BLE-MIDI module is free for the next central (another phone, the PC).
   */
  close(): void {
    if (!this.open) return;
    this.open = false;
    this.retire();
    this.released = this.writer.idle().then(() => this.disconnect());
    this.closed();
  }
}

/** Open links by device id: picking the same piano again returns the link already open. */
const openLinks = new Map<string, BleLink>();

/**
 * The GATT connection dropped between `gatt.connect()` and the service / characteristic lookup.
 * Chrome: "GATT Server is disconnected. Cannot retrieve services. (Re)connect first with
 * `device.gatt.connect`." (NetworkError) — field 2026-11-01: the first connect after a page reload
 * failed with it, the second worked.
 */
export function isGattDropped(e: unknown): boolean {
  const err = e as { message?: string } | null;
  return /GATT Server is disconnected|Connection attempt failed/i.test(err?.message ?? "");
}

/** connect + service + characteristic + notifications; on a dropped GATT connection once more after `retryDelayMs`. */
async function openGatt(device: BtDevice, retryDelayMs: number) {
  for (let attempt = 0; ; attempt++) {
    try {
      const server = await device.gatt!.connect();
      const service = await server.getPrimaryService(BLE_MIDI_SERVICE);
      const ch = await service.getCharacteristic(BLE_MIDI_CHARACTERISTIC);
      await ch.startNotifications();
      return { server, ch };
    } catch (e) {
      if (attempt >= 1 || !isGattDropped(e)) throw e;
      await new Promise((r) => setTimeout(r, retryDelayMs));
    }
  }
}

/** Connect (or return the open link to) one BLE-MIDI device. Exported for tests. */
export async function connectBleDevice(device: BtDevice, mtu = 23, opt: { retryDelayMs?: number } = {}): Promise<BleLink> {
  const id = device.id ?? device.name ?? "BLE-MIDI";
  const cur = openLinks.get(id);
  if (cur?.isOpen) return cur;
  if (cur) await cur.released; // the old link is still letting the connection go: do not race its disconnect
  const { server, ch } = await openGatt(device, opt.retryDelayMs ?? 500);
  // another open of this device finished first (a hung attempt the watchdog gave up on comes back late): that link
  // is the one in use — a second link on the characteristic would take its notifications
  const now = openLinks.get(id);
  if (now?.isOpen) return now;
  const link = new BleLink(device.name ?? "BLE-MIDI", ch, () => server.disconnect(), mtu, `ble:${id}`);
  link.reopen = () => connectBleDevice(device, mtu, opt);
  openLinks.set(id, link);
  const gone = () => {
    device.removeEventListener?.("gattserverdisconnected", gone);
    link.close();
  };
  device.addEventListener("gattserverdisconnected", gone);
  link.onClose(() => device.removeEventListener?.("gattserverdisconnected", gone));
  return link;
}

type Bluetooth = { requestDevice(o: object): Promise<BtDevice>; getDevices?(): Promise<BtDevice[]> };
const bluetooth = () => (globalThis.navigator as unknown as { bluetooth?: Bluetooth } | undefined)?.bluetooth;

/**
 * Can a remembered device be reopened after a reload without the chooser? Needs `getDevices()`,
 * which desktop Chrome has only behind chrome://flags/#enable-web-bluetooth-new-permissions-backend
 * (field 2026-11-01: `navigator.bluetooth.getDevices is not a function`).
 */
export function bleReopenAvailable(): boolean {
  return typeof bluetooth()?.getDevices === "function";
}

/**
 * A remembered device (key `ble:<id>`) after a page reload, without the chooser: Chrome's
 * `getDevices()` lists the devices this origin was granted. Throws when it is not available.
 */
export async function reopenBleMidi(key: string, mtu = 23): Promise<BleLink> {
  const bt = bluetooth();
  if (typeof bt?.getDevices !== "function") throw new Error("Web Bluetooth getDevices() is not available");
  const dev = (await bt.getDevices()).find((d) => `ble:${d.id}` === key);
  if (!dev) throw new Error("the remembered Bluetooth device is not granted any more");
  return connectBleDevice(dev, mtu);
}

/** The browser's device chooser alone (needs a user gesture): the BLE-MIDI device the user picked, not connected yet. */
export async function requestBleDevice(): Promise<BtDevice> {
  const bt = bluetooth();
  if (!bt) throw new Error("Web Bluetooth is not available in this browser");
  return bt.requestDevice({ filters: [{ services: [BLE_MIDI_SERVICE] }] });
}

/** The browser's device chooser; the piano already open comes back as the same link (no second session). */
export async function requestBleMidi(mtu = 23): Promise<BleLink> {
  return connectBleDevice(await requestBleDevice(), mtu);
}
