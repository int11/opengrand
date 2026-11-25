// Music tab (ui/screens/music.ts): the built-in songs are the panel app's MIDI files, not the piano's - the tab lists
// them (protocol/builtinsongs.ts) and plays one only through the panel shell (kwmcore bridge). These tests check the
// bytes and bridge calls, with a fake shell standing in for the panel.
import { byText, change, field, pianoPair, tick } from "./_dom.ts";
import { afterEach, beforeEach, test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const { bindStore } = await import("../src/ui/controls.ts");
const S = await import("../src/ui/screens.ts");
const { handParts, trackArray, beatTicks, _resetMusicForTests } = await import("../src/ui/screens/music.ts");
const { builtinBooks, builtinCount, songKey, regionOf, LESSON_BOOKS, CONCERT_MAGIC, PIANO_MUSIC } = await import("../src/protocol/builtinsongs.ts");
const { _resetKwmcoreForTests } = await import("../src/transport/kwmcore.ts");
const i18n = await import("../src/i18n.ts");

type G = { kwmcore?: unknown; KWM?: Record<string, (...a: string[]) => void> };
const g = globalThis as unknown as G;

/** The shell's metronome calls of the last fakeShell (kept apart so the player sequences stay readable). */
let metro: { cmd: string; obj: Record<string, unknown> }[] = [];
/** A panel shell that answers selectSmf / setConcertMagicMode and records every bridge call. */
function fakeShell(opt: { failSelect?: boolean; files?: string[]; info?: Record<string, unknown> } = {}) {
  const calls: { cmd: string; obj: Record<string, unknown> }[] = [];
  metro = [];
  g.kwmcore = {
    postMessage(url: string) {
      const i = url.indexOf("?");
      const cmd = url.substring(10, i - 1);
      const obj = JSON.parse(url.substring(i + 1)) as Record<string, unknown>;
      (/Metronome/.test(cmd) ? metro : calls).push({ cmd, obj });
      if (cmd === "getSmfList") setTimeout(() => g.KWM!.resolvePromise(cmd, JSON.stringify((opt.files ?? []).map((name) => ({ name })))), 1);
      if (cmd === "selectSmf" || cmd === "setConcertMagicMode") {
        setTimeout(() => (opt.failSelect && cmd === "selectSmf" ? g.KWM!.rejectPromise(cmd, "{}") : g.KWM!.resolvePromise(cmd, JSON.stringify(cmd === "selectSmf" && opt.info ? opt.info : { format: 1, totalTime: 1000 }))), 1);
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

test("music tab on the panel: a tap plays the piece through the shell; hand practice mutes that hand in the shell's player", async () => {
  const inv = bookOf(LESSON_BOOKS, "inventions");
  const files = [stub(inv, 3, "T-A.mid"), stub(inv, 4, "T-B.mid"), stub(bookOf(PIANO_MUSIC, "piano"), 13, "T-C.mid"), "Kawai/updater/X.mid"];
  const calls = fakeShell({ files });
  const { store, sent } = await setup();
  const el = S.musicScreen(store);
  document.body.append(el);
  assert.equal(el.querySelector("#builtin-where"), null);
  change(field<HTMLSelectElement>(el, "Lesson book"), "inventions");
  assert.ok(!el.querySelector("#hand-practice")!.hasAttribute("hidden"));
  // choose the left hand to play: the shell's player leaves it out (the second hand part muted) - set after the select
  byText(el, "button", "Left hand").click();
  assert.equal(calls.length, 0, "nothing loaded yet: nothing to mute");
  (rows(el)[3].querySelector("button") as HTMLButtonElement).click();
  await tick(30);
  assert.deepEqual(calls.map((c) => c.cmd), ["getSmfList", "selectSmf", "setSmfTrack", "setSmfRelativeTempo", "playStartSmf"]);
  assert.deepEqual(calls[0].obj, { bundle: true }, "the shell lists its own song files");
  calls.shift();
  assert.deepEqual(calls[0].obj, { path: "Kawai/default/T-A.mid", quantise: 256 });
  assert.deepEqual(calls[1].obj, { array: [1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1] }, "channel 2 muted (numbers, not booleans)");
  calls.splice(1, 1);
  assert.deepEqual(calls[1].obj, { tempo: 1 }, "the lesson tempo again after each select (1× until moved)");
  calls.splice(1, 1);
  assert.deepEqual(calls[1].obj, { interval: 480 }, "ticks per beat, as the original app passes");
  assert.ok(!sent.some((m) => m.includes(" 55 01 ")), "no Volume frames to the piano");
  assert.equal(rows(el)[3].getAttribute("data-playing"), "1");
  assert.match(el.querySelector("#music-player .pl-title")!.textContent!, /Invention No\. 4/);
  assert.ok(!sent.some((m) => m.includes(" 52 00 7F") || m.includes(" 52 1D ")), "no SongSelect / MusicMode for a lesson piece");
  // pause / resume / next / stop are the shell's player
  (el.querySelector("#music-player .pl-main") as HTMLElement).click(); // the main button pauses while playing
  (el.querySelector("#music-player .pl-main") as HTMLElement).click(); // and plays again
  (el.querySelector('#music-player [data-act="next"]') as HTMLElement).click();
  await tick(30);
  (el.querySelector('#music-player [data-act="stop"]') as HTMLElement).click();
  assert.deepEqual(calls.slice(2).map((c) => c.cmd), ["playPauseSmf", "playStartSmf", "playStopSmf", "selectSmf", "setSmfTrack", "setSmfRelativeTempo", "playStartSmf", "playStopSmf"],
    "the next piece is muted again after its select");
  assert.equal(calls[5].obj.path, "Kawai/default/T-B.mid");
  assert.deepEqual(calls[6].obj.array, [1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]);
  // Piano Music plays with both hands again
  byText(el, "button", "Piano Music").click();
  (rows(el)[13].querySelector("button") as HTMLButtonElement).click();
  await tick(30);
  assert.equal(calls.at(-3)!.obj.path, "Kawai/default/T-C.mid");
  assert.equal(calls.at(-2)!.cmd, "setSmfRelativeTempo", "a fresh select mutes nothing: no setSmfTrack for Piano Music");
  assert.equal(calls.filter((c) => c.cmd === "getSmfList").length, 0, "the list is asked once");
  assert.ok(!sent.some((m) => m.includes(" 55 01 ")), "still no Volume frames");
  assert.equal(beatTicks({ division: 480, beatInfo: [{ numra: 6, denom: 8 }] }), 720, "6/8: a dotted quarter");
  assert.equal(beatTicks({ division: 480, beatInfo: [{ numra: 3, denom: 4 }] }), 480);
  el.remove();
});

test("hand practice: the hand parts come from selectSmf's existTrack; a change while playing is sent at once", async () => {
  // format 1: track 0 is the conductor track (no channel messages), the hands are tracks 1 and 2
  assert.deepEqual(handParts([false, true, true, ...Array(13).fill(false)]), [1, 2]);
  assert.deepEqual(handParts([true, true, ...Array(14).fill(false)]), [0, 1], "format 0: channels 1 and 2");
  assert.deepEqual(handParts(undefined), [0, 1]);
  assert.deepEqual(handParts([false, false, false, true]), [3, 4], "one part only: the next index");
  assert.deepEqual(trackArray("both", [1, 2]), Array(16).fill(1));
  assert.equal(trackArray("right", [1, 2]).indexOf(0), 1);
  assert.equal(trackArray("left", [1, 2]).indexOf(0), 2);

  const inv = bookOf(LESSON_BOOKS, "inventions");
  const files = [stub(inv, 0, "T-A.mid")];
  const calls = fakeShell({ files, info: { format: 1, division: 480, existTrack: [false, true, true, ...Array(13).fill(false)] } });
  const { store, sent } = await setup();
  const el = S.musicScreen(store);
  document.body.append(el);
  change(field<HTMLSelectElement>(el, "Lesson book"), "inventions");
  (rows(el)[0].querySelector("button") as HTMLButtonElement).click();
  await tick(30);
  assert.equal(calls.filter((c) => c.cmd === "setSmfTrack").length, 0, "both hands: nothing muted, nothing sent");
  const tracks = () => calls.filter((c) => c.cmd === "setSmfTrack").map((c) => (c.obj.array as number[]).indexOf(0));
  byText(el, "button", "Right hand").click();
  assert.deepEqual(tracks(), [1], "right hand = the first hand part (track 1)");
  byText(el, "button", "Right hand").click();
  assert.deepEqual(tracks(), [1], "the same choice again: not re-sent");
  byText(el, "button", "Left hand").click();
  byText(el, "button", "Both hands").click();
  assert.deepEqual(tracks(), [1, 2, -1], "left = track 2, both = nothing muted");
  // paused: still the loaded piece - sent; stopped: nothing until the next select
  (el.querySelector("#music-player .pl-main") as HTMLElement).click();
  byText(el, "button", "Left hand").click();
  assert.deepEqual(tracks(), [1, 2, -1, 2]);
  (el.querySelector('#music-player [data-act="stop"]') as HTMLElement).click();
  byText(el, "button", "Right hand").click();
  assert.deepEqual(tracks(), [1, 2, -1, 2], "stopped: sent with the next select instead");
  assert.ok(!sent.some((m) => m.includes(" 55 01 ")), "no Volume frames");
  el.remove();
});

test("music tab on the panel: lesson tempo 0.5×–2× is the shell's relative speed (setSmfRelativeTempo), sent while a lesson piece plays and after each select; 1× for Piano Music", async () => {
  const { relativeSpeed } = await import("../src/ui/screens/music.ts");
  assert.deepEqual([0, 25, 50, 60, 100].map(relativeSpeed), [0.5, 0.75, 1, 1.2, 2]);
  const inv = bookOf(LESSON_BOOKS, "inventions");
  const files = [stub(inv, 3, "T-A.mid"), stub(bookOf(PIANO_MUSIC, "piano"), 13, "T-C.mid")];
  const calls = fakeShell({ files });
  const { store } = await setup();
  const el = S.musicScreen(store);
  document.body.append(el);
  const speed = el.querySelector("#lesson-tempo input") as HTMLInputElement;
  const out = () => el.querySelector("#lesson-tempo output")!.textContent;
  assert.ok(!el.querySelector("#lesson-tempo")!.hasAttribute("hidden"));
  assert.equal(out(), "1.00×");
  // moved before anything plays: nothing sent yet, the next lesson piece starts at that speed
  change(speed, "25", "input");
  assert.equal(out(), "0.75×");
  assert.equal(calls.filter((c) => c.cmd === "setSmfRelativeTempo").length, 0);
  change(field<HTMLSelectElement>(el, "Lesson book"), "inventions");
  (rows(el)[3].querySelector("button") as HTMLButtonElement).click();
  await tick(30);
  const rel = () => calls.filter((c) => c.cmd === "setSmfRelativeTempo").map((c) => c.obj.tempo);
  assert.deepEqual(rel(), [0.75]);
  const i = calls.findIndex((c) => c.cmd === "setSmfRelativeTempo");
  assert.deepEqual([calls[i - 1]!.cmd, calls[i + 1]!.cmd], ["selectSmf", "playStartSmf"]);
  // while it plays a move goes to the shell at once
  change(speed, "100");
  assert.deepEqual(rel(), [0.75, 2]);
  assert.equal(out(), "2.00×");
  // Piano Music: the shell back at 1×; the lesson speed is kept for the next lesson piece
  byText(el, "button", "Piano Music").click();
  (rows(el)[13].querySelector("button") as HTMLButtonElement).click();
  await tick(30);
  assert.deepEqual(rel(), [0.75, 2, 1]);
  change(speed, "50");
  assert.deepEqual(rel(), [0.75, 2, 1], "not a lesson piece: the slider waits for the next one");
  (el.querySelector('#music-player [data-act="stop"]') as HTMLElement).click();
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
  assert.deepEqual(calls.map((c) => c.cmd), ["selectSmf", "setSmfRelativeTempo", "setConcertMagicMode", "setConcertMagicNativeTrigger"]);
  assert.equal(calls[0].obj.path, "Kawai/default/T-D.mid");
  assert.deepEqual(calls[1].obj, { tempo: 1 }, "not a lesson: the shell back at 1×");
  calls.splice(1, 1);
  assert.deepEqual(calls[1].obj, { mode: 1, smooth: 0, triggerTrack: 0 }, "the original app's own setup");
  assert.deepEqual(calls[2].obj, { isNative: true });
  assert.ok(sent.includes("F0 40 7F 31 08 02 52 1D 7F 05 F7"), "MusicMode 5");
  assert.equal(el.querySelector("#music-player-state")!.textContent, "Tap any keys");
  assert.ok(!/Concert Magic mode|Smooth tempo|Trigger part/.test(el.textContent!), "no Concert Magic settings: the original app has none");
  assert.equal(el.querySelectorAll("section:not([hidden]) select").length, 1, "only the group picker");
  assert.deepEqual(metro.map((c) => c.cmd), ["enableMetronome", "setMetronomeNativeAudio", "muteMetronome"]);
  assert.deepEqual(metro[2].obj, { on: true }, "no beat clicks in Concert Magic");
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

test("music tab on the panel: lesson metronome volume is the shell's beat clicks (unmuted only for a lesson piece) and the piano's MetronomeVolume, put back after the lesson", async () => {
  const { metroVolume } = await import("../src/ui/screens/music.ts");
  const { findParam } = await import("../src/protocol/codecs.ts");
  assert.deepEqual([0, 6, 7, 64, 100, 127].map(metroVolume), [0, 0, 0.1, 0.5, 0.8, 1], "0..127 as 0..1 in 0.1 steps");
  const inv = bookOf(LESSON_BOOKS, "inventions");
  const files = [stub(inv, 3, "T-A.mid"), stub(bookOf(PIANO_MUSIC, "piano"), 13, "T-C.mid")];
  const calls = fakeShell({ files });
  const { store, sent } = await setup();
  await store.set(findParam("MetronomeVolume")!, 0x7f, [74]); // the player's own metronome volume
  const el = S.musicScreen(store);
  document.body.append(el);
  const vol = el.querySelector("#lesson-metronome input") as HTMLInputElement;
  assert.ok(!el.querySelector("#lesson-metronome")!.hasAttribute("hidden"));
  assert.equal(vol.value, "0", "the original's default: silent");
  assert.equal(vol.max, "127");
  change(vol, "64", "input");
  assert.equal(metro.length, 0, "nothing plays: nothing sent");
  const from = sent.length;
  change(field<HTMLSelectElement>(el, "Lesson book"), "inventions");
  (rows(el)[3].querySelector("button") as HTMLButtonElement).click();
  await tick(30);
  assert.deepEqual(metro, [
    { cmd: "enableMetronome", obj: { on: false } },
    { cmd: "setMetronomeNativeAudio", obj: { on: false } },
    { cmd: "setMetronomeVolume", obj: { volume: 0.5 } },
    { cmd: "muteMetronome", obj: { on: false } },
  ], "booleans and a number, as the shell reads them");
  assert.ok(sent.slice(from).some((m) => m.endsWith(" 56 03 7F 40 F7")), "the piano's MetronomeVolume = the slider");
  assert.equal(calls.at(-1)!.cmd, "playStartSmf");
  // moved while the lesson piece plays: at once
  change(vol, "127");
  assert.deepEqual(metro.at(-1), { cmd: "setMetronomeVolume", obj: { volume: 1 } });
  await tick();
  assert.ok(sent.some((m) => m.endsWith(" 56 03 7F 7F F7")));
  // Piano Music: clicks muted, the piano's own value back; setup is not sent again
  const j = sent.length;
  byText(el, "button", "Piano Music").click();
  (rows(el)[13].querySelector("button") as HTMLButtonElement).click();
  await tick(30);
  assert.deepEqual(metro.slice(5), [{ cmd: "muteMetronome", obj: { on: true } }]);
  assert.ok(sent.slice(j).some((m) => m.endsWith(" 56 03 7F 4A F7")), "74 again");
  const n = metro.length;
  change(vol, "10");
  assert.equal(metro.length, n, "not a lesson piece: the slider waits for the next one");
  // a lesson piece then Stop: the value goes back on Stop too
  byText(el, "button", "Lesson").click();
  (rows(el)[3].querySelector("button") as HTMLButtonElement).click();
  await tick(30);
  assert.deepEqual(metro.at(-2), { cmd: "setMetronomeVolume", obj: { volume: 0.1 } });
  const k = sent.length;
  (el.querySelector('#music-player [data-act="stop"]') as HTMLElement).click();
  await tick();
  assert.ok(sent.slice(k).some((m) => m.endsWith(" 56 03 7F 4A F7")), "Stop puts the piano's value back");
  assert.ok(!sent.some((m) => m.includes(" 55 01 ")), "no Volume frames");
  el.remove();
});
