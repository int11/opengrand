// Music tab (ui/screens/music.ts): the built-in songs are the panel app's MIDI files, not the piano's - the tab lists
// them (protocol/builtinsongs.ts) and plays one only through the panel shell (kwmcore bridge). These tests check the
// bytes and bridge calls, with a fake shell standing in for the panel.
import { byText, change, field, pianoPair, tick } from "./_dom.ts";
import { afterEach, beforeEach, test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const { bindStore } = await import("../src/ui/controls.ts");
const S = await import("../src/ui/screens.ts");
const { handVolumes, beatTicks, _resetMusicForTests } = await import("../src/ui/screens/music.ts");
const { builtinBooks, builtinCount, songKey, regionOf, LESSON_BOOKS, CONCERT_MAGIC, PIANO_MUSIC } = await import("../src/protocol/builtinsongs.ts");
const { _resetKwmcoreForTests } = await import("../src/transport/kwmcore.ts");
const i18n = await import("../src/i18n.ts");

type G = { kwmcore?: unknown; KWM?: Record<string, (...a: string[]) => void> };
const g = globalThis as unknown as G;

/** A panel shell that answers selectSmf / setConcertMagicMode and records every bridge call. */
function fakeShell(opt: { failSelect?: boolean; files?: string[] } = {}) {
  const calls: { cmd: string; obj: Record<string, unknown> }[] = [];
  g.kwmcore = {
    postMessage(url: string) {
      const i = url.indexOf("?");
      const cmd = url.substring(10, i - 1);
      const obj = JSON.parse(url.substring(i + 1)) as Record<string, unknown>;
      calls.push({ cmd, obj });
      if (cmd === "getSmfList") setTimeout(() => g.KWM!.resolvePromise(cmd, JSON.stringify((opt.files ?? []).map((name) => ({ name })))), 1);
      if (cmd === "selectSmf" || cmd === "setConcertMagicMode") {
        setTimeout(() => (opt.failSelect && cmd === "selectSmf" ? g.KWM!.rejectPromise(cmd, "{}") : g.KWM!.resolvePromise(cmd, '{"format":1,"totalTime":1000}')), 1);
      }
    },
  };
  return calls;
}

// The catalog keys are hashes of the panel app's own paths, which this repo does not hold: a test gives a few pieces
// keys of made-up paths and serves those paths from the fake shell.
type Book = { songs: { key: string; region?: string }[] };
const saved: [{ key: string }, string][] = [];
function stub(book: Book, i: number, name: string): string {
  const s = book.songs.filter((x) => !x.region || x.region === "eu")[i]; // the row as the EU list shows it
  saved.push([s, s.key]);
  s.key = songKey(`Kawai/default/${name}`);
  return `Kawai/default/${name}`;
}
const bookOf = (list: readonly (Book & { id: string })[], id: string) => list.find((b) => b.id === id)!;

async function setup() {
  const p = await pianoPair();
  const store = new ParamStore();
  bindStore(store);
  store.attach(p.session);
  return { ...p, store };
}

beforeEach(() => {
  localStorage.clear();
  i18n.setLang("en");
});
afterEach(() => {
  for (const [s, k] of saved.splice(0)) s.key = k;
  _resetMusicForTests();
  _resetKwmcoreForTests();
  delete g.kwmcore;
});

const visible = (el: Element) => [...el.querySelectorAll("section")].filter((s) => !s.hidden).map((s) => s.querySelector("h3")!.textContent);
const rows = (el: Element) => [...el.querySelectorAll("section:not([hidden]) .song-row")];

test("built-in song list: CA79 books and counts per region, keys only (no file names)", () => {
  assert.equal(builtinCount("lesson"), 750, "Alfred 223 + other books 387 + finger exercises 140");
  assert.equal(builtinCount("lesson", "us"), 750);
  assert.equal(builtinCount("cm"), 175);
  assert.equal(builtinCount("cm", "us"), 176);
  assert.equal(builtinCount("piano"), 29);
  assert.equal(builtinCount("piano", "us"), 42 + 88, "US: other pieces and the hymns");
  assert.deepEqual(builtinBooks("piano").map((b) => b.id), ["piano"], "no hymns in the EU list");
  const all = [LESSON_BOOKS, CONCERT_MAGIC, PIANO_MUSIC].flatMap((l) => l.flatMap((b) => b.songs));
  assert.equal(all.length, 750 + 196 + 71 + 88);
  assert.equal(new Set(all.map((x) => x.key)).size, all.length, "every piece has its own key");
  assert.ok(all.every((x) => /^[0-9a-z]{1,7}$/.test(x.key)), "keys are 32-bit hashes in base 36");
  assert.equal(songKey("a"), (0xe40c292c).toString(36), "FNV-1a 32");
  assert.equal(regionOf(2), "us");
  assert.equal(regionOf(1), "eu");
  assert.equal(regionOf(undefined), "eu");
  assert.equal(LESSON_BOOKS.find((b) => b.id === "sonatinas")!.songs.length, 60);
  assert.equal(LESSON_BOOKS.find((b) => b.id === "alfred-1a")!.songs.length, 41);
  assert.equal(LESSON_BOOKS.find((b) => b.id === "arp-min")!.hands, false, "exercises have no hand parts");
});

test("music tab without the panel shell: three modes with one line each, real lists, nothing sent for a piece", async () => {
  const { store, sent } = await setup();
  const el = S.musicScreen(store);
  document.body.append(el);
  assert.deepEqual([...el.querySelectorAll(".modes button")].map((b) => b.textContent), ["Lesson", "Concert Magic", "Piano Music"]);
  assert.deepEqual(visible(el), ["Music", "Lesson"]);
  assert.match(el.querySelector("#music-mode-line")!.textContent!, /one hand at a time/);
  assert.ok(el.querySelector("#builtin-where"), "says where the pieces play");
  // lesson: book → pieces
  const books = field<HTMLSelectElement>(el, "Lesson book");
  assert.equal(books.options.length, LESSON_BOOKS.length);
  assert.equal(rows(el).length, 126, "Beyer first");
  change(books, "burgmuller");
  assert.equal(rows(el).length, 25);
  assert.match(rows(el)[0].textContent!, /La candeur/);
  // a row cannot be played here and sends nothing (no SongSelect, no MusicMode)
  const before = sent.length;
  (rows(el)[0].querySelector("button") as HTMLButtonElement).click();
  await tick();
  assert.ok((rows(el)[0].querySelector("button") as HTMLButtonElement).disabled);
  assert.equal(sent.length, before);
  assert.ok(!sent.some((m) => m.includes(" 52 00 7F") || m.includes(" 52 1D ")), "no SongSelect / MusicMode for built-in songs");
  assert.ok(el.querySelector("#hand-practice")!.hasAttribute("hidden"), "hand practice only where a piece can play");
  // the mode buttons switch the list and the line only
  byText(el, "button", "Concert Magic").click();
  assert.deepEqual(visible(el), ["Music", "Concert Magic"]);
  assert.match(el.querySelector("#music-mode-line")!.textContent!, /Tap any keys in rhythm/);
  assert.equal(rows(el).length, builtinBooks("cm")[0].songs.length);
  byText(el, "button", "Piano Music").click();
  assert.equal(rows(el).length, 29);
  assert.equal(el.querySelector("section:not([hidden]) select"), null, "one list, no group picker");
  assert.equal(sent.length, before, "switching modes sends nothing");
  // the recorder's part pill and options are not shown for these pieces
  assert.ok((el.querySelector('#music-player [data-act="part"]') as HTMLElement).hidden);
  assert.ok((el.querySelector('#music-player [data-act="options"]') as HTMLElement).hidden);
  el.remove();
});

test("music tab on the panel: a tap plays the piece through the shell; hand practice sets the two hand volumes", async () => {
  const inv = bookOf(LESSON_BOOKS, "inventions");
  const files = [stub(inv, 3, "T-A.mid"), stub(inv, 4, "T-B.mid"), stub(bookOf(PIANO_MUSIC, "piano"), 13, "T-C.mid"), "Kawai/updater/X.mid"];
  const calls = fakeShell({ files });
  const { store, sent } = await setup();
  const el = S.musicScreen(store);
  document.body.append(el);
  assert.equal(el.querySelector("#builtin-where"), null);
  change(field<HTMLSelectElement>(el, "Lesson book"), "inventions");
  assert.ok(!el.querySelector("#hand-practice")!.hasAttribute("hidden"));
  // choose the left hand to play: the piano leaves it out (MIDI2ch volume 0), right hand stays (Global 100)
  byText(el, "button", "Left hand").click();
  (rows(el)[3].querySelector("button") as HTMLButtonElement).click();
  await tick(30);
  assert.deepEqual(calls.map((c) => c.cmd), ["getSmfList", "selectSmf", "playStartSmf"]);
  assert.deepEqual(calls[0].obj, { bundle: true }, "the shell lists its own song files");
  calls.shift();
  assert.deepEqual(calls[0].obj, { path: "Kawai/default/T-A.mid", quantise: 256 });
  assert.deepEqual(calls[1].obj, { interval: 480 }, "ticks per beat, as the original app passes");
  assert.ok(sent.includes("F0 40 7F 10 08 02 55 01 0F 64 F7") && sent.includes("F0 40 7F 10 08 02 55 01 31 00 F7"), "Global 100, MIDI2ch 0");
  assert.equal(rows(el)[3].getAttribute("data-playing"), "1");
  assert.match(el.querySelector("#music-player .pl-title")!.textContent!, /Invention No\. 4/);
  assert.ok(!sent.some((m) => m.includes(" 52 00 7F") || m.includes(" 52 1D ")), "no SongSelect / MusicMode for a lesson piece");
  // pause / resume / next / stop are the shell's player
  (el.querySelector("#music-player .pl-main") as HTMLElement).click(); // the main button pauses while playing
  (el.querySelector("#music-player .pl-main") as HTMLElement).click(); // and plays again
  (el.querySelector('#music-player [data-act="next"]') as HTMLElement).click();
  await tick(30);
  (el.querySelector('#music-player [data-act="stop"]') as HTMLElement).click();
  assert.deepEqual(calls.slice(2).map((c) => c.cmd), ["playPauseSmf", "playStartSmf", "playStopSmf", "selectSmf", "playStartSmf", "playStopSmf"]);
  await tick();
  assert.equal(sent.filter((m) => m === "F0 40 7F 10 08 02 55 01 31 64 F7").length, 1, "Stop puts the left hand back");
  assert.equal(calls[5].obj.path, "Kawai/default/T-B.mid");
  // Piano Music plays with both hands again
  byText(el, "button", "Piano Music").click();
  (rows(el)[13].querySelector("button") as HTMLButtonElement).click();
  await tick(30);
  assert.equal(calls.at(-2)!.obj.path, "Kawai/default/T-C.mid");
  assert.equal(calls.filter((c) => c.cmd === "getSmfList").length, 0, "the list is asked once");
  assert.deepEqual(handVolumes("right"), [0, 100]);
  assert.equal(beatTicks({ division: 480, beatInfo: [{ numra: 6, denom: 8 }] }), 720, "6/8: a dotted quarter");
  assert.equal(beatTicks({ division: 480, beatInfo: [{ numra: 3, denom: 4 }] }), 480);
  el.remove();
});

test("music tab on the panel: Concert Magic sets the shell up, mutes the piano's own keys (MusicMode 5) and shows the tap result", async () => {
  const calls = fakeShell({ files: [stub(bookOf(CONCERT_MAGIC, "cm-classical"), 1, "T-D.mid")] });
  const { store, sent } = await setup();
  const el = S.musicScreen(store);
  document.body.append(el);
  byText(el, "button", "Concert Magic").click();
  change(field<HTMLSelectElement>(el, "Group"), "cm-classical");
  (rows(el)[1].querySelector("button") as HTMLButtonElement).click(); // Ode to Joy
  await tick(30);
  calls.shift(); // getSmfList
  assert.deepEqual(calls.map((c) => c.cmd), ["selectSmf", "setConcertMagicMode", "setConcertMagicNativeTrigger"]);
  assert.equal(calls[0].obj.path, "Kawai/default/T-D.mid");
  assert.deepEqual(calls[1].obj, { mode: 1, smooth: 0, triggerTrack: 0 });
  assert.deepEqual(calls[2].obj, { isNative: true });
  assert.ok(sent.includes("F0 40 7F 31 08 02 52 1D 7F 05 F7"), "MusicMode 5");
  assert.equal(el.querySelector("#music-player-state")!.textContent, "Tap any keys");
  // a setting change goes to the shell while Concert Magic is on
  change(field<HTMLSelectElement>(el, "Concert Magic mode"), "0");
  await tick(10);
  assert.deepEqual(calls.at(-1)!.obj, { mode: 0, smooth: 0, triggerTrack: 0 });
  // the shell's tap report
  g.KWM!.donePlayConcertMagic("60", "90", "0.5", "1", "2");
  assert.equal(el.querySelector("#cm-result")!.textContent, "Fast");
  // stop: native trigger off, the piano's keys sound again (MusicMode 0)
  (el.querySelector('#music-player [data-act="stop"]') as HTMLElement).click();
  assert.deepEqual(calls.at(-1), { cmd: "setConcertMagicNativeTrigger", obj: { isNative: false } });
  await tick();
  assert.ok(sent.includes("F0 40 7F 31 08 02 52 1D 7F 00 F7"), "MusicMode 0");
  el.remove();
});

test("music tab on the panel: a piece the shell cannot open, or does not have, says so", async () => {
  fakeShell({ failSelect: true, files: [stub(bookOf(PIANO_MUSIC, "piano"), 0, "T-E.mid")] });
  const { store } = await setup();
  const el = S.musicScreen(store);
  document.body.append(el);
  byText(el, "button", "Piano Music").click();
  (rows(el)[0].querySelector("button") as HTMLButtonElement).click();
  await tick(30);
  assert.match(el.querySelector("#music-player-state")!.textContent!, /could not open/);
  (rows(el)[1].querySelector("button") as HTMLButtonElement).click(); // not in the shell's list
  await tick(30);
  assert.match(el.querySelector("#music-player-state")!.textContent!, /not in the panel app/);
  el.remove();
});

test("music tab on the panel: Stop on a new screen ends Concert Magic started from an earlier one (a tab switch rebuilds the screen)", async () => {
  const calls = fakeShell({ files: [stub(bookOf(CONCERT_MAGIC, "cm-children"), 0, "T-F.mid")] });
  const { store, sent } = await setup();
  const a = S.musicScreen(store);
  document.body.append(a);
  byText(a, "button", "Concert Magic").click();
  (rows(a)[0].querySelector("button") as HTMLButtonElement).click();
  await tick(30);
  assert.ok(sent.includes("F0 40 7F 31 08 02 52 1D 7F 05 F7"));
  a.remove();
  const b = S.musicScreen(store);
  document.body.append(b);
  assert.deepEqual(visible(b), ["Music", "Concert Magic"], "the new screen shows what plays");
  assert.equal(rows(b)[0].getAttribute("data-playing"), "1");
  (b.querySelector('#music-player [data-act="stop"]') as HTMLElement).click();
  await tick();
  assert.deepEqual(calls.at(-1), { cmd: "setConcertMagicNativeTrigger", obj: { isNative: false } });
  assert.ok(sent.includes("F0 40 7F 31 08 02 52 1D 7F 00 F7"), "MusicMode 0: the keys sound again");
  b.remove();
});
