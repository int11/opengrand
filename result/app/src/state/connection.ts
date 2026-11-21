// Connection state machine (queue conn).
//
// One place decides what the app's connection is doing; the header, the Connect button and the
// takeover banner draw only `connView(state)`. The same machine runs on every transport (Web
// Bluetooth / Web MIDI in Chrome, the Android shell, the panel's KWMcore, the simulator socket):
// transports only report "link closed", the session reports CONNECT reply / dump end / 7E 01 00.
//
// Field symptoms it replaces (2026-11-01): after "Take control again" the header stayed on
// "Connecting to CA79…" (the status line was set only by the first connect); "Connect" while
// connected opened a second link + session on the same piano and the old session's DISCONNECT made
// the stock hand control to the panel, which displaced the new session ("another device took
// control" — the app had displaced itself); every button worked in every state.
//
// Pure part: `reduce(state, event) -> { state, effects }`. `ConnectionManager` runs the effects
// (open links, start / release sessions, timers) and turns session / link callbacks into events.
// Every session event carries the generation of the session that raised it: an event of an older
// session (released, replaced) is dropped, so cleaning up our own old session can never look like
// another device taking over.
//
// Boot notice (was the A3 boot re-grab until 2026-11-21): the panel CONNECTs late in its own start-up,
// so an app that reaches a freshly powered piano first is displaced (7E 01 00) seconds later without
// anyone touching the panel — on a firmware with one binding (the stock, the loader without a main). With the
// session core (ota_main) the panel's CONNECT displaces nobody and none of this happens. A session opens a window
// (`bootNotice.windowMs`); a displacement inside it is shown as "the panel took control as it started up" with
// "Use this app" — nothing automatic. Field 2026-11-18 (escape boot, stock): the old rule took control back once,
// 8 s later, and the panel took it again at once — its start-up keeps CONNECTing until one of its sessions
// connects (main.ts tryLink: a session that hears nothing fails, the next try comes 3 s later), and the app
// cannot see when that ends (the stock tells an unbound port nothing) — so any fixed delay can land in a gap and
// the app was left with "Take control again" on a piano just switched on. Off in the panel builds.
//
// Auto-reconnect (desktop app, field 2026-11-03): the piano was power-cycled, the link closed (lost) and
// nothing reopened it — lost waited for a click, and the header 「연결」 opened the device list instead of
// the piano just lost. With `autoReconnect` on, a lost link is reopened (the same BluetoothDevice / remembered
// endpoint) after a backoff, again and again until it is back or the user disconnects; an attempt that fails
// goes back to lost and waits for the next one. The header button / banner button in lost reopen at once.
// Second field case (2026-11-03): Windows dropped and re-made the CA79 BLE connection; the held BluetoothDevice
// never reconnected, an app restart (a new device lookup) did. With `reacquire` (desktop app) the reopens
// alternate between the held link and the endpoint looked up afresh (ManagerOptions.reacquire).
//
// Re-bind (field 2026-11-09): after an OTA "apply now" the new payload's session core starts with an empty binding
// table — the screen said "Connected", every bound-only op answered "not bound" (`1D 0A 02` -> `5D 0A 02 …`) and
// nothing CONNECTed again. The session reports the signs (apply done, Identify changed, a "not bound" answer —
// PianoSession.onRebind); ready / waking answers with CONNECT + full re-sync on the same session (`rebind`), once:
// a "not bound" answer after an automatic re-bind of the same session is left alone (no CONNECT loop), apply /
// Identify change always re-bind (each is one swap). "Reconnect" (connected menu / any retry while connected)
// always does a real CONNECT handshake — before, re-picking the same piano sent nothing.
import type { PianoSession, RebindReason } from "../protocol/session.ts";
import type { MidiLink } from "../transport/link.ts";
import { t } from "../i18n.ts";

export type Phase =
  | "idle" // not connected (suspended: the app let the piano go while in the background)
  | "connecting" // CONNECT + identity queries
  | "syncing" // full parameter dump
  | "ready"
  | "waking" // ready, waking the piano from OpenGrand standby (open-to-wake)
  | "displaced" // 7E 01 00: another device (panel / another app) holds the binding; link fine
  | "retaking" // "Take control again": CONNECT + dump on the same session
  | "lost" // the link closed under us (BLE out of range, piano off, cable)
  | "failed"; // connect / retake gave up or timed out; link kept for "Try again"

