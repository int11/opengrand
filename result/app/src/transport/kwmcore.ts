// Transport over the ORIGINAL panel shell (Kawai PianoRemote 3.4.12 "KWM4 App").
// Used when our web UI is installed into that shell as a UI bundle (appUI.xxx): the Java side stays stock, only the page is ours.
//
// Bridge, as read from the decompiled shell (jp.co.kawai.kwmcore):
//   JS → Java  `kwmcore.postMessage("kwmcore://<cmd>/?<json>")`
//              (`@JavascriptInterface KWMCoreInterface.postMessage`; iOS build:
//              `webkit.messageHandlers.kwmcore.postMessage`). Java takes cmd =
//              text between "kwmcore://" and the char before the first "?".
//   Java → JS  evaluateJavascript of fixed snippets on the global `KWM` object:
//              KWM.resolvePromise('<cmd>','<json>') / KWM.rejectPromise(…)
//              KWM.callbackNative('<cmd>','<json>',keep)
//              KWM.onMidiMessage([b0,b1,…],'<deviceId>','<sec>')   (incoming MIDI)
//              KWM.onDeviceStateChange()                            (device added/removed)
//              KWM.devices=[{id,name,driver,suspended?}]
//              KWM.localUiVersion='…' / latestUiVersion / localAppVersion / latestAppVersion / latestBleMidiVersion
//              KWM.dispatchEvent('deviceready',{midi,ble,…})        (after every page load)
//   The shell only forwards MIDI and fires deviceready once `typeof KWM === "object"`
//   at onPageFinished (KWMWebViewClient → CoreMGR.detectKWM), so installKwm()
//   must run while the page loads.
//   Promises are keyed by command NAME, not by call id: two overlapping calls of
//   the same command would both settle on the first answer. callPromise() queues
//   them per command.
//
// MIDI: `sendMidiMessage {msg:[…], id?}` → KWMMidiManager.sendMessage (no id = all
// devices). The panel's main board is the single TYPE_USB device, id "USB".
// Incoming bytes come parsed: one complete message per call, SysEx whole at F7
// (Java buffer 512 B — longer SysEx is dropped by the shell).
import { BaseLink, type LinkCandidate } from "./link.ts";

/** What the page sees of the Java interface (window.kwmcore). */
export interface KwmcoreHost {
  postMessage(url: string): void;
}

export interface KwmDevice {
  id: string;
  name: string;
  driver?: string;
  suspended?: boolean;
}

/** The global the shell calls into. Field names are the shell's. */
export interface KwmGlobal {
  devices: KwmDevice[];
  localUiVersion: string | null;
  latestUiVersion: string | null;
  localAppVersion: string | null;
  latestAppVersion: string | null;
  latestBleMidiVersion: string | null;
  isBleMidiWindowShown: boolean;
  willSystemPowerOff: boolean;
  resolvePromise(cmd: string, json: string): void;
  rejectPromise(cmd: string, json: string): void;
  callbackNative(cmd: string, json: string, keep: boolean): void;
  onMidiMessage(msg: number[], id: string, time: string): void;
  onSmfMidiMessage(msg: number[], time: string): void;
  onDeviceStateChange(): void;
  dispatchEvent(id: string, arg: unknown): void;
  donePlayConcertMagic(...a: unknown[]): void;
  onPlaySmfJson(name: string): void;
  /** set by the boot guard in ng/index.html / by us: last deviceready detail */
  __ready?: unknown;
  /** marks that our handlers are installed (not the shell's or the original UI's) */
  __ca79ng?: true;
}

type G = { kwmcore?: unknown; webkit?: { messageHandlers?: { kwmcore?: unknown } }; KWM?: Partial<KwmGlobal>; document?: Document };
const g = globalThis as unknown as G;

function hasPost(o: unknown): o is KwmcoreHost {
  return typeof o === "object" && o !== null && typeof (o as KwmcoreHost).postMessage === "function";
}

