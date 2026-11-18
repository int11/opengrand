// Feature 80: chord dictionary — algorithmic generation, inversions,
// recognition of played notes, on-screen keyboard.
import "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";
import { byText, change, field, pianoPair, tick } from "./_dom.ts";

const C = await import("../src/chords.ts");
const { chordScreen } = await import("../src/ui/chords.ts");
const { createContext } = await import("../src/ui/context.ts");
const { ParamStore } = await import("../src/state/store.ts");

test("15 qualities x 12 roots are generated from interval formulas", () => {
  assert.equal(C.QUALITIES.length, 15);
  const ids = C.QUALITIES.map((q) => q.id);
  for (const q of ["maj", "min", "7", "maj7", "m7", "dim", "aug", "sus4", "sus2", "add9", "6", "m6", "9", "m7b5", "dim7"]) assert.ok(ids.includes(q), q);
  const all = C.allChords();
  assert.equal(all.length, 180);
  assert.equal(new Set(all.map((c) => c.name)).size, 180);
  assert.deepEqual(C.buildChord(0, "maj").notes, [60, 64, 67]);
  assert.deepEqual(C.buildChord(9, "m7").notes, [57, 60, 64, 67]); // A3 C4 E4 G4 (kept near middle C)
  assert.deepEqual(C.buildChord(2, "m7b5").notes.map((n) => n % 12), [2, 5, 8, 0]);
  assert.deepEqual(C.buildChord(7, "9").notes.map((n) => n - C.buildChord(7, "9").notes[0]), [0, 4, 7, 10, 14]);
  assert.equal(C.buildChord(10, "dim7").name, "Bbdim7");
});

test("inversions rotate the lowest notes up an octave", () => {
  assert.deepEqual(C.buildChord(0, "maj", 1).notes, [64, 67, 72]);
  assert.deepEqual(C.buildChord(0, "maj", 2).notes, [55, 60, 64]); // 67 72 76 lowered an octave
  assert.equal(C.inversionCount(C.QUALITIES.find((q) => q.id === "maj7")!), 4);
  assert.deepEqual(C.buildChord(0, "maj7", 3).notes.map((n) => n % 12), [11, 0, 4, 7]);
  assert.throws(() => C.buildChord(0, "nope"));
});

test("played notes are recognised in any inversion", () => {
  assert.equal(C.identifyChord([60, 64, 67]), "C");
  assert.equal(C.identifyChord([64, 67, 72]), "C/E");
  assert.equal(C.identifyChord([60, 64, 67, 69]), "C6"); // not Am7/C
  assert.equal(C.identifyChord([57, 60, 64, 67]), "Am7");
  assert.equal(C.identifyChord([62, 65, 68, 71]), "Ddim7");
  assert.equal(C.identifyChord([60, 62]), null);
  for (const c of C.allChords()) assert.ok(C.identifyChord(c.notes), `recognise ${c.name}`);
});

test("chord screen lights the chord on the keyboard, shows played keys and plays notes", async () => {
  const { session, sent, piano } = await pianoPair();
  const store = new ParamStore();
  const ctx = createContext(store, { session: () => session });
  const el = chordScreen(ctx);
  document.body.append(el);
  const lit = () => [...el.querySelectorAll("section:first-of-type .key.on")].map((k) => Number(k.getAttribute("data-note")));
  assert.deepEqual(lit(), [60, 64, 67]);
  change(field<HTMLSelectElement>(el, "Root"), "2");
  change(field<HTMLSelectElement>(el, "Chord type"), "m7");
  change(field<HTMLSelectElement>(el, "Inversion"), "1");
  assert.equal(el.querySelector(".chord-name")!.textContent, "Dm7");
  assert.deepEqual(lit(), [65, 69, 72, 74]);
  assert.equal(field<HTMLSelectElement>(el, "Inversion").options.length, 4);

  // the piano's own keys (non-Kawai MIDI from the session) light the second keyboard
  session.onMidi((m) => ctx.midi.dispatchEvent(new CustomEvent("midi", { detail: m })));
  for (const n of [55, 59, 62]) piano.send(Uint8Array.of(0x90, n, 70));
  await tick(5);
  assert.equal(el.querySelector(".chord-played")!.textContent, "G");
  assert.equal(el.querySelectorAll(".key.held").length, 3);
  piano.send(Uint8Array.of(0x80, 55, 0));
  await tick(5);
  assert.equal(el.querySelectorAll(".key.held").length, 2);

  byText(el, "button", /Play on piano/).click();
  await tick(5);
  assert.ok(sent.includes("90 41 50") && sent.includes("90 4A 50"), sent.join(","));
  el.remove();
});
