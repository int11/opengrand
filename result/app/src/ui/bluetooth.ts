// 「블루투스」 page — one component for every surface (side-nav tab on wide screens, the header chip everywhere):
// BT audio sources (op 0x1D 04-09: what plays now, the selected source, 「이 기기로 듣기」 with the switch progress, 「끊기」
// asked twice, 「선호 기기」), whether a signal reaches the piano's record bus (op 1D 0B, the DSP's capture), the piano's own Bluetooth settings, the policy and the piano's
// reconnect, the BLE-MIDI link of this app; in the closed 「고급 · 개발자용」 fold the module restart, the explicit AUTOCALL
// action with its confirmation, the late stop and the module lines (the module's device type, AVRCP browsing and audio
// routing are kept by the firmware itself, no card).
// Rows are keyed by device and updated in place — a refresh never replaces a button the user is pressing.
import { t } from "../i18n.ts";
import {
  AC_OP, AC_STATE, BtPort, CODEC_NAME, LOG_FLAG, MOD_HEALTH, PLAY_STATUS, POLICY_STORE, SRC_ACT, SRC_DEV, SRC_DEV2, SRC_FLAG, SRC_MODE, SRC_OPTION, SRC_RECONNECT, SW_PHASE, SW_RESULT,
  SW_TRIED, levelDb, levelVerdict, macShort, macText,
  type BtAutocall, type BtLevel, type BtLogRecord, type BtSource, type BtSourcePolicy, type BtSources,
} from "../protocol/btport.ts";
import type { ParamStore } from "../state/store.ts";
import { control } from "./controls.ts";
import { h, section } from "./dom.ts";

/** 「끊기」 asks once more: the first tap only arms it for this long (field bug 2026-11-01 evening, record 12 §4). */
export const CLOSE_CONFIRM_MS = 4000;

/** One-line hint for payloads without the source manager. */
/** Firmware without the source list (1D 04): switching means reconnecting by hand. */
export const BT_AUDIO_SWITCH_HINT = "To switch the audio source (phone ↔ TV): disconnect Bluetooth on the device playing now, then connect the other one to the piano.";
/** Firmware with the source manager: switching happens on the Bluetooth tab. */
export const BT_AUDIO_TAB_HINT = "To change which device plays through the piano, open the Bluetooth tab and press “Listen on this device” on that device.";

/** One device line: …11:a1 · playing / connected / waiting … (the selected mark is a badge next to it).  `s` (the whole
 *  reply) adds what only it knows: 「재생을 한 번 눌러 주세요」 only after the piano's own tries (PLAY, its START, reopening
 *  the sound link) ended "ready" for this device; the codec of its stream. */
export function sourceText(d: BtSource, s?: BtSources | null, slot = -1): string {
  const sel = !!(d.state2 & SRC_DEV2.selected);
  const asked = sel && !!s?.sw && !s.sw.phase && s.sw.result === SW_RESULT.ready;
  const st = d.state & SRC_DEV.closing ? t("closing…")
    : d.state2 & SRC_DEV2.calling ? t("connecting…")
    : d.state & SRC_DEV.stream ? t("playing")
    : d.state2 & SRC_DEV2.parked && !d.a2dp ? t("sound link closed by the piano — pick it to reconnect")
    : sel ? (d.a2dp ? (asked ? t("waiting — press play on it once") : t("waiting")) : t("waiting — not linked for sound"))
    : d.a2dp ? t("connected") : t("connected · no sound link");
  const parts = [d.state & SRC_DEV.addr ? macShort(d.addr) : t("(address not known yet)"), st];
  const c = s?.codec;
  if (d.state & SRC_DEV.stream && c && c.dev === slot && CODEC_NAME[c.codec]) parts.push(CODEC_NAME[c.codec]!);
  const ps = playStatusText(d);
  if (ps && !(d.state & SRC_DEV.stream && ((d.ps ?? 0) & 7) === PLAY_STATUS.playing)) parts.push(ps);   // not twice
  if (d.state2 & SRC_DEV2.refused) parts.push(t("its play was held back"));
  if (d.state & SRC_DEV.pref) parts.push(t("preferred"));
  return parts.join(" · ");
}

/** The device's own player as it reports it (AVRCP play status, firmware after ota_main 9e6c8c32): 「재생 중」 /
 *  「일시정지」 / 「정지」 - shown apart from the sound link because Windows streams silence while paused. */
export function playStatusText(d: BtSource): string {
  switch ((d.ps ?? 0) & 7) {
    case PLAY_STATUS.playing: case PLAY_STATUS.seeking: return t("player: playing");
    case PLAY_STATUS.paused: return t("player: paused");
    case PLAY_STATUS.stopped: return t("player: stopped");
    case PLAY_STATUS.error: return t("player: error");
  }
  return "";
}