/** The Java (or iOS) bridge object, or null outside the original shell. */
export function kwmcoreHost(): KwmcoreHost | null {
  if (hasPost(g.kwmcore)) return g.kwmcore;
  const w = g.webkit?.messageHandlers?.kwmcore;
  return hasPost(w) ? w : null;
}

export function kwmcoreAvailable(): boolean {
  return kwmcoreHost() !== null;
}

/** Same URL the original kwm.js builds: kwmcore://<cmd>/?<json>. */
export function kwmUrl(cmd: string, obj?: object | null): string {
  if (!/^[A-Za-z_][\w]*$/.test(cmd)) throw new Error(`bad kwmcore command ${cmd}`);
  return `kwmcore://${cmd}/?${JSON.stringify(obj ?? {})}`;
}

/** Fire-and-forget command. Returns false outside the shell. */
export function callNative(cmd: string, obj?: object | null): boolean {
  const host = kwmcoreHost();
  if (!host) return false;
  host.postMessage(kwmUrl(cmd, obj));
  return true;
}

// ---- Java → JS plumbing -------------------------------------------------------

type Waiter = { resolve: (v: unknown) => void; reject: (e: Error) => void; timer: ReturnType<typeof setTimeout> | null };
const pending = new Map<string, Waiter[]>();
const callbacks = new Map<string, (v: unknown) => void>();
const shellCounts: Record<string, number> = {};
const midiListeners = new Set<(msg: Uint8Array, id: string) => void>();
const deviceListeners = new Set<() => void>();
const events = new EventTarget();

function parse(json: string): unknown {
  if (!json) return null;
  try {
    return JSON.parse(json);
  } catch {
    return json;
  }
}

function settle(cmd: string, ok: boolean, json: string): void {
  const q = pending.get(cmd);
  const w = q?.shift();
  if (!w) return;
  if (q && q.length === 0) pending.delete(cmd);
  if (w.timer) clearTimeout(w.timer);
  if (ok) w.resolve(parse(json));
  else w.reject(Object.assign(new Error(`${cmd} rejected by the shell`), { detail: parse(json) }));
  sendNext(cmd);
}

// queued-but-not-yet-sent calls per command (one in flight per command)
const queued = new Map<string, { obj: object | null | undefined; timeoutMs: number }[]>();

function sendNext(cmd: string): void {
  const q = queued.get(cmd);
  const next = q?.shift();
  if (!next) return;
  if (q && q.length === 0) queued.delete(cmd);
  const w = pending.get(cmd)?.[0];
  if (w && next.timeoutMs > 0) {
    w.timer = setTimeout(() => settle(cmd, false, JSON.stringify({ error: "timeout" })), next.timeoutMs);
  }
  if (!callNative(cmd, next.obj)) settle(cmd, false, JSON.stringify({ error: "no kwmcore bridge" }));
}

/**
 * Command answered by KWM.resolvePromise/rejectPromise. Calls of the same command
 * are serialised (the shell keys promises by command name). Rejects after
 * `timeoutMs` (0 = never) — several commands never answer outside embedMode.
 */
export function callPromise<T = unknown>(cmd: string, obj?: object | null, timeoutMs = 5000): Promise<T> {
  installKwm();
  return new Promise<T>((resolve, reject) => {
    const q = pending.get(cmd) ?? [];
    q.push({ resolve: resolve as (v: unknown) => void, reject, timer: null });
    pending.set(cmd, q);
    const wq = queued.get(cmd) ?? [];
    wq.push({ obj, timeoutMs });
    queued.set(cmd, wq);
    if (q.length === 1) sendNext(cmd);
  });
}