export interface ConnState {
  phase: Phase;
  /** bumped on every phase change (and a watchdog re-arm): a timeout for an older seq is stale */
  seq: number;
  /** generation of the session the phase belongs to */
  gen: number;
  /** endpoint of the current link (MidiLink.key), null when none */
  key: string | null;
  link: string;
  model: string;
  version: string;
  error: string;
  /** idle because the app went to the background / was closed: resume reconnects */
  suspended: boolean;
  /** boot notice enabled (manager option; constant for the manager's life) */
  bootNotice: boolean;
  /** the boot window of the current session is open and unused: a displacement now is the panel starting up */
  grace: boolean;
  /** displaced inside the boot window: the panel took control as it started up (shown as such; nothing automatic) */
  bootTook: boolean;
  /** auto-reconnect enabled (manager option; constant for the manager's life) */
  autoReconnect: boolean;
  /** the session under way reopens a lost link: a failure goes back to lost (and the next attempt), not failed */
  reopening: boolean;
  /** reopen attempts since the link was lost (picks the backoff delay); 0 after a sync */
  reconnects: number;
  /** the session already re-bound once on its own (apply / Identify change / "not bound"): another "not bound" is left alone */
  rebound: boolean;
  /** retaking because this app re-binds (automatic re-bind, "Reconnect" while connected), not after a displacement */
  rejoin: boolean;
}

export const INITIAL: ConnState = { phase: "idle", seq: 0, gen: 0, key: null, link: "", model: "", version: "", error: "", suspended: false, bootNotice: false, grace: false, bootTook: false, autoReconnect: false, reopening: false, reconnects: 0, rebound: false, rejoin: false };

export type ConnEvent =
  | { type: "connectButton" } // header button
  | { type: "pick"; key: string; link: string } // a link was chosen (user picker, auto-link, remembered device)
  | { type: "disconnect" } // user "Disconnect"
  | { type: "retake" } // banner "Take control again"
  | { type: "retry" } // banner "Try again" / "Reconnect", connected menu "Reconnect"
  | { type: "suspend" } // app hidden for a while / page closed / shell stopped
  | { type: "resume" } // app back in front
  | { type: "identified"; gen: number } // CONNECT + model answered (session -> syncing)
  | { type: "synced"; gen: number; model: string; version: string } // dump end (session -> ready)
  | { type: "displaced"; gen: number } // 7E 01 00
  | { type: "invited"; gen: number } // 7E 01 01 from the piano: CONNECT again + re-sync
  | { type: "rebind"; gen: number; why: RebindReason } // the piano may have dropped our binding (PianoSession.onRebind)
  | { type: "closed"; gen: number } // link closed
  | { type: "failed"; gen: number; error: string } // connect() / takeControl() / open rejected
  | { type: "wakeStart"; gen: number }
  | { type: "wakeDone"; gen: number }
  | { type: "graceEnd"; gen: number } // the boot window of session `gen` closed
  | { type: "reconnectTick"; seq: number } // the auto-reconnect delay of the lost phase entered at `seq` ran out
  | { type: "timeout"; seq: number }; // watchdog of the phase entered at `seq`

export type Effect =
  | { type: "picker" } // show the device list
  | { type: "menu" } // connected: "Disconnect / Other device"
  | { type: "start"; gen: number; source: "choice" | "current" | "reopen" } // new session; link = chosen / current / reopened
  | { type: "retake" }
  | { type: "disconnect" } // DISCONNECT (7E 01 00) + close the link
  | { type: "release" } // retire the session silently, keep the link
  | { type: "grace"; gen: number } // open the boot window of session `gen`
  | { type: "bootTook" } // trace only: displaced inside the boot window
  | { type: "reconnectArm"; seq: number; attempt: number } // schedule auto-reconnect attempt `attempt` (0-based) of the lost phase `seq`
  | { type: "arm"; phase: Phase; seq: number }
  | { type: "disarm" };

/** Phases with a watchdog: they must end in ready / displaced / failed, never stay. */
export const TIMED: readonly Phase[] = ["connecting", "syncing", "retaking", "waking"];
/** Phases that hold a live session on a link. */
const LIVE: readonly Phase[] = ["connecting", "syncing", "ready", "waking", "displaced", "retaking"];
/** Busy phases: the Connect button does nothing. */
const BUSY: readonly Phase[] = ["connecting", "syncing", "retaking"];

