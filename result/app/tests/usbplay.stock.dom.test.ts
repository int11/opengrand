// USB playback against a fake piano that follows the STOCK's two USB players as the emulator runs them on the real
// stock image + ota_main (result/stockfw/tools/roundtrip_usbplay.py
// 「USB 재생 겹침 · 정지」):
//   - 24 08 <file> sets the selected type (0x413CA3F5) FIRST; while that type's player plays the stock answers Busy
//     (7E 03 01 00 00) and loads nothing (FUN_4014F364 job 8).
//   - Music (32 52 08) in MusicMode 3 goes to the selected type's player only (FUN_4015DBD4): MID -> SMF player,
//     MP3 / WAV -> audio player; other modes report 2 and touch no player (FUN_4015DB98).
//   - the Music report (FUN_4015C27C) is the selected type's player's: SMF Play -> 32 52 08 7F 01, SMF stop ->
//     bar 1 twice + 7F 02; the audio player sends nothing right away.
//   - MusicMode 0 / 5 from 3 stops the selected type's player only (FUN_4015DCC4), MusicMode 2 none.
//   - the SMF player sends MusicProgress 32 52 03 7F 00 00 <bar> <beat> 00 00 every beat while it plays.
// The field of 2026-11-13: SONG_01.MID ▶ (row stayed ▶), then 1.mp3 ▶ (both sounded), then 「■ 정지」 stopped only
// the MP3 and Reset / Pause / MusicMode 0 / 3 / 2 never reached the MID.
import { pianoPair, tick } from "./_dom.ts";
import { afterEach, beforeEach, test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const { bindStore } = await import("../src/ui/controls.ts");
const { createContext } = await import("../src/ui/context.ts");
const { filesScreen } = await import("../src/ui/files.ts");
const { usbListState } = await import("../src/ui/usblist.ts");
const { USB_STOP, usbPlayback } = await import("../src/ui/usbplay.ts");
const FA = await import("../src/protocol/fileaccess.ts");
const { fromHex, toHex } = await import("../src/protocol/sysex.ts");
const i18n = await import("../src/i18n.ts");

Object.assign(USB_STOP, { grace: 10, watch: 120, idle: 400 });
const BEAT_MS = 25;

const closers: (() => void)[] = [];
beforeEach(() => {
  localStorage.clear();
  i18n.setLang("en");
});
afterEach(() => {
  for (const c of closers.splice(0)) c();
  document.body.replaceChildren();
});

const ACK = fromHex("F0 40 7F 24 04 1E 7F F7");
const OK_STOCK = fromHex("F0 40 7F 32 04 1E 51 7E 7F 00 00 F7");
const BUSY = fromHex("F0 40 7F 7E 04 1E 03 01 00 00 F7");
const TYPE_BY_EXT: Record<string, number> = { KSO: 3, MID: 4, MP3: 5, WAV: 6 };
const music = (v: number) => Uint8Array.from([0xf0, 0x40, 0x7f, 0x32, 0x04, 0x1e, 0x52, 0x08, 0x7f, v, 0xf7]);
const progress = (bar: number, beat: number) => Uint8Array.from([0xf0, 0x40, 0x7f, 0x32, 0x04, 0x1e, 0x52, 0x03, 0x7f, 0, bar >> 7, bar & 0x7f, beat, 0, 0, 0xf7]);

type Piano = { send(m: Uint8Array): void; onMessage(h: (m: Uint8Array) => void): void };

/** The stock's USB players as the emulator shows them (see the header). */
function stockPlayers(link: Piano, sent: string[], opt: { stuck?: boolean; lateAudio?: boolean } = {}) {
  // replies a BLE event later (~8 ms), in order - as on the air; a loopback answer inside the send would arrive
  // before the app's own code after the send has run (the field's order: the app's ▶ done, then the echo)
  let lastAt = 0;
  const piano = {
    send: (m: Uint8Array) => {
      const at = Math.max(Date.now() + 8, lastAt + 1);
      lastAt = at;
      setTimeout(() => link.send(m), at - Date.now());
    },
    onMessage: (h: (m: Uint8Array) => void) => link.onMessage(h),
  };
  const st = {
    mode: 0,
    sel: 0,
    smf: 2, // 0x41942842: 0 pause, 1 play, 2 stop
    audio: false, // the audio player sounds
    bar: 1,
    beat: 0,
    smfName: "",
    audioName: "",
    names: [] as string[],
    cur: [] as number[],
    op: 0,
    timer: null as ReturnType<typeof setInterval> | null,
  };
  const clock = () => {
    if (st.smf === 1 && !st.timer) {
      st.timer = setInterval(() => {
        if (++st.beat > 3) (st.beat = 0), st.bar++;
        piano.send(progress(st.bar, st.beat));
      }, BEAT_MS);
    } else if (st.smf !== 1 && st.timer) {
      clearInterval(st.timer);
      st.timer = null;
    }
  };
  closers.push(() => st.timer && clearInterval(st.timer));
  const smfStop = () => {
    if (opt.stuck) return; // a piano that does not stop (the retry / the panel message)
    st.smf = 2;
    st.bar = 1;
    st.beat = 0;
    clock();
    piano.send(progress(1, 0));
    piano.send(progress(1, 0));
  };
  piano.onMessage((m) => {
    sent.push(toHex(m));
    if (m[1] !== 0x40 || m[4] !== 0x08) return;
    const fn = m[3];
    if (fn === 0x10 && m[7] !== 0x1d) return void piano.send(Uint8Array.from([0xf0, 0x40, 0x7f, 0x07, 0x04, 0x1e, m[6], m[7], m[8], 0xf7]));
    if ((fn === 0x31 || fn === 0x10) && m[6] === 0x52 && m[7] === 0x1d) {
      const v = m[9];
      // FUN_4015DCC4: 0 / 5 from 3 stops the selected type's player; any other mode is only stored
      if (v !== st.mode && (v === 0 || v === 5) && st.mode === 3) {
        if (st.sel === 4 && st.smf !== 2) {
          smfStop();
          piano.send(music(st.smf));
        } else if (st.sel === 5 || st.sel === 6) st.audio = false;
      }
      st.mode = v;
      piano.send(Uint8Array.from([0xf0, 0x40, 0x7f, 0x32, 0x04, 0x1e, 0x52, 0x1d, 0x7f, v, 0xf7]));
      return;
    }
    if (fn === 0x32 && m[6] === 0x52 && m[7] === 0x08) {
      const v = m[9];
      if (st.mode !== 3) return void piano.send(music(2)); // FUN_4015DB98
      if (st.sel === 4) {
        // FUN_401737A4
        if (v === 1 && st.smf !== 1) st.smf = 1;
        else if (v === 0 && st.smf === 1) st.smf = 0;
        else if (v === 2) smfStop();
        clock();
        piano.send(music(st.smf));
      } else if (st.sel === 5 || st.sel === 6) {
        // FUN_40123848(v): the executor plays / stops; no report right away. lateAudio: the executor acts a moment
        // later on the player of the type selected THEN (field 2026-11-15 F13b: a MID select ~60 ms after the audio
        // Reset and the MP3 played on)
        if (opt.lateAudio) setTimeout(() => { if (st.sel === 5 || st.sel === 6) st.audio = v === 1; }, 50);
        else st.audio = v === 1;
      } else piano.send(music(2));
      return;
    }
    if (fn !== 0x24) return;
    const v1 = m[6];
    if (v1 >= 0x04 && v1 <= 0x0b) {
      piano.send(ACK);
      st.op = v1;
      st.names = [];
      st.cur = [];
      return;
    }
    if (v1 === 0x03) {
      piano.send(ACK);
      for (const b of FA.readPacket(m) ?? []) {
        if (b) st.cur.push(b);
        else {
          st.names.push(String.fromCharCode(...st.cur));
          st.cur = [];
        }
      }
      if (st.names.length !== 1 || st.op !== 0x08) return;
      const name = st.names[0];
      const type = TYPE_BY_EXT[name.split(".").pop()!.toUpperCase()] ?? 0xff;
      st.sel = type; // job 8 stores the type before it looks at the player
      if ((type === 4 && st.smf === 1) || ((type === 5 || type === 6) && st.audio)) return void piano.send(BUSY);
      if (type === 4) {
        st.smfName = name;
        st.bar = 1;
        st.beat = 0;
        piano.send(progress(1, 0));
        piano.send(progress(1, 0));
      } else if (type === 5 || type === 6) st.audioName = name;
      piano.send(OK_STOCK);
    }
  });
  return st;
}

const FILES = ["SONG_01.MID", "UP01.MID", "TEST01.MID", "1.mp3", "2.mp3", "233.mp3"];

async function setup(opt: { stuck?: boolean; syncMode?: number; lateAudio?: boolean } = {}) {
  const p = await pianoPair();
  const store = new ParamStore();
  bindStore(store);
  store.attach(p.session);
  const ctx = createContext(store, { session: () => p.session });
  const st = stockPlayers(p.piano, p.sent, opt);
  ctx.usbFiles = [...FILES];
  Object.assign(usbListState(ctx), { at: Date.now(), session: p.session });
  // the connect sync's MusicMode (10 52 1D): the store keeps it, the app's 31 52 1D never replaces it
  if (opt.syncMode !== undefined) {
    st.mode = opt.syncMode;
    p.notify([0x52, 0x1d, 0x7f, opt.syncMode]);
    await tick(5);
  }
  const el = filesScreen(ctx);
  document.body.append(el);
  await tick(10);
  return { ...p, store, ctx, st, el };
}

const row = (el: HTMLElement, f: string, act: string) => el.querySelector(`[data-file="${f}"] [data-act="${act}"]`) as HTMLElement | null;
const resultText = (el: HTMLElement) => el.querySelector("#usb-result")!.textContent ?? "";
const STOP_MS = USB_STOP.grace + USB_STOP.watch + 150;
const RESET = "F0 40 7F 32 08 02 52 08 7F 02 F7";
const PLAY = "F0 40 7F 32 08 02 52 08 7F 01 F7";

test("field 11-13 ①: the store's MusicMode from the connect sync no longer turns the playing MID row back to ▶", async () => {
  const { st, el, store } = await setup({ syncMode: 0 });
  assert.equal(store.get((await import("../src/protocol/codecs.ts")).findParam("MusicMode", 0x10), 0x7f)?.[0], 0, "stale 0 in the store");
  row(el, "SONG_01.MID", "play")!.click();
  await tick(BEAT_MS * 4);
  assert.equal(st.smf, 1, "the SMF player plays (32 52 08 7F 01 came back)");
  assert.ok(row(el, "SONG_01.MID", "stop"), "■ on the playing row (the old code read MusicMode 0 from the store and showed ▶)");
  row(el, "SONG_01.MID", "stop")!.click();
  await tick(STOP_MS);
  assert.equal(st.smf, 2, "the row's ■ stopped the MID");
  assert.ok(row(el, "SONG_01.MID", "play"));
  assert.equal(resultText(el), "Stop: done");
});

test("field 11-13 ②: ▶ on an MP3 while the MID plays stops the MID first — never both at once", async () => {
  const { st, el, sent } = await setup({ syncMode: 0 });
  row(el, "SONG_01.MID", "play")!.click();
  await tick(BEAT_MS * 4);
  assert.equal(st.smf, 1);
  const n0 = sent.length;
  row(el, "1.mp3", "play")!.click();
  await tick(STOP_MS + 100);
  assert.equal(st.smf, 2, "the MID was stopped before the MP3 started");
  assert.equal(st.audio, true, "the MP3 plays");
  const tx = sent.slice(n0);
  const firstPlay = tx.indexOf(PLAY);
  const midReset = tx.findIndex((s, i) => s === RESET && tx.slice(0, i).some((x) => x.startsWith("F0 40 7F 24 08 02 03")));
  assert.ok(midReset >= 0 && midReset < firstPlay, "a file selected + Reset before the Play");
  assert.ok(row(el, "1.mp3", "stop"), "■ on the MP3 row");
  assert.ok(row(el, "SONG_01.MID", "play"), "▶ on the MID row");
});

test("field 11-13 ③: with the MID and an MP3 both playing (started elsewhere), the section ■ stops both", async () => {
  const { st, el, sent, piano } = await setup();
  // the field state, made on the piano directly: MID playing, then an MP3 selected and played
  Object.assign(st, { mode: 3, sel: 5, smf: 1, audio: true, smfName: "\\SONG_01.MID", audioName: "\\1.mp3" });
  st.bar = 3;
  piano.send(progress(3, 0));
  await tick(BEAT_MS * 3);
  (el.querySelector('[data-act="usb-stop"]') as HTMLElement).click();
  await tick(STOP_MS);
  assert.equal(st.audio, false, "the audio player stopped");
  assert.equal(st.smf, 2, "the SMF player stopped: a MID selected (Busy, type 4 set) and Reset");
  assert.ok(sent.includes(RESET));
  assert.ok(sent.filter((s) => s === "F0 40 7F 31 08 02 52 1D 7F 03 F7").length >= 3, "MusicMode 3 before each Reset");
  assert.equal(resultText(el), "Stop: done");
});

test("field 11-15 F13b: an MP3 playing alone stops from the section ■ — its player gets the last select + Reset", async () => {
  const { st, el, sent } = await setup({ lateAudio: true });
  row(el, "1.mp3", "play")!.click();
  await tick(400);
  assert.equal(st.audio, true, "the MP3 plays");
  const n0 = sent.length;
  (el.querySelector('[data-act="usb-stop"]') as HTMLElement).click();
  await tick(2 * STOP_MS + 400);
  assert.equal(st.audio, false, "the audio player stopped (the old order audio → smf left it playing)");
  const tx = sent.slice(n0);
  const lastSel = tx.filter((x) => x.startsWith("F0 40 7F 24 08 02 03")).pop()!;
  assert.match(lastSel, /31 2E 6D 70 33/, "the last file selected before the last Reset is the MP3");
  assert.equal(resultText(el), "Stop: done");
});

test("field 11-13 ④: the old stop (Reset with an MP3 selected) does not reach the MID — the fake is the stock", async () => {
  // what the field did by hand, against the fake: proves the fake reproduces the field, so the tests above mean something
  const { st, piano, session } = await setup();
  Object.assign(st, { mode: 3, sel: 5, smf: 1, audio: true });
  for (const body of [[0x52, 0x08, 0x7f, 2], [0x52, 0x08, 0x7f, 0], [0x52, 0x1d, 0x7f, 0], [0x52, 0x1d, 0x7f, 3], [0x52, 0x08, 0x7f, 2], [0x52, 0x1d, 0x7f, 2], [0x52, 0x08, 0x7f, 2]]) {
    await session.sendParam({ fn: body[1] === 0x1d ? 0x31 : 0x32, v1: body[0], v2: body[1], part: 0x7f, data: [body[3]] });
    await tick(5);
  }
  assert.equal(st.smf, 1, "the MID still plays");
  assert.equal(st.audio, false, "only the MP3 stopped");
  void piano;
});

test("a USB file never starts on top of another: the select answered Busy (playback the app did not know) stops first", async () => {
  const { st, el, piano } = await setup();
  // the panel started SONG_01.MID; this app saw nothing of it yet (no progress arrived)
  Object.assign(st, { mode: 3, sel: 4, smf: 1 });
  row(el, "UP01.MID", "play")!.click();
  await tick(STOP_MS + 100);
  assert.equal(st.smf, 1, "UP01.MID plays");
  assert.ok(st.smfName.endsWith("UP01.MID"), "loaded after the stop (the first select was Busy)");
  assert.ok(row(el, "UP01.MID", "stop"));
  void piano;
});

test("a piano that keeps playing: the stop is tried twice, then the panel way is shown; ▶ is refused meanwhile", async () => {
  const { st, el, sent } = await setup({ stuck: true });
  row(el, "SONG_01.MID", "play")!.click();
  await tick(BEAT_MS * 4);
  assert.equal(st.smf, 1);
  const n0 = sent.length;
  row(el, "SONG_01.MID", "stop")!.click();
  await tick(STOP_MS * 2 + 100);
  const resets = sent.slice(n0).filter((s) => s === RESET).length;
  assert.equal(resets, 2 * 3, "two attempts of three Resets each (selected, audio, SMF)");
  assert.match(resultText(el), /panel's USB memory screen/);
  assert.match(resultText(el), /switch the piano off and on/);
  // ▶ on another file: the stop is tried first, fails, and nothing starts
  const n1 = sent.length;
  row(el, "1.mp3", "play")!.click();
  await tick(STOP_MS * 2 + 100);
  assert.ok(!sent.slice(n1).includes(PLAY), "no Play while the MID plays on");
  assert.equal(st.audio, false);
  assert.match(resultText(el), /1\.mp3 was not started/);
});

test("ko / ja: the new USB playback messages are translated", async () => {
  for (const lang of ["ko", "ja"] as const) {
    i18n.setLang(lang);
    for (const s of [
      "The piano still plays from the USB memory. Stop it on the piano: on the panel's USB memory screen choose the playing file and press Stop; if it still plays, switch the piano off and on.",
      "Stopping the USB playback that runs first…",
      "{name} was not started: the USB playback that runs did not stop.",
    ]) assert.notEqual(i18n.t(s, { name: "x" }), s.replace("{name}", "x"), `${lang}: ${s}`);
  }
});

test("progress decides: bars that advance mean the SMF player plays; a rewind to bar 1 or a repeat does not", async () => {
  const { ctx, piano } = await setup();
  const st = usbPlayback(ctx);
  piano.send(progress(1, 0));
  piano.send(progress(1, 0));
  await tick(5);
  assert.equal(st.smf.on, false, "select: bar 1 twice");
  piano.send(progress(1, 1));
  await tick(5);
  assert.equal(st.smf.on, true, "a beat on");
  piano.send(music(2)); // mode unknown: not a USB player report
  await tick(5);
  assert.equal(st.smf.on, true);
  await tick(USB_STOP.idle + 50);
  const { usbRunning } = await import("../src/ui/usbplay.ts");
  assert.equal(usbRunning(st, "smf"), false, "no beat for the idle time: taken as stopped (paused / ended)");
});
