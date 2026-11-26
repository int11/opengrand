// The header's power button (owner 2026-11-15): THE way to put the
// piano into OpenGrand standby and wake it, next to the connect button on every surface. Piano on → 「대기로」 sends
// op 19 01 (what the Settings card's 「지금 대기」 sent); in standby → 「깨우기」 sends 19 02 (the card's 「대기 해제」).
// Shown only while connected to a piano that reports the power feature (Identify feat1 0x10); the state comes from
// 19 00 at each connect, the piano's standby notices (59 7F …) and the answer to each press. No real power off here:
// the remote off (19 03) is only in Settings → 전원 → 개발자용.
import { t } from "../i18n.ts";
import { NgClient, ngHas, NgPower, PolicyResult, parseStandbyNotice, type StandbyReply, type StandbyStateName } from "../protocol/ngext.ts";
import type { PianoSession } from "../protocol/session.ts";
import { type ConnState, quietRejoin } from "../state/connection.ts";
import { h } from "./dom.ts";
import { identifyShared } from "./identify.ts";

const ICON = '<svg viewBox="0 0 24 24" focusable="false"><path d="M12 3v8"/><path d="M6.6 6.6a7.5 7.5 0 1 0 10.8 0"/></svg>';

export interface PowerButton {
  el: HTMLButtonElement;
  /** the piano's standby state as last seen ("" before the first answer) */
  state(): StandbyStateName | "";
  /** ask 19 00 again (after another way changed the state) */
  refresh(): void;
  /** redraw the label in the current language (the header is not rebuilt on a language change) */
  redraw(): void;
  /** the 10 s 19 00 poll is running (only while shown: live link to a piano with the power feature) */
  polling(): boolean;
}

/** One line for a refused / unanswered press (the header shows it under the bar). */
export function powerRcText(r: StandbyReply | null): string {
  if (!r) return t("No answer from the piano.");
  if (r.rc === PolicyResult.NotBound) return t("The piano is controlled by another device.");
  if (r.rc === PolicyResult.NoHooks) return t("Standby needs the runtime hooks (OTA loader v2).");
  return t("The piano rejected this value.");
}

/**
 * `say` gets a line after a press that did not work ("" clears it); `changed` runs whenever the state changes (the
 * Settings card redraws its state line).
 */
export function powerButton(session: () => PianoSession | null, conn: { state: ConnState; onChange(fn: (s: ConnState) => void): unknown },
  opts: { say?: (msg: string) => void; changed?: (st: StandbyStateName) => void; pollMs?: number } = {}): PowerButton {
  const ico = h("span", { class: "ico", "aria-hidden": "true" });
  ico.innerHTML = ICON;
  const lbl = h("span", { class: "lbl" });
  const el = h("button", { type: "button", class: "powerbtn", id: "power-btn", hidden: true }, ico, lbl) as HTMLButtonElement;
  let st: StandbyStateName | "" = "";
  let bound: PianoSession | null = null;
  let offNotice: (() => void) | null = null;
  let gen = "";
  let poll: ReturnType<typeof setInterval> | null = null;
  let attachSeq = 0;
  const draw = () => {
    const sleeping = st === "standby";
    const waking = st === "waking";
    lbl.textContent = sleeping ? t("Wake") : waking ? t("Waking…") : t("To standby");
    el.dataset.action = sleeping ? "wake" : "standby-now";
    el.dataset.state = st;
    el.disabled = waking || st === "off-pending" || st === "off" || !st;
    const full = sleeping ? t("Piano: in standby — press to wake it") : t("Piano: on — press to put it into standby");
    el.title = full;
    el.setAttribute("aria-label", full);
  };
  const set = (next: StandbyStateName) => {
    if (next === st) return;
    st = next;
    draw();
    opts.changed?.(next);
  };
  const live = () => ["ready", "waking", "displaced"].includes(conn.state.phase);
  // The piano sends its standby notice (59 7F) only to the stock-bound controller, never to the panel port — so while
  // the panel holds the binding the app hears nothing (field 2026-11-15: APO standby, button stayed 「대기로」;
  // power_cave.c notice()). Ask 19 00 every 10 s while shown: a read, not APO activity (power.h power_frame_quiet).
  // The timer exists only while the button is shown — started after the power feature was seen on a live link,
  // stopped on every connection change (lost / other piano / stock firmware never starts it).
  const stopPoll = () => {
    if (poll !== null) clearInterval(poll);
    poll = null;
  };
  const startPoll = () => {
    stopPoll();
    poll = setInterval(() => {
      if (!bound || el.hidden || st === "waking" || !live()) return;
      if (typeof document !== "undefined" && document.visibilityState === "hidden") return;
      void query();
    }, opts.pollMs ?? 10_000);
  };
  /** not attached: no session, no notice listener, no poll, no button */
  const detach = () => {
    attachSeq++;
    offNotice?.();
    offNotice = null;
    bound = null;
    stopPoll();
    st = "";
    el.hidden = true;
  };
  const attach = async () => {
    detach();
    const my = attachSeq;
    const s = session();
    if (!s || !live()) return;
    const id = await identifyShared(s);
    if (my !== attachSeq || session() !== s || !live()) return; // the connection changed meanwhile
    if (!ngHas(id, "power")) return; // stock / no power port: no button, no poll
    bound = s;
    offNotice = s.onMidi((m) => {
      const n = parseStandbyNotice(m);
      if (!n) return;
      set(n);
      // a notice reached us: this build sends them to every core-bound controller (ota_main c1343a8a+),
      // so the 10 s 19 00 poll is not needed for the rest of this connection (kept for older slots until then)
      stopPoll();
    });
    el.hidden = false;
    draw();
    // ngver 2+ builds send the standby notice to us even while the panel holds the binding: no 19 00 poll at all
    if (!ngHas(id, "standbyfanout")) startPoll();
    await query();
  };
  const query = async () => {
    const s = bound;
    if (!s || !live()) return;
    const q = await new NgPower(new NgClient(s)).standby("query");
    if (q && bound === s) set(q.state);
  };
  el.addEventListener("click", async () => {
    const s = bound;
    if (!s || el.disabled) return;
    const wake = st === "standby";
    el.disabled = true;
    const r = await new NgPower(new NgClient(s)).standby(wake ? "wake" : "enter");
    opts.say?.(r?.rc === PolicyResult.Ok ? "" : powerRcText(r));
    if (r) set(r.state);
    draw();
  });
  conn.onChange((c) => {
    if (quietRejoin(c)) return; // the file channel's rejoin: same session, keep the button
    const now = live() ? `${c.gen}` : "";
    if (now === gen) return;
    gen = now;
    void attach();
  });
  return { el, state: () => st, refresh: () => void query(), redraw: () => draw(), polling: () => poll !== null };
}