const has = (list: readonly Phase[], p: Phase) => list.includes(p);

function go(s: ConnState, phase: Phase, patch: Partial<ConnState> = {}): ConnState {
  // the boot notice belongs to the displaced phase only (a retake, a lost link … end it)
  return { ...s, bootTook: false, rejoin: false, ...patch, phase, seq: s.seq + 1 };
}

/** The transition table. Unknown / stale events leave the state as it is, with no effects. */
export function reduce(s: ConnState, ev: ConnEvent): { state: ConnState; effects: Effect[] } {
  const r = step(s, ev);
  const effects = [...r.effects];
  if (r.state.seq !== s.seq) effects.push(has(TIMED, r.state.phase) ? { type: "arm", phase: r.state.phase, seq: r.state.seq } : { type: "disarm" });
  return { state: r.state, effects };
}

function step(s: ConnState, ev: ConnEvent): { state: ConnState; effects: Effect[] } {
  const same = { state: s, effects: [] as Effect[] };
  if ("gen" in ev && ev.gen !== s.gen) return same; // an older session: never ours to show
  const start = (source: "choice" | "current" | "reopen", patch: Partial<ConnState> = {}) => {
    const gen = s.gen + 1;
    const effects: Effect[] = [{ type: "start", gen, source }];
    if (s.bootNotice) effects.push({ type: "grace", gen });
    return { state: go(s, "connecting", { gen, error: "", suspended: false, grace: s.bootNotice, reopening: false, reconnects: 0, rebound: false, ...patch }), effects };
  };
  // reopen the link just lost (banner / header button / auto-reconnect): with auto-reconnect a failure comes back to lost
  const reopen = (auto: boolean) => start("reopen", { reopening: s.autoReconnect, reconnects: s.reconnects + (auto ? 1 : 0) });
  // the link is gone: lost, and with auto-reconnect the next attempt scheduled
  const lose = (patch: Partial<ConnState>, effects: Effect[] = []) => {
    const state = go(s, "lost", { reopening: false, ...patch });
    if (s.autoReconnect && state.key) effects.push({ type: "reconnectArm", seq: state.seq, attempt: state.reconnects });
    return { state, effects };
  };
  switch (ev.type) {
    case "connectButton":
      // lost: the piano just lost first (field 2026-11-03: the button opened the list); a failed reopen shows the list next time
      if (s.phase === "lost") return reopen(false);
      if (s.phase === "idle" || s.phase === "failed") return { state: s, effects: [{ type: "picker" }] };
      if (s.phase === "ready" || s.phase === "waking" || s.phase === "displaced") return { state: s, effects: [{ type: "menu" }] };
      return same; // connecting / syncing / retaking: wait
    case "pick":
      if (ev.key === s.key && has(LIVE, s.phase)) {
        // the piano this app is already on: no second link, no second session
        if (s.phase === "displaced") return { state: go(s, "retaking"), effects: [{ type: "retake" }] };
        return same;
      }
      if (ev.key === s.key && s.phase === "failed") return start("current");
      return start("choice", { key: ev.key, link: ev.link, model: "", version: "" });
    case "disconnect":
      if (s.phase === "idle" && !s.suspended) return same;
      return { state: go(s, "idle", { gen: s.gen + 1, key: null, link: "", model: "", version: "", error: "", suspended: false, grace: false }), effects: [{ type: "disconnect" }] };
    case "suspend":
      if (!has(LIVE, s.phase) && s.phase !== "failed") return same;
      return { state: go(s, "idle", { gen: s.gen + 1, error: "", suspended: true, grace: false }), effects: [{ type: "disconnect" }] };
    case "resume":
      return s.phase === "idle" && s.suspended && s.key ? start("reopen") : same;
    case "retake":
      return s.phase === "displaced" ? { state: go(s, "retaking"), effects: [{ type: "retake" }] } : same;
    case "retry":
      if (s.phase === "failed") return start("current");
      if (s.phase === "lost") return reopen(false);
      if (s.phase === "idle" && s.suspended && s.key) return start("reopen");
      if (s.phase === "idle") return { state: s, effects: [{ type: "picker" }] };
      // connected: a real CONNECT handshake + re-sync on the same link / session (field 2026-11-09: it sent nothing)
      if (s.phase === "ready" || s.phase === "waking") return { state: go(s, "retaking", { rejoin: true, rebound: false }), effects: [{ type: "retake" }] };
      if (s.phase === "displaced") return { state: go(s, "retaking"), effects: [{ type: "retake" }] };
      return same;
    case "rebind":
      // connecting / syncing / retaking: the CONNECT under way answers it; displaced: another device holds control
      if (s.phase !== "ready" && s.phase !== "waking") return same;
      if (ev.why === "notBound" && s.rebound) return same; // once: no CONNECT loop if CONNECT does not bind
      return { state: go(s, "retaking", { rejoin: true, rebound: true }), effects: [{ type: "retake" }] };
    case "identified":
      if (s.phase === "connecting") return { state: go(s, "syncing"), effects: [] };
      if (s.phase === "retaking") return { state: go(s, "retaking", { rejoin: s.rejoin }), effects: [] }; // re-arm with the dump budget
      return same;
    case "synced":
      if (s.phase === "connecting" || s.phase === "syncing" || s.phase === "retaking") return { state: go(s, "ready", { model: ev.model, version: ev.version, error: "", reopening: false, reconnects: 0 }), effects: [] };
      return same;
    case "displaced":
      if (!has(LIVE, s.phase) || s.phase === "displaced") return same;
      // inside the boot window: the panel's start-up CONNECT — say so, take nothing back (field 2026-11-18)
      // the link is up (someone answered): a later failure is not a reopen any more
      if (s.grace) return { state: go(s, "displaced", { grace: false, bootTook: true, reopening: false, reconnects: 0 }), effects: [{ type: "bootTook" }] };
      return { state: go(s, "displaced", { reopening: false, reconnects: 0 }), effects: [] };
    case "invited":
      // the piano asks the controller to CONNECT again (stock: to the panel when the bound controller
      // left; OpenGrand: after a live payload swap) - as the original panel app: CONNECT + full request.
      // A CONNECT already under way (connecting / syncing / retaking) answers it by itself.
      if (s.phase === "ready" || s.phase === "waking" || s.phase === "displaced") return { state: go(s, "retaking"), effects: [{ type: "retake" }] };
      return same;
    case "graceEnd":
      return s.grace ? { state: { ...s, grace: false }, effects: [] } : same;
    case "reconnectTick":
      return s.phase === "lost" && ev.seq === s.seq ? reopen(true) : same;
    case "closed":
      return has(LIVE, s.phase) || s.phase === "failed" ? lose({}) : same;
    case "failed":
      if (!has(BUSY, s.phase)) return same;
      if (s.reopening) return lose({ error: ev.error }, [{ type: "release" }]);
      return { state: go(s, "failed", { error: ev.error }), effects: [{ type: "release" }] };
    case "wakeStart":
      return s.phase === "ready" ? { state: go(s, "waking"), effects: [] } : same;
    case "wakeDone":
      return s.phase === "waking" ? { state: go(s, "ready"), effects: [] } : same;
    case "timeout":
      if (ev.seq !== s.seq) return same;
      if (s.phase === "waking") return { state: go(s, "ready"), effects: [] };
      if (has(BUSY, s.phase) && s.reopening) return lose({ error: "timeout" }, [{ type: "release" }]);
      if (has(BUSY, s.phase)) return { state: go(s, "failed", { error: "timeout" }), effects: [{ type: "release" }] };
      return same;
  }
}

