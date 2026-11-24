// Metronome tab.
import { t, tl } from "../../i18n.ts";
import { beat, bpm, findParam } from "../../protocol/codecs.ts";
import { RHYTHMS } from "../../protocol/params.gen.ts";
import type { ParamStore } from "../../state/store.ts";
import { control } from "../controls.ts";
import { h, section } from "../dom.ts";

/** The metronome tempo range (manual p.44: 10~400 BPM, ±1 by [–] [+], ±10 by an up / down swipe). */
export const TEMPO_MIN = 10;
export const TEMPO_MAX = 400;

/**
 * Tempo terms (manual p.44 「Tempo term」, shown in Metronome mode only): the name for a tempo and the BPM span it
 * covers - the spans the stock remote app uses. The [<] [>] buttons go to the start of the term before / after.
 */
export const TEMPO_TERMS: readonly { name: string; min: number; max: number }[] = [
  { name: "Grave", min: 10, max: 45 },
  { name: "Largo", min: 46, max: 51 },
  { name: "Lento", min: 52, max: 55 },
  { name: "Larghetto", min: 56, max: 59 },
  { name: "Adagio", min: 60, max: 65 },
  { name: "Adagietto", min: 66, max: 71 },
  { name: "Andante", min: 72, max: 79 },
  { name: "Andantino", min: 80, max: 87 },
  { name: "Maestoso", min: 88, max: 95 },
  { name: "Moderato", min: 96, max: 107 },
  { name: "Allegretto", min: 108, max: 119 },
  { name: "Animato", min: 120, max: 131 },
  { name: "Allegro", min: 132, max: 159 },
  { name: "Vivace", min: 160, max: 183 },
  { name: "Presto", min: 184, max: 191 },
  { name: "Vivacissimo", min: 192, max: 207 },
  { name: "Prestissimo", min: 208, max: 400 },
];

/** The index into TEMPO_TERMS for a BPM (-1 outside 10..400). */
export function tempoTermIndex(bpmValue: number): number {
  return TEMPO_TERMS.findIndex((x) => bpmValue >= x.min && bpmValue <= x.max);
}

/** The tempo the [<] (-1) / [>] (+1) term buttons go to: the start of the neighbouring term, or null at either end. */
export function tempoTermStep(bpmValue: number, dir: -1 | 1): number | null {
  const i = tempoTermIndex(bpmValue);
  const next = i < 0 ? undefined : TEMPO_TERMS[i + dir];
  return next ? next.min : null;
}

export function metronomeScreen(store: ParamStore, beatEvents: EventTarget): HTMLElement {
  const tempoP = findParam("MetronomeTempo")!;
  const beatP = findParam("BeatSelect")!;
  const rhythmP = findParam("RhythmSelect")!;
  const runP = findParam("Metronome")!; // 56 08 run state (0 stop / 1 run) — the other controller's start/stop arrives here
  for (const p of [tempoP, beatP, rhythmP]) store.want(p, 0x7f); // runP is not asked for: notifications and beats set it
  // "–" until the piano's tempo is known (not a 120 that looks like the piano's value)
  const tempoOut = h("div", { class: "tempo" }, store.get(tempoP, 0x7f) ? String(bpm.decode(store.get(tempoP, 0x7f)!)) : "–");
  // the tempo term under the number (Metronome mode only, as on the panel); "–" until the piano's tempo is known
  const termOut = h("span", { class: "tempo-term-name" }, "–");
  const termStep = (dir: -1 | 1) => {
    const d = store.get(tempoP, 0x7f);
    const v = d ? tempoTermStep(bpm.decode(d), dir) : null;
    if (v !== null) setTempo(v);
  };
  const termPrev = h("button", { class: "tempo-term-step", "aria-label": t("Previous tempo term"), onclick: () => termStep(-1) }, "‹") as HTMLButtonElement;
  const termNext = h("button", { class: "tempo-term-step", "aria-label": t("Next tempo term"), onclick: () => termStep(1) }, "›") as HTMLButtonElement;
  const termRow = h("div", { class: "tempo-term" }, termPrev, termOut, termNext);
  const modeP = findParam("MetronomeMode")!;
  const syncTerm = () => {
    const d = store.get(tempoP, 0x7f);
    const i = d ? tempoTermIndex(bpm.decode(d)) : -1;
    termOut.textContent = i >= 0 ? TEMPO_TERMS[i]!.name : "–";
    termPrev.disabled = i <= 0;
    termNext.disabled = i < 0 || i >= TEMPO_TERMS.length - 1;
    termRow.hidden = store.get(modeP, 0x7f)?.[0] === 1; // Rhythm mode: no term
  };
  const dots = h("div", { class: "beats" });
  let running = false;
  const runBtn = h("button", { class: "primary big" }, t("Start"));
  // the button follows the piano: our own press, the 56 08 value (dump / fan-out from the panel or another app), beats
  const setRunning = (on: boolean) => {
    running = on;
    runBtn.textContent = running ? t("Stop") : t("Start");
  };
  const setTempo = (v: number) => void store.set(tempoP, 0x7f, bpm.encode(Math.max(TEMPO_MIN, Math.min(TEMPO_MAX, v))));
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
    if (k === "MetronomeTempo@127" || k === "MetronomeMode@127") syncTerm();
    if (k === "Metronome@127") setRunning(store.get(runP, 0x7f)?.[0] === 1);
  });
  syncMetronome();
  syncTerm();
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
      // ±1 and ±10 (the panel's left / right and up / down swipes)
      h("div", { class: "tempo-row" },
        h("button", { class: "tempo-10", "aria-label": t("Tempo −10"), onclick: () => setTempo(curTempo() - 10) }, "−10"),
        h("button", { onclick: () => setTempo(curTempo() - 1) }, "−"),
        tempoOut,
        h("button", { onclick: () => setTempo(curTempo() + 1) }, "+"),
        h("button", { class: "tempo-10", "aria-label": t("Tempo +10"), onclick: () => setTempo(curTempo() + 10) }, "+10")),
      termRow,
      dots,
      runBtn,
      h("label", { class: "row" }, h("span", {}, t("Time signature")), sigSel),
      control(store, "MetronomeMode", { label: t("Mode"), part: 0x7f }),
      h("label", { class: "row" }, h("span", {}, t("Rhythm")), rhythmSel),
      control(store, "MetronomeVolume", { label: t("Volume"), part: 0x7f }),
    ),
  );
}
