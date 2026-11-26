// Settings > Power (「전원」), the OpenGrand part: standby / off / wake now, what the auto power-off does, the
// standby limit, Bluetooth audio in standby and how long it keeps the piano on — 0x7D op 0x18 / 0x19 (REMOTE_PROTOCOL.md
// section 10.4). Shown only when the piano answers Identify with the power feature bit; a stock piano does not answer
// 0x7D at all and keeps its stock auto power-off (the "Auto power off" row above it in the same card).
import { t } from "../i18n.ts";
import type { PianoSession } from "../protocol/session.ts";
import { apoSecondsLeft, type ApoTestReply, CAP_NONE, HOLD_UNLIMITED, NgClient, ngHas, NgPower, OffReason, POWER_POLICY_DEFAULT, PolicyResult, type PowerPolicy, type PowerPolicyReply, type RemoteOffReply, type StandbyStateName } from "../protocol/ngext.ts";
import type { AppContext } from "./context.ts";
import { btAudioText } from "./btaudio.ts";
import { confirmTap } from "./confirm2.ts";
import { h } from "./dom.ts";
import { currentScope, onDispose } from "./scope.ts";
import { gpioSection } from "./gpio.ts";
import { identifyShared } from "./identify.ts";

// 300 = 5 h, the default (owner decision 2026-11-02)
const HOLD_CHOICES = [0, 30, 60, 120, 240, 300, 480, HOLD_UNLIMITED];
// CAP_NONE: standby until woken (ota_power3 and later; older payloads answer rc 1)
const CAP_CHOICES = [60, 120, 240, 480, 720, 1440, CAP_NONE];

const minutes = (m: number) => (m % 60 === 0 ? t("{n} h", { n: m / 60 }) : t("{n} min", { n: m }));
const capLabel = (m: number) => (m === CAP_NONE ? t("No limit (until woken)") : minutes(m));
const holdLabel = (m: number) => (m === 0 ? t("Off (stock)") : m === HOLD_UNLIMITED ? t("While playing (no limit)") : minutes(m));
/** The Bluetooth audio rule in one sentence, for the chosen max hold. */
// + one sentence for the play status (owner decision 2026-11-03): a source that reports paused / stopped
// over AVRCP does not hold although its stream stays open (Windows)
export const holdRuleText = (m: number): string =>
  m === 0 ? t("Bluetooth audio does not keep the piano on: the normal auto power-off applies.")
  : (m === HOLD_UNLIMITED ? t("While Bluetooth audio is playing, the piano does not switch itself off; when it stops, the normal auto power-off time counts from then.")
    : t("While Bluetooth audio is playing, the piano does not switch itself off for up to {n}; when it stops, the normal auto power-off time counts from then; after {n} it switches off right away (unless you played or used it within the auto power-off time).", { n: minutes(m) }))
    + " " + t("A device that reports it is paused counts as stopped.");
/** The same rule in a few words, shown under the setting (the full rule is behind 「자세히」). */
export const holdShortText = (m: number): string =>
  m === 0 ? t("Bluetooth audio does not keep the piano on.")
  : m === HOLD_UNLIMITED ? t("While Bluetooth audio plays, the piano stays on.")
  : t("While Bluetooth audio plays, the piano stays on for up to {n}.", { n: minutes(m) });

function select(label: string, name: string, choices: [number, string][], value: number, on: (v: number) => void): HTMLElement {
  if (!choices.some(([v]) => v === value)) choices = [...choices, [value, String(value)]];
  const sel = h("select", { name }, ...choices.map(([v, l]) => h("option", { value: String(v), selected: v === value }, l))) as HTMLSelectElement;
  sel.addEventListener("change", () => on(Number(sel.value)));
  return h("label", { class: "row" }, h("span", {}, label), sel);
}