/** error of a retry with nothing to reopen: the view asks for the piano again and the picker opens */
export const NO_LINK = "nolink";

// ---------------- view ----------------

type Text = [key: string, vars?: Record<string, string | number>];
export interface ConnView {
  status: Text;
  /** header button: "pick" opens the device list, "menu" the connected menu, "none" disabled */
  button: { label: string; action: "pick" | "menu" | "none" };
  banner: { msg: Text; button: { label: string; action: "retake" | "retry"; disabled: boolean } | null } | null;
}

const BOOT_TOOK = "The piano's panel took control as it started up — this firmware lets one device control the piano at a time.";
const LOST_RETRYING = "The connection to {name} was lost — reconnecting automatically…";

export function connView(s: ConnState): ConnView {
  const id: Text = ["{model}{version} · {link}", { model: s.model, version: s.version ? " v" + s.version : "", link: s.link }];
  const err = s.error === "timeout" ? "no answer" : s.error === NO_LINK ? t("choose the piano again") : s.error;
  switch (s.phase) {
    case "idle":
      return { status: [s.suspended ? "Paused while the app is in the background" : "Not connected"], button: { label: "Connect", action: "pick" }, banner: null };
    case "connecting":
      // an auto-reconnect attempt keeps the lost banner (no flicker between attempts)
      return { status: ["Connecting to {name}…", { name: s.link }], button: { label: "Connecting…", action: "none" }, banner: s.reopening && s.autoReconnect ? { msg: [LOST_RETRYING, { name: s.link }], button: { label: "Reconnect", action: "retry", disabled: true } } : null };
    case "syncing":
      return { status: ["Syncing with {name}…", { name: s.link }], button: { label: "Connecting…", action: "none" }, banner: null };
    case "ready":
      return { status: id, button: { label: "Connected", action: "menu" }, banner: null };
    case "waking":
      return { status: ["Waking the piano…"], button: { label: "Connected", action: "menu" }, banner: null };
    case "displaced":
      // the panel's start-up (boot window): what happened and the one way to the app, not an alarm (field 2026-11-18)
      if (s.bootTook) return { status: id, button: { label: "Connected", action: "menu" }, banner: { msg: [BOOT_TOOK], button: { label: "Use this app", action: "retake", disabled: false } } };
      return { status: id, button: { label: "Connected", action: "menu" }, banner: { msg: ["Another device (the panel or another app) took control"], button: { label: "Take control again", action: "retake", disabled: false } } };
    case "retaking":
      if (s.rejoin) return { status: ["Reconnecting to {name}…", { name: s.link }], button: { label: "Connecting…", action: "none" }, banner: null };
      return { status: ["Taking control…"], button: { label: "Connecting…", action: "none" }, banner: { msg: ["Taking control…"], button: { label: "Take control again", action: "retake", disabled: true } } };
    case "lost":
      return { status: ["Disconnected"], button: { label: "Connect", action: "pick" }, banner: { msg: [s.autoReconnect && s.key ? LOST_RETRYING : "The connection to {name} was lost.", { name: s.link }], button: { label: "Reconnect", action: "retry", disabled: false } } };
    case "failed":
      return { status: ["Connection failed: {e}", { e: err }], button: { label: "Connect", action: "pick" }, banner: { msg: ["Connection failed: {e}", { e: err }], button: { label: "Try again", action: "retry", disabled: false } } };
  }
}

