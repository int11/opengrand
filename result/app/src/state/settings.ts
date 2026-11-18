// App-local settings and user data (features 69, 71-73, 75, 77, 79).
// These live on the controller, not on the piano, so they are kept in
// localStorage. Storage can be missing or throw (private mode, blocked site
// data, old WebViews with DOM storage disabled): every access is guarded and
// the app falls back to defaults in memory.
import type { Lang } from "../i18n.ts";

export function safeStorage(): Storage | null {
  try {
    const s = (globalThis as { localStorage?: Storage }).localStorage;
    return s ?? null;
  } catch {
    return null;
  }
}

export function readJson<T>(key: string, fallback: T, storage: Storage | null = safeStorage()): T {
  try {
    const raw = storage?.getItem(key);
    return raw ? (JSON.parse(raw) as T) : fallback;
  } catch {
    return fallback;
  }
}

export function writeJson(key: string, value: unknown, storage: Storage | null = safeStorage()): boolean {
  try {
    if (!storage) return false;
    storage.setItem(key, JSON.stringify(value));
    return true;
  } catch {
    return false;
  }
}

// ---------------- System settings ----------------

export const TAB_IDS = ["sound", "music", "metronome", "technician", "chords", "files", "bluetooth", "settings", "info"] as const;
export type TabId = (typeof TAB_IDS)[number];

export interface AppSettings {
  lang: Lang;
  /** LCD brightness 1..10 (mapped to backlight 0..255 by the shell) */
  brightness: number;
  /** display scale in percent */
  scale: number;
  /** minutes without touch before the LCD is dimmed; 0 = never */
  autoOffMin: number;
  /** tab shown at start-up */
  startTab: TabId;
  /** wake the piano from OpenGrand standby when this app connects (app only, not the built-in panel) */
  autoWake: boolean;
}

export const SCALES = [80, 90, 100, 110, 125, 150];
/** The original panel's Auto Display Off choices (app.js displayOff.time: Never, 30 s, 1–5 min);
 * the index is the `displayOff` value in the panel's user/uicache.json (state/panelcache.ts). */
export const AUTO_OFF_MINUTES = [0, 0.5, 1, 2, 3, 4, 5];
export const DEFAULT_SETTINGS: AppSettings = { lang: "en", brightness: 8, scale: 100, autoOffMin: 0, startTab: "sound", autoWake: true };
const SETTINGS_KEY = "ca79ng.settings";

export function guessLang(): Lang {
  // The built-in panel (original Kawai shell, `kwmcore` bridge) starts in Korean — its WebView locale is
  // not the user's and the stock UI has no Korean (user decision 2026-11-01); a choice made in the app or
  // the chooser is stored and wins from then on.
  if (typeof (globalThis as { kwmcore?: unknown }).kwmcore === "object") return "ko";
  const n = (globalThis as { navigator?: { language?: string } }).navigator?.language ?? "en";
  return n.startsWith("ja") ? "ja" : n.startsWith("ko") ? "ko" : "en";
}

/** Load settings, repairing anything out of range (storage is user-writable). */
export function loadSettings(storage: Storage | null = safeStorage()): AppSettings {
  const s = { ...DEFAULT_SETTINGS, lang: guessLang(), ...readJson<Partial<AppSettings>>(SETTINGS_KEY, {}, storage) };
  if (!["en", "ja", "ko"].includes(s.lang)) s.lang = "en";
  s.brightness = clampInt(s.brightness, 1, 10, DEFAULT_SETTINGS.brightness);
  if (!SCALES.includes(s.scale)) s.scale = 100;
  if (!AUTO_OFF_MINUTES.includes(s.autoOffMin)) s.autoOffMin = 0;
  if (!(TAB_IDS as readonly string[]).includes(s.startTab)) s.startTab = "sound"; // "piano": the Piano tab is in the Sound tab since 2026-11-09
  if (typeof s.autoWake !== "boolean") s.autoWake = true;
  return s;
}

export function saveSettings(s: AppSettings, storage: Storage | null = safeStorage()): boolean {
  return writeJson(SETTINGS_KEY, s, storage);
}

function clampInt(v: unknown, lo: number, hi: number, dflt: number): number {
  const n = typeof v === "number" && Number.isFinite(v) ? Math.round(v) : dflt;
  return Math.max(lo, Math.min(hi, n));
}

// ---------------- Favorites / recent / user sounds ----------------

export interface SoundRef {
  msb: number;
  lsb: number;
  pc: number;
  name: string;
}

