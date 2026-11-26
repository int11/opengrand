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
const { transport } = await import("../src/ui/transport.ts");

function statusRaw(state: number, song: number, bar = 0, store = 0, recPart = 0): number[] {
  const raw = new Array(48).fill(0);
  raw[0] = 1; raw[1] = state; raw[3] = song; raw[4] = recPart; raw[6] = 2; raw[8] = 120; raw[10] = 120; raw[16] = 3; raw[29] = bar;
  raw[19] = store; // the song table check: 0 OK, 0xFF not read yet, else broken (recview.h RV_STORE_*)
  return raw;
}

/** full: every song holds a recording; bad: the table failed its check - the core sends 70 zero bytes */
function listRaw(o: { full?: boolean; bad?: boolean } = {}): number[] {
  const raw = new Array(70).fill(0);
  if (o.bad) return raw;
  const put = (i: number, len: number, bars: number, flags: number) => {
    raw[7 * i] = len & 0xff; raw[7 * i + 1] = len >> 8; raw[7 * i + 4] = bars; raw[7 * i + 6] = flags;
  };
  put(0, 0x200, 3, 1);
  put(1, 0x300, 5, 3);
  for (let i = 2; i < 10; i++) put(i, o.full ? 0x180 : 0x74, o.full ? 2 : 0, o.full ? 1 : 0);
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

/** The Identify of the field piano on 2026-11-24 (ota_main): ngver 4, V2.19, feat 3B 77 7F 3F - feat3 0x01 = the core's
 *  record control (op 0x1E cmd 8-11). */
const OTA_MAIN_ID = [0x04, 0x02, 0x13, 0x3b, 0x77, 0x7f, 0x3f];

/** A session stand-in: Identify (feat2, or a whole identify), op 0x1E 00 / 01 / 02 like recctl_cave.c - with id's
 *  feat3 0x01 also the record commands 8 / 11 (R2). store: the song table check (status byte 19). */
function fakeSession(feat2: number, o: { id?: number[]; store?: number; full?: boolean; dropStatus?: number; recPart?: number } = {}) {
  const listeners: ((m: Uint8Array) => void)[] = [];
  const asked: number[][] = [];
  const push = (m: Uint8Array) => queueMicrotask(() => listeners.forEach((l) => l(m)));
  const reply = (op: number, data: number[]) => push(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...data, 0xf7]));
  let state = 2, song = 0, bar = 0;
  const rec2 = !!o.id && ((o.id[6] ?? 0) & 1) === 1;
  let store = o.store ?? 0;
  // status byte 4: the part a recording goes to - cmd 8 with part 0 / 1 sets it (recctl.c rec_arm)
  let recPart = o.recPart ?? 0;
  return {
    /** the piano's table check changes (a recovery) */
    setStore(v: number) {
      store = v;
    },
    state: "ready",
    asked,
    push,
    /** the Kawai frames the app sent through the session (none expected from the recorder tab with a stand-in store) */
    params: [] as unknown[],
    onMidi(fn: (m: Uint8Array) => void) {
      listeners.push(fn);
      return () => listeners.splice(listeners.indexOf(fn), 1);
    },
    // the file client the USB list makes (no Kawai frames here)
    onFrame: () => () => {},
    onState: () => () => {},
    async sendParam(...a: unknown[]) {
      this.params.push(a);
      return true;
    },
    sendRaw(m: Uint8Array) {
      const op = m[6]!;
      asked.push([...m.slice(6, m.length - 1)]);
      if (op === 0x01) reply(0x01, o.id ?? [1, 2, 19, 0x3b, 0x77, feat2]);
      if (op !== 0x1e) return;
      const sub = m[7]!;
      if (sub === 0 && o.dropStatus && o.dropStatus-- > 0) return; // the answer is lost
      if (sub === 0) reply(0x1e, [0, 0, ...ng.pack7(statusRaw(state, song, bar, store, recPart))]);
      if (sub === 1) reply(0x1e, [1, 0, ...ng.pack7(listRaw({ full: o.full, bad: store !== 0 }))]);
      if (sub === 2) {
        const cmd = m[8]!, arg = (m[9]! << 7) | m[10]!;
        const recState = state >= 3 && state <= 6;
        if (cmd >= 8 && !rec2) return reply(0x1e, [2, 2, cmd]); // unknown cmd without feat3 0x01
        if (store !== 0 && cmd !== 2 && cmd !== 3) return reply(0x1e, [2, 3, cmd]); // table check failed: busy
        if (recState && [1, 4, 5, 6, 7].includes(cmd)) return reply(0x1e, [2, 4, cmd]);
        if (cmd === 1 && song >= 2) return reply(0x1e, [2, 6, cmd]); // nothing recorded
        if (cmd === 8 && song < 2 && !(arg & 0x100)) return reply(0x1e, [2, 11, cmd]); // the part holds a recording
        if (cmd === 11 && state !== 3 && state !== 5 && state !== 6) return reply(0x1e, [2, 10, cmd]);
        reply(0x1e, [2, 0, cmd]);
        if (cmd === 1) { state = 1; bar = bar || 2; }
        if (cmd === 2) { state = 2; bar = 0; }
        if (cmd === 3) state = state === 4 ? 2 : recState ? state : 0;
        if (cmd === 4) { song = arg; bar = 0; }
        if (cmd === 7) bar = arg;
        if (cmd === 8) { state = 3; if ((arg & 0x7f) !== 0x7f) recPart = arg & 0x7f; }
        if (cmd === 11) { state = 4; bar = 1; }
        reply(0x1e, [0, 0, ...ng.pack7(statusRaw(state, song, bar, store, recPart))]);
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
  // Rec records into the piano: no target or format to choose first (owner 2026-11-23), and no part on the bar
  assert.equal(el.querySelector("#rec-target"), null);
  assert.equal(panelBtn(el, "part").hidden, true);
  // owner 2026-11-24: no 「피아노」 badge, no 「곡 N」 next to the number - number + what the song holds
  assert.equal(songRow(el, 1).querySelector(".file-ext"), null);
  assert.equal(songRow(el, 1).querySelector(".song-name"), null);
  assert.match(songRow(el, 1).querySelector(".song-pick")!.getAttribute("aria-label")!, /^Song 1 · /);
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
  // the tap also asked the USB player to play A.MID - a transport job (ui/transport.ts); this stand-in has no file channel,
  // so it ends only when its file request times out. The ■ below waits for it (one sequence at a time).
  for (let i = 0; i < 400 && transport(ctx).busy(); i++) await tick(50);
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
  assert.equal(pill.hidden, true, "one part: said in the options sheet only");
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
  assert.equal(el.querySelector('[data-file="B.KSO"] [data-act="import"]')!.textContent, "Into the piano");
  assert.deepEqual([...el.querySelectorAll('[data-file="A.MP3"] .row-acts button')].map((b) => b.getAttribute("data-act")), ["rename", "delete"]);
  assert.equal(el.querySelector("#usb-import-song"), null, "no import target to choose");
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
  // ▶ in standby on the stock: nothing is sent - Music 1 would play the song (FUN_4016A050 state 3) and Music 4 is never
  // sent in MusicMode 2 (field incident 2026-11-24); the first key starts the recording
  assert.equal(el.querySelector("#rec-hint")!.textContent, "Play a key to start recording.");
  const n0 = sent.length;
  panelBtn(el, "play").click();
  await tick(10);
  assert.equal(sent.length, n0, "nothing sent");
  assert.equal(el.querySelector("#rec-note")!.textContent, "Play a key to start recording.");
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
  assert.ok(!sent.includes(music(4)), "never Music 4 (32 52 08 7F 04)");
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

test("owner 2026-11-23, rule 2: Rec records into an empty piano song chosen by the app; when it stops the sheet says where it is and offers USB", async () => {
  const store = new ParamStore();
  const s = fakeSession(0x1f);
  const params: number[][] = [];
  // the Kawai bytes this screen sends as commands (the store has no session here)
  store.command = async (p, _part, data) => (params.push([p.fn, p.v1, p.v2, ...data]), true);
  const ctx = createContext(store, { session: () => s as never });
  const el = recorderScreen(ctx);
  document.body.append(el);
  await wait();
  // song 1 (selected) and song 2 hold recordings, song 3 is the first empty one: no question, no target, no format
  assert.equal(selected(el), "Song 13 bars");
  panelBtn(el, "rec").click();
  await wait();
  assert.equal((el.querySelector("#rec-overwrite") as HTMLElement).hidden, true, "nothing is asked while a song is empty");
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 4, 0, 2], "op select: song 3");
  assert.deepEqual(params.at(-1), [0x32, 0x52, 0x08, 3], "Music 3 = record standby");
  assert.match(selected(el)!, /^Song 3/);
  // the piano records in song 3, then stops
  const sheet = el.querySelector("#save-sheet") as HTMLElement;
  s.push(frame([0x00, 0x00, ...ng.pack7(statusRaw(4, 2, 1))]));
  await wait(4);
  assert.equal(sheet.hasAttribute("open"), false);
  s.push(frame([0x00, 0x00, ...ng.pack7(statusRaw(2, 2))]));
  await wait(4);
  assert.equal(sheet.hasAttribute("open"), true);
  assert.deepEqual([sheet.querySelector("h3")!.textContent, sheet.querySelector("#save-sheet-sub")!.textContent], ["Saved: piano song 3", "Also save to USB"]);
  assert.deepEqual([...sheet.querySelectorAll(".chip")].map((c) => c.textContent), ["MID", "KSO", "MP3", "WAV"]);
  (sheet.querySelector('[data-act="save-close"]') as HTMLElement).click();
  // the same sheet from a song's ⋯ menu (rule 3)
  (songRow(el, 2).querySelector('[data-act="song-more"]') as HTMLElement).click();
  assert.deepEqual([...songRow(el, 2).querySelectorAll(".row-acts button")].map((b) => b.textContent), ["Save to USB"]);
  (songRow(el, 2).querySelector('[data-act="song-save"]') as HTMLElement).click();
  assert.deepEqual([sheet.hasAttribute("open"), sheet.querySelector("h3")!.textContent, sheet.querySelector("#save-sheet-sub")!.textContent], [true, "Song 2", "Save to USB"]);
  el.remove();
});

test("field 2026-11-27: a recording another device started (its notices reach every controller) opens no sheet here", async () => {
  const store = new ParamStore();
  const s = fakeSession(0x1f);
  store.command = async () => true;
  const ctx = createContext(store, { session: () => s as never });
  const el = recorderScreen(ctx);
  document.body.append(el);
  await wait();
  const sheet = el.querySelector("#save-sheet") as HTMLElement;
  // Rec was pressed elsewhere (the desktop): this screen only sees the piano record in song 3 and stop
  s.push(frame([0x00, 0x00, ...ng.pack7(statusRaw(3, 2))]));
  await wait(4);
  s.push(frame([0x00, 0x00, ...ng.pack7(statusRaw(4, 2, 1))]));
  await wait(4);
  s.push(frame([0x00, 0x00, ...ng.pack7(statusRaw(2, 2))]));
  await wait(4);
  assert.equal(sheet.hasAttribute("open"), false, "not this device's recording: no 「Saved」 sheet");
  el.remove();
});

test("field 2026-11-23: in MusicMode 3 the piano's state and position are a USB player's - the bar does not show the piano song as playing", async () => {
  const store = new ParamStore();
  const s = fakeSession(0x1f);
  const ctx = createContext(store, { session: () => s as never });
  const el = recorderScreen(ctx);
  document.body.append(el);
  await wait();
  const usb = statusRaw(1, 1, 13);
  usb[6] = 3; // MusicMode 3: USB memory
  s.push(frame([0x00, 0x00, ...ng.pack7(usb)]));
  await wait(4);
  assert.equal(stateText(el), "Stopped", "not 「재생 중 · 곡 2 · 13/13마디」");
  assert.equal(songRow(el, 2).getAttribute("data-playing"), null);
  // back in the recorder mode the piano's word counts again
  s.push(frame([0x00, 0x00, ...ng.pack7(statusRaw(1, 1, 2))]));
  await wait(4);
  assert.equal(stateText(el), "Playing");
  el.remove();
});

test("「피아노에 넣기」 on a MID: read, convert to KSO (checked), write it next to the MID, then 24 09 into an empty song", async () => {
  const { checkKso } = await import("../src/protocol/kso.ts");
  const { buildSmf, note: smfNote } = await import("./fixtures/kso_cases.ts");
  const store = new ParamStore();
  const s = fakeSession(0x1f);
  const calls: unknown[][] = [];
  const written = new Map<string, Uint8Array>();
  let corrupt = false;
  let smf: Uint8Array = buildSmf(0, 96, [smfNote(0, 0, 96, 60)]);
  const files = {
    list: async () => ctx.usbFiles,
    read: async (n: string) => (calls.push(["read", n]), written.get(n) ?? smf),
    write: async (n: string, d: Uint8Array) => (calls.push(["write", n, checkKso(d)]), written.set(n, corrupt ? d.slice(1) : d), ctx.usbFiles.push(n), 0),
    delete: async (n: string) => (calls.push(["delete", n]), 0),
    load: async (n: string, song: number) => (calls.push(["load", n, song]), 0),
  };
  const ctx = createContext(store, { session: () => s as never, files: () => files as never });
  ctx.usbFiles = ["S.MID", "S.KSO", "BAD.MID"];
  const el = recorderScreen(ctx);
  document.body.append(el);
  await wait();
  (el.querySelector("#rec-lists [data-list=usb]") as HTMLElement).click();
  for (const f of ["S.MID", "BAD.MID"]) (el.querySelector(`[data-file="${f}"] [data-act="file-more"]`) as HTMLElement).click();
  (el.querySelector('[data-file="S.MID"] [data-act="import"]') as HTMLElement).click();
  await wait();
  // songs 1 and 2 are recorded: song 3 (index 2) is the first empty one; S.KSO exists, so the new file is S_2.KSO
  assert.deepEqual(calls, [["read", "S.MID"], ["write", "S_2.KSO", null], ["read", "S_2.KSO"], ["load", "S_2.KSO", 2]], "the KSO passed checkKso and was read back before 24 09");
  assert.equal(el.querySelector("#rec-note")!.textContent, "S.MID is now piano song 3.");
  // a file the converter refuses: said in plain words, nothing written, nothing imported
  calls.length = 0;
  smf = Uint8Array.from([1, 2, 3]);
  (el.querySelector('[data-file="BAD.MID"] [data-act="import"]') as HTMLElement).click();
  await wait();
  assert.deepEqual(calls, [["read", "BAD.MID"]]);
  assert.equal(el.querySelector("#usb-result")!.textContent, "Convert to a piano song failed: This is not a MIDI file the app can read.");
  // the file on the USB memory is not what was written: deleted, never imported (a bad KSO can format all ten songs)
  calls.length = 0;
  corrupt = true;
  smf = buildSmf(0, 96, [smfNote(0, 0, 96, 60)]);
  (el.querySelector('[data-file="S.MID"] [data-act="import"]') as HTMLElement).click();
  await wait();
  assert.deepEqual(calls.map((c) => c[0]), ["read", "write", "read", "delete"]);
  assert.match(el.querySelector("#usb-result")!.textContent!, /did not pass the safety check/);
  el.remove();
});

/** Every Kawai byte the recorder tab sends through the store, as [fn, v1, v2, ...data]. */
function tapStore(store: InstanceType<typeof ParamStore>) {
  const params: number[][] = [];
  store.command = async (p, _part, data) => (params.push([p.fn, p.v1, p.v2, ...data]), true);
  store.set = async (p, _part, data) => (params.push([p.fn, p.v1, p.v2, ...data]), true);
  store.send = async (m) => (params.push([m.fn ?? 0x10, m.v1, m.v2, ...m.data]), true);
  return params;
}
/** a Music command (fn 0x10 / 0x31 / 0x32, 52 08) - the recorder tab must not send any on a piano with the record op */
const isMusic = (p: number[]) => p[1] === 0x52 && p[2] === 0x08;

test("decode: a zero-length song entry is not known (never empty); empty = 0x74 bytes and no track; status byte 19 / 28", () => {
  const raw = listRaw();
  for (let i = 7 * 4; i < 7 * 5; i++) raw[i] = 0; // song 5: all zero
  raw[7 * 5] = 0x74; raw[7 * 5 + 6] = 4; // song 6: an empty-sized frame with an extra track
  const songs = ng.decodeSongList(Uint8Array.from(raw))!;
  assert.deepEqual([songs[2], songs[4], songs[5]].map((g) => [g.empty, g.unknown]), [[true, false], [false, true], [false, false]]);
  assert.ok(ng.decodeSongList(new Uint8Array(70))!.every((g) => g.unknown && !g.empty), "70 zero bytes: nothing known");
  const st = statusRaw(2, 0, 0, 7);
  st[28] = 3;
  const d = ng.decodeRecorderStatus(Uint8Array.from(st))!;
  assert.deepEqual([d.store, d.storeAt, ng.recorderTableOk(d)], [7, 3, false]);
  assert.equal(ng.recorderTableOk(ng.decodeRecorderStatus(Uint8Array.from(statusRaw(2, 0, 0, 0xff)))), false, "0xFF: not read yet");
  assert.equal(ng.recorderTableOk(ng.decodeRecorderStatus(Uint8Array.from(statusRaw(2, 0)))), true);
});

test("field incident 2026-11-24, ota_main (feat3 0x01): ● = op cmd 8, ▶ in standby = op cmd 11, ■ = op cmd 2 - no Music byte, never Music 4", async () => {
  const store = new ParamStore();
  const params = tapStore(store);
  const s = fakeSession(0, { id: OTA_MAIN_ID });
  const ctx = createContext(store, { session: () => s as never });
  const el = recorderScreen(ctx);
  document.body.append(el);
  await wait();
  // ● : song 3 is the first empty one - op select, then op record standby (part = the piano's, no overwrite)
  panelBtn(el, "rec").click();
  await wait();
  assert.deepEqual(s.asked.slice(-2), [[0x1e, 0x02, 4, 0, 2], [0x1e, 0x02, 8, 0, 0x7f]]);
  assert.equal(stateText(el), "Ready to record");
  assert.equal((el.querySelector("#rec-hint") as HTMLElement).hidden, false);
  assert.equal(el.querySelector("#rec-hint")!.textContent, "Play a key or press Play to start recording.");
  // ▶ in standby: op 1E 02 0B - the core starts the recording (what Music 1 did not do on the field piano)
  panelBtn(el, "play").click();
  await wait();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 11, 0, 0]);
  assert.equal(stateText(el), "Recording");
  assert.equal(el.querySelector("#rec-note")!.textContent, "");
  // while recording the one play / pause button pauses (= ends the recording through the op, like ■)
  assert.equal(mainAct(el), "pause");
  // ■ ends it through the op (the core commits once)
  panelBtn(el, "stop").click();
  await wait();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 2, 0, 0]);
  assert.equal(stateText(el), "Stopped");
  assert.equal((el.querySelector("#save-sheet") as HTMLElement).hasAttribute("open"), true, "「Saved: piano song 3」");
  // no Kawai Music byte at all - above all no 32 52 08 7F 04
  assert.deepEqual(params.filter(isMusic), []);
  assert.equal(s.params.length, 0);
  assert.deepEqual(s.asked.filter((a) => a[0] === 0x1e && a[1] === 0x02).map((a) => a[2]), [4, 8, 11, 2], "select, standby, start, stop - no play");
  el.remove();
});

