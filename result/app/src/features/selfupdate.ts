// Online self-update, page side. The shells do the work (Android phone: SelfUpdate.kt; Windows desktop:
// result/desktop/src/selfupdate.cjs): fetch the latest GitHub release, check the Ed25519 signature of its update.json,
// download, check size + sha256, and install (Android: the system installer, the user confirms) or stage and swap
// (desktop: on restart / quit). This side only decides WHEN to check — the on/off setting (default on), shortly after
// start, every 24 h while running, only while online — and keeps the latest state for the banner and the Info screen.
// A browser, the built-in panel and shells built before this feature have no updater: nothing is shown.
import { bridge, onSelfUpdateStatus } from "../transport/android.ts";
import { desktopShell } from "../transport/desktop.ts";
import { readJson, safeStorage, writeJson } from "../state/settings.ts";

export type SelfUpdatePhase = "idle" | "checking" | "uptodate" | "downloading" | "ready" | "installing" | "error";

export interface SelfUpdateState {
  phase: SelfUpdatePhase;
  /** installed version */
  current?: string;
  /** newer version offered by the latest release */
  available?: string;
  /** 0..100 while downloading */
  progress?: number;
  /** offline | ratelimit | norelease | http | manifest | signature | sha256 | size | apk | extract | write | install | error */
  error?: string;
  message?: string;
  /** what the ready button does: Android "install" (system installer), desktop "restart" (swap + relaunch),
   *  "manual" (portable / dev build: open the release page) */
  action?: "install" | "restart" | "manual";
  /** desktop: why the update is manual (portable, dev, platform, root) */
  reason?: string;
  page?: string;
  checkedAt?: number;
  /** desktop automation run: no checks on its own */
  noAuto?: boolean;
  /** desktop: what the swap helper reported after the last restart */
  lastResult?: { state: "ok" | "failed"; error: string };
}

export interface SelfUpdater {
  kind: "android" | "desktop";
  state(): Promise<SelfUpdateState | null>;
  check(): Promise<void>;
  apply(): Promise<boolean>;
  openPage(): void;
  onChange(cb: (s: SelfUpdateState) => void): () => void;
}

type Global = Record<string, unknown>;

function parse(json: string): SelfUpdateState | null {
  try {
    const o = JSON.parse(json) as SelfUpdateState;
    return o && typeof o.phase === "string" ? o : null;
  } catch {
    return null;
  }
}

/** The shell's updater, or null (browser, panel, older shell). */
export function selfUpdater(g: Global = globalThis as unknown as Global): SelfUpdater | null {
  const d = desktopShell(g)?.update;
  if (d) {
    return {
      kind: "desktop",
      state: async () => (await d.state()) ?? null,
      check: async () => void (await d.check()),
      apply: () => d.apply(),
      openPage: () => void d.openPage(),
      onChange: (cb) => d.onChange(cb),
    };
  }
  const nb = g === (globalThis as unknown as Global) ? bridge() : ((g.ca79ng as ReturnType<typeof bridge>) ?? null);
  if (nb?.selfUpdateState && nb.selfUpdateCheck && nb.selfUpdateApply) {
    const first = nb.selfUpdateState();
    if (!first) return null; // the panel build: has the methods, no updater
    return {
      kind: "android",
      state: async () => parse(nb.selfUpdateState!()),
      check: async () => void nb.selfUpdateCheck!(),
      apply: async () => nb.selfUpdateApply!(),
      openPage: () => {}, // never "manual" on Android: the APK always goes to the system installer
      onChange: (cb) => onSelfUpdateStatus((o) => cb(o as unknown as SelfUpdateState)),
    };
  }
  return null;
}

// ---------------------------------------------------------------- setting + schedule

export const AUTO_UPDATE_KEY = "ca79ng.selfupdate";
export const START_DELAY_MS = 20_000;
export const CHECK_EVERY_MS = 24 * 60 * 60 * 1000;
export const TICK_MS = 60 * 60 * 1000;

export interface AutoUpdatePrefs {
  enabled: boolean;
  /** ms epoch of the last check this page started */
  lastCheck: number;
}

export function loadAutoUpdatePrefs(storage: Storage | null = safeStorage()): AutoUpdatePrefs {
  const p = readJson<Partial<AutoUpdatePrefs>>(AUTO_UPDATE_KEY, {}, storage);
  return { enabled: p.enabled !== false, lastCheck: typeof p.lastCheck === "number" ? p.lastCheck : 0 };
}

function savePrefs(p: AutoUpdatePrefs, storage: Storage | null): void {
  writeJson(AUTO_UPDATE_KEY, p, storage);
}

/** Problems after which the next check waits for the next tick / the network instead of being retried at once. */
export const QUIET_ERRORS = new Set(["offline", "ratelimit", "norelease"]);

