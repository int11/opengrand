// Android shell bridge. The Kotlin side (android/) exposes `window.ca79ng`:
//   ca79ng.listDevices(): string            JSON [{id,name,driver}]  ("USB-MIDI" / "BLE-MIDI")
//   ca79ng.open(id): boolean
//   ca79ng.send(id, hexString): void
//   ca79ng.close(id): void
//   ca79ng.panel(): string                  JSON {embedded:boolean, lcd?:{w,h,dpi}}
//   ca79ng.setBacklight(level 0..255): void
//   ca79ng.usbPath(): string                "" when no stick
//   ca79ng.scanBle(): void                  5 s BLE-MIDI scan, results via onDevices
//   ca79ng.appInfo(): string                JSON {version, versionCode, flavor}
//   ca79ng.checkUpdate(): string            JSON UpdateCheck (USB <usb>/CA79NG/manifest.json + app.apk)
//   ca79ng.installUpdate(): boolean         verify sha256 + install; status via onUpdate
//   ca79ng.findDfuPackage(): string         JSON {path, version?, source} or {} (BLE module DFU zip)
//   ca79ng.startDfu(path): boolean          scan bootloader + Nordic DFU; progress via onDfu
//   ca79ng.abortDfu(): void
// and calls back into `window.ca79ngNative.onMidi(id, hex)` / `.onDetached(id)` / `.onDevices()`
// / `.onUpdate(json)` / `.onDfu(json)` / `.onLifecycle("background" | "foreground")` (activity
// stopped for a while / back in front: the app lets the piano go and takes it back).
//
// Compared with the original 122-command `kwmcore` bridge
// this is deliberately
// small: playback/recording now live in the firmware, the app only transports MIDI.
import { fromHex, toHex } from "../protocol/sysex.ts";
import { BaseLink, type LinkCandidate, type MidiLink } from "./link.ts";

interface NativeBridge {
  listDevices(): string;
  open(id: string): boolean;
  send(id: string, hex: string): void;
  close(id: string): void;
  panel(): string;
  setBacklight(level: number): void;
  usbPath(): string;
  scanBle(): void;
  // optional: shells built before features 86/87 lack these
  appInfo?(): string;
  checkUpdate?(): string;
  installUpdate?(): boolean;
  findDfuPackage?(): string;
  startDfu?(path: string): boolean;
  abortDfu?(): void;
}

type Listener = (hex: string) => void;
const listeners = new Map<string, Listener>();
const closers = new Map<string, () => void>();
const deviceListeners = new Set<() => void>();
const updateListeners = new Set<(status: Record<string, unknown>) => void>();
const dfuListeners = new Set<(status: Record<string, unknown>) => void>();
const lifecycleListeners = new Set<(state: "background" | "foreground") => void>();

export function bridge(): NativeBridge | null {
  return (globalThis as unknown as { ca79ng?: NativeBridge }).ca79ng ?? null;
}

// Callbacks invoked by Kotlin via evaluateJavascript.
(globalThis as unknown as { ca79ngNative: object }).ca79ngNative = {
  onMidi(id: string, hex: string) {
    listeners.get(id)?.(hex);
  },
  onDetached(id: string) {
    closers.get(id)?.();
  },
  onDevices() {
    for (const l of deviceListeners) l();
  },
  onUpdate(json: string) {
    const st = parseJson(json);
    for (const l of updateListeners) l(st);
  },
  onDfu(json: string) {
    const st = parseJson(json);
    for (const l of dfuListeners) l(st);
  },
  onLifecycle(state: string) {
    if (state === "background" || state === "foreground") for (const l of lifecycleListeners) l(state);
  },
};

/** Shell activity lifecycle (MainActivity onStop + grace / onStart). Returns an unsubscribe function. */
export function onShellLifecycle(l: (state: "background" | "foreground") => void): () => void {
  lifecycleListeners.add(l);
  return () => lifecycleListeners.delete(l);
}

function parseJson(json: string): Record<string, unknown> {
  try {
    return JSON.parse(json) as Record<string, unknown>;
  } catch {
    return { error: "bad status from shell" };
  }
}

export function onUpdateStatus(l: (s: Record<string, unknown>) => void): () => void {
  updateListeners.add(l);
  return () => updateListeners.delete(l);
}

export function onDfuStatus(l: (s: Record<string, unknown>) => void): () => void {
  dfuListeners.add(l);
  return () => dfuListeners.delete(l);
}

export interface ShellInfo {
  version: string;
  versionCode?: number;
  flavor?: string;
}

/** Version of the Android shell (null in a browser or on an old shell). */
export function shellInfo(): ShellInfo | null {
  const nb = bridge();
  if (!nb?.appInfo) return null;
  const o = parseJson(nb.appInfo());
  return typeof o.version === "string" ? (o as unknown as ShellInfo) : null;
}

class AndroidLink extends BaseLink {
  readonly kind = "android" as const;
  readonly name: string;
  readonly key: string;
  private readonly id: string;
  private readonly nb: NativeBridge;
  constructor(nb: NativeBridge, id: string, name: string) {
    super();
    this.nb = nb;
    this.id = id;
    this.name = name;
    this.key = androidKey(id);
    listeners.set(id, (hex) => this.emit(fromHex(hex)));
    closers.set(id, () => this.closed());
  }
  send(msg: Uint8Array): void {
    this.nb.send(this.id, toHex(msg));
  }
  reopen(): Promise<MidiLink> {
    return openAndroid(this.nb, this.id, this.name);
  }
  close(): void {
    this.nb.close(this.id);
    listeners.delete(this.id);
    closers.delete(this.id);
    this.closed();
  }
}

export function androidAvailable(): boolean {
  return bridge() !== null;
}

export function listAndroid(): LinkCandidate[] {
  const nb = bridge();
  if (!nb) return [];
  const devices = JSON.parse(nb.listDevices()) as { id: string; name: string; driver: string }[];
  return devices.map((d) => ({
    kind: "android" as const,
    id: d.id,
    name: `${d.name} (${d.driver})`,
    key: androidKey(d.id),
    open: () => openAndroid(nb, d.id, d.name),
  }));
}

export const androidKey = (id: string): string => `android:${id}`;

async function openAndroid(nb: NativeBridge, id: string, name: string): Promise<MidiLink> {
  if (!nb.open(id)) throw new Error(`cannot open ${name}`);
  return new AndroidLink(nb, id, name);
}

/** A remembered shell device (key `android:<id>`), opened without a scan: BLE ids are MAC addresses. */
export function androidCandidate(key: string, name: string): LinkCandidate | null {
  const nb = bridge();
  if (!nb || !key.startsWith("android:")) return null;
  const id = key.slice("android:".length);
  return { kind: "android", id, name, key, open: () => openAndroid(nb, id, name) };
}

export interface PanelInfo {
  embedded: boolean;
  lcd?: { w: number; h: number; dpi: number };
}

/** Whether we run on the instrument's built-in panel. The original app never
 * detected this itself (the UI set it explicitly); our shell reports it. */
export function panelInfo(): PanelInfo {
  const nb = bridge();
  if (!nb) return { embedded: false };
  try {
    return JSON.parse(nb.panel()) as PanelInfo;
  } catch {
    return { embedded: false };
  }
}

export function setBacklight(level: number): void {
  bridge()?.setBacklight(Math.max(0, Math.min(255, Math.round(level))));
}

/** Start a BLE-MIDI scan in the shell; `onChange` fires as devices appear. */
export function scanBle(onChange: () => void): () => void {
  deviceListeners.add(onChange);
  bridge()?.scanBle();
  return () => deviceListeners.delete(onChange);
}