test("ota_main (feat3 0x01): recording over a song only after the question, then op cmd 8 with the overwrite bit; ▶ outside standby is not a start", async () => {
  const store = new ParamStore();
  const params = tapStore(store);
  const s = fakeSession(0, { id: OTA_MAIN_ID, full: true });
  const ctx = createContext(store, { session: () => s as never });
  const el = recorderScreen(ctx);
  document.body.append(el);
  await wait();
  const over = el.querySelector("#rec-overwrite") as HTMLElement;
  const n = s.asked.length;
  panelBtn(el, "rec").click();
  await wait();
  assert.equal(over.hidden, false, "every song is recorded: asked first");
  assert.equal(s.asked.length, n, "nothing sent before the answer");
  (over.querySelector('[data-act="rec-confirm"]') as HTMLElement).click();
  await wait();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 8, 2, 0x7f], "0x7F | 0x100: the part may hold a recording");
  assert.equal(stateText(el), "Ready to record");
  panelBtn(el, "stop").click(); // stop in standby: no take
  await wait();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 2, 0, 0]);
  panelBtn(el, "play").click(); // stopped: ▶ plays the song (op cmd 1), never cmd 11
  await wait();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 1, 0, 0]);
  assert.ok(!s.asked.some((a) => a[0] === 0x1e && a[1] === 0x02 && a[2] === 11));
  assert.deepEqual(params.filter(isMusic), []);
  el.remove();
});