// ---------------- manager ----------------

/** A link to open: a picker entry, an auto-link, a remembered device. */
export interface LinkChoice {
  /** known before opening (shell / Web MIDI ids): a re-pick of the current piano opens nothing */
  key?: string;
  name: string;
  open(): Promise<MidiLink>;
}

export interface ManagerOptions {
  makeSession(link: MidiLink): PianoSession;
  /** per-reply budget handed to connect() / takeControl() (default 3000) */
  replyMs?: number;
  /**
   * Boot notice: a 7E 01 00 within `windowMs` of a session start is shown as the panel's start-up taking control
   * (nothing automatic). Absent / null: off (the panel builds — they are the panel).
   */
  bootNotice?: { windowMs: number } | null;
  /**
   * Auto-reconnect: a lost link is reopened after `delaysMs[attempt]` (the last delay repeats), until it is back or
   * the user disconnects. Absent / null: off — a lost link waits for the banner / header button.
   */
  autoReconnect?: { delaysMs: readonly number[] } | null;
  /**
   * Re-acquire a lost endpoint afresh instead of reopening the link object held from before (desktop app, field
   * 2026-11-03: after Windows dropped and re-made the CA79 BLE connection, `gatt.connect()` on the held
   * BluetoothDevice never came back; a new app start — a new device lookup — connected at once). With it, reopens
   * of a lost link alternate: held link, then this, then the held link … until one opens. `null` = does not apply
   * to this key (not BLE): the held reopen is used. Absent: always the held reopen (browser tab, phone, panel).
   */
  reacquire?(key: string | null): Promise<MidiLink> | null;
  /** watchdogs per phase (ms) */
  limits?: Partial<Record<"connecting" | "syncing" | "retaking" | "waking", number>>;
  /** after the first contact of a session (ready, or displaced while connecting): open-to-wake; a promise = the phase "waking" */
  wake?(s: PianoSession): Promise<unknown> | null;
  /** the session the screens use changed (null: none) */
  onSession?(s: PianoSession | null): void;
  /** a session reached ready on this link: remember it for the next start (null: forget, after "Disconnect") */
  remember?(link: { key: string; name: string } | null): void;
  /** effects the UI draws (device list, connected menu) */
  openPicker?(): void;
  openMenu?(): void;
  trace?(line: string): void;
  setTimer?(fn: () => void, ms: number): unknown;
  clearTimer?(h: unknown): void;
}

