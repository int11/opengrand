// Chord dictionary screen (feature 80): root x quality x inversion, shown on a
// two-octave on-screen keyboard. Notes the piano sends (the player's own
// keys) light up on the same keyboard and are named, so it doubles as a
// "what am I playing" display. "Play" sends ordinary note on/off messages.
import { NOTE_NAMES, QUALITIES, buildChord, identifyChord, inversionCount } from "../chords.ts";
import { t } from "../i18n.ts";
import type { AppContext } from "./context.ts";
import { h, section } from "./dom.ts";

const BLACK = new Set([1, 3, 6, 8, 10]);
export const KB_LOW = 48; // C3
export const KB_HIGH = 83; // B5 (3 octaves)

export function keyboard(low = KB_LOW, high = KB_HIGH): HTMLElement {
  const kb = h("div", { class: "kb", role: "img" });
  let white = 0;
  for (let n = low; n <= high; n++) {
    const black = BLACK.has(n % 12);
    const k = h("span", { class: black ? "key black" : "key white", "data-note": n });
    if (black) k.style.left = `calc(${white} * var(--kw) - var(--kb) / 2)`;
    else white++;
    kb.append(k);
  }
  kb.style.setProperty("--whites", String(white));
  return kb;
}

export function lightKeys(kb: HTMLElement, notes: Iterable<number>, cls: string): void {
  const set = new Set(notes);
  kb.querySelectorAll<HTMLElement>(".key").forEach((k) => k.classList.toggle(cls, set.has(Number(k.getAttribute("data-note")))));
}

export function chordScreen(ctx: AppContext): HTMLElement {
  let root = 0;
  let quality = "maj";
  let inversion = 0;
  const held = new Set<number>();

  const rootSel = h("select", {}, ...NOTE_NAMES.map((n, i) => h("option", { value: String(i) }, n))) as HTMLSelectElement;
  const qualSel = h("select", {}, ...QUALITIES.map((q) => h("option", { value: q.id }, q.symbol || t("major")))) as HTMLSelectElement;
  const invSel = h("select", {}) as HTMLSelectElement;
  const name = h("div", { class: "chord-name" });
  const notesOut = h("div", { class: "muted chord-notes" });
  const kb = keyboard();
  const playedName = h("div", { class: "chord-played" }, "–");
  const kbPlayed = keyboard();

  const invLabel = (i: number) => (i === 0 ? t("Root position") : i === 1 ? t("1st inversion") : i === 2 ? t("2nd inversion") : i === 3 ? t("3rd inversion") : t("{n}th inversion", { n: i }));
  const fillInversions = () => {
    const q = QUALITIES.find((x) => x.id === quality)!;
    invSel.replaceChildren(...Array.from({ length: inversionCount(q) }, (_, i) => h("option", { value: String(i) }, invLabel(i))));
    if (inversion >= inversionCount(q)) inversion = 0;
    invSel.value = String(inversion);
  };
  const render = () => {
    const c = buildChord(root, quality, inversion);
    name.textContent = c.name;
    notesOut.textContent = c.notes.map((n) => `${NOTE_NAMES[n % 12]}${Math.floor(n / 12) - 1}`).join(" · ");
    lightKeys(kb, c.notes, "on");
    return c;
  };
  rootSel.addEventListener("change", () => { root = Number(rootSel.value); render(); });
  qualSel.addEventListener("change", () => { quality = qualSel.value; fillInversions(); render(); });
  invSel.addEventListener("change", () => { inversion = Number(invSel.value); render(); });

  // play on the piano: note on, then off after 1.2 s (channel 1)
  // A tap while a chord still sounds ends that chord first: its pending note-offs used to cut the new chord short
  // (same notes) or arrive in the middle of it.
  let sounding: { off: () => void; timer: ReturnType<typeof setTimeout> } | null = null;
  const play = () => {
    const s = ctx.session();
    if (!s) return;
    if (sounding) {
      clearTimeout(sounding.timer);
      sounding.off();
    }
    const notes = render().notes;
    for (const n of notes) s.sendRaw(Uint8Array.of(0x90, n, 80));
    const off = () => {
      for (const n of notes) s.sendRaw(Uint8Array.of(0x80, n, 0));
      sounding = null;
    };
    sounding = { off, timer: setTimeout(off, 1200) };
  };

  // what the player is holding, from the piano's MIDI out
  const onMidi = (e: Event) => {
    const m = (e as CustomEvent<Uint8Array>).detail;
    // drop the listener once this screen has been replaced
    if (screen.isConnected) attached = true;
    else if (attached) return ctx.midi.removeEventListener("midi", onMidi);
    const st = m[0] & 0xf0;
    if (st === 0x90 && m[2] > 0) held.add(m[1]);
    else if (st === 0x80 || (st === 0x90 && m[2] === 0)) held.delete(m[1]);
    else if (st === 0xb0 && (m[1] === 123 || m[1] === 120)) held.clear();
    else return;
    lightKeys(kbPlayed, held, "held");
    playedName.textContent = identifyChord([...held]) ?? (held.size ? [...held].sort((a, b) => a - b).map((n) => NOTE_NAMES[n % 12]).join(" ") : "–");
  };
  let attached = false;
  ctx.midi.addEventListener("midi", onMidi);

  fillInversions();
  render();
  const screen = h(
    "div",
    { class: "screen" },
    section(
      t("Chord dictionary"),
      h("label", { class: "row" }, h("span", {}, t("Root")), rootSel),
      h("label", { class: "row" }, h("span", {}, t("Chord type")), qualSel),
      h("label", { class: "row" }, h("span", {}, t("Inversion")), invSel),
      name,
      kb,
      notesOut,
      h("div", { class: "transport" }, h("button", { class: "primary", onclick: play }, `▶ ${t("Play on piano")}`)),
    ),
    section(t("You are playing"), playedName, kbPlayed),
  );
  return screen;
}