export function powerPolicySection(ctx: AppContext): HTMLElement {
  // a block inside the Settings tab's 「전원」 card (screens/settings.ts, after the stock auto power-off row)
  const el = h("div", { id: "power-policy", hidden: true });
  const s = ctx.session();
  if (!s || s.state !== "ready") return el;
  const power = new NgPower(new NgClient(s));
  const note = h("p", { class: "muted", id: "power-status" });

  const errorText = (r: PowerPolicyReply | null): string =>
    !r ? t("No answer from the piano.")
    : r.rc === PolicyResult.Ok ? t("Saved on the piano.")
    : r.rc === PolicyResult.NotBound ? t("The piano is controlled by another device.")
    : r.rc === PolicyResult.SaveFailed ? t("Applied, but the piano could not save it.")
    : t("The piano rejected this value.");
  // the same wording as the header badge (ui/btaudio.ts): what Bluetooth audio does now, standby included
  const btLine = h("p", { id: "bt-audio-now", hidden: true });
  const showDiag = async () => {
    const d = await power.diag();
    if (!d) return; // only the ota_power slot payload answers op 0x1A
    // the piano went to standby (APO, test) or woke while the card is shown: redraw so the state line matches
    // (field 2026-11-11, TH5: the state appeared only after a tab switch)
    if (shown && shown.standby !== null && d.state !== shown.standby) render(shown.pol, d.state);
    btLine.textContent = t("Now: {state}", { state: btAudioText(d) });
    btLine.hidden = false;
  };

  let tvWake = false; // the piano acts on btWake (Identify feat1 0x40)
  let shown: { pol: PowerPolicy; standby: StandbyStateName | null } | null = null;
  // One write at a time, each built when it is sent from the policy the piano last confirmed (two writes in flight
  // both took the first `58 01` reply, and the second one, built from the policy drawn before the first, undid it)
  let writes: Promise<void> = Promise.resolve();
  const write = (job: () => Promise<void>) => {
    note.textContent = t("Saving…");
    writes = writes.then(job, job);
  };
  const render = (pol: PowerPolicy, standby: StandbyStateName | null) => {
    shown = { pol, standby };
    const apply = (patch: Partial<PowerPolicy>) => write(async () => {
      const cur = shown ?? { pol, standby };
      const r = await power.set({ ...cur.pol, ...patch });
      // no answer: draw the piano's last known policy again, not the value the select was moved to
      render(r ? r.policy : cur.pol, shown?.standby ?? cur.standby);
      // payloads before ota_power3 do not know the "no limit" value (REMOTE_PROTOCOL 10.4)
      note.textContent = r?.rc === PolicyResult.BadValue && patch.standbyCapMin === CAP_NONE
        ? t("This firmware has no \"no limit\" standby (it needs ota_power3).") : errorText(r);
    });
    const connected = h("input", { type: "checkbox", name: "holdConnected", checked: pol.holdConnected }) as HTMLInputElement;
    connected.addEventListener("change", () => void apply({ holdConnected: connected.checked }));
    // TV wake: keep Bluetooth audio on in standby, a stream start (TV / phone plays) wakes the piano
    const btWake = h("input", { type: "checkbox", name: "btWake", checked: pol.btWake }) as HTMLInputElement;
    btWake.addEventListener("change", () => void apply({ btWake: btWake.checked }));
    const autoWake = h("input", { type: "checkbox", name: "autoWake", checked: ctx.settings.autoWake }) as HTMLInputElement;
    autoWake.addEventListener("change", () => ctx.updateSettings({ autoWake: autoWake.checked }));
    // standby now (19 01) / wake (19 02): the header's power button since owner 2026-11-15 (ui/powerbtn.ts) - not here.
    // The remote off (19 03), the auto power-off test, the diagnostic line and 「GPIO 읽기」 are field-test tools: the
    // Info tab's 「개발자용」 card (powerDevSection below, owner 2026-11-16).
    // Order (owner 2026-11-14 / 11-15): the state line first, then the
    // settings, the mechanism behind a closed 「자세히」. Standby / wake: the header's power button.
    el.replaceChildren(
      ...(standby && standby !== "on" ? [h("p", { id: "standby-state" }, t("The piano is in standby."), " ", t("Wake it with the power button at the top."))] : []),
      btLine,
      select(t("When auto power off expires"), "apoAction", [[1, t("Standby")], [0, t("Power off")]], pol.apoStandby ? 1 : 0, (v) => void apply({ apoStandby: v === 1 })),
      select(t("Standby limit"), "standbyCap", CAP_CHOICES.map((m) => [m, capLabel(m)] as [number, string]), pol.standbyCapMin, (v) => void apply({ standbyCapMin: v })),
      ...(tvWake ? [h("label", { class: "row" }, h("span", {}, t("Keep Bluetooth audio on in standby (wake on TV sound)")), btWake)] : []),
      h("p", { class: "muted", id: "standby-rule" }, tvWake && pol.btWake
        ? t("In standby the speakers are off but Bluetooth audio stays on: when the TV or phone starts playing, the piano wakes up.")
        : t("In standby the speakers and Bluetooth audio are off; a key, the panel or this app wakes the piano."),
        " ", pol.standbyCapMin === CAP_NONE
          ? t("No standby limit: it stays in standby until something wakes it and never switches itself off.")
          : t("After the standby limit it switches off (never while it is saving).")),
      select(t("Bluetooth audio keeps the piano on"), "maxHold", HOLD_CHOICES.map((m) => [m, holdLabel(m)] as [number, string]), pol.maxHoldMin, (v) => void apply({ maxHoldMin: v })),
      h("p", { class: "muted", id: "hold-rule" }, holdShortText(pol.maxHoldMin)),
      h("label", { class: "row" }, h("span", {}, t("Also while only connected")), connected),
      h("details", { id: "hold-detail", class: "more" }, h("summary", {}, t("Details")), h("p", { class: "muted" }, holdRuleText(pol.maxHoldMin))),
      // app-local (localStorage): wake the piano from standby when this app connects (main.ts)
      ...(ctx.panel ? [] : [h("label", { class: "row" }, h("span", {}, t("Wake the piano when this app opens")), autoWake)]),
      h("div", { class: "transport" },
        h("button", { "data-action": "power-defaults", onclick: () => write(async () => {
          const r = await power.reset();
          if (r) render(r.policy, shown?.standby ?? standby);
          note.textContent = errorText(r);
        }) }, t("Restore defaults"))),
      note,
    );
    el.hidden = false;
  };

  void (async () => {
    const id = await identifyShared(s);
    if (!ngHas(id, "power")) return; // stock (or older OpenGrand): stock APO only
    tvWake = ngHas(id, "tvwake");
    const [r, sb] = [await power.get(), await power.standby("query")];
    render(r?.policy ?? POWER_POLICY_DEFAULT, sb?.state ?? null);
    if (!r) note.textContent = t("No answer from the piano.");
    void showDiag();
  })();
  return el;
}