test("field incident 2026-11-24: a song table that failed its check - a warning instead of 「Empty」 rows; Rec, import, KSO tap, save refused, nothing sent", async () => {
  const store = new ParamStore();
  const params = tapStore(store);
  const s = fakeSession(0, { id: OTA_MAIN_ID, store: 3 });
  const calls: unknown[][] = [];
  const files = {
    list: async () => ctx.usbFiles,
    read: async (n: string) => (calls.push(["read", n]), new Uint8Array(0)),
    write: async (n: string) => (calls.push(["write", n]), 0),
    delete: async (n: string) => (calls.push(["delete", n]), 0),
    load: async (n: string, song: number) => (calls.push(["load", n, song]), 0),
  };
  const ctx = createContext(store, { session: () => s as never, files: () => files as never });
  ctx.usbFiles = ["B.KSO", "S.MID"];
  const el = recorderScreen(ctx);
  document.body.append(el);
  await wait();
  const BAD = "The piano's song list is not valid. Check it before switching the piano off.";
  const warn = el.querySelector("#songs-table-warn") as HTMLElement;
  assert.deepEqual([warn.hidden, warn.textContent, warn.getAttribute("role")], [false, BAD, "alert"]);
  assert.equal(el.querySelector("#songs-status")!.textContent, BAD);
  for (let n = 1; n <= 10; n++) {
    assert.equal(songRow(el, n).getAttribute("data-state"), "unknown", `song ${n}`);
    assert.equal(songRow(el, n).querySelector(".song-info")!.textContent, "Unknown");
  }
  assert.ok(!el.querySelector("#songs-list")!.textContent!.includes("Empty"));
  const note = () => el.querySelector("#rec-note")!.textContent;
  // a refusal may ask the status again (1E 00, a read); no command, no Kawai byte, no file call
  const cmds = () => s.asked.filter((x) => !(x[0] === 0x01 || (x[0] === 0x1e && x[1] <= 1))).length;
  const sent = () => [cmds(), params.length, s.params.length, calls.length];
  const before = sent();
  // ● : refused with the reason, no song chosen for it, no standby
  panelBtn(el, "rec").click();
  await wait();
  assert.equal(note(), BAD);
  assert.equal((el.querySelector("#rec-overwrite") as HTMLElement).hidden, true);
  // the USB list: a KSO tap (plays through a piano song), 「Into the piano」 on a KSO and on a MID
  (el.querySelector("#rec-lists [data-list=usb]") as HTMLElement).click();
  (el.querySelector('[data-file="B.KSO"] [data-act="file-pick"]') as HTMLElement).click();
  await wait();
  assert.equal(note(), BAD);
  for (const f of ["B.KSO", "S.MID"]) {
    (el.querySelector(`[data-file="${f}"] [data-act="file-more"]`) as HTMLElement).click();
    (el.querySelector(`[data-file="${f}"] [data-act="import"]`) as HTMLElement).click();
    await wait();
    assert.equal(note(), BAD, f);
  }
  // 「USB로 저장」 from a row's ⋯
  (el.querySelector("#rec-lists [data-list=internal]") as HTMLElement).click();
  (songRow(el, 1).querySelector('[data-act="song-more"]') as HTMLElement).click();
  (songRow(el, 1).querySelector('[data-act="song-save"]') as HTMLElement).click();
  await wait();
  assert.equal((el.querySelector("#save-sheet") as HTMLElement).hasAttribute("open"), false);
  assert.equal(note(), BAD);
  assert.deepEqual(sent(), before, "nothing went to the piano or the USB memory");
  // the piano's check passes again (after the recovery): the list is read again and the warning goes
  s.setStore(0);
  s.push(frame([0x00, 0x00, ...ng.pack7(statusRaw(2, 0))]));
  s.push(frame([0x01, 0x00, ...ng.pack7(listRaw())]));
  await wait();
  assert.equal(warn.hidden, true);
  assert.equal(songRow(el, 3).querySelector(".song-info")!.textContent, "Empty");
  el.remove();
});