/** The switch line: progress while it runs, then the result. */
export function switchText(s: BtSources, target: string): string {
  const sw = s.sw;
  if (!sw) return "";
  const others = s.devices.filter((d) => d.state & SRC_DEV.stream && !(d.state2 & SRC_DEV2.selected));
  const from = others.map((d) => macShort(d.addr)).join(", ") || t("the other device");
  // the piano never stops another source's stream (firmware after ota_main c0b89c60 - a Windows Bluetooth audio device
  // stayed silent after the piano's stop): one without remote control (no AVRCP) must be
  // paused on that device; one with it gets PAUSE and the piano waits for it to let go
  const noRc = others.filter((d) => !(d.state2 & SRC_DEV2.avrcp)).map((d) => macShort(d.addr)).join(", ");
  const tgt = s.devices.find((d) => d.state2 & SRC_DEV2.selected);
  switch (sw.phase) {
    case SW_PHASE.pause: return noRc
      ? t("Switching… the piano cannot pause {from} — pause it on that device", { from: noRc })
      : t("Switching… pausing {from}", { from });
    case SW_PHASE.suspend: return t("Switching… stopping the sound of {from}", { from });
    case SW_PHASE.close: return t("Switching… cutting the sound link of {from}", { from });
    case SW_PHASE.link: case SW_PHASE.call: return t("Switching… connecting {to}", { to: target });
    case SW_PHASE.settle: case SW_PHASE.play: return t("Switching… asking {to} to play", { to: target });
    case SW_PHASE.start: return t("Switching… the piano starts the sound of {to}", { to: target });
    case SW_PHASE.reopen: return t("Switching… reconnecting the sound link of {to}", { to: target });
    case SW_PHASE.direct: return t("Switching… the piano asks for {to}'s sound directly", { to: target });
  }
  const secs = (sw.ms / 1000).toFixed(1);
  switch (sw.result) {
    // result 1 = the target's stream is up (+ AVRCP PLAY sent): the module cannot tell sound from silence (field
    // 2026-11-02 afternoon: Windows streamed silence on a fresh link while its player was paused) - say what was done
    // no PLAY on purpose (firmware after 9e6c8c32): Windows takes PLAY as play/pause, so a source that plays is left alone
    case SW_RESULT.playing: return (s.swTried & SW_TRIED.play
      ? t("Asked {to} to play ({s} s) — if you hear nothing, press play on it", { to: target, s: secs })
      : s.swTried & SW_TRIED.kept
        ? t("{to} was already playing, so it was left as it is ({s} s)", { to: target, s: secs })
        : s.swTried & SW_TRIED.notOurs
          ? t("{to}'s sound link is open ({s} s) — its play state is unknown, so no play request was sent; if you hear nothing, press play on it", { to: target, s: secs })
          : t("{to}'s sound link is open ({s} s) — if you hear nothing, press play on it", { to: target, s: secs }))
      + (s.swTried & (SW_TRIED.start | SW_TRIED.reopen) ? " " + t("(the piano restarted its sound)") : "");
    // the piano never starts the sound of a device without remote control (firmware after ota_main f2056ae8: a Windows
    // stream the piano's START opened stayed silent when the PC played): its own play opens it
    case SW_RESULT.ready: return tgt && !(tgt.state2 & SRC_DEV2.avrcp) && !(s.swTried & (SW_TRIED.start | SW_TRIED.play))
      ? t("{to} is selected — the piano has no remote control of it, so press play on that device", { to: target })
      : t("{to} is selected — the piano could not start it; press play on it once", { to: target });
    // a BUSY whose blocking stream has closed since is stale (field 2026-11-10 night: the PC let go 20 s after its
    // pause; firmware after f2056ae8 goes on with the switch or clears the result itself - this covers the older one)
    case SW_RESULT.busy: return !others.length
      ? t("The other device has let go of the sound — press “Listen on this device” again.")
      : noRc
      ? t("The piano cannot pause {from} (no remote control from it) — pause it on that device, or disconnect it, then try again.", { from: noRc })
      : t("The other device would not let go of the sound — stop it there, or disconnect it.");
    case SW_RESULT.noLink: return t("Could not connect {to} — is it on and in range?", { to: target });
    case SW_RESULT.cancelled: return t("The switch was cancelled (standby or the module restarted).");
  }
  return "";
}

/** the late stop's waits offered (0.1 s units - the firmware takes 1..100) */
export const LATE_WAITS = [1, 5, 10, 20, 45];


/** the piano's own reconnect while AUTOCALL is off (firmware after ota_main e5bb2372): value, label, one-line explanation */
export const RECONNECT_TEXT: [number, string, string][] = [
  [SRC_RECONNECT.off, "Off", "The piano never calls a device by itself — connect from the device."],
  [SRC_RECONNECT.pref, "Preferred only", "The piano calls only the preferred device (none set: nothing)."],
  [SRC_RECONNECT.prefLast, "Preferred · last", "The piano calls the preferred device, or else the one you picked last. (default)"],
];