/** Info › 「개발자용」 › 전원 (owner 2026-11-16): the power field-test
 * tools that used to sit in Settings › 전원 › 개발자용 — the remote off (19 03, feat2 0x20) the way the auto power-off
 * goes, the auto power-off test 1 / 5 min (19 04), the diagnostic line (op 0x1A) and 「GPIO 읽기」 (op 0x17). Same ops,
 * same ids (`#remote-off-row` · `#apo-test` · `[data-action=apo-test-*|remote-off]` · `#apo-test-left` · `#power-diag` ·
 * `#gpio-diag`). Hidden until the piano answers Identify with the power bit; `onShow` tells the card to show itself. */
/** the APO test's countdown read (op 0x1A) interval - tests shorten it */
export const APO_TEST = { pollMs: 5000 };

export function powerDevSection(ctx: AppContext, onShow?: () => void): HTMLElement {
  const el = h("div", { id: "power-dev", hidden: true });
  const s = ctx.session();
  if (!s || s.state !== "ready") return el;
  const power = new NgPower(new NgClient(s));
  const note = h("p", { class: "muted", id: "power-dev-status" });
  const diagLine = h("p", { class: "muted", id: "power-diag", hidden: true });
  const apoLeft = h("span", { class: "muted", id: "apo-test-left" });
  // the APO test's countdown loop: one at a time (a new press or Cancel ends the previous one), gone with the screen
  const scope = currentScope();
  let apoSeq = 0;
  let apoTimer: ReturnType<typeof setTimeout> | null = null;
  const stopApo = () => {
    apoSeq++;
    if (apoTimer !== null) clearTimeout(apoTimer);
    apoTimer = null;
  };
  onDispose(stopApo);
  // 「GPIO 읽기」 (op 0x17, read only): built once, kept across re-renders (its notice
  // subscription lives with it); older payloads stay silent and the box says so
  const gpioBox = gpioSection(s);
  const tools = h("div", {});
  let remoteOff = false; // the piano answers 19 03 (Identify feat2 0x20, 09_p3_power)
  let shown: { pol: PowerPolicy; standby: StandbyStateName | null } | null = null;
  const showDiag = async () => {
    const d = await power.diag();
    if (!d) return; // only the ota_power slot payload answers op 0x1A
    // standby entered / left while shown (APO, the test): redraw so the off / test rows match
    if (shown && shown.standby !== null && d.state !== shown.standby) render(shown.pol, d.state);
    const left = apoSecondsLeft(d);
    apoLeft.textContent = left === null ? "" : t("{s} s left", { s: left });
    diagLine.textContent = t("Piano: {state}, hooks {hooks}, auto power-off counter {s} s, BT audio {bt}, store {store}", {
      state: d.state, hooks: "0x" + d.hooks.toString(16), s: Math.round(d.counter / 50), bt: d.btAudio, store: d.store,
    });
    diagLine.hidden = false;
  };
  const render = (pol: PowerPolicy, standby: StandbyStateName | null) => {
    shown = { pol, standby };
    // remote off (19 03): where the auto power-off goes, after a confirmation; the piano refuses while recording,
    // during a file operation or a flash write (rc 6 + reason)
    const off = remoteOff && standby === "on"
      ? h("button", { "data-action": "remote-off", onclick: (e: Event) => {
          const msg = pol.apoStandby
            ? t("Put the piano into standby now? Speakers go silent; a key, the panel or this app wakes it.")
            : t("Switch the piano off now? It can only be switched on again with its power button.");
          confirmTap(e.currentTarget, msg, async () => {
            const r = await power.remoteOff();
            note.textContent = remoteOffText(r);
            if (r) render(pol, r.state);
            void showDiag();
          });
        } }, pol.apoStandby ? t("Turn off (standby)") : t("Turn off"))
      : null;
    // APO test shortcut (19 04): the auto power-off in 1 / 5 min through the normal path (RAM only)
    const apoTest = (secs: number) => async () => {
      stopApo();
      const my = apoSeq;
      const r = await power.apoTest(secs);
      if (my !== apoSeq || scope?.disposed) return; // another press (its reply is the one shown) or the screen left
      note.textContent = apoTestText(r);
      if (r) render(pol, r.state);
      void showDiag();
      // the countdown from op 0x1A (a read: not activity) every 5 s while the block is shown
      const until = Date.now() + (secs + 15) * 1000;
      const again = () => {
        apoTimer = null;
        if (my !== apoSeq || scope?.disposed) return;
        if (!el.isConnected || el.hidden || Date.now() > until || r?.rc !== PolicyResult.Ok || !secs) return;
        if (typeof document === "undefined" || document.visibilityState !== "hidden") void showDiag();
        apoTimer = setTimeout(again, APO_TEST.pollMs);
      };
      if (el.isConnected) apoTimer = setTimeout(again, APO_TEST.pollMs);
    };
    const testRow = remoteOff && standby === "on"
      ? h("div", { class: "row", id: "apo-test" }, h("span", {}, t("Auto power-off test")),
          h("button", { "data-action": "apo-test-60", onclick: apoTest(60) }, t("{n} min", { n: 1 })),
          h("button", { "data-action": "apo-test-300", onclick: apoTest(300) }, t("{n} min", { n: 5 })),
          h("button", { "data-action": "apo-test-cancel", onclick: apoTest(0) }, t("Cancel")),
          apoLeft)
      : null;
    tools.replaceChildren(
      ...(off ? [h("div", { class: "row", id: "remote-off-row" }, h("span", {}, t("Off the way auto power-off does")), off)] : []),
      ...(testRow ? [testRow] : []));
  };
  el.append(h("h4", {}, t("Power")), tools, note, diagLine, gpioBox);

  void (async () => {
    const id = await identifyShared(s);
    if (!ngHas(id, "power")) return; // stock (or older OpenGrand): nothing to test
    remoteOff = ngHas(id, "remoteoff");
    const [r, sb] = [await power.get(), await power.standby("query")];
    render(r?.policy ?? POWER_POLICY_DEFAULT, sb?.state ?? null);
    el.hidden = false;
    onShow?.();
    void showDiag();
  })();
  return el;
}