test("song table not read yet (status byte 19 = 0xFF): rows unknown, Rec refused with that reason", async () => {
  const store = new ParamStore();
  const params = tapStore(store);
  const s = fakeSession(0, { id: OTA_MAIN_ID, store: 0xff });
  const ctx = createContext(store, { session: () => s as never });
  const el = recorderScreen(ctx);
  document.body.append(el);
  await wait();
  const UNREAD = "The piano has not checked its song list yet.";
  assert.equal(el.querySelector("#songs-table-warn")!.textContent, UNREAD);
  const cmds = () => s.asked.filter((x) => x[0] === 0x1e && x[1] === 0x02).length;
  const n = cmds();
  panelBtn(el, "rec").click();
  await wait();
  assert.equal(el.querySelector("#rec-note")!.textContent, UNREAD);
  assert.equal(cmds(), n);
  assert.deepEqual(params, []);
  el.remove();
});

test("the first status answer is lost: nothing is taken as empty until a status says the table is good, then the list is read again", async () => {
  const store = new ParamStore();
  const params = tapStore(store);
  const s = fakeSession(0, { id: OTA_MAIN_ID, dropStatus: 1 });
  const ctx = createContext(store, { session: () => s as never });
  const el = recorderScreen(ctx);
  document.body.append(el);
  for (let i = 0; i < 30; i++) await tick(30); // the status times out (700 ms)
  assert.equal(el.querySelector("#songs-table-warn")!.textContent, "The piano has not checked its song list yet.");
  assert.equal(songRow(el, 3).getAttribute("data-state"), "unknown");
  const n = s.asked.filter((a) => a[0] === 0x1e && a[1] === 0x02).length;
  // Rec: refused, and the status is asked again - it answers now (table good): the list is read again
  panelBtn(el, "rec").click();
  await wait(40);
  assert.equal(el.querySelector("#rec-note")!.textContent, "The piano has not checked its song list yet.");
  assert.equal(s.asked.filter((a) => a[0] === 0x1e && a[1] === 0x02).length, n, "no command with the refusal");
  assert.deepEqual(params, []);
  assert.equal((el.querySelector("#songs-table-warn") as HTMLElement).hidden, true);
  assert.equal(songRow(el, 3).querySelector(".song-info")!.textContent, "Empty");
  assert.equal(el.querySelector("#songs-status")!.textContent, "2 of 10 songs recorded");
  el.remove();
});

