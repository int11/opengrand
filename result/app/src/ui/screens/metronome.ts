// Metronome tab.
import { t, tl } from "../../i18n.ts";
import { beat, bpm, findParam } from "../../protocol/codecs.ts";
import { RHYTHMS } from "../../protocol/params.gen.ts";
import type { ParamStore } from "../../state/store.ts";
import { control } from "../controls.ts";
import { h, section } from "../dom.ts";

export function metronomeScreen(store: ParamStore, beatEvents: EventTarget): HTMLElement {
  const tempoP = findParam("MetronomeTempo")!;
  const beatP = findParam("BeatSelect")!;
  const rhythmP = findParam("RhythmSelect")!;
  const runP = findParam("Metronome")!; // 56 08 run state (0 stop / 1 run) — the other controller's start/stop arrives here
  for (const p of [tempoP, beatP, rhythmP]) store.want(p, 0x7f); // runP is not asked for: notifications and beats set it
  // "–" until the piano's tempo is known (not a 120 that looks like the piano's value)
  const tempoOut = h("div", { class: "tempo" }, store.get(tempoP, 0x7f) ? String(bpm.decode(store.get(tempoP, 0x7f)!)) : "–");
  const dots = h("div", { class: "beats" });
  let running = false;
  const runBtn = h("button", { class: "primary big" }, t("Start"));
  // the button follows the piano: our own press, the 56 08 value (dump / fan-out from the panel or another app), beats
  const setRunning = (on: boolean) => {
    running = on;
    runBtn.textContent = running ? t("Stop") : t("Start");
  };
  const setTempo = (v: number) => void store.set(tempoP, 0x7f, bpm.encode(Math.max(10, Math.min(400, v))));
  const curTempo = () => bpm.decode(store.get(tempoP, 0x7f) ?? [0, 120]);
  beatEvents.addEventListener("beat", (e) => {
    const n = (e as CustomEvent<number>).detail;
    setRunning(true); // beats only come while it runs (started here, on the panel or on another app)
    dots.replaceChildren(...Array.from({ length: Math.max(n, 4) }, (_, i) => h("span", { class: i === n - 1 ? "dot on" : "dot" })));
  });
  const sigs: [number, number][] = [[1, 4], [2, 4], [3, 4], [4, 4], [5, 4], [3, 8], [6, 8], [7, 8], [9, 8], [12, 8]];
  const sigSel = h("select", {}, ...sigs.map(([n, d], i) => h("option", { value: String(i) }, `${n}/${d}`))) as HTMLSelectElement;
  sigSel.addEventListener("change", () => {
    if (sigSel.selectedIndex < 0) return;
    const [n, d] = sigs[Number(sigSel.value)];
    void store.set(beatP, 0x7f, beat.encode(n, d));
  });
  const rhythmSel = h("select", {}, ...RHYTHMS.map((r) => h("option", { value: String(r.index) }, `${tl(r.category)} · ${r.name}`))) as HTMLSelectElement;
  rhythmSel.addEventListener("change", () => {
    if (rhythmSel.selectedIndex >= 0) void store.set(rhythmP, 0x7f, [Number(rhythmSel.value)]);
  });
  // follow the piano (dump, read-back, notifications): BeatSelect [num, denom index], RhythmSelect [index]
  const syncMetronome = () => {
    const t = store.get(tempoP, 0x7f);
    tempoOut.textContent = t ? String(bpm.decode(t)) : "–";
    const b = store.get(beatP, 0x7f);
    const bi = b ? sigs.findIndex(([n, d]) => { const x = beat.decode(b); return x.num === n && x.den === d; }) : -1;
    if (bi >= 0) sigSel.value = String(bi);
    else sigSel.selectedIndex = -1; // blank until known
    const r = store.get(rhythmP, 0x7f)?.[0];
    if (r !== undefined && RHYTHMS.some((x) => x.index === r)) rhythmSel.value = String(r);
    else rhythmSel.selectedIndex = -1;
  };
  store.subscribe((k) => {
    if (k === "MetronomeTempo@127" || k === "BeatSelect@127" || k === "RhythmSelect@127") syncMetronome();
    if (k === "Metronome@127") setRunning(store.get(runP, 0x7f)?.[0] === 1);
  });
  syncMetronome();
  setRunning(store.get(runP, 0x7f)?.[0] === 1);
  runBtn.addEventListener("click", () => {
    setRunning(!running);
    void store.command(findParam("Metronome", 0x32)!, 0x7f, [running ? 1 : 0]);
  });
  return h(
    "div",
    { class: "screen" },
    section(
      t("Metronome"),
      h("div", { class: "tempo-row" }, h("button", { onclick: () => setTempo(curTempo() - 1) }, "−"), tempoOut, h("button", { onclick: () => setTempo(curTempo() + 1) }, "+")),
      dots,
      runBtn,
      h("label", { class: "row" }, h("span", {}, t("Time signature")), sigSel),
      control(store, "MetronomeMode", { label: t("Mode"), part: 0x7f }),
      h("label", { class: "row" }, h("span", {}, t("Rhythm")), rhythmSel),
      control(store, "MetronomeVolume", { label: t("Volume"), part: 0x7f }),
    ),
  );
}