const RC_TEXT: Record<number, string> = {
  1: "Rejected.", 2: "Only the controlling device can change this — take control first.", 3: "The piano is busy — try again.",
  4: "Bluetooth Audio is off on the piano.", 7: "That device is no longer connected.", 8: "The piano is in standby.",
  9: "That device has no remote-control (AVRCP) link — pause it on the device itself, or disconnect it.",
  10: "Read the module setting first.",
};

/** One module-log line for the developer view: `  12.34 ← A2DP STREAMING START 4` (→ sent, · the piano's note). */
export function logLine(r: BtLogRecord): string {
  const dir = r.flags & LOG_FLAG.sent ? "→" : r.flags & LOG_FLAG.event ? "·" : "←";
  return `${(r.tMs / 1000).toFixed(2).padStart(8)} ${dir} ${r.text}${r.flags & LOG_FLAG.cut ? " …" : ""}`;
}

export interface BtPageOptions {
  refreshMs?: number;
  confirmMs?: number;
  /** the piano's own Bluetooth settings (BluetoothAudio, its volume, BluetoothMIDI) */
  store?: ParamStore;
  /** this app's MIDI link, for the BLE-MIDI card */
  link?: () => { kind: string; name: string } | null;
  /** another controller bound too (the header's co-bound text), "" none */
  coBound?: () => string;
  /** opens the connect menu (absent in the panel shell: always connected, no menu) */
  openConnect?: () => void;
}

interface SourceRow {
  el: HTMLElement;
  text: HTMLElement;
  badge: HTMLElement;
  use: HTMLButtonElement;
  close: HTMLButtonElement;
  pref: HTMLButtonElement;
  disarm?: ReturnType<typeof setTimeout>;
}

const LOG_KEEP = 500;