test("field incident 2026-11-24: ● into part 2 of a song with no part 1 asks first (record part = status byte 4, even with both parts playing) - part 1 / part 2 / cancel", async () => {
  const store = new ParamStore();
  const params = tapStore(store);
  // the options say both parts play (IntSongPart 7F), but the piano still records into part 2 (7F leaves it)
  store.fromWire({ fn: 0x10, v1: 0x52, v2: 0x01, part: 0x7f, data: [0x7f] });
  const s = fakeSession(0, { id: OTA_MAIN_ID, recPart: 1 });
  const ctx = createContext(store, { session: () => s as never });
  const el = recorderScreen(ctx);
  document.body.append(el);
  await wait();
  assert.equal(ng.decodeRecorderStatus(Uint8Array.from(statusRaw(2, 0, 0, 0, 1)))!.recPart, 1, "status byte 4");
  const over = el.querySelector("#rec-overwrite") as HTMLElement;
  const ops = () => s.asked.filter((a) => a[0] === 0x1e && a[1] === 0x02);
  // song 3 is the first empty one: no part 1 there - asked before the song choice goes out
  panelBtn(el, "rec").click();
  await wait();
  assert.equal(over.hidden, false);
  assert.equal(over.querySelector("p")!.textContent, "Song 3 has no part 1, and this take goes into part 2. Record into part 1 instead?");
  assert.deepEqual([...over.querySelectorAll("button")].map((b) => [b.getAttribute("data-act"), b.textContent]),
    [["rec-part1", "● Record into part 1"], ["rec-part2", "Record into part 2"], ["rec-cancel", "Cancel"]]);
  assert.deepEqual(ops(), [], "nothing sent before the answer");
  // cancel: nothing
  (over.querySelector('[data-act="rec-cancel"]') as HTMLElement).click();
  await wait();
  assert.equal(over.hidden, true);
  assert.deepEqual(ops(), []);
  // part 1 instead: select song 3, then record standby into part 0 (the core sets IntSongPart itself)
  panelBtn(el, "rec").click();
  await wait();
  (over.querySelector('[data-act="rec-part1"]') as HTMLElement).click();
  await wait();
  assert.deepEqual(ops(), [[0x1e, 0x02, 4, 0, 2], [0x1e, 0x02, 8, 0, 0]]);
  assert.equal(stateText(el), "Ready to record");
  assert.deepEqual(params.filter(isMusic), []);
  el.remove();
});