/** Install our handlers on the global KWM object (created if missing, identity kept). */
export function installKwm(): KwmGlobal {
  const k = (g.KWM ??= {}) as KwmGlobal;
  if (k.__ca79ng) return k;
  const prevReady = k.__ready;
  Object.assign(k, {
    devices: k.devices ?? [],
    localUiVersion: k.localUiVersion ?? null,
    latestUiVersion: k.latestUiVersion ?? null,
    localAppVersion: k.localAppVersion ?? null,
    latestAppVersion: k.latestAppVersion ?? null,
    latestBleMidiVersion: k.latestBleMidiVersion ?? null,
    isBleMidiWindowShown: k.isBleMidiWindowShown ?? false,
    willSystemPowerOff: false,
    resolvePromise: (cmd: string, json: string) => settle(cmd, true, json),
    rejectPromise: (cmd: string, json: string) => settle(cmd, false, json),
    callbackNative: (cmd: string, json: string, keep: boolean) => {
      shellCounts[cmd] = (shellCounts[cmd] ?? 0) + 1;
      const cb = callbacks.get(cmd);
      if (!cb) return;
      if (!keep) callbacks.delete(cmd);
      cb(parse(json));
    },
    onMidiMessage: (msg: number[], id: string) => {
      if (!msg || !msg.length) return;
      const bytes = Uint8Array.from(msg, (b) => b & 0xff);
      for (const l of midiListeners) l(bytes, id ?? "");
    },
    onSmfMidiMessage: () => {},
    onDeviceStateChange: () => {
      for (const l of deviceListeners) l();
    },
    dispatchEvent: (id: string, arg: unknown) => {
      if (id === "deviceready") k.__ready = arg ?? {};
      events.dispatchEvent(new CustomEvent(id, { detail: arg }));
      // same as the original kwm.js, for anything else listening on document
      g.document?.dispatchEvent(new CustomEvent(id, { detail: arg }));
    },
    donePlayConcertMagic: () => {},
    onPlaySmfJson: () => {},
    __ca79ng: true,
  } satisfies Partial<KwmGlobal>);
  if (prevReady !== undefined) k.__ready = prevReady;
  return k;
}

export function kwm(): KwmGlobal {
  return installKwm();
}

/**
 * Resolves once the shell fired deviceready for this page (or immediately if it
 * already did). Falls back after `timeoutMs` so a shell that never fires it
 * still lets us ask getMidiDevices.
 */
export function deviceReady(timeoutMs = 4000): Promise<{ ready: boolean; detail: unknown }> {
  const k = installKwm();
  if (k.__ready !== undefined) return Promise.resolve({ ready: true, detail: k.__ready });
  return new Promise((resolve) => {
    const t = setTimeout(() => {
      events.removeEventListener("deviceready", on);
      resolve({ ready: false, detail: null });
    }, timeoutMs);
    const on = (e: Event) => {
      clearTimeout(t);
      resolve({ ready: true, detail: (e as CustomEvent).detail });
    };
    events.addEventListener("deviceready", on, { once: true });
  });
}

/** Device list from the shell (also refreshes KWM.devices). */
export async function midiDevices(): Promise<KwmDevice[]> {
  const list = await callPromise<KwmDevice[]>("getMidiDevices", null, 3000);
  const arr = Array.isArray(list) ? list : [];
  kwm().devices = arr;
  return arr;
}

/** Fires when the shell reports a MIDI device added/removed. */
export function onDeviceChange(l: () => void): () => void {
  installKwm();
  deviceListeners.add(l);
  return () => deviceListeners.delete(l);
}

// ---- MIDI link ----------------------------------------------------------------

export class KwmcoreLink extends BaseLink {
  readonly kind = "kwmcore" as const;
  readonly name: string;
  readonly id: string;
  readonly key: string;
  private readonly onMidi: (msg: Uint8Array, id: string) => void;
  private readonly offDevice: () => void;
  private open = true;
  constructor(dev: KwmDevice) {
    super();
    installKwm();
    this.id = dev.id;
    this.key = `kwmcore:${dev.id}`;
    this.name = `${dev.name} (KWMcore${dev.driver ? " " + dev.driver : ""})`;
    this.onMidi = (msg, id) => {
      if (this.open && (id === this.id || this.id === "")) this.emit(msg);
    };
    midiListeners.add(this.onMidi);
    // a removed device ends the link; the original UI reloaded the whole page here
    this.offDevice = onDeviceChange(() => {
      void midiDevices()
        .then((list) => {
          if (this.open && !list.some((d) => d.id === this.id)) this.shutdown();
        })
        .catch(() => {});
    });
  }
  send(msg: Uint8Array): void {
    if (!this.open || msg.length === 0) return;
    const obj: { msg: number[]; id?: string } = { msg: Array.from(msg) };
    if (this.id) obj.id = this.id;
    callNative("sendMidiMessage", obj);
  }
  close(): void {
    this.shutdown();
  }
  private shutdown(): void {
    if (!this.open) return;
    this.open = false;
    midiListeners.delete(this.onMidi);
    this.offDevice();
    this.closed();
  }
}