// connect(): CONNECT + model ≤ 2 × replyMs, three optional queries ≤ 6 × replyMs/3 → ~12 s at 3 s.
// The dump: idle 2 × replyMs, cap 120 s, one retry (session.ts) — the watchdog only catches a hang.
const LIMITS = { connecting: 20000, syncing: 260000, retaking: 280000, waking: 10000 };

/**
 * Phone / PC app default for `bootNotice`. Window: the panel's start-up CONNECT comes after its Android boot, the
 * UI chooser and the UI's own start — not measured yet (bootms); it only picks the banner's words, so it is wide.
 */
export const BOOT_NOTICE = { windowMs: 120000 };

/**
 * Desktop app default for `autoReconnect` (field 2026-11-03): a power-cycled piano's BLE module advertises again
 * ~10-20 s after power-on; the first attempts come soon, then every 30 s while the app stays open.
 */
export const AUTO_RECONNECT = { delaysMs: [3000, 5000, 10000, 15000, 30000] } as const;

const SETTLED: readonly Phase[] = ["idle", "ready", "waking", "displaced", "lost", "failed"];

export class ConnectionManager {
  state: ConnState = INITIAL;
  session: PianoSession | null = null;
  private link: MidiLink | null = null;
  private readonly dead = new WeakSet<MidiLink>();
  private readonly adopted = new WeakSet<MidiLink>();
  private reopenFn: (() => Promise<MidiLink>) | null = null;
  /** the next reopen re-acquires the endpoint (`reacquire`) instead of the held link: the last held reopen did not open */
  private freshNext = false;
  private pending: { choice: LinkChoice; link: MidiLink | null } | null = null;
  private offSession = () => {};
  private timer: unknown = null;
  private graceTimer: unknown = null;
  private reTimer: unknown = null;
  private readonly listeners = new Set<(s: ConnState) => void>();
  private readonly opt: ManagerOptions;
  private readonly limits: typeof LIMITS;
  private readonly setTimer: (fn: () => void, ms: number) => unknown;
  private readonly clearTimer: (h: unknown) => void;

  constructor(opt: ManagerOptions) {
    this.opt = opt;
    this.limits = { ...LIMITS, ...opt.limits };
    this.setTimer = opt.setTimer ?? ((fn, ms) => setTimeout(fn, ms));
    this.clearTimer = opt.clearTimer ?? ((h) => clearTimeout(h as ReturnType<typeof setTimeout>));
    this.state = { ...INITIAL, bootNotice: !!opt.bootNotice, autoReconnect: !!opt.autoReconnect?.delaysMs.length };
  }

  get phase(): Phase {
    return this.state.phase;
  }
  /** The current link (for tests and the MIDI log). */
  get currentLink(): MidiLink | null {
    return this.link;
  }