/** The page (a `.screen` of cards).  It polls 1D 04 while it is in the document. */
export function bluetoothPage(port: () => BtPort | null, opts: BtPageOptions = {}): HTMLElement {
  // Layout (owner 2026-11-14): everyday cards first — what plays now
  // and the device list, the piano's own Bluetooth settings, how devices are chosen / reconnected, this app's MIDI link —
  // then one closed 「고급 · 개발자용」 fold (module restart, AUTOCALL, late stop, module lines). Nothing about power here:
  // standby / off / wake live in Settings → 전원.
  // ---- BT audio ----
  const nowLine = h("p", { class: "bt-now", id: "bt-now", role: "status", hidden: true });
  const status = h("p", { class: "muted", id: "bt-src-status" }, t("Reading…"));
  const swLine = h("p", { class: "bt-sw", id: "bt-sw", role: "status", "aria-live": "polite", hidden: true });
  const listEl = h("div", { class: "bt-devs", id: "bt-src-list" });
  const msg = h("p", { class: "muted", id: "bt-src-msg" });
  // select only: another device keeps its sound open - the piano does nothing about it (user decision 2026-11-09)
  // module health: the module rebooting by itself (field 2026-11-02) - the loop warning with the AUTOCALL action inline
  const healthText = h("p", { id: "bt-health-text" });
  const healthAc = h("button", { type: "button", id: "bt-health-ac", hidden: true, onclick: () => askAutocallOff() }, t("Turn AUTOCALL off…"));
  const health = h("div", { class: "bt-confirm", id: "bt-health", role: "alert", hidden: true }, healthText, healthAc);
  // what actually reaches the piano's audio path (1D 0B): the DSP's capture of the record bus - piano + BT audio + LINE IN
  const levelLine = h("p", { id: "bt-level", role: "status", hidden: true });
  const audio = section(t("Bluetooth audio"), nowLine, status, health, levelLine, listEl, swLine, msg,
    h("p", { class: "muted" }, t("Every device stays connected; the piano plays the one you pick here.")));
  audio.id = "bt-audio-card";
  // ---- the piano's settings ----
  const piano = opts.store ? section(t("Piano Bluetooth settings"),
    control(opts.store, "BluetoothAudio", { label: t("Bluetooth Audio") }),
    control(opts.store, "BluetoothSetting", { label: t("Bluetooth Audio volume") }),
    control(opts.store, "BluetoothMIDI", { label: t("Bluetooth MIDI") })) : null;
  if (piano) piano.id = "bt-piano-card";
  // ---- choosing / reconnecting devices ----
  const pol = h("div", { id: "bt-src-policy" });
  // the piano's own reconnect (used while AUTOCALL is off) - user decision 2026-11-09; hidden on older firmware
  const rcBox = h("div", { id: "bt-reconnect", hidden: true });
  // AUTOCALL on: the module reconnects by itself and the piano's choice is not used (firmware: no call while AUTOCALL is on)
  const rcAc = h("p", { class: "muted", id: "bt-rc-ac", hidden: true }, t("Not used now: the Bluetooth module reconnects by itself (AUTOCALL is on — see Advanced below)."));
  const polStore = h("p", { class: "muted", id: "bt-src-store" });
  const behaviour = section(t("Choosing and reconnecting devices"), pol, rcBox, rcAc, polStore);
  behaviour.id = "bt-policy-card";
  behaviour.hidden = true;   // shown once the piano answers 1D 04
  // ---- BLE MIDI ----
  const midiLine = h("p", { id: "bt-midi-link" });
  const midiCo = h("p", { class: "muted", id: "bt-midi-co" });
  const midi = section(t("Bluetooth MIDI (this app)"), midiLine, midiCo,
    h("button", { type: "button", id: "bt-midi-connect", hidden: !opts.openConnect, onclick: () => opts.openConnect?.() }, t("Connection menu")));
  midi.id = "bt-midi-card";
  // ---- advanced · developer: one closed fold at the bottom ----
  // module restart (1D 07): a fix to try when a device plays but the piano stays silent; it drops every link - shown once the piano answers 1D 04
  const restart = h("button", { type: "button", id: "bt-src-restart", onclick: async () => { const p = port(); if (p) say(await p.restartModule()); void load(); } },
    t("Restart the Bluetooth audio module"));
  const restartBox = h("div", { id: "bt-restart-box", hidden: true },
    h("p", { class: "muted" }, t("Try this when a device plays but the piano stays silent. Every connected device drops for a few seconds.")), restart);
  // ---- AUTOCALL ----
  const acText = h("p", { id: "bt-ac-state" }, t("Not read yet."));
  const acMsg = h("p", { class: "muted", id: "bt-ac-msg" });
  const acRead = h("button", { type: "button", id: "bt-ac-read", onclick: () => void acDo(AC_OP.read) }, t("Read the module setting"));
  const acAsk = h("button", { type: "button", id: "bt-ac-ask", hidden: true, onclick: () => { acConfirm.hidden = false; acAsk.hidden = true; } });
  const acYes = h("button", { type: "button", class: "danger", id: "bt-ac-yes", onclick: () => { acConfirm.hidden = true; void acDo(acWant); } });
  const acNo = h("button", { type: "button", id: "bt-ac-no", onclick: () => { acConfirm.hidden = true; acAsk.hidden = false; } }, t("Cancel"));
  const acWhy = h("p", { class: "muted", id: "bt-ac-why" });
  const acConfirm = h("div", { class: "bt-confirm", id: "bt-ac-confirm", hidden: true }, acWhy, h("div", { class: "inline" }, acYes, acNo));
  const autocall = h("div", { id: "bt-ac-card", hidden: true },   // shown once the piano answers 1D 04 layout 2
    h("h4", {}, t("Bluetooth module's own reconnect (AUTOCALL)")), acText,
    h("div", { class: "inline" }, acRead, acAsk), acConfirm, acMsg);
  // ---- the late stop and the module lines ----
  const lateBox = h("div", { id: "bt-src-late", hidden: true });
  const logPre = h("pre", { class: "bt-log", id: "bt-src-log" });
  const logBox = h("details", { id: "bt-src-dev" }, h("summary", {}, t("Module lines")), logPre) as HTMLDetailsElement;
  const devFold = h("details", { id: "bt-dev-fold", class: "fold card-fold" },
    h("summary", {}, h("span", { class: "fold-title" }, t("Advanced · developer"))),
    h("div", { class: "fold-body" }, restartBox, autocall, h("div", { id: "bt-dev-late" }, lateBox, logBox))) as HTMLDetailsElement;
  const dev = h("section", { class: "card", id: "bt-dev-card" }, devFold);

  const root = h("div", { class: "screen", id: "bt-page" }, audio, ...(piano ? [piano] : []), behaviour, midi, dev);

  const rows = new Map<string, SourceRow>();
  const logLines: string[] = [];
  let logNext: number | null = null;
  let timer: ReturnType<typeof setTimeout> | undefined;
  let last: BtSources | null = null;
  let polKey = "";
  let attached = false;
  let switching: string | null = null;   // the device the user switched to from this page (its result is shown)
  let acWant: number = AC_OP.off;

  const say = (rc: number | null, a?: number) => {
    msg.textContent = rc === 0 ? (a === SRC_ACT.close ? t("Disconnecting…") : "")
      : rc === null ? t("No reply.") : t(RC_TEXT[rc] ?? "Rejected ({c}).", { c: rc });
  };
  const devOf = (key: string) => last?.devices.find((d, i) => rowKey(d, i) === key) ?? null;
  const send = async (a: number, key: string) => {
    const d = devOf(key);
    const p = port();
    if (!d || !(d.state & SRC_DEV.addr) || !p) return;
    if (a === SRC_ACT.use) {
      switching = macText(d.addr);
      swLine.hidden = false;
      swLine.textContent = t("Switching to {to}…", { to: macShort(d.addr) });
    }
    const rc = await p.sourceAction(a, d.addr);
    say(rc, a);
    if (rc !== 0 && a === SRC_ACT.use) {
      switching = null;
      swLine.hidden = true;
    }
    void load();
  };
  const setPol = async (p: BtSourcePolicy) => { const bp = port(); if (!bp) return; const r = await bp.setPolicy(p); say(r ? r.rc : null); void load(); };
  const disarm = (r: SourceRow) => {
    clearTimeout(r.disarm);
    r.disarm = undefined;
    delete r.close.dataset.armed;
    r.close.classList.remove("danger");
    r.close.textContent = t("Disconnect it");
  };
  const makeRow = (key: string): SourceRow => {
    const r = { text: h("span", { class: "bt-dev-name" }), badge: h("span", { class: "badge", hidden: true }, t("selected")) } as SourceRow;
    r.use = h("button", { type: "button", class: "primary", "data-action": "use", onclick: () => void send(SRC_ACT.use, key) }, t("Listen to this one"));
    r.close = h("button", {
      type: "button", "data-action": "close", onclick: () => {
        if (!r.close.dataset.armed) {
          r.close.dataset.armed = "1";
          r.close.classList.add("danger");
          r.close.textContent = t("Disconnect? Tap again");
          r.disarm = setTimeout(() => disarm(r), opts.confirmMs ?? CLOSE_CONFIRM_MS);
          return;
        }
        disarm(r);
        void send(SRC_ACT.close, key);
      },
    }, t("Disconnect it"));
    r.pref = h("button", {
      type: "button", "data-action": "pref", onclick: () => {
        const d = devOf(key);
        if (d && last) void setPol({ ...last.policy, pref: d.state & SRC_DEV.pref ? null : d.addr });
      },
    }, t("Prefer"));
    r.el = h("div", { class: "bt-dev", "data-mac": key }, h("div", { class: "bt-dev-head" }, r.text, r.badge), h("div", { class: "inline" }, r.use, r.pref, r.close));
    return r;
  };
  const drawPolicy = (p: BtSourcePolicy) => {
    const follow = p.mode === SRC_MODE.select && !!(p.flags & SRC_FLAG.follow);
    const only = p.mode === SRC_MODE.select && !follow;
    pol.replaceChildren(
      h("p", { class: "sub" }, t("When another device plays")),
      h("div", { class: "segmented", id: "bt-src-mode" },
        h("button", { type: "button", class: only ? "on" : undefined, "data-mode": "select", onclick: () => void setPol({ ...p, mode: SRC_MODE.select, flags: p.flags & ~SRC_FLAG.follow }) }, t("Keep the one I picked")),
        h("button", { type: "button", class: follow ? "on" : undefined, "data-mode": "follow", onclick: () => void setPol({ ...p, mode: SRC_MODE.select, flags: p.flags | SRC_FLAG.follow }) }, t("Switch to it"))),
      h("p", { class: "muted" }, only ? t("A device you did not pick that starts playing is paused and its sound held back — it stays connected.")
        : follow ? t("The device that starts playing becomes the one the piano plays.")
        : t("An older behaviour is set on the piano (no automatic choice) — pick one of the two above.")),
      h("p", { class: "muted" }, p.pref ? t("Preferred: {mac} — reconnected first.", { mac: macShort(p.pref) }) : t("No preferred device.")));
    polStore.textContent = p.store != null && p.store & POLICY_STORE.kept
      ? t("The preferred device, auto reconnect and the choice above follow the startup setting (Settings → Startup settings).")
      : t("These settings last until the piano is switched off.");
    drawLate(p);
    drawReconnect(p);
  };
  // 「자동 재연결」: off / preferred only / preferred · last - one line each; it applies at power-on and at the wake from a
  // standby that switched the module off; at power-on the value is as the startup setting says
  // (stock = factory, current / power-off = the snapshot's value)
  const drawReconnect = (p: BtSourcePolicy) => {
    rcBox.hidden = p.reconnect == null;
    if (p.reconnect == null) return;
    const cur = RECONNECT_TEXT.find(([v]) => v === p.reconnect);
    const st = p.store ?? 0;
    rcBox.replaceChildren(
      h("p", { class: "sub" }, t("Reconnect at power-on")),
      h("div", { class: "segmented", id: "bt-rc" }, ...RECONNECT_TEXT.map(([v, label]) =>
        h("button", { type: "button", class: p.reconnect === v ? "on" : undefined, "data-reconnect": String(v), onclick: () => void setPol({ ...p, strategy: p.strategy ?? 0, reconnect: v }) }, t(label)))),
      h("p", { class: "muted", id: "bt-rc-why" }, cur ? t(cur[2]) : ""),
      h("p", { class: "muted", id: "bt-rc-store" }, st & POLICY_STORE.failed ? t("Could not save it on the piano — try again.")
        : st & POLICY_STORE.pending ? t("Saving on the piano…") : t("Used when the piano is switched on, and when it wakes from a standby that switched Bluetooth audio off.")));
  };
  // the late stop (developer): the one switch's option and its wait (the experimental
  // strategies of §11 are gone, the app writes strategy 0); a firmware without the option shows nothing here
  const drawLate = (p: BtSourcePolicy) => {
    const items = lateStopBox(p);
    lateBox.hidden = !items.length;
    lateBox.replaceChildren(...(items.length ? [h("p", {}, t("When switching to another device")), ...items,
      h("p", { class: "muted" }, t("For trying on the piano: lasts until it is switched off."))] : []));
  };
  // the opt-in late stop (firmware after ota_main 87579f5c): one toggle, RAM on the piano;
  // its wait after the paused report (firmware after 753d97fb): 0.1 / 0.5 / 1 / 2 / 4.5 s (0.1 s = the default
  // since before 1 s)
  const lateStopBox = (p: BtSourcePolicy) => {
    if (p.options == null || p.reconnect == null) return [];
    const on = !!(p.options & SRC_OPTION.lateStop);
    const wait = p.lateWait == null ? 45 : p.lateWait || 10;   /* 0: only an app that wrote 0 - the old default 1.0 s */
    return [
      h("div", { class: "segmented", id: "bt-src-latestop" },
        h("button", { type: "button", class: on ? undefined : "on", "data-latestop": "0", onclick: () => void setPol({ ...p, strategy: p.strategy ?? 0, options: (p.options ?? 0) & ~SRC_OPTION.lateStop }) }, t("Off")),
        h("button", { type: "button", class: on ? "on" : undefined, "data-latestop": "1", onclick: () => void setPol({ ...p, strategy: p.strategy ?? 0, options: (p.options ?? 0) | SRC_OPTION.lateStop }) }, t("If the PC holds its sound, the piano stops it (experiment)"))),
      ...(p.lateWait == null ? [] : [h("p", {}, t("Wait after it reports paused")),
        h("div", { class: "segmented", id: "bt-src-latewait" }, ...LATE_WAITS.map((v) =>
          h("button", { type: "button", class: wait === v ? "on" : undefined, "data-latewait": String(v),
            onclick: () => void setPol({ ...p, strategy: p.strategy ?? 0, lateWait: v }) }, t("{s} s", { s: String(v / 10) }))))]),
      h("p", { class: "muted", id: "bt-src-latestop-why" }, on
        ? t("On: when you switch away from a device with remote control that reports paused but keeps its sound open {s} s later (a PC holds it about 10 s, a phone about 3 s), the piano stops its sound stream so the switch is quicker. If the PC is then silent: restart the Bluetooth audio module; if still silent, restart its Bluetooth audio device in Windows, and turn this off.", { s: String(wait / 10) })
        : t("Off: the piano waits until the device lets its sound go by itself (a PC about 10 s).")),
    ];
  };
  const drawAutocall = (ac: { state: number; op: number; rc: number } | null, line: string) => {
    if (!ac) {
      autocall.hidden = true;
      return;
    }
    autocall.hidden = false;
    acRead.disabled = !!ac.op;
    if (ac.op) acText.textContent = t("Writing / reading the module setting…");
    else if (ac.state === AC_STATE.on) acText.textContent = t("On — the module reconnects the last device by itself ({line}).", { line });
    else if (ac.state === AC_STATE.off) {
      const rc = last?.policy.reconnect;
      acText.textContent = rc === SRC_RECONNECT.off ? t("Off — the piano does not reconnect devices by itself either (see below).")
        : rc === SRC_RECONNECT.pref ? t("Off — the piano reconnects the preferred device.")
        : t("Off — the piano reconnects the preferred (or last picked) device.");
    }
    else acText.textContent = t("Not read yet.");
    const want = ac.state === AC_STATE.on ? AC_OP.off : ac.state === AC_STATE.off ? AC_OP.restore : 0;
    if (!want || ac.op) {
      acAsk.hidden = true;
      acConfirm.hidden = true;
      return;
    }
    if (want !== acWant) {
      acWant = want;
      acConfirm.hidden = true;
    }
    if (acAskPending && want === AC_OP.off) {
      acAskPending = false;
      acConfirm.hidden = false;   // asked from the loop warning: the confirmation, still nothing written
    }
    if (acConfirm.hidden) acAsk.hidden = false;
    acAsk.textContent = want === AC_OP.off ? t("Turn AUTOCALL off…") : t("Restore AUTOCALL…");
    acWhy.textContent = want === AC_OP.off
      ? t("This changes a setting stored in the Bluetooth module. With AUTOCALL off the module no longer calls the last device by itself; the piano calls the preferred (or last picked) device instead, with pauses, and never in standby. Restore writes back exactly: {line}", { line })
      : t("This writes the stored module setting back: {line}", { line });
    acYes.textContent = want === AC_OP.off ? t("Turn it off") : t("Restore it");
  };
  let acLine = "";
  let acAskPending = false;   // the loop warning's 「AUTOCALL 끄기…」 before AUTOCALL was read: open the confirm once read
  /** the loop warning's action: the same confirmation as the AUTOCALL card (nothing is written here) */
  const askAutocallOff = () => {
    devFold.open = true;   // the AUTOCALL box lives in the closed 「고급 · 개발자용」 fold
    if (last?.autocall?.state === AC_STATE.on && acWant === AC_OP.off) {
      acConfirm.hidden = false;
      acAsk.hidden = true;
      acConfirm.scrollIntoView?.({ block: "nearest" });
    } else {
      acAskPending = true;
      void acDo(AC_OP.read);
    }
  };
  const drawLevel = (l: BtLevel | null) => {
    levelLine.hidden = !l;
    if (!l) return;
    const v = levelVerdict(l);
    const db = levelDb(Math.max(l.peakL, l.peakR));
    levelLine.dataset.verdict = v;
    // the level itself is for developers (ko text says only 있음 / 없음, 15_ko_labels): in the tooltip
    levelLine.title = v === "signal" ? `${db} dBFS` : "";
    levelLine.textContent = v === "signal" ? t("Piano BT input: signal ({db} dBFS)", { db })
      : v === "silent" ? t("Piano BT input: no signal")
      : t("Piano BT input: measuring…");
  };
  const drawHealth = (s: BtSources) => {
    const hl = s.health;
    const loop = !!hl && !!(hl.flags & MOD_HEALTH.loop);
    const recent = !!hl && hl.reboots > 0 && hl.sinceS !== null && hl.sinceS < 120;
    const pin = !!hl && !!(hl.flags & MOD_HEALTH.pin);   // the decided module settings not kept
    health.hidden = !loop && !recent && !pin;
    health.classList.toggle("warn", loop);
    if (!hl || health.hidden) return;
    healthText.textContent = loop
      ? t("The Bluetooth audio module keeps restarting by itself ({n} times). Remove “CA79 Audio” on the device that was connecting (e.g. the PC) and pair it again, or turn AUTOCALL off below. Meanwhile the piano does not reconnect devices by itself.", { n: hl.reboots })
      : recent ? t("The Bluetooth audio module restarted by itself {s} s ago.", { s: hl.sinceS ?? 0 })
      : t("The piano could not set the Bluetooth audio module's settings (device type, PC connection, audio route) this time — see the module lines.");
    healthAc.hidden = !loop || s.autocall?.state === AC_STATE.off;
  };
  const acDo = async (op: number) => {
    const p = port();
    if (!p) return;
    const r = await p.autocall(op);
    acMsg.textContent = !r ? t("No reply.") : r.rc === 0 ? (op === AC_OP.read ? t("Reading…") : t("Sent — reading it back…")) : t(RC_TEXT[r.rc] ?? "Rejected ({c}).", { c: r.rc });
    if (r) acLine = r.line;
    void load();
  };
  const draw = (s: BtSources) => {
    last = s;
    status.textContent = !s.moduleOn ? t("The Bluetooth audio module is off (standby or the Bluetooth Audio setting).")
      : !s.devices.length ? t("No device is connected to “CA79 Audio”.") : "";
    status.hidden = !status.textContent;
    const seen = new Set<string>();
    s.devices.forEach((d, i) => {
      const key = rowKey(d, i);
      seen.add(key);
      let r = rows.get(key);
      if (!r) {
        r = makeRow(key);
        rows.set(key, r);
        listEl.append(r.el);
      }
      r.el.setAttribute("data-state", String(d.state));
      r.el.setAttribute("data-state2", String(d.state2));
      const txt = sourceText(d, s, i);
      if (r.text.textContent !== txt) r.text.textContent = txt;
      const sel = !!(d.state2 & SRC_DEV2.selected);
      r.el.classList.toggle("selected", sel);
      r.badge.hidden = !sel;
      const noAddr = !(d.state & SRC_DEV.addr);
      r.use.hidden = sel && !!(d.state & SRC_DEV.stream);
      r.use.disabled = noAddr || !!(d.state2 & SRC_DEV2.target);
      r.close.disabled = noAddr;
      r.pref.disabled = noAddr;
      const prefTxt = d.state & SRC_DEV.pref ? t("Not preferred") : t("Prefer");
      if (r.pref.textContent !== prefTxt) r.pref.textContent = prefTxt;
    });
    for (const [key, r] of rows) {
      if (seen.has(key)) continue;
      disarm(r);
      r.el.remove();
      rows.delete(key);
    }
    if (switching && s.sw) {
      const target = macShort(switching.split(":").map((x) => parseInt(x, 16)));
      const txt = switchText(s, target);
      if (txt) {
        swLine.hidden = false;
        swLine.textContent = txt;
      }
      swLine.dataset.result = s.sw.phase ? "" : String(s.sw.result);
    } else if (!s.sw) {
      swLine.hidden = true;
    }
    const key = JSON.stringify(s.policy);
    if (key !== polKey) {
      polKey = key;
      drawPolicy(s.policy);
    }
    drawAutocall(s.autocall, acLine || "SET CONTROL AUTOCALL 19 5000 A2DP");
    rcAc.hidden = rcBox.hidden || s.autocall?.state !== AC_STATE.on;
    drawHealth(s);
    drawNow(s);
  };
  // the line at the top: which device the piano plays now (the selected one), so nobody has to read the rows for it
  const drawNow = (s: BtSources) => {
    nowLine.hidden = !s.moduleOn || !s.devices.length;
    if (nowLine.hidden) return;
    const sel = s.devices.find((d) => d.state2 & SRC_DEV2.selected && d.state & SRC_DEV.addr);
    const txt = !sel ? t("No device picked yet — press “Listen to this one” on the device you want to hear.")
      : sel.state & SRC_DEV.stream ? t("Playing through the piano: {dev}", { dev: macShort(sel.addr) })
      : t("Picked: {dev} — no sound from it yet", { dev: macShort(sel.addr) });
    if (nowLine.textContent !== txt) nowLine.textContent = txt;
    nowLine.dataset.playing = sel && sel.state & SRC_DEV.stream ? "1" : "";
  };
  const drawMidi = () => {
    const l = opts.link?.() ?? null;
    midiLine.textContent = !l ? t("This app is not connected to the piano.")
      : l.kind === "ble" ? t("This app holds the piano's Bluetooth MIDI link ({name}).", { name: l.name })
      : t("This app is connected over {kind} ({name}) — the Bluetooth MIDI link is free for another device.", {
        kind: l.kind === "kwmcore" ? t("the panel's built-in link") : l.kind === "websocket" ? t("the simulator") : "USB / MIDI", name: l.name,
      });
    const co = opts.coBound?.() ?? "";
    midiCo.textContent = co;
    midiCo.hidden = !co;
  };
  const pullLog = async () => {
    const p = port();
    if (!logBox.open || !p) return;
    for (let i = 0; i < 10; i++) {
      const c = await p.moduleLog(logNext ?? 0);
      if (!root.isConnected) return;
      if (!c) {
        if (!logLines.length) logPre.textContent = t("This piano firmware keeps no module lines.");
        return;
      }
      const recs = logNext === null ? c.records : c.records.filter((r) => ((r.seq - logNext!) & 0x3fff) < 0x2000);
      if (logNext !== null && recs.length && recs[0]!.seq !== logNext) logLines.push(t("… {n} older lines were overwritten", { n: (recs[0]!.seq - logNext) & 0x3fff }));
      for (const r of recs) logLines.push(logLine(r));
      if (!recs.length) {
        logNext = c.next;
        break;
      }
      logNext = (recs[recs.length - 1]!.seq + 1) & 0x3fff;
      if (logNext === c.next) break;
    }
    logLines.splice(0, Math.max(0, logLines.length - LOG_KEEP));
    logPre.textContent = logLines.length ? logLines.join("\n") : t("No module lines yet.");
  };
  const born = Date.now();
  /** polls while the page is shown; a page never put in the document stops after 5 s */
  const alive = () => {
    if (root.isConnected) attached = true;
    return attached ? root.isConnected : Date.now() - born < 5000;
  };
  // A hidden window (minimized desktop app, background tab) asks nothing: every 1D 04 / 1D 08 is a frame the piano
  // receives, and an idle app must not look busy. Back in view -> at once.
  const hidden = () => typeof document !== "undefined" && document.visibilityState === "hidden";
  const onVisible = () => {
    if (!alive()) document.removeEventListener("visibilitychange", onVisible);
    else if (!hidden()) void load();
  };
  if (typeof document !== "undefined") document.addEventListener("visibilitychange", onVisible);
  const load = async () => {
    clearTimeout(timer);
    if (!alive()) return;
    if (hidden()) return; // onVisible starts it again
    drawMidi();
    const p = port();
    const s = p ? await p.sources() : null;
    if (!alive()) return;
    if (s) {
      draw(s);
      if (s.autocall && !acLine) {
        const a: BtAutocall | null = await p!.autocall(AC_OP.state);
        if (a) acLine = a.line;
      }
    } else if (!last) {
      status.hidden = false;
      status.textContent = !p ? t("Connect to the piano to see its Bluetooth audio devices.") : t("This piano firmware cannot list the Bluetooth audio devices.");
      msg.textContent = p ? t(BT_AUDIO_SWITCH_HINT) : "";
      autocall.hidden = true;
      behaviour.hidden = true;   // no source manager on this payload: no policy, no module restart op
      restartBox.hidden = true;
    }
    if (s) behaviour.hidden = restartBox.hidden = false;
    drawLevel(s ? await p!.level() : null);   // null: a payload without 1D 0B - the line stays hidden
    if (!alive()) return;
    await pullLog();
    if (!alive()) return;
    timer = setTimeout(() => void load(), opts.refreshMs ?? 2000);
  };
  logBox.addEventListener("toggle", () => void pullLog());
  setTimeout(() => void load(), 0);
  return root;
}

/** a device's row key: its address, or its table slot while the address is not known yet */
function rowKey(d: BtSource, i: number): string {
  return d.state & SRC_DEV.addr ? macText(d.addr) : `slot-${i}`;
}