/** What a remote off (19 03) did, for the status line. */
export function remoteOffText(r: RemoteOffReply | null): string {
  if (!r) return t("No answer from the piano.");
  if (r.rc === PolicyResult.Ok) return r.state === "standby" ? t("The piano is in standby.") : t("The piano is switching off.");
  if (r.rc === PolicyResult.NotBound) return t("The piano is controlled by another device.");
  if (r.rc === PolicyResult.BadState) return t("The piano is not on.");
  if (r.rc === PolicyResult.Busy)
    return r.why === OffReason.Recording ? t("Not now: the piano is recording. Stop the recording first.")
      : r.why === OffReason.FileOp ? t("Not now: the piano is busy with a file. Try again in a moment.")
      : t("Not now: the piano is saving. Try again in a moment.");
  return t("The piano rejected this value.");
}

/** What an APO test (19 04) did, for the status line. */
export function apoTestText(r: ApoTestReply | null): string {
  if (!r) return t("No answer from the piano (this firmware may not have the test).");
  if (r.rc === PolicyResult.Ok)
    return r.secs === 0 ? t("Auto power-off: the full menu time again.")
      : r.held ? t("Bluetooth audio is playing: the auto power-off waits until it stops.")
      : t("Auto power-off in {s} s unless you play or change a setting.", { s: r.secs });
  if (r.rc === PolicyResult.ApoOff) return t("Auto power off is set to Off on the piano.");
  if (r.rc === PolicyResult.NotBound) return t("The piano is controlled by another device.");
  if (r.rc === PolicyResult.BadState) return t("The piano is not on.");
  return t("The piano rejected this value.");
}

/** App start-up wake (owner decision 2026-11-01): after
 * connect + sync, ask the standby state (op 19 00) and, if the piano is in standby (1), wake it (19 02).
 * 19 00 / 19 02 answer on any port, so this works even when another device (the panel) holds the
 * controller binding. Returns true when it woke the piano. The caller checks the app setting. */
export async function wakeOnConnect(s: PianoSession): Promise<boolean> {
  const ng = new NgClient(s);
  const id = await ng.identify();
  if (!ngHas(id, "power")) return false; // stock or no power port
  const power = new NgPower(ng);
  const q = await power.standby("query");
  if (q?.state !== "standby") return false;
  const r = await power.standby("wake");
  return r?.rc === PolicyResult.Ok;
}