export async function listKwmcore(): Promise<LinkCandidate[]> {
  if (!kwmcoreAvailable()) return [];
  let devs: KwmDevice[];
  try {
    devs = await midiDevices();
  } catch {
    devs = kwm().devices ?? [];
  }
  return devs.map((d) => ({
    kind: "kwmcore" as const,
    id: d.id,
    key: `kwmcore:${d.id}`,
    name: `${d.name} (KWMcore ${d.driver ?? ""})`.replace(/ \)$/, ")"),
    open: async () => new KwmcoreLink(d),
  }));
}

/**
 * Panel auto-connect, like the original EmbeddedMode boot: wait for deviceready,
 * then take the main board (TYPE_USB, id "USB"; else the only device).
 * Resolves null when no device is there (caller retries on onDeviceChange).
 */
export async function autoLink(readyTimeoutMs = 4000): Promise<KwmcoreLink | null> {
  if (!kwmcoreAvailable()) return null;
  await deviceReady(readyTimeoutMs);
  let devs: KwmDevice[];
  try {
    devs = await midiDevices();
  } catch {
    devs = kwm().devices ?? [];
  }
  const live = devs.filter((d) => !d.suspended);
  const pick = live.find((d) => d.id === "USB") ?? live.find((d) => /usb/i.test(d.driver ?? "")) ?? (live.length === 1 ? live[0] : undefined);
  return pick ? new KwmcoreLink(pick) : null;
}

// ---- panel services the original shell offers (embedMode-only in Java) ---------

/** Shell built as the panel (embed) flavor — `KWMCoreManager.embedMode`. */
export async function embeddedMode(): Promise<boolean> {
  try {
    return (await callPromise<unknown>("getEmbeddedMode", null, 3000)) === true;
  } catch {
    return false;
  }
}

/** LCD backlight via Settings.System screen_brightness; the shell maps 0..100 → 0..255. */
export function setLcdBacklight(percent: number): void {
  callNative("setLcdBacklight", { brightness: Math.max(0, Math.min(100, Math.round(percent))) });
}

/** Adapter for DisplayController (0..255 scale of our own shell). */
export function setBacklight255(level: number): void {
  setLcdBacklight((Math.max(0, Math.min(255, level)) * 100) / 255);
}

export async function lcdBacklight(): Promise<number | null> {
  const o = await callPromise<{ brightness?: string | number }>("getLcdBacklight", null, 3000).catch(() => null);
  const n = Number(o?.brightness);
  return Number.isFinite(n) ? n : null;
}

export function lcdInfo(): Promise<{ width: number; height: number; dpi: number } | null> {
  return callPromise<{ width: number; height: number; dpi: number }>("getLcdInfo", null, 3000).catch(() => null);
}

/** Panel "USB for UPDATE" stick: `/storage/<name>` or "" when none. */
export async function usbPath(): Promise<string> {
  const o = await callPromise<{ path?: string }>("getUsbPath", null, 5000).catch(() => null);
  return typeof o?.path === "string" ? o.path : "";
}

export function usbActivity(): Promise<unknown> {
  return callPromise("getUsbActivity", null, 3000).catch(() => null);
}

export async function jniVersion(): Promise<string | null> {
  const o = await callPromise<{ version?: string }>("getJniVersion", null, 3000).catch(() => null);
  return o?.version ?? null;
}

