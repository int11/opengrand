// Piano songs (OpenGrand playback control, 0x7D op 0x1E - REMOTE_PROTOCOL.md section 10.9, firmware
// result/firmware/ports/cave/recctl_cave.c): the decoders, the
// NgRecorder op calls and the Recorder tab's recording panel (ui/recorder.ts): with Identify feat2 0x10 it follows the
// piano's notices and sends op 0x1E; without it (stock) the Kawai Music bytes.
import { change, pianoPair, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const ng = await import("../src/protocol/ngext.ts");
const { ParamStore } = await import("../src/state/store.ts");
const { createContext } = await import("../src/ui/context.ts");
const { bindStore } = await import("../src/ui/controls.ts");
const { recorderScreen } = await import("../src/ui/recorder.ts");

function statusRaw(state: number, song: number, bar = 0): number[] {
  const raw = new Array(48).fill(0);
  raw[0] = 1; raw[1] = state; raw[3] = song; raw[6] = 2; raw[8] = 120; raw[10] = 120; raw[16] = 3; raw[29] = bar;
  return raw;
}

function listRaw(): number[] {
  const raw = new Array(70).fill(0);
  const put = (i: number, len: number, bars: number, flags: number) => {
    raw[7 * i] = len & 0xff; raw[7 * i + 1] = len >> 8; raw[7 * i + 4] = bars; raw[7 * i + 6] = flags;
  };
  put(0, 0x200, 3, 1);
  put(1, 0x300, 5, 3);
  for (let i = 2; i < 10; i++) put(i, 0x74, 0, 0);
  return raw;
}

const frame = (data: number[]) => Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, 0x5e, ...data, 0xf7]);

test("op 0x1E records decode (status 48 B, song list 70 B, event)", () => {
  const st = ng.decodeRecorderStatus(Uint8Array.from(statusRaw(1, 1, 2)))!;
  assert.deepEqual([st.playing, st.recording, st.song, st.bar, st.tempo, st.maxBars], [true, false, 1, 2, 120, 3]);
  assert.equal(ng.decodeRecorderStatus(new Uint8Array(10)), null);
  const songs = ng.decodeSongList(Uint8Array.from(listRaw()))!;
  assert.deepEqual(songs.slice(0, 3).map((g) => [g.bars, g.part1, g.part2, g.empty]),
    [[3, true, false, false], [5, true, true, false], [0, false, false, true]]);
  const n = ng.parseRecorderNotice(frame([0x00, 0x00, ...ng.pack7(statusRaw(4, 0))]))!;
  assert.equal(n.status!.recording, true);
  assert.deepEqual(ng.parseRecorderNotice(frame([0x03, 0x01, 0x04]))!.event, { event: 1, cmd: 4 });
  assert.equal(ng.NgOp.Recorder, 0x1e);
  assert.equal(ng.NG_FEAT2_RECORDER, 0x10);
});

/** A session stand-in: Identify (feat2), op 0x1E 00 / 01 / 02 like recctl_cave.c. */
function fakeSession(feat2: number) {
  const listeners: ((m: Uint8Array) => void)[] = [];
  const asked: number[][] = [];
  const push = (m: Uint8Array) => queueMicrotask(() => listeners.forEach((l) => l(m)));
  const reply = (op: number, data: number[]) => push(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...data, 0xf7]));
  let state = 2, song = 0, bar = 0;
  return {
    state: "ready",
    asked,
    push,
    onMidi(fn: (m: Uint8Array) => void) {
      listeners.push(fn);
      return () => listeners.splice(listeners.indexOf(fn), 1);
    },
    // the file client the USB list makes (no Kawai frames here)
    onFrame: () => () => {},
    onState: () => () => {},
    sendParam: async () => true,
    sendRaw(m: Uint8Array) {
      const op = m[6]!;
      asked.push([...m.slice(6, m.length - 1)]);
      if (op === 0x01) reply(0x01, [1, 2, 19, 0x3b, 0x77, feat2]);
      if (op !== 0x1e) return;
      const sub = m[7]!;
      if (sub === 0) reply(0x1e, [0, 0, ...ng.pack7(statusRaw(state, song))]);
      if (sub === 1) reply(0x1e, [1, 0, ...ng.pack7(listRaw())]);
      if (sub === 2) {
        const cmd = m[8]!, arg = (m[9]! << 7) | m[10]!;
        if (cmd === 1 && song >= 2) return reply(0x1e, [2, 6, cmd]); // nothing recorded
        reply(0x1e, [2, 0, cmd]);
        if (cmd === 1) { state = 1; bar = bar || 2; }
        if (cmd === 2) { state = 2; bar = 0; }
        if (cmd === 3) state = 0;
        if (cmd === 4) { song = arg; bar = 0; }
        if (cmd === 7) bar = arg;
        reply(0x1e, [0, 0, ...ng.pack7(statusRaw(state, song, bar))]);
        reply(0x1e, [3, 1, cmd]);
      }
    },
  };
}