/** One saved parameter value (by name, so a regenerated table keeps it). */
export interface SavedParam {
  name: string;
  part: number;
  data: number[];
}

/**
 * A user sound ("Save this sound"): keyboard mode, the Main/Layer/Lower sounds and, since
 * 2026-11-03, the Main1 effect / VT values and tuning (`params`; older entries have none).
 */
export interface UserSound {
  name: string;
  keyboardMode: number;
  parts: { part: number; sound: SoundRef }[];
  params?: SavedParam[];
}

export const RECENT_MAX = 20;
const LIB_KEY = "ca79ng.library";

interface LibraryData {
  favorites: SoundRef[];
  recent: SoundRef[];
  user: UserSound[];
  /** the "User" Virtual Technician preset: Main1 VT values that matched no preset */
  userVt: SavedParam[] | null;
}

const same = (a: SoundRef, b: SoundRef) => a.msb === b.msb && a.lsb === b.lsb && a.pc === b.pc;

export class SoundLibrary {
  private data: LibraryData;
  private readonly storage: Storage | null;
  private listeners = new Set<() => void>();

  constructor(storage: Storage | null = safeStorage()) {
    this.storage = storage;
    const d = readJson<Partial<LibraryData>>(LIB_KEY, {}, storage);
    this.data = {
      favorites: Array.isArray(d.favorites) ? d.favorites.filter(isRef) : [],
      recent: Array.isArray(d.recent) ? d.recent.filter(isRef).slice(0, RECENT_MAX) : [],
      user: Array.isArray(d.user)
        ? d.user.filter((u) => u && typeof u.name === "string" && Array.isArray(u.parts)).map((u) => ({ ...u, params: Array.isArray(u.params) ? u.params.filter(isSaved) : undefined }))
        : [],
      userVt: Array.isArray(d.userVt) ? d.userVt.filter(isSaved) : null,
    };
  }

  get favorites(): readonly SoundRef[] {
    return this.data.favorites;
  }
  get recent(): readonly SoundRef[] {
    return this.data.recent;
  }
  get user(): readonly UserSound[] {
    return this.data.user;
  }

  get userVt(): readonly SavedParam[] | null {
    return this.data.userVt;
  }

  setUserVt(v: SavedParam[] | null): void {
    this.data.userVt = v;
    this.commit();
  }

  onChange(l: () => void): () => void {
    this.listeners.add(l);
    return () => this.listeners.delete(l);
  }

  isFavorite(s: SoundRef): boolean {
    return this.data.favorites.some((f) => same(f, s));
  }

  toggleFavorite(s: SoundRef): boolean {
    const on = !this.isFavorite(s);
    this.data.favorites = on ? [...this.data.favorites, pick(s)] : this.data.favorites.filter((f) => !same(f, s));
    this.commit();
    return on;
  }

  /** Most recent first, no duplicates, at most RECENT_MAX entries. */
  pushRecent(s: SoundRef): void {
    this.data.recent = [pick(s), ...this.data.recent.filter((r) => !same(r, s))].slice(0, RECENT_MAX);
    this.commit();
  }

  saveUser(u: UserSound): void {
    this.data.user = [...this.data.user.filter((x) => x.name !== u.name), u];
    this.commit();
  }

  removeUser(name: string): void {
    this.data.user = this.data.user.filter((x) => x.name !== name);
    this.commit();
  }

  clearFavorites(): void {
    this.data.favorites = [];
    this.commit();
  }
  clearRecent(): void {
    this.data.recent = [];
    this.commit();
  }
  clearUser(): void {
    this.data.user = [];
    this.commit();
  }

  private commit(): void {
    writeJson(LIB_KEY, this.data, this.storage);
    for (const l of this.listeners) l();
  }
}

function pick(s: SoundRef): SoundRef {
  return { msb: s.msb, lsb: s.lsb, pc: s.pc, name: s.name };
}

function isRef(x: unknown): x is SoundRef {
  const r = x as SoundRef;
  return !!r && [r.msb, r.lsb, r.pc].every((v) => Number.isInteger(v) && v >= 0 && v <= 127) && typeof r.name === "string";
}

function isSaved(x: unknown): x is SavedParam {
  const r = x as SavedParam;
  return !!r && typeof r.name === "string" && Number.isInteger(r.part) && Array.isArray(r.data) && r.data.every((v) => Number.isInteger(v) && v >= 0 && v <= 127);
}