test("part 2 anyway keeps today's bytes; a song with part 1 records part 2 without a question", async () => {
  const store = new ParamStore();
  tapStore(store);
  store.fromWire({ fn: 0x10, v1: 0x52, v2: 0x01, part: 0x7f, data: [1] });
  const s = fakeSession(0, { id: OTA_MAIN_ID, recPart: 1 });
  const ctx = createContext(store, { session: () => s as never });
  const el = recorderScreen(ctx);
  document.body.append(el);
  await wait();
  const over = el.querySelector("#rec-overwrite") as HTMLElement;
  const ops = () => s.asked.filter((a) => a[0] === 0x1e && a[1] === 0x02);
  // song 1 (selected) holds part 1 alone and part 2 is chosen: its second hand - nothing asked
  assert.equal(selected(el), "Song 13 bars");
  panelBtn(el, "rec").click();
  await wait();
  assert.equal(over.hidden, true);
  assert.deepEqual(ops(), [[0x1e, 0x02, 8, 0, 0x7f]]);
  panelBtn(el, "stop").click();
  await wait();
  // an empty song: asked; 「part 2」 sends what ● always sent (the piano's part)
  (songRow(el, 4).querySelector('[data-act="song-pick"]') as HTMLElement).click();
  await wait();
  const n = ops().length;
  panelBtn(el, "rec").click();
  await wait();
  assert.equal(over.hidden, false);
  assert.equal(ops().length, n);
  (over.querySelector('[data-act="rec-part2"]') as HTMLElement).click();
  await wait();
  assert.deepEqual(ops().at(-1), [0x1e, 0x02, 8, 0, 0x7f]);
  el.remove();
});