const wait = async (n = 20) => { for (let i = 0; i < n; i++) await tick(2); };
const panelBtn = (el: HTMLElement, act: string) => el.querySelector(`#rec-panel [data-act="${act}"]`) as HTMLButtonElement;
// the player bar (ui/screens/songctl.ts): the state a screen reader hears, the position pill, what is loaded
const stateText = (el: HTMLElement) => el.querySelector("#rec-panel-state")!.textContent;
const position = (el: HTMLElement) => el.querySelector("#rec-panel-pos")!.textContent;
const selected = (el: HTMLElement) => el.querySelector("#rec-panel-info")!.textContent;
const mainAct = (el: HTMLElement) => el.querySelector("#rec-panel .pl-main")!.getAttribute("data-act");
const songRow = (el: HTMLElement, n: number) => el.querySelector(`#songs-list [data-song="${n}"]`) as HTMLElement;

test("recorder tab layout: the player bar on top, then [Piano songs | USB memory]; no USB audio target shown", () => {
  const ctx = createContext(new ParamStore());
  const el = recorderScreen(ctx);
  document.body.append(el);
  assert.deepEqual([...el.children].map((c) => c.id), ["rec-panel", "rec-files"]);
  // one row of buttons: play / pause (one button), previous / next, stop - and Rec, the part, the options
  assert.deepEqual([...el.querySelectorAll("#rec-panel .pl-left button, #rec-panel .pl-right button")].map((b) => b.getAttribute("data-act")),
    ["play", "prev", "next", "stop", "rec", "part", "options"]);
  assert.deepEqual([...el.querySelectorAll("#rec-panel .pl-left button, #rec-panel .pl-right button")].map((b) => b.getAttribute("aria-label") ?? b.getAttribute("title")),
    ["Play", "Previous song", "Next song", "Stop", "Rec", "Part", "Playback options"]);
  // nothing chosen, nothing known: no position, no slider, no previous
  assert.equal(selected(el), "Choose a song below.");
  assert.equal(position(el), "–");
  assert.equal((el.querySelector("#rec-panel .pl-range") as HTMLElement).hidden, true, "the line is a slider only where the piano can jump");
  assert.deepEqual([panelBtn(el, "prev").disabled, panelBtn(el, "next").disabled], [true, false]);
  assert.deepEqual([...el.querySelectorAll("#rec-panel [data-part-select] button")].map((b) => b.textContent), ["Part 1", "Part 2", "Both"]);
  // the recording target: one today (the piano's own songs), so its buttons are not drawn
  const target = el.querySelector("#rec-target") as HTMLElement;
  assert.equal(target.hidden, true);
  assert.deepEqual([...target.querySelectorAll("button")].map((b) => b.getAttribute("data-target")), ["internal"]);
  // part / tempo / transpose / seek / A-B are a sheet behind the options button
  const more = el.querySelector("#rec-panel-options") as HTMLDialogElement;
  assert.equal(more.open, false);
  assert.deepEqual([...more.querySelectorAll(".row > span:first-child")].map((s) => s.textContent), ["Part", "Song tempo", "Song transpose", "Repeat", "Play mode", "Seek", "A-B repeat"]);
  panelBtn(el, "options").click();
  assert.equal(more.open, true);
  (more.querySelector('[data-act="options-close"]') as HTMLElement).click();
  assert.equal(more.open, false);
  // the two lists: the piano's songs first, the USB memory behind the switch (remembered while the app runs)
  const lists = [...el.querySelectorAll<HTMLElement>("#rec-lists button")];
  assert.deepEqual(lists.map((b) => [b.textContent, b.classList.contains("on")]), [["Piano songs", true], ["USB memory", false]]);
  assert.deepEqual([(el.querySelector("#songs-pane") as HTMLElement).hidden, (el.querySelector("#usb-pane") as HTMLElement).hidden], [false, true]);
  assert.equal(el.querySelectorAll("#songs-list .song-row").length, 10);
  lists[1].click();
  assert.deepEqual([(el.querySelector("#songs-pane") as HTMLElement).hidden, (el.querySelector("#usb-pane") as HTMLElement).hidden], [true, false]);
  const again = recorderScreen(ctx);
  assert.equal((again.querySelector("#usb-pane") as HTMLElement).hidden, false, "the list last shown");
  el.remove();
});