export interface KwmVersions {
  localUi: string | null;
  usbUi: string | null;
  localApp: string | null;
  usbApp: string | null;
  usbBleMidi: string | null;
}

/**
 * getVersions: in embedMode the shell reads <usb>/version.xxx (UI bundle),
 * <usb>/Apk/version.xxx|txt and <usb>/BleFirm/version.txt, then sets the KWM.*
 * version fields before resolving.
 */
export async function versions(): Promise<KwmVersions> {
  await callPromise("getVersions", null, 15000).catch(() => null);
  const k = kwm();
  const s = (v: string | null | undefined) => (v === undefined || v === null || v === "" ? null : v);
  return { localUi: s(k.localUiVersion), usbUi: s(k.latestUiVersion), localApp: s(k.localAppVersion), usbApp: s(k.latestAppVersion), usbBleMidi: s(k.latestBleMidiVersion) };
}

/**
 * Install the UI bundle on the panel's USB stick (version.xxx + appUI.xxx at the
 * stick root): downloadUiData copies + decrypts + checks MD5 (and on success
 * deletes files/appUI), updateUiData unzips it. No version comparison in Java.
 * Takes about as long as the original 70 MB bundle (≈ a minute).
 */
export async function installUiBundleFromUsb(): Promise<void> {
  await callPromise("downloadUiData", null, 0);
  await callPromise("updateUiData", null, 0);
}

/** Opens android.settings.SETTINGS (the original hidden "OS Settings" button). */
export function launchAndroidSettings(): void {
  callNative("launchAndroidSettingsApp");
}

/** FLAG_KEEP_SCREEN_ON on/off (the shell ignores the value and always keeps it on — `keepScreen`). */
export function keepScreenOn(on: boolean): void {
  callNative("disableAutoLock", { on });
}

/**
 * Read a file anywhere the Kawai app may read, through its `loadJson` (see the 「Android 탐색」 tool): the shell
 * opens `new File(getFilesDir(), name)`, so a `../` name escapes its files folder. Text only — the shell joins lines
 * WITHOUT newlines and decodes UTF-8 (binary comes back mangled); rejects when the file is missing or unreadable.
 * CAUTION: the shell reads `line.charAt(0)` of the first line, so a file whose first line is EMPTY throws inside the
 * shell (likely crashing the panel app) — never point it at such files (e.g. /system/build.prop starts with "\n").
 */
export function readAnyFile(absPath: string): Promise<string> {
  return callPromise<string>("loadJson", { name: "../../../.." + absPath }, 5000).then((v) => (typeof v === "string" ? v : JSON.stringify(v)));
}

/** Write text through the shell's `saveJson` (same `../` escape) — used only for a new dump folder on the USB stick. */
export function writeAnyFile(absPath: string, text: string): Promise<unknown> {
  return callPromise("saveJson", { name: "../../../.." + absPath, json: text }, 5000);
}

export function openUrl(path: string): Promise<unknown> {
  return callPromise("openURL", { path }, 3000);
}

/**
 * DANGEROUS — not wired to any screen. `updateApk {name}` decrypts <usb>/<name>.xxx
 * and hands it to the OEM updater that overwrites /system/priv-app/<name>/<name>.apk
 * (step S5 only, owner decision).
 */
export function updateApkDangerous(name: string): Promise<unknown> {
  return callPromise("updateApk", { name }, 0);
}

/** How many times the shell called `KWM.callbackNative` per command since the page loaded, for the panel screen
 *  report: during a lesson piece `onMetronomeEvent` counts the shell's beat events (the same gate as its clicks). */
export function shellCallbackCounts(): Record<string, number> {
  return { ...shellCounts };
}

/** Test hook: forget queued calls and listeners. */
export function _resetKwmcoreForTests(): void {
  for (const q of pending.values()) for (const w of q) if (w.timer) clearTimeout(w.timer);
  pending.clear();
  queued.clear();
  callbacks.clear();
  for (const k of Object.keys(shellCounts)) delete shellCounts[k];
  midiListeners.clear();
  deviceListeners.clear();
  delete g.KWM;
}