/**
 * Whether a scheduled check is due now. `startup`: the one check shortly after start (any time since the last one);
 * otherwise 24 h after the last check.
 */
export function checkDue(o: { enabled: boolean; online: boolean; noAuto?: boolean; busy: boolean; ready: boolean; now: number; lastCheck: number; startup: boolean }): boolean {
  if (!o.enabled || !o.online || o.noAuto || o.busy || o.ready) return false;
  return o.startup || o.now - o.lastCheck >= CHECK_EVERY_MS;
}

export interface SelfUpdateHub {
  readonly updater: SelfUpdater;
  /** latest known state (null until the shell answered) */
  state: SelfUpdateState | null;
  prefs(): AutoUpdatePrefs;
  setEnabled(on: boolean): void;
  /** 「지금 확인」 (also when the setting is off) */
  checkNow(): Promise<void>;
  apply(): Promise<boolean>;
  onChange(cb: (s: SelfUpdateState | null) => void): () => void;
  stop(): void;
}

export interface HubDeps {
  storage?: Storage | null;
  now?: () => number;
  online?: () => boolean;
  setTimeout?: (f: () => void, ms: number) => unknown;
  setInterval?: (f: () => void, ms: number) => unknown;
  clearTimeout?: (h: unknown) => void;
  clearInterval?: (h: unknown) => void;
  /** window: "online" events retry a check that found no network */
  events?: { addEventListener(t: string, f: () => void): void; removeEventListener(t: string, f: () => void): void } | null;
}

/** Start the schedule for `updater` and keep its state. */
export function startSelfUpdate(updater: SelfUpdater, deps: HubDeps = {}): SelfUpdateHub {
  const storage = deps.storage === undefined ? safeStorage() : deps.storage;
  const now = deps.now ?? (() => Date.now());
  const online = deps.online ?? (() => (globalThis as { navigator?: { onLine?: boolean } }).navigator?.onLine !== false);
  const sT = deps.setTimeout ?? ((f, ms) => setTimeout(f, ms));
  const sI = deps.setInterval ?? ((f, ms) => setInterval(f, ms));
  const cT = deps.clearTimeout ?? ((h) => clearTimeout(h as ReturnType<typeof setTimeout>));
  const cI = deps.clearInterval ?? ((h) => clearInterval(h as ReturnType<typeof setInterval>));
  const events = deps.events === undefined ? ((globalThis as unknown as { addEventListener?: unknown }).addEventListener ? (globalThis as unknown as HubDeps["events"]) : null) : deps.events;
  const listeners = new Set<(s: SelfUpdateState | null) => void>();
  let prefs = loadAutoUpdatePrefs(storage);

  const hub: SelfUpdateHub = {
    updater,
    state: null,
    prefs: () => prefs,
    setEnabled(on) {
      prefs = { ...prefs, enabled: on };
      savePrefs(prefs, storage);
      if (on) maybe(false);
    },
    async checkNow() {
      prefs = { ...prefs, lastCheck: now() };
      savePrefs(prefs, storage);
      await updater.check();
      await refresh();
    },
    apply: () => updater.apply(),
    onChange(cb) {
      listeners.add(cb);
      return () => listeners.delete(cb);
    },
    stop() {
      cT(startTimer);
      cI(tick);
      off();
      events?.removeEventListener("online", onOnline);
    },
  };
  const emit = (s: SelfUpdateState | null) => {
    hub.state = s;
    for (const l of [...listeners]) l(s);
  };
  const refresh = async () => emit(await updater.state());
  const busy = () => hub.state?.phase === "checking" || hub.state?.phase === "downloading";
  const ready = () => hub.state?.phase === "ready";

  function maybe(startup: boolean): void {
    if (!checkDue({ enabled: prefs.enabled, online: online(), noAuto: hub.state?.noAuto, busy: busy(), ready: ready(), now: now(), lastCheck: prefs.lastCheck, startup })) return;
    void hub.checkNow();
  }
  const onOnline = () => {
    if (hub.state?.phase === "error" && hub.state.error === "offline") maybe(true);
  };

  const off = updater.onChange((s) => emit(s));
  void refresh(); // a staged update from an earlier run shows as ready at once
  const startTimer = sT(() => maybe(true), START_DELAY_MS);
  const tick = sI(() => maybe(false), TICK_MS);
  events?.addEventListener("online", onOnline);
  return hub;
}

// ---------------------------------------------------------------- one hub per page

let shared: SelfUpdateHub | null | undefined;

/** The page's hub (started on first use), or null without an updater. */
export function selfUpdateHub(): SelfUpdateHub | null {
  if (shared === undefined) {
    const u = selfUpdater();
    shared = u ? startSelfUpdate(u) : null;
  }
  return shared;
}