test("recording panel on OpenGrand (feat2 0x10): state line, selected song, op 0x1E play / select, refusal text, notices", async () => {
  const store = new ParamStore();
  const s = fakeSession(0x1f);
  const ctx = createContext(store, { session: () => s as never });
  ctx.usbFiles = ["A.MID"];
  const el = recorderScreen(ctx);
  document.body.append(el);
  await wait();
  assert.equal(stateText(el), "Stopped");
  assert.equal(selected(el), "Song 13 bars", "the piano's selected song, with what it holds");
  assert.equal(songRow(el, 1).getAttribute("data-on"), "1");
  assert.deepEqual([1, 2, 3].map((n) => songRow(el, n).querySelector(".song-info")!.textContent), ["3 bars", "5 bars · Part 1 + Part 2", "Empty"]);
  panelBtn(el, "play").click();
  await wait();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 1, 0, 0]);
  assert.equal(stateText(el), "Playing");
  assert.equal(position(el), "2 / 3 bars", "the piano's bar of the song's bars");
  assert.equal(el.querySelector("#rec-panel")!.getAttribute("data-play"), "1");
  assert.equal(mainAct(el), "pause", "the one play / pause button now pauses");
  assert.equal(songRow(el, 1).getAttribute("data-playing"), "1", "the playing song's row is marked");
  assert.ok(songRow(el, 1).querySelector(".eq"));
  // the progress line is a slider on this piano: letting go on bar 3 sends the bar seek (op 0x1E cmd 7) once
  const range = el.querySelector("#rec-panel .pl-range") as HTMLInputElement;
  assert.deepEqual([range.hidden, range.min, range.max, range.value], [false, "1", "3", "2"]);
  const n0 = s.asked.length;
  change(range, "3", "input");
  assert.equal(position(el), "3 / 3 bars", "the dot follows the finger");
  assert.equal(s.asked.length, n0, "nothing is sent while it is dragged");
  change(range, "3");
  await wait();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 7, 0, 3]);
  assert.equal(s.asked.length, n0 + 1);
  // pause is the same button; then it plays again
  panelBtn(el, "pause").click();
  await wait();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 3, 0, 0]);
  assert.deepEqual([stateText(el), mainAct(el)], ["Paused", "play"]);
  panelBtn(el, "stop").click();
  await wait();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 2, 0, 0]);
  assert.equal(stateText(el), "Stopped");
  assert.equal(position(el), "1 / 3 bars", "stopped = at the first bar");
  // ⏭ / ⏮: the next / previous song number (op select)
  panelBtn(el, "next").click();
  await wait();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 4, 0, 1]);
  assert.equal(selected(el), "Song 25 bars · Part 1 + Part 2");
  panelBtn(el, "prev").click();
  await wait();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 4, 0, 0]);
  assert.equal(panelBtn(el, "prev").disabled, true, "song 1 has no previous");
  // a USB file chosen (nothing plays): the bar holds it - until the piano song plays (started on the panel): then the
  // bar is the song's again and ■ is the recorder's stop, not the USB players'
  (el.querySelector('[data-file="A.MID"] [data-act="file-pick"]') as HTMLElement).click();
  await wait(5);
  assert.equal(selected(el), "A.MIDUSB");
  s.push(frame([0x00, 0x00, ...ng.pack7(statusRaw(1, 0, 2))]));
  await wait(5);
  assert.equal(selected(el), "Song 13 bars");
  panelBtn(el, "stop").click();
  await wait();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 2, 0, 0]);
  // song 3 (empty): chosen in the list, the piano refuses Play with rc 6
  (songRow(el, 3).querySelector('[data-act="song-pick"]') as HTMLElement).click();
  await wait();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 4, 0, 2]);
  assert.equal(selected(el), "Song 3Empty");
  assert.equal(songRow(el, 3).querySelector('[data-act="song-more"]'), null, "an empty song has nothing to export or delete");
  panelBtn(el, "play").click();
  await wait();
  assert.equal(el.querySelector("#rec-note")!.textContent, "Nothing recorded in this song.");
  // an unsolicited notice: the panel selected song 2 and recording waits there
  s.push(frame([0x00, 0x00, ...ng.pack7(statusRaw(3, 1))]));
  await wait(5);
  assert.equal(stateText(el), "Ready to record");
  assert.equal(selected(el), "Song 25 bars · Part 1 + Part 2");
  assert.equal((el.querySelector("#rec-hint") as HTMLElement).hidden, false);
  // while a recording waits or runs the op does not act: Stop goes as the Kawai Music byte (no session on this store: not sent),
  // and another song cannot be chosen
  const before = s.asked.length;
  panelBtn(el, "stop").click();
  (songRow(el, 1).querySelector('[data-act="song-pick"]') as HTMLElement).click();
  await wait(5);
  assert.equal(s.asked.length, before, "no op 0x1E in a record state");
  assert.equal(el.querySelector("#rec-note")!.textContent, "Stop the recording before choosing another song.");
  assert.equal(selected(el), "Song 25 bars · Part 1 + Part 2");
  el.remove();
});