  /** Returns an unsubscribe function. Called with the state after every change. */
  onChange(fn: (s: ConnState) => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  /** Resolves with the phase as soon as it is not connecting / syncing / retaking. */
  settled(): Promise<Phase> {
    if (SETTLED.includes(this.phase)) return Promise.resolve(this.phase);
    return new Promise((resolve) => {
      const off = this.onChange((s) => {
        if (SETTLED.includes(s.phase)) {
          off();
          resolve(s.phase);
        }
      });
    });
  }

  connectButton(): void {
    this.dispatch({ type: "connectButton" });
  }
  disconnect(): void {
    this.dispatch({ type: "disconnect" });
  }
  retake(): void {
    this.dispatch({ type: "retake" });
  }
  retry(): void {
    this.dispatch({ type: "retry" });
  }
  suspend(): void {
    this.dispatch({ type: "suspend" });
  }
  resume(): void {
    this.dispatch({ type: "resume" });
  }

  /**
   * Connect through `c`. Idempotent: the piano this app is already on (same key) opens nothing and
   * starts no session (displaced: it takes control again). Resolves with the settled phase.
   */
  async choose(c: LinkChoice): Promise<Phase> {
    let link: MidiLink | null = null;
    let key = c.key;
    if (!key) {
      try {
        link = await c.open(); // e.g. the browser's Bluetooth chooser: the key is known only now
      } catch (e) {
        this.trace(`open ${c.name} failed: ${(e as Error).message}`);
        throw e;
      }
      key = linkKey(link);
    }
    this.pending = { choice: c, link };
    this.dispatch({ type: "pick", key, link: link?.name ?? c.name });
    this.pending = null;
    return this.settled();
  }

  /** Connect over a link already open (auto-link, ?sim, tests). */
  attach(link: MidiLink): Promise<Phase> {
    return this.choose({ key: linkKey(link), name: link.name, open: async () => link });
  }

  dispatch(ev: ConnEvent): void {
    const prev = this.state;
    const { state, effects } = reduce(prev, ev);
    if (state !== prev) {
      if (state.phase !== prev.phase) this.trace(`conn ${prev.phase} -> ${state.phase} (${ev.type})`);
      this.state = state;
    }
    for (const e of effects) this.run(e);
    if (state !== prev) for (const l of [...this.listeners]) l(this.state);
  }

  private trace(line: string): void {
    this.opt.trace?.(line);
  }

  private run(e: Effect): void {
    switch (e.type) {
      case "picker":
        this.opt.openPicker?.();
        break;
      case "menu":
        this.opt.openMenu?.();
        break;
      case "start":
        void this.start(e.gen, e.source, this.pending);
        break;
      case "retake": {
        const s = this.session;
        const gen = this.state.gen;
        if (!s) break;
        s.takeControl(this.opt.replyMs ?? 3000).catch((err: Error) => {
          if (!s.displaced) this.dispatch({ type: "failed", gen, error: err.message });
        });
        break;
      }
      case "disconnect": {
        const s = this.session;
        const link = this.link;
        this.offSession(); // our own DISCONNECT must not come back as an event
        if (s && link && !this.dead.has(link)) s.disconnect(); // 7E 01 00: the stock hands control back to the panel
        this.dropSession();
        if (link && !this.dead.has(link)) link.close(); // BLE: GATT disconnect, the module is free for another device
        this.link = null;
        // no session any more: its boot window and a pending reconnect go with it
        for (const h of [this.graceTimer, this.reTimer]) this.clearTimer(h);
        this.graceTimer = this.reTimer = null;
        this.freshNext = false;
        if (this.state.key === null) {
          this.reopenFn = null;
          this.opt.remember?.(null);
        }
        break;
      }
      case "release":
        this.dropSession(); // unsubscribed first: its own "closed" is not a lost link
        break;
      case "arm": {
        this.clearTimer(this.timer);
        const seq = e.seq;
        this.timer = this.setTimer(() => this.dispatch({ type: "timeout", seq }), this.limits[e.phase as keyof typeof LIMITS]);
        break;
      }
      case "disarm":
        this.clearTimer(this.timer);
        this.timer = null;
        break;
      case "grace": {
        const gen = e.gen;
        this.clearTimer(this.graceTimer);
        this.graceTimer = this.setTimer(() => this.dispatch({ type: "graceEnd", gen }), this.opt.bootNotice?.windowMs ?? 0);
        break;
      }
      case "bootTook":
        // the stock binds one controller: taking it back would only fight the panel's start-up (field 2026-11-18)
        this.trace("displaced right after the session started (panel start-up?): not taking control back");
        break;
      case "reconnectArm": {
        const d = this.opt.autoReconnect?.delaysMs ?? [];
        if (!d.length) break;
        const seq = e.seq;
        const ms = d[Math.min(e.attempt, d.length - 1)]!;
        this.trace(`link lost: reconnect attempt ${e.attempt + 1} in ${ms / 1000} s`);
        this.clearTimer(this.reTimer);
        this.reTimer = this.setTimer(() => this.dispatch({ type: "reconnectTick", seq }), ms);
        break;
      }
    }
  }

  private dropSession(): void {
    this.offSession();
    this.offSession = () => {};
    const s = this.session;
    this.session = null;
    s?.release();
    this.opt.onSession?.(null);
  }

  private async start(gen: number, source: "choice" | "current" | "reopen", pending: { choice: LinkChoice; link: MidiLink | null } | null): Promise<void> {
    let link: MidiLink;
    try {
      if (source === "current" && pending?.link) link = pending.link; // a fresh link to the same endpoint
      else if (source === "current" && this.link && !this.dead.has(this.link)) link = this.link;
      else if (source === "choice" && pending) {
        this.freshNext = false; // another endpoint: its own link object
        link = pending.link ?? (await pending.choice.open());
      } else {
        // reopen of a lost link: the held link object, or — after it failed — the endpoint re-acquired afresh
        const fresh = source === "reopen" && this.freshNext ? (this.opt.reacquire?.(this.state.key) ?? null) : null;
        // the next reopen alternates unless this one opens (a hung attempt ends by the watchdog, never here)
        if (source === "reopen") this.freshNext = !fresh && !!this.opt.reacquire;
        if (fresh) this.trace("reopen: re-acquiring the device (the held link did not reopen)");
        if (fresh) link = await fresh;
        else if (this.reopenFn) link = await this.reopenFn();
        else {
          // nothing to reopen (the last link never opened - field 2026-11-01 "no link to open"): say so, offer the list
          this.dispatch({ type: "failed", gen, error: NO_LINK });
          this.opt.openPicker?.();
          return;
        }
      }
      this.freshNext = false; // the endpoint opened: the held link is good again
    } catch (err) {
      this.dispatch({ type: "failed", gen, error: (err as Error).message });
      return;
    }
    if (this.dead.has(link)) {
      this.dispatch({ type: "failed", gen, error: "link closed" });
      return;
    }
    // superseded while the link was opening, or the watchdog gave up on it (failed / lost): a BLE link stays open
    // and the next reopen of the same device takes it (ble.ts openLinks)
    if (this.state.gen !== gen || this.state.phase !== "connecting") return;
    const old = this.link;
    this.dropSession(); // the old session goes silently: no DISCONNECT on a port we keep using
    // a different endpoint: let the old one go (its session is already retired, so no "lost")
    if (old && old !== link && !this.dead.has(old) && linkKey(old) !== linkKey(link)) old.close();
    this.link = link;
    this.reopenFn = link.reopen ? () => link.reopen!() : source === "choice" && pending ? () => pending.choice.open() : this.reopenFn;
    if (!this.adopted.has(link)) {
      this.adopted.add(link);
      link.onClose(() => this.dead.add(link));
    }
    this.run1(gen, link);
  }

  private run1(gen: number, link: MidiLink): void {
    const s = this.opt.makeSession(link);
    this.session = s;
    let contact = false;
    const firstContact = (ready: boolean) => {
      if (contact) return;
      contact = true;
      const p = this.opt.wake?.(s) ?? null;
      if (p && ready) {
        this.dispatch({ type: "wakeStart", gen });
        void p.finally(() => this.dispatch({ type: "wakeDone", gen }));
      }
    };
    const offInvite = s.onInvite(() => this.dispatch({ type: "invited", gen }));
    const offRebind = s.onRebind((why) => this.dispatch({ type: "rebind", gen, why }));
    const offState = s.onState((st) => {
      if (st === "syncing") this.dispatch({ type: "identified", gen });
      else if (st === "ready") {
        this.dispatch({ type: "synced", gen, model: s.identity?.model ?? "", version: s.identity?.version ?? "" });
        if (this.state.gen === gen && this.state.key) this.opt.remember?.({ key: this.state.key, name: this.state.link });
        firstContact(true);
      } else if (st === "displaced") {
        this.dispatch({ type: "displaced", gen });
        firstContact(false); // 19 00 / 19 02 answer on any port
      } else if (st === "closed") this.dispatch({ type: "closed", gen });
    });
    this.offSession = () => {
      offState();
      offInvite();
      offRebind();
    };
    this.opt.onSession?.(s);
    s.connect(this.opt.replyMs ?? 3000).catch((err: Error) => {
      if (!s.displaced && s.state !== "closed") this.dispatch({ type: "failed", gen, error: err.message });
    });
  }
}

export function linkKey(link: MidiLink): string {
  return link.key ?? `${link.kind}:${link.name}`;
}
