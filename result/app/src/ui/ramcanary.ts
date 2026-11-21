// Info › 개발자용: 「RAM 카나리」 — op 0x20 of ota_main ngver 4 (REMOTE_PROTOCOL.md section 10.13). Pick the blocks (T1-A + T1-B
// by default), arm (this app must be the bound controller), read, disarm. The piano fills the blocks a bounded step per
// engine-loop pass and keeps checking them; the view lists per block whether it is filled, how many sweeps passed,
// how many words changed and where. Plain text rows with data-* attributes so an automated test can read the result.
import { t } from "../i18n.ts";
import type { PianoSession } from "../protocol/session.ts";
import {
  NgClient, NgRamCanary, RAM_CANARY_DEFAULT_MASK, RAM_CANARY_REGIONS, type RamCanaryPhase, type RamCanaryRegion, type RamCanaryStatus,
} from "../protocol/ngext.ts";
import { h } from "./dom.ts";

const hex32 = (v: number) => "0x" + (v >>> 0).toString(16).toUpperCase().padStart(8, "0");
const kb = (lo: number, hi: number) => Math.round((hi - lo) / 1024);
const phaseText = (p: RamCanaryPhase) => (p === "filling" ? t("filling") : p === "verifying" ? t("checking") : t("off"));
const names = (mask: number) => RAM_CANARY_REGIONS.filter((_r, i) => mask & (1 << i)).map((r) => r.name).join(" + ") || "–";

/** One status line: "checking · 1234 passes · filled 2024448 · checked 4048896 words · step 180 µs (longest 230 µs)". */
export function canaryStatusText(s: RamCanaryStatus): string {
  return t("{phase} ({armed}) · {passes} passes · filled {filled} · checked {checked} words · step {last} µs (longest {max} µs)", {
    phase: phaseText(s.phase), armed: names(s.armed), passes: s.passes, filled: s.filledWords, checked: s.verifiedWords, last: s.lastUs, max: s.maxUs,
  });
}

/** One block: "T1-B: checking · 12 sweeps · changed now 3 (most 3) · first 0x41944100 = 0x12345678 (sweep 2) · 0x41944100–0x41964000". */
export function canaryRegionText(r: RamCanaryRegion): string {
  const name = RAM_CANARY_REGIONS[r.index]?.name ?? `#${r.index}`;
  if (!r.armed && !r.sweeps && r.state === "off") return t("{name}: not armed", { name });
  const base = t("{name}: {state} · {sweeps} sweeps · changed now {changed} (most {max})", {
    name, state: phaseText(r.state), sweeps: r.sweeps, changed: r.changed, max: r.changedMax,
  });
  return r.firstSweep
    ? base + " · " + t("first {addr} = {value} (sweep {sweep}) · {lo}–{hi}", {
      addr: hex32(r.firstAddr), value: hex32(r.firstValue), sweep: r.firstSweep, lo: hex32(r.minAddr), hi: hex32(r.maxAddr),
    })
    : base;
}

export function ramCanarySection(session: () => PianoSession | null): HTMLElement {
  const status = h("p", { class: "muted", id: "canary-status" });
  const summary = h("p", { class: "muted", id: "canary-summary" });
  const rows = h("div", { id: "canary-regions" });
  const boxes = RAM_CANARY_REGIONS.map((r, i) =>
    h("input", { type: "checkbox", name: "canaryRegion", "data-region": i, checked: !!(RAM_CANARY_DEFAULT_MASK & (1 << i)) }) as HTMLInputElement);
  const picks = h("div", { id: "canary-pick" }, ...RAM_CANARY_REGIONS.map((r, i) =>
    h("label", { class: "row" }, h("span", {}, `${r.name} ${hex32(r.lo)}–${hex32(r.hi)} (${kb(r.lo, r.hi)} KB)`), boxes[i]!)));
  const client = () => {
    const s = session();
    return s ? new NgRamCanary(new NgClient(s)) : null;
  };
  const noAnswer = () => {
    status.textContent = t("No answer from the piano (this firmware may not have the RAM canary).");
  };

  const read = async () => {
    const c = client();
    if (!c) return;
    const st = await c.status();
    if (!st) return noAnswer();
    summary.textContent = canaryStatusText(st);
    summary.dataset.phase = st.phase;
    summary.dataset.dirty = String(st.dirty);
    const regs = await c.regions(Math.min(st.regions, RAM_CANARY_REGIONS.length));
    rows.replaceChildren(...regs.map((r, i) => r
      ? h("p", { class: "canary-region", "data-region": i, "data-state": r.state, "data-sweeps": r.sweeps, "data-changed": r.changed,
        "data-first": r.firstSweep ? hex32(r.firstAddr) : "" }, canaryRegionText(r))
      : h("p", { class: "canary-region muted", "data-region": i }, t("{name}: no answer", { name: RAM_CANARY_REGIONS[i]!.name }))));
    status.textContent = st.dirty ? t("Changed words found: those blocks are written by something else.") : t("Read.");
  };

  const arm = async () => {
    const c = client();
    if (!c) return;
    const mask = boxes.reduce((m, b, i) => (b.checked ? m | (1 << i) : m), 0);
    if (!mask) {
      status.textContent = t("Pick at least one block.");
      return;
    }
    const r = await c.arm(mask);
    if (!r) return noAnswer();
    status.textContent = r.rc === 0 ? t("Armed: {regions}. The piano fills them, then keeps checking.", { regions: names(r.armed) })
      : r.rc === 2 ? t("The piano is controlled by another device.") : t("The piano rejected this value.");
    if (r.rc === 0) await read();
  };

  const disarm = async () => {
    const c = client();
    if (!c) return;
    const r = await c.disarm();
    if (!r) return noAnswer();
    status.textContent = t("Disarmed. The results stay readable until the next arm.");
    await read();
  };

  return h("div", { id: "ram-canary", hidden: true },
    h("h4", {}, t("RAM canary")),
    h("p", { class: "muted" }, t("Fills unused RAM blocks with a pattern and watches whether anything else writes there - the field test before OpenGrand uses them. Nothing is written until you arm it; switching the piano off and on ends it.")),
    picks,
    h("div", { class: "inline acts" },
      h("button", { "data-action": "canary-arm", onclick: () => void arm() }, t("Arm")),
      h("button", { "data-action": "canary-read", onclick: () => void read() }, t("Read")),
      h("button", { "data-action": "canary-disarm", onclick: () => void disarm() }, t("Disarm"))),
    status, summary, rows);
}