test("owner 2026-11-23: a tap on a song row plays a recorded song (an empty one is only chosen); the part shows only where it means something", async () => {
  const store = new ParamStore();
  const s = fakeSession(0x1f);
  const ctx = createContext(store, { session: () => s as never });
  ctx.usbFiles = ["A.MP3", "B.KSO"];
  const el = recorderScreen(ctx);
  document.body.append(el);
  await wait();
  // song 2 holds a recording: one tap selects it and plays it (op 0x1E select, then play)
  (songRow(el, 2).querySelector('[data-act="song-pick"]') as HTMLElement).click();
  await wait();
  assert.deepEqual(s.asked.slice(-2), [[0x1e, 0x02, 4, 0, 1], [0x1e, 0x02, 1, 0, 0]]);
  assert.equal(stateText(el), "Playing");
  // the playing song's row again: nothing is sent
  const n = s.asked.length;
  (songRow(el, 2).querySelector('[data-act="song-pick"]') as HTMLElement).click();
  await wait();
  assert.equal(s.asked.length, n);
  panelBtn(el, "stop").click();
  await wait();
  // song 3 is empty: chosen (it is where Rec records), not played
  (songRow(el, 3).querySelector('[data-act="song-pick"]') as HTMLElement).click();
  await wait();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 4, 0, 2]);
  // the part: both parts (the usual case) puts nothing on the bar; one part alone does
  const pill = panelBtn(el, "part");
  const partRow = el.querySelector("#rec-panel-options .pl-part-row") as HTMLElement;
  store.fromWire({ fn: 0x10, v1: 0x52, v2: 0x01, part: 0x7f, data: [0x7f] });
  await wait(2);
  assert.equal(pill.hidden, true, "both parts: no pill");
  store.fromWire({ fn: 0x10, v1: 0x52, v2: 0x01, part: 0x7f, data: [0] });
  await wait(2);
  assert.deepEqual([pill.hidden, pill.textContent], [false, "Part1"]);
  assert.equal(partRow.hidden, false);
  assert.match(partRow.textContent!, /each hand can be recorded on its own/);
  // a USB file in the bar has no parts: no pill, no part row in the options
  (el.querySelector("#rec-lists [data-list=usb]") as HTMLElement).click();
  (el.querySelector('[data-file="A.MP3"] [data-act="file-pick"]') as HTMLElement).click();
  await wait(5);
  assert.equal(selected(el), "A.MP3USB");
  assert.deepEqual([pill.hidden, partRow.hidden], [true, true]);
  // into a piano song: only a KSO (the piano refuses MID / MP3 / WAV)
  for (const f of ["A.MP3", "B.KSO"]) (el.querySelector(`[data-file="${f}"] [data-act="file-more"]`) as HTMLElement).click();
  assert.equal(el.querySelector('[data-file="A.MP3"] [data-act="import"]'), null);
  assert.equal(el.querySelector('[data-file="B.KSO"] [data-act="import"]')!.textContent, "Into piano song 3");
  el.remove();
});

