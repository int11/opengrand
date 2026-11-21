// USB tab against the STOCK V2.19, round 2: the owner's field log of 2026-10-31 (stock + core_probe,
// PC Chrome over BLE) and the stock behaviour run under Unicorn in scripts/analysis/stock_fn24b.py
//   - Play: Music=Play (32 52 08 7F 01) reaches a player only in MusicMode 3 (USB memory); in mode 6
//     (Piano Music) the stock answers 32 52 08 7F 02 and nothing plays — the field symptom.
//   - Import 24 09 takes <song> <type>; a bare 24 09 ends in 7E 03 00 01 70 (0xF0), and the stock
//     imports only KSO (type 3).
//   - Save: 24 0A <song> <type> + name (32 52 20 is a no-op on the stock).
//   - Write: 24 05 01 00 (other d1/d2 report success without writing); TopOfData is the byte count;
//     a length that is a multiple of 21 needs one empty DataPacket.
import { byText, pianoPair, tick } from "./_dom.ts";
import { beforeEach, test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const { bindStore } = await import("../src/ui/controls.ts");
const { createContext } = await import("../src/ui/context.ts");
const { recorderScreen } = await import("../src/ui/recorder.ts");
const { usbListState } = await import("../src/ui/usblist.ts");
const FA = await import("../src/protocol/fileaccess.ts");
const { fromHex, toHex } = await import("../src/protocol/sysex.ts");
const i18n = await import("../src/i18n.ts");
const { USB_STOP } = await import("../src/ui/usbplay.ts");
Object.assign(USB_STOP, { grace: 10, watch: 60, idle: 400 }); // a stop waits grace + watch for the piano to stay quiet
const STOP_WAIT = 150;

beforeEach(() => {
  localStorage.clear();
  i18n.setLang("en");
});

// stock replies, byte for byte from the field log (usb_ops_261031)
const ACK = fromHex("F0 40 7F 24 04 1E 7F F7");
const OK_STOCK = fromHex("F0 40 7F 32 04 1E 51 7E 7F 00 00 F7");
const NOT_ACCEPTED = fromHex("F0 40 7F 7E 04 1E 03 00 01 70 F7");
const MAXTIME_MID = fromHex("F0 40 7F 10 04 1E 52 18 7F 00 00 02 00 00 00 F7");
const MAXTIME_MP3 = fromHex("F0 40 7F 10 04 1E 52 18 7F 01 00 00 00 00 00 F7");
const PROGRESS = fromHex("F0 40 7F 32 04 1E 52 03 7F 00 00 01 00 00 00 F7");
const MUSIC_STOPPED = fromHex("F0 40 7F 32 04 1E 52 08 7F 02 F7");
const MUSIC_PLAYING = fromHex("F0 40 7F 32 04 1E 52 08 7F 01 F7");
const TYPE_BY_EXT: Record<string, number> = { KSO: 3, MID: 4, MP3: 5, WAV: 6 }; // FUN_4012796C

/** The stock's fn 0x24 / MusicMode / Music behaviour as stock_fn24b.py shows it. */
function stockUsb(piano: { send(m: Uint8Array): void; onMessage(h: (m: Uint8Array) => void): void }, sent: string[]) {
  const st = {
    mode: 0,
    selected: 0, // file type of the 24 08 file (0x413CA3F5)
    player: "" as "" | "smf" | "audio",
    files: new Map<string, number[]>([["\\TEST01.MID", [1, 2, 3]]]),
    op: 0,
    d1: 0,
    d2: 0,
    names: [] as string[],
    cur: [] as number[],
    writeFlag: false, // 0x413CA3ED
    up: { state: 0, len: 0, buf: [] as number[], name: "" },
    saved: [] as [number, number, string][],
    music: [] as number[], // Music values the piano got (32 52 08)
  };
  const done = (ok: boolean) => piano.send(ok ? OK_STOCK : NOT_ACCEPTED);
  piano.onMessage((m) => {
    sent.push(toHex(m)); // replaces pianoPair's recorder (one handler per link end)
    if (m[1] !== 0x40 || m[4] !== 0x08) return;
    if (m[3] === 0x10 && m[7] !== 0x1d) return void piano.send(Uint8Array.from([0xf0, 0x40, 0x7f, 0x07, 0x04, 0x1e, m[6], m[7], m[8], 0xf7]));
    const fn = m[3];
    if ((fn === 0x31 || fn === 0x10) && m[6] === 0x52 && m[7] === 0x1d) {
      st.mode = m[9];
      piano.send(Uint8Array.from([0xf0, 0x40, 0x7f, 0x32, 0x04, 0x1e, 0x52, 0x1d, 0x7f, m[9], 0xf7]));
      return;
    }
    if (fn === 0x32 && m[6] === 0x52 && m[7] === 0x08) {
      // FUN_4015DBD4: mode 3 -> SMF player (MID) / audio player (MP3, WAV); other modes report the state
      // value 2 (Reset) in mode 3: FUN_401737A4(2) -> FUN_401736AC stops the SMF player, FUN_40123848(2) the audio
      // one; the SMF path then reports the state (FUN_4015C27C -> 32 52 08 7F <state>)
      st.music.push(m[9]);
      if (m[9] === 1 && st.mode === 3 && st.selected === 4) {
        st.player = "smf";
        piano.send(MUSIC_PLAYING);
      } else if (m[9] === 1 && st.mode === 3 && (st.selected === 5 || st.selected === 6)) st.player = "audio";
      else {
        if (m[9] === 2 && st.mode === 3) st.player = "";
        piano.send(MUSIC_STOPPED);
      }
      return;
    }
    if (fn !== 0x24) return;
    const v1 = m[6];
    if (v1 >= 0x04 && v1 <= 0x0b) {
      piano.send(ACK);
      st.op = v1;
      st.d1 = m.length > 8 ? m[7] : 0xf7; // FUN_4014F870 reads the bytes after v1, F7 included
      st.d2 = m.length > 9 ? m[8] : 0;
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
      if (st.names.length !== 1) return;
      const name = st.names[0];
      const type = TYPE_BY_EXT[name.split(".").pop()!.toUpperCase()] ?? 0xff;
      const table = [6, 5, 4, 3][st.d2] ?? 0; // 0x400FD905
      if (st.op === 0x08) {
        st.selected = type;
        piano.send(type === 4 ? MAXTIME_MID : MAXTIME_MP3);
        piano.send(PROGRESS);
        done(true);
      } else if (st.op === 0x09) done(table === 3); // FUN_4014F1A8: KSO only
      else if (st.op === 0x0a) {
        st.saved.push([st.d1, table, name]);
        done(table === 3 || table === 4);
      } else if (st.op === 0x05) st.writeFlag = true; // executor case 1: no reply
      return;
    }
    if (v1 === 0x01) {
      piano.send(ACK);
      st.up = { state: 1, len: (m[7] << 21) | (m[8] << 14) | (m[9] << 7) | m[10], buf: [], name: st.names[0] ?? "" };
      return;
    }
    if (v1 === 0x02) {
      piano.send(ACK);
      const raw = FA.readPacket(m) ?? [];
      const counter = m[7];
      st.up.buf.push(...raw);
      // FUN_4014FB94: complete only when a group offset of this packet reaches the length
      for (let g = 0; g < 3; g++) {
        if (st.up.len <= (counter * 3 + g) * 7) {
          st.up.state = 2;
          break;
        }
      }
      return;
    }
    if (v1 === 0x7b) {
      piano.send(ACK);
      if (st.up.state !== 2) return void piano.send(fromHex("F0 40 7F 7E 04 1E 03 00 01 6D F7"));
      if (!st.writeFlag) return done(false);
      st.writeFlag = false;
      if (st.d1 === 1 && st.d2 === 0) st.files.set(st.up.name, st.up.buf.slice(0, st.up.len));
      done(true); // FUN_4014FA30 returns 0 even when d1/d2 said "do not write"
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
  const st = stockUsb(p.piano, p.sent);
  ctx.usbFiles = ["TEST01.MID", "1.mp3", "SONG.KSO"];
  // a list this session already read (fresh cache): opening the tab does not ask again (ui/usblist.ts)
  Object.assign(usbListState(ctx), { at: Date.now(), session: p.session });
  const el = recorderScreen(ctx);
  document.body.append(el);
  await tick(10);
  return { ...p, store, ctx, st, el };
}

test("field log: ▶ in MusicMode 6 got 32 52 08 7F 02 and no sound; the app now plays in MusicMode 3", async () => {
  const { st, el, sent } = await setup();
  // what the field log shows the old app doing: 31 52 1D 7F 06 + 24 08 + Play -> 52 08 = 02
  st.mode = 6;
  st.selected = 4;
  (el.querySelector('[data-file="TEST01.MID"] [data-act="play"]') as HTMLElement).click();
  await tick(60);
  assert.ok(sent.includes("F0 40 7F 31 08 02 52 1D 7F 03 F7"), "MusicMode 3 (USB memory), not 6 (Piano Music)");
  assert.ok(!sent.includes("F0 40 7F 31 08 02 52 1D 7F 06 F7"));
  const i = sent.indexOf("F0 40 7F 31 08 02 52 1D 7F 03 F7");
  const j = sent.indexOf("F0 40 7F 24 08 02 08 F7");
  const k = sent.indexOf("F0 40 7F 32 08 02 52 08 7F 01 F7");
  assert.ok(i >= 0 && i < j && j < k, "mode, then select, then Play");
  assert.equal(st.player, "smf", "the SMF player starts (FUN_401737A4(1))");
  assert.equal(el.querySelector("#usb-result")!.textContent, "Play: done");

  // the MID plays: the app stops it first (ui/usbplay.ts), then the same sequence for the MP3
  (el.querySelector('[data-file="1.mp3"] [data-act="play"]') as HTMLElement).click();
  await tick(STOP_WAIT + 60);
  assert.equal(st.player, "audio", "MP3: same sequence, the audio player starts (FUN_40123848(1))");
  el.remove();
});

// field 2026-11-09: a USB file had only ▶ — once it played the app could not stop it. Stop = Music Reset
// (32 52 08 7F 02), which FUN_4015DBD4 in MusicMode 3 hands to the player the 24 08 file started.
const STOP = "F0 40 7F 32 08 02 52 08 7F 02 F7";
const row = (el: HTMLElement, f: string, act: string) => el.querySelector(`[data-file="${f}"] [data-act="${act}"]`) as HTMLElement | null;

test("USB playback: the playing row shows ■ and stops the SMF player with Music Reset; ▶ comes back", async () => {
  const { st, el, sent } = await setup();
  st.selected = 4;
  row(el, "TEST01.MID", "play")!.click();
  await tick(60);
  assert.equal(st.player, "smf");
  assert.ok(row(el, "TEST01.MID", "stop"), "■ on the playing row");
  assert.equal(row(el, "TEST01.MID", "play"), null);
  assert.ok(row(el, "1.mp3", "play"), "other rows keep ▶");
  row(el, "TEST01.MID", "stop")!.click();
  await tick(STOP_WAIT);
  assert.ok(sent.includes(STOP), "Music Reset");
  assert.equal(st.mode, 3, "no mode change: the stop goes to the USB player");
  assert.equal(st.player, "", "FUN_401737A4(2) stopped it");
  assert.equal(el.querySelector("#usb-result")!.textContent, "Stop: done");
  assert.ok(row(el, "TEST01.MID", "play"), "▶ again");
  el.remove();
});

test("USB playback: MP3 (no state report) still gets ■; the section ■ stops too; the tab opened again remembers", async () => {
  const { st, el, sent, ctx } = await setup();
  st.selected = 5;
  row(el, "1.mp3", "play")!.click();
  await tick(60);
  assert.equal(st.player, "audio");
  assert.ok(row(el, "1.mp3", "stop"), "■ from the app's own ▶");
  // the tab replaced and opened again (same session): still ■
  el.remove();
  const el2 = recorderScreen(ctx);
  document.body.append(el2);
  await tick(10);
  assert.ok(row(el2, "1.mp3", "stop"));
  (el2.querySelector('[data-act="usb-stop"]') as HTMLElement).click();
  await tick(STOP_WAIT);
  // the selected type's player, then each type with a file (MP3, MID): three Resets (ui/usbplay.ts)
  assert.equal(sent.filter((s) => s === STOP).length, 3);
  assert.equal(st.player, "");
  assert.ok(row(el2, "1.mp3", "play"));
  el2.remove();
});

test("USB playback: the piano's notices drive the row — Reset from the panel gives ▶ back, a MusicMode alone does not", async () => {
  const { st, el, piano } = await setup();
  st.selected = 4;
  row(el, "TEST01.MID", "play")!.click();
  await tick(60);
  assert.ok(row(el, "TEST01.MID", "stop"));
  piano.send(MUSIC_STOPPED); // the panel's Reset, relayed
  await tick(10);
  assert.ok(row(el, "TEST01.MID", "play"), "stopped elsewhere -> ▶");
  piano.send(MUSIC_PLAYING); // played again from the panel (same file still selected)
  await tick(10);
  assert.ok(row(el, "TEST01.MID", "stop"));
  // MusicMode 2 only stores the mode (FUN_4015DCC4): the USB players play on (field 2026-11-13, emulator
  // roundtrip_usbplay.py) - the row keeps ■; a Music report in mode 2 is the recorder's, not ours
  piano.send(fromHex("F0 40 7F 32 04 1E 52 1D 7F 02 F7"));
  await tick(10);
  assert.ok(row(el, "TEST01.MID", "stop"));
  piano.send(MUSIC_STOPPED);
  await tick(10);
  assert.ok(row(el, "TEST01.MID", "stop"), "mode 2: 32 52 08 7F 02 is the recorder's state");
  el.remove();
});

test("field log: import 24 09 without arguments -> 7E 03 00 01 70; now 24 09 <song> <type>, KSO only on the stock", async () => {
  const { el, sent } = await setup();
  (el.querySelector('[data-file="TEST01.MID"] [data-act="import"]') as HTMLElement).click();
  await tick(60);
  assert.ok(sent.includes("F0 40 7F 24 08 02 09 00 02 F7"), "song 1 (0), type index 2 = MID");
  assert.equal(el.querySelector("#usb-result")!.textContent, "Import failed: Not accepted", "the stock imports only KSO");
  (el.querySelector('[data-file="SONG.KSO"] [data-act="import"]') as HTMLElement).click();
  await tick(60);
  assert.ok(sent.includes("F0 40 7F 24 08 02 09 00 03 F7"), "type index 3 = KSO");
  assert.equal(el.querySelector("#usb-result")!.textContent, "Import: done");
  el.remove();
});

test("save as SMF: 24 0A <song> 02 + SONG_nn.MID, answered 32 51 7E by the stock", async () => {
  const { el, sent, st } = await setup();
  (el.querySelector('[data-song="3"] [data-act="song-pick"]') as HTMLButtonElement).click(); // choose song 3: its buttons appear
  await tick(40);
  (el.querySelector('[data-song="3"] [data-act="song-save"]') as HTMLButtonElement).click(); // 「USB로 (SMF)」
  await tick(60);
  assert.ok(sent.includes("F0 40 7F 24 08 02 0A 02 02 F7"));
  assert.deepEqual(st.saved, [[2, 4, "\\SONG_03.MID"]], "FUN_4014F0E4(song 2, type 4 = MID)");
  assert.equal(el.querySelector("#songs-status")!.textContent, "Save as SMF: done", "said next to the song list");
  el.remove();
});

test("write: 24 05 01 00, TopOfData = byte count, an empty packet after a full one", async () => {
  const { st, session, sent } = await setup();
  const fa = new FA.FileAccessClient(session, 500);
  const d21 = Uint8Array.from({ length: 21 }, (_, i) => 0x40 + i);
  assert.equal(await fa.write("U21.MID", d21), 0);
  assert.ok(sent.includes("F0 40 7F 24 08 02 05 01 00 F7"), "d1 = 1, d2 = 0: the only form the stock writes");
  assert.ok(sent.includes("F0 40 7F 24 08 02 01 00 00 00 15 F7"), "TopOfData 21 bytes");
  const data = sent.filter((m) => m.startsWith("F0 40 7F 24 08 02 02 "));
  assert.equal(data.length, 2, "one full packet + one empty packet");
  assert.equal(data[1], toHex(FA.packet(0x02, 1, [], 0x7f)));
  assert.deepEqual(st.files.get("\\U21.MID"), [...d21]);

  // the last packet's groups start at 21c, 21c + 7, 21c + 14: 15..20 B left -> none reaches the length -> empty packet
  for (const [n, pk] of [[14, 1], [15, 2], [20, 2], [35, 2], [36, 3]] as const) {
    const d = Uint8Array.from({ length: n }, (_, i) => (i * 11) & 0xff);
    assert.equal(FA.dataPackets(d, 0x7f).length, pk, `${n} B -> ${pk} packets`);
    assert.equal(await fa.write(`L${n}.MID`, d), 0);
    assert.deepEqual(st.files.get(`\\L${n}.MID`), [...d]);
  }
  await assert.rejects(fa.write("BIG.MP3", new Uint8Array(FA.FILE_MAX)), (e: unknown) => e instanceof FA.FileAccessError && e.code === 0xe8);

  const d5 = Uint8Array.from([1, 2, 3, 0x80, 0xff]);
  assert.equal(await fa.write("U5.MID", d5), 0);
  assert.deepEqual(st.files.get("\\U5.MID"), [...d5]);
  assert.ok(sent.includes("F0 40 7F 24 08 02 01 00 00 00 05 F7"));
});

test("stock: an unnamed upload (playData) is refused with 0xF0", async () => {
  const { session } = await setup();
  const fa = new FA.FileAccessClient(session, 500);
  await assert.rejects(fa.playData(Uint8Array.from([1, 2, 3])), (e: unknown) => e instanceof FA.FileAccessError && e.code === 0xf0);
});