test("without the record control (op 0x1E, Music bytes): part 1 instead = IntSongPart 0, then Music 3; part 1 recorded = no question", async () => {
  const store = new ParamStore();
  const params = tapStore(store);
  const s = fakeSession(0x1f, { recPart: 1 });
  const ctx = createContext(store, { session: () => s as never });
  const el = recorderScreen(ctx);
  document.body.append(el);
  await wait();
  const over = el.querySelector("#rec-overwrite") as HTMLElement;
  panelBtn(el, "rec").click();
  await wait();
  assert.equal(over.hidden, false);
  assert.ok(!params.some(isMusic));
  (over.querySelector('[data-act="rec-part1"]') as HTMLElement).click();
  await wait();
  const i = params.findIndex((p) => p[1] === 0x52 && p[2] === 0x01);
  assert.deepEqual(params[i], [0x10, 0x52, 0x01, 0], "IntSongPart 0");
  assert.deepEqual(params.slice(i + 1).filter(isMusic), [[0x32, 0x52, 0x08, 3]], "then Music 3");
  el.remove();
  // the piano records part 1: nothing asked
  const s1 = fakeSession(0x1f, { recPart: 0 });
  const el1 = recorderScreen(createContext(store, { session: () => s1 as never }));
  document.body.append(el1);
  await wait();
  panelBtn(el1, "rec").click();
  await wait();
  assert.equal((el1.querySelector("#rec-overwrite") as HTMLElement).hidden, true);
  el1.remove();
});