/** A stock piano: pianoPair ACKs every fn 0x10; nothing answers Identify. */
async function stock() {
  const p = await pianoPair();
  const store = new ParamStore();
  bindStore(store);
  store.attach(p.session);
  const ctx = createContext(store, { session: () => p.session });
  const el = recorderScreen(ctx);
  document.body.append(el);
  await tick(10);
  const music = (v: number) => `F0 40 7F 32 08 02 52 08 7F 0${v} F7`;
  return { ...p, store, ctx, el, music };
}
const MODE2 = "F0 40 7F 31 08 02 52 1D 7F 02 F7";

test("recording panel on the stock: Rec needs a song, asks before recording over one, then MusicMode 2 + Music 3; Stop / Play are Music bytes", async () => {
  const { el, sent, store, music, notify } = await stock();
  // the piano's song is not known yet: nothing is recorded blindly
  panelBtn(el, "rec").click();
  await tick(10);
  assert.equal(el.querySelector("#rec-note")!.textContent, "Choose a song first.");
  assert.ok(!sent.includes(music(3)));
  // choose song 4: MusicMode 2 once, SongSelect; the stock answers with its length (MaxTime 10 52 18, both parts on)
  store.fromWire({ fn: 0x10, v1: 0x52, v2: 0x01, part: 0x7f, data: [0x7f] });
  (songRow(el, 4).querySelector('[data-act="song-pick"]') as HTMLElement).click();
  await tick(20);
  assert.ok(sent.indexOf(MODE2) >= 0 && sent.indexOf(MODE2) < sent.indexOf("F0 40 7F 10 08 02 52 00 7F 00 00 03 F7"), "MusicMode 2, then SongSelect 4");
  notify([0x52, 0x18, 0x7f, 0, 0, 9, 0, 0, 0]);
  await tick(5);
  assert.equal(selected(el), "Song 49 bars");
  assert.equal(songRow(el, 4).getAttribute("data-state"), "data");
  // Rec on a song with data: a question first, nothing sent
  const over = el.querySelector("#rec-overwrite") as HTMLElement;
  panelBtn(el, "rec").click();
  await tick(10);
  assert.equal(over.hidden, false);
  assert.match(over.textContent!, /Song 4 already has a recording\. Recording again replaces it\./);
  assert.ok(!sent.includes(music(3)));
  (over.querySelector('[data-act="rec-cancel"]') as HTMLElement).click();
  assert.equal(over.hidden, true);
  panelBtn(el, "rec").click();
  (over.querySelector('[data-act="rec-confirm"]') as HTMLElement).click();
  await tick(20);
  assert.ok(sent.includes(music(3)), "record standby");
  assert.equal(sent.filter((m) => m === MODE2).length, 1, "the mode is not sent again");
  // the piano's Music report drives the state line
  notify([0x52, 0x08, 0x7f, 3]);
  await tick(5);
  assert.equal(stateText(el), "Ready to record");
  panelBtn(el, "play").click(); // starts the recording that waits
  await tick(10);
  assert.ok(sent.includes(music(1)));
  notify([0x52, 0x08, 0x7f, 4]);
  await tick(5);
  assert.equal(stateText(el), "Recording");
  assert.equal(el.querySelector("#rec-panel")!.getAttribute("data-play"), "4");
  panelBtn(el, "stop").click();
  await tick(10);
  assert.ok(sent.includes(music(2)));
  notify([0x52, 0x08, 0x7f, 2]);
  await tick(5);
  assert.equal(stateText(el), "Stopped");
  // an empty song records at once
  (songRow(el, 6).querySelector('[data-act="song-pick"]') as HTMLElement).click();
  await tick(20);
  notify([0x52, 0x18, 0x7f, 0, 0, 0, 0, 0, 0]);
  await tick(5);
  assert.equal(selected(el), "Song 6Empty");
  const n = sent.filter((m) => m === music(3)).length;
  panelBtn(el, "rec").click();
  await tick(20);
  assert.equal(over.hidden, true);
  assert.equal(sent.filter((m) => m === music(3)).length, n + 1);
  el.remove();
});
