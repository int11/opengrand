// Recorder tab, the piano's songs list (ui/recorder.ts; was the Files tab's "Internal songs") against the STOCK V2.19: replies replayed byte for byte from the stock image
// run under Unicorn with a synthetic RecorderData (result/stockfw/tools/stock_songs.py --json
// tests/fixtures/stock_songs.json).
//   - SongSelect: empty song -> 7E 03 00 01 6C (0xEC); always MaxTime 10 52 18 7F [0, bars hi, lo, 0, 0, 0]
//   - IntSongPart 0/1: 0xEC when the selected song has no track for that part
//   - InternalSongDelete 32 52 21 7F [04|7F, hi, lo, part] -> 32 51 7E 7F 00 00 (d0 = 0 is ignored)
// After a delete the fake answers by the same rules (the emulator cannot run the stock's edit itself).
import { pianoPair, tick } from "./_dom.ts";
import { beforeEach, test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const { ParamStore } = await import("../src/state/store.ts");
const { bindStore } = await import("../src/ui/controls.ts");
const { createContext } = await import("../src/ui/context.ts");
const { recorderScreen } = await import("../src/ui/recorder.ts");
const { InternalSongs } = await import("../src/protocol/songs.ts");
const { fromHex, toHex } = await import("../src/protocol/sysex.ts");
const i18n = await import("../src/i18n.ts");

beforeEach(() => {
  localStorage.clear();
  i18n.setLang("en");
});

type Capture = { label: string; tx: string; rx: string[] };
const FIXTURE: Capture[] = JSON.parse(readFileSync(new URL("./fixtures/stock_songs.json", import.meta.url), "utf8"));
const LAYOUT: [number, number][] = [[3, 0], [2, 5], [0, 4], [200, 0], [0, 0], [0, 0], [0, 0], [0, 0], [0, 0], [0, 0]]; // stock_songs.py LAYOUT
const ACK = (v2: number) => `F0 40 7F 07 04 1E 52 ${v2.toString(16).padStart(2, "0").toUpperCase()} 7F F7`;
const NODATA = "F0 40 7F 7E 04 1E 03 00 01 6C F7";
const hex2 = (n: number) => n.toString(16).padStart(2, "0").toUpperCase();
const MAXTIME = (bars: number) => `F0 40 7F 10 04 1E 52 18 7F 00 ${hex2((bars >> 7) & 0x7f)} ${hex2(bars & 0x7f)} 00 00 00 F7`;
const DELETE_OK = "F0 40 7F 32 04 1E 51 7E 7F 00 00 F7"; // FUN_4014F294(0), stock_songs.py D
const FA_ACK = "F0 40 7F 24 04 1E 7F F7";
const FA_OK = "F0 40 7F 32 04 1E 51 7E 7F 00 00 F7";

/** The stock's recorder answers: the captured bytes while the store is untouched, the same rules after a delete. */
function stockSongs(piano: { send(m: Uint8Array): void; onMessage(h: (m: Uint8Array) => void): void }, sent: string[]) {
  const st = { sel: 0, layout: LAYOUT.map((p) => [...p] as [number, number]), touched: new Set<number>(), replayed: 0, ruled: 0, played: [] as number[], saved: [] as string[], faOp: -1 };
  const capture = (label: string, tx: string) => FIXTURE.find((c) => c.label === label && c.tx === tx);
  const out = (frames: string[]) => frames.forEach((f) => piano.send(fromHex(f)));
  piano.onMessage((m) => {
    const tx = toHex(m);
    sent.push(tx);
    if (m[1] !== 0x40 || m[4] !== 0x08) return;
    const [fn, v1, v2] = [m[3], m[6], m[7]];
    if (fn === 0x31 && v1 === 0x52 && v2 === 0x1d) return out([`F0 40 7F 32 04 1E 52 1D 7F ${hex2(m[9])} F7`]);
    if (fn === 0x10 && v1 === 0x52 && v2 === 0x00) {
      st.sel = (m[10] << 7) | m[11];
      const c = !st.touched.has(st.sel) && capture(`select ${st.sel + 1}`, tx);
      if (c) return st.replayed++, out(c.rx);
      st.ruled++;
      const bars = Math.max(...st.layout[st.sel]);
      return out([ACK(0x00), ...(bars ? [] : [NODATA]), MAXTIME(bars)]);
    }
    if (fn === 0x10 && v1 === 0x52 && v2 === 0x01) {
      const p = m[9];
      const c = !st.touched.has(st.sel) && capture(`song ${st.sel + 1} part 0x${p.toString(16)}`, tx);
      if (c) return st.replayed++, out(c.rx);
      st.ruled++;
      const has = p === 0x7f ? Math.max(...st.layout[st.sel]) : st.layout[st.sel][p];
      return out([ACK(0x01), ...(has ? [] : [NODATA]), MAXTIME(has)]);
    }
    if (fn === 0x10) return out([`F0 40 7F 07 04 1E ${hex2(v1)} ${hex2(v2)} ${hex2(m[8])} F7`]);
    if (fn === 0x32 && v1 === 0x52 && v2 === 0x08) return void st.played.push(m[9]);
    if (fn === 0x32 && v1 === 0x52 && v2 === 0x21) {
      const [d0, n, part] = [m[9], (m[10] << 7) | m[11], m[12]];
      if (d0 !== 0x04 && d0 !== 0x7f) return; // apply 0x40160AD4
      const s = n < 10 ? n : 0;
      if (part === 0x7f) st.layout[s] = [0, 0];
      else st.layout[s][part] = 0;
      st.touched.add(s);
      return out([DELETE_OK]);
    }
    if (fn === 0x24) {
      if (v1 === 0x7f) return; // the app's ACK of our 24 1F
      out([FA_ACK]);
      if (v1 === 0x11) return out(["F0 40 7F 24 04 1E 1F F7"]); // empty USB list
      if (v1 >= 0x04 && v1 <= 0x0b) st.faOp = v1;
      else if (v1 === 0x03 && st.faOp === 0x0a) {
        st.saved.push(tx);
        out([FA_OK]);
      }
    }
  });
  return st;
}

async function setup() {
  const p = await pianoPair();
  const store = new ParamStore();
  bindStore(store);
  store.attach(p.session);
  const ctx = createContext(store, { session: () => p.session });
  const st = stockSongs(p.piano, p.sent);
  const el = recorderScreen(ctx);
  document.body.append(el);
  await tick(10);
  return { ...p, store, ctx, st, el };
}

const row = (el: HTMLElement, n: number) => el.querySelector(`[data-song="${n}"]`) as HTMLElement;
const info = (el: HTMLElement, n: number) => row(el, n).querySelector(".song-info")!.textContent;

test("fixture: the emulator captured 0xEC + MaxTime for every song and part probe", () => {
  const sel = FIXTURE.filter((c) => c.label.startsWith("select "));
  assert.equal(sel.length, 10);
  for (const c of sel) {
    const n = Number(c.label.slice(7)) - 1;
    const bars = Math.max(...LAYOUT[n]);
    assert.ok(c.rx.includes(MAXTIME(bars)), `${c.label}: MaxTime ${bars}`);
    assert.equal(c.rx.includes(NODATA), bars === 0, `${c.label}: 0xEC only when empty`);
  }
  const del0 = FIXTURE.find((c) => c.label === "delete d0 = 0")!;
  assert.deepEqual(del0.rx, [], "InternalSongDelete with d0 = 0 is ignored by the stock");
  assert.deepEqual(FIXTURE.find((c) => c.label === "delete song 2 both")!.rx, [DELETE_OK]);
});

test("scan: 10 rows from the stock's answers, then the piano's song and part are put back", async () => {
  const { el, sent, st, store } = await setup();
  // the piano had song 3 selected, Part 2 playing
  store.fromWire({ fn: 0x10, v1: 0x52, v2: 0x00, part: 0x7f, data: [0, 0, 2] });
  store.fromWire({ fn: 0x10, v1: 0x52, v2: 0x01, part: 0x7f, data: [1] });
  assert.equal(info(el, 1), "not scanned");
  (el.querySelector('[data-act="songs-scan"]') as HTMLElement).click();
  await tick(400);
  assert.equal(st.ruled, 0, "every answer was replayed from the emulator capture");
  assert.ok(st.replayed > 10);
  assert.equal(info(el, 1), "3 bars");
  assert.equal(info(el, 2), "5 bars · Part 1 + Part 2");
  assert.equal(info(el, 3), "4 bars · Part 2");
  assert.equal(info(el, 4), "200 bars");
  for (let n = 5; n <= 10; n++) {
    assert.equal(info(el, n), "Empty");
    assert.equal(row(el, n).getAttribute("data-state"), "empty");
    assert.equal(row(el, n).querySelector('[data-act="song-more"]'), null, "nothing to export in an empty song");
  }
  assert.equal(el.querySelector("#songs-status")!.textContent, "4 of 10 songs recorded");
  // the scan put the piano's song 3 back: it is the lit row; every recorded row has its export buttons behind ⋯
  assert.equal(row(el, 3).getAttribute("data-on"), "1");
  assert.equal(row(el, 3).querySelector('[data-act="song-save"]')!.textContent, "Save to USB", "one way out; the formats are in its sheet");
  assert.equal(el.querySelectorAll('.song-row .row-acts:not([hidden])').length, 0, "closed until ⋯ is tapped");
  assert.equal(el.querySelector("#rec-panel-info")!.textContent, "Song 34 bars · Part 2", "the player bar names the selected song");

  assert.equal(sent.find((m) => m.includes(" 52 1D ")), "F0 40 7F 31 08 02 52 1D 7F 02 F7", "MusicMode 2 (internal recorder) first");
  const tail = sent.filter((m) => / 10 08 02 52 0[01] /.test(m)).slice(-2);
  assert.deepEqual(tail, ["F0 40 7F 10 08 02 52 00 7F 00 00 02 F7", "F0 40 7F 10 08 02 52 01 7F 01 F7"], "song 3 and Part 2 again");
  assert.ok(!sent.some((m) => m.includes(" 52 21 ")), "a scan never deletes");
  el.remove();
});

// the stock's InternalSongDelete re-writes the song from the last take instead of deleting it (emulator + field
// 2026-11-15): on a stock piano the rows offer no Delete
test("delete on the stock: no Delete is offered (the selected song says why) and nothing is sent", async () => {
  const { el, sent } = await setup();
  (el.querySelector('[data-act="songs-scan"]') as HTMLElement).click();
  await tick(400);
  (row(el, 2).querySelector('[data-act="song-pick"]') as HTMLElement).click();
  await tick(60);
  assert.equal(row(el, 2).getAttribute("data-on"), "1");
  assert.ok(sent.includes("F0 40 7F 10 08 02 52 00 7F 00 00 01 F7"), "choosing the row selects song 2 on the piano");
  assert.ok(row(el, 2).querySelector('[data-act="song-save"]'), "export is there");
  assert.equal(el.querySelectorAll('[data-act^="song-delete"]').length, 0, "no Delete anywhere");
  (row(el, 2).querySelector('[data-act="song-more"]') as HTMLElement).click(); // ⋯: the song's other buttons
  assert.match(row(el, 2).querySelector(".row-note")!.textContent!, /OpenGrand song delete/);
  await tick(100);
  assert.ok(!sent.some((m) => m.includes(" 52 21 ")), "no InternalSongDelete");
  assert.equal(info(el, 2), "5 bars · Part 1 + Part 2");
  el.remove();
});

test("play / stop from the player bar, save as SMF from a song row", async () => {
  const { el, sent, st } = await setup();
  // a tap on the row chooses the song (MusicMode 2, SongSelect); the bar's ▶ plays it
  (row(el, 4).querySelector('[data-act="song-pick"]') as HTMLElement).click();
  await tick(30);
  (el.querySelector('#rec-panel [data-act="play"]') as HTMLElement).click();
  await tick(60);
  const i = sent.indexOf("F0 40 7F 31 08 02 52 1D 7F 02 F7");
  const j = sent.indexOf("F0 40 7F 10 08 02 52 00 7F 00 00 03 F7");
  const k = sent.indexOf("F0 40 7F 32 08 02 52 08 7F 01 F7");
  assert.ok(i >= 0 && i < j && j < k, "MusicMode 2, SongSelect 4, Play");
  assert.deepEqual(st.played, [1]);
  assert.equal(row(el, 4).getAttribute("data-on"), "1", "the played song is the selected one");
  (el.querySelector('#rec-panel [data-act="stop"]') as HTMLElement).click(); // the player bar's ■
  await tick(20);
  assert.deepEqual(st.played, [1, 2], "Stop = Music Reset");
  (row(el, 4).querySelector('[data-act="song-save"]') as HTMLElement).click();
  (el.querySelector('#save-sheet [data-format="MID"]') as HTMLElement).click(); // the one format list (ui/savesheet.ts)
  await tick(80);
  assert.ok(sent.includes("F0 40 7F 24 08 02 0A 03 02 F7"), "24 0A <song 4 = 3> <type MID>");
  assert.equal(st.saved.length, 1);
  assert.equal(el.querySelector("#songs-status")!.textContent, "Saved to USB: SONG_04.MID");
  el.remove();
});

test("InternalSongs.info: no MaxTime (old firmware) -> bars null, parts not probed", async () => {
  const p = await pianoPair(); // plain loopback: ACKs fn 0x10, never answers MaxTime
  const c = new InternalSongs(p.session, 30);
  const i = await c.info(0);
  assert.deepEqual(i, { n: 0, hasData: true, bars: null, parts: null });
});