test("ko: the part-2 question reads Korean", async () => {
  const i18n = await import("../src/i18n.ts");
  const store = new ParamStore();
  tapStore(store);
  const s = fakeSession(0, { id: OTA_MAIN_ID, recPart: 1 });
  i18n.setLang("ko");
  try {
    const el = recorderScreen(createContext(store, { session: () => s as never }));
    document.body.append(el);
    await wait();
    panelBtn(el, "rec").click();
    await wait();
    const over = el.querySelector("#rec-overwrite") as HTMLElement;
    assert.match(over.querySelector("p")!.textContent!, /^곡 3에는 파트 1이 없는데 이 녹음은 파트 2로 들어갑니다\./);
    assert.deepEqual([...over.querySelectorAll("button")].map((b) => b.textContent), ["● 파트 1로 녹음", "파트 2로 녹음", "취소"]);
    el.remove();
  } finally {
    i18n.setLang("en");
  }
});

test("song tempo reads a share of the song's own tempo (op status bytes 10-11) on OpenGrand; the plain BPM without it", async () => {
  const { songTempoText } = await import("../src/ui/screens/songctl.ts");
  assert.equal(songTempoText(96, 120), "♩=96 · 0.80×");
  assert.equal(songTempoText(240, 120), "♩=240 · 2.00×");
  assert.equal(songTempoText(96, null), "♩=96");
  assert.equal(songTempoText(96, 0), "♩=96", "0 = the piano does not know it: no share invented");
  const store = new ParamStore();
  bindStore(store);
  const s = fakeSession(0x1f);
  const ctx = createContext(store, { session: () => s as never });
  const el = recorderScreen(ctx);
  document.body.append(el);
  await wait();
  const out = () => el.querySelector("#rec-panel-options output")!.textContent;
  assert.equal(out(), "–", "the piano's song tempo is not known yet");
  // the song tempo (MusicTempo 10 52 02 7F hi lo) comes; the status says the song's own tempo is 120
  store.fromWire({ fn: 0x10, v1: 0x52, v2: 0x02, part: 0x7f, data: [0, 96] });
  await wait();
  assert.equal(out(), "♩=96 · 0.80×");
  store.fromWire({ fn: 0x10, v1: 0x52, v2: 0x02, part: 0x7f, data: [1, 0x10] });
  await wait();
  assert.equal(out(), "♩=144 · 1.20×", "a tempo change keeps the base");
  el.remove();
  // the stock (no op status): no base, the plain BPM
  const { el: el2, store: store2 } = await stock();
  store2.fromWire({ fn: 0x10, v1: 0x52, v2: 0x02, part: 0x7f, data: [0, 96] });
  await tick(10);
  assert.equal(el2.querySelector("#rec-panel-options output")!.textContent, "♩=96");
  el2.remove();
});
