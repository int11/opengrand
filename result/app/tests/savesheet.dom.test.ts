// 「USB로 저장」 (ui/savesheet.ts) and Convert to Audio (ui/audiorec.ts) against a stand-in for the stock V2.19:
// the bytes of the stock's USB audio recorder as the apply function (10 52 23 at 0x401608D8), the audio recorder's
// handler FUN_40123E34 and the fn 0x24 worker FUN_4014F364 take them. Not run on a piano.
import { pianoPair, tick } from "./_dom.ts";
import { beforeEach, test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const { bindStore } = await import("../src/ui/controls.ts");
const { createContext } = await import("../src/ui/context.ts");
const { saveSheet } = await import("../src/ui/savesheet.ts");
const { AUDIOREC, freeName } = await import("../src/ui/audiorec.ts");
const { KINDS, kindOf } = await import("../src/ui/songkinds.ts");
const { usbPlayback, usbAnyRunning } = await import("../src/ui/usbplay.ts");
const FA = await import("../src/protocol/fileaccess.ts");
const { fromHex, toHex } = await import("../src/protocol/sysex.ts");
const i18n = await import("../src/i18n.ts");
Object.assign(AUDIOREC, { stepMs: 150 });

beforeEach(() => {
  localStorage.clear();
  i18n.setLang("en");
});

const ACK24 = fromHex("F0 40 7F 24 04 1E 7F F7");
const OK_STOCK = fromHex("F0 40 7F 32 04 1E 51 7E 7F 00 00 F7");
const note = (v2: number, ...d: number[]) => Uint8Array.from([0xf0, 0x40, 0x7f, 0x32, 0x04, 0x1e, 0x52, v2, 0x7f, ...d, 0xf7]);

/** The stock's audio recorder as far as the remote sees it. */
function audioPiano(piano: { send(m: Uint8Array): void; onMessage(h: (m: Uint8Array) => void): void }, sent: string[], usb = true, internalStandby = false) {
  const st = { internalRec: false, mode: 0, fmt: -1, recMode: -1, state: 2, ops: [] as [number, ...string[]][], op: 0, need: 0, names: [] as string[], cur: [] as number[] };
  piano.onMessage((m) => {
    sent.push(toHex(m));
    if (m[1] !== 0x40 || m[4] !== 0x08) return;
    const fn = m[3], v1 = m[6], v2 = m[7];
    if (fn === 0x10) piano.send(Uint8Array.from([0xf0, 0x40, 0x7f, 0x07, 0x04, 0x1e, v1, v2, m[8], 0xf7]));
    if (fn === 0x10 && v1 === 0x52 && v2 === 0x1c) st.recMode = m[9];
    if (fn === 0x10 && v1 === 0x52 && v2 === 0x00) piano.send(note(0x18, 0, 0, 2, 0, 0, 0)); // MaxTime after a SongSelect
    if (fn === 0x10 && v1 === 0x52 && v2 === 0x23) {
      // 0x401608D8: MusicMode queued, format and RecMode written only for mode 3, Music 3 queued
      if (!usb) {
        // FUN_4015DCC4: no media -> the mode stays, 52 1D says the old one, Result 0x21
        piano.send(note(0x1d, st.mode));
        piano.send(fromHex("F0 40 7F 7E 04 1E 03 00 00 21 F7"));
        // FUN_4015DCC4: the frame's Music 3 then runs in the old mode - in mode 2 the internal
        // recorder goes to record standby and reports it
        if (internalStandby && st.mode === 2) { st.state = 3; piano.send(note(0x08, 3)); }
        return;
      }
      st.mode = m[9]; st.fmt = m[10]; st.recMode = m[11]; st.state = 3;
      piano.send(note(0x1d, 3));
      piano.send(note(0x08, 3));
    }
    if (fn === 0x31 && v1 === 0x52 && v2 === 0x1d) {
      st.mode = m[9];
      piano.send(note(0x1d, m[9]));
    }
    if (fn === 0x32 && v1 === 0x52 && v2 === 0x08) {
      if (st.mode === 2 && m[9] === 4) st.internalRec = true; // a recording in a piano song - must never happen here
      if (st.mode === 2 && m[9] === 2 && st.state === 3) { st.state = 2; piano.send(note(0x08, 2)); }
      if (st.mode !== 3) return;
      if (m[9] === 4) { st.state = 4; piano.send(note(0x08, 4)); }
      if (m[9] === 2) { st.state = 2; piano.send(note(0x08, 2)); }
    }
    if (fn !== 0x24) return;
    piano.send(ACK24);
    if (v1 === 0x06 || v1 === 0x07 || v1 === 0x0a) {
      st.op = v1; st.need = v1 === 0x07 ? 2 : 1; st.names = []; st.cur = [];
      if (v1 === 0x0a) st.names.push(`${m[7]}/${m[8]}`), st.need++;
    } else if (v1 === 0x03) {
      for (const b of FA.readPacket(m) ?? []) {
        if (b) st.cur.push(b);
        else { st.names.push(String.fromCharCode(...st.cur)); st.cur = []; }
      }
      if (st.names.length === st.need) {
        st.ops.push([st.op, ...st.names]);
        piano.send(OK_STOCK);
      }
    }
  });
  return st;
}

async function setup(usb = true, internalStandby = false) {
  const p = await pianoPair();
  const store = new ParamStore();
  bindStore(store);
  store.attach(p.session);
  const ctx = createContext(store, { session: () => p.session });
  const st = audioPiano(p.piano, p.sent, usb, internalStandby);
  let refreshed = 0, quiet = 0;
  const out = document.createElement("p");
  const sheet = saveSheet(ctx, "save-sheet", { lock: { busy: false }, formats: KINDS.piano.saveTo, quiet: async () => void quiet++, refresh: async () => void refreshed++, out });
  document.body.replaceChildren(sheet.el);
  const chip = (f: string) => sheet.el.querySelector(`[data-format="${f}"]`) as HTMLButtonElement;
  const status = () => sheet.el.querySelector("#save-sheet-status")!.textContent;
  const cancel = sheet.el.querySelector('[data-act="save-cancel"]') as HTMLButtonElement;
  return { ...p, store, ctx, st, sheet, chip, status, cancel, out, counts: () => ({ refreshed, quiet }) };
}
const order = (sent: string[], ...frames: string[]) => {
  let at = -1;
  for (const f of frames) {
    const i = sent.indexOf(f, at + 1);
    assert.ok(i > at, `${f} after #${at} in\n${sent.join("\n")}`);
    at = i;
  }
};
const MODE2 = "F0 40 7F 31 08 02 52 1D 7F 02 F7";
const REC = "F0 40 7F 32 08 02 52 08 7F 04 F7";
const STOP = "F0 40 7F 32 08 02 52 08 7F 02 F7";
const RECMODE0 = "F0 40 7F 10 08 02 52 1C 7F 00 F7";

test("the capability table: one format list for a piano song; a USB file's kind decides parts / pause / into the piano", () => {
  assert.deepEqual(KINDS.piano.saveTo, ["MID", "KSO", "MP3", "WAV"]);
  assert.deepEqual([KINDS.MID.parts, KINDS.MP3.parts, KINDS.WAV.parts, KINDS.KSO.parts, KINDS.piano.parts], [false, false, false, true, true]);
  assert.deepEqual((["MID", "MP3", "WAV", "KSO"] as const).map((k) => KINDS[k].intoPiano), [true, false, false, true], "a MID through the converter, a KSO as it is");
  assert.deepEqual((["MID", "MP3", "WAV", "KSO"] as const).map((k) => KINDS[k].howPlayed), ["direct", "direct", "direct", "via piano song"]);
  assert.deepEqual([kindOf("DIR\\a.mp3"), kindOf("B.KSO"), kindOf("x.txt")], ["MP3", "KSO", null]);
  assert.equal(freeName(["song_02.mp3", "SONG_02_2.MP3"], "SONG_02", "MP3"), "SONG_02_3.MP3", "a rename onto an existing name would replace it silently");
});

test("the sheet: Song N / Save to USB with MID · KSO · MP3 · WAV; after a recording 「Saved: piano song N」 / 「Also save to USB」", async () => {
  const { sheet, chip } = await setup();
  sheet.open(1);
  assert.equal(sheet.el.hasAttribute("open"), true);
  assert.deepEqual([sheet.el.querySelector("h3")!.textContent, sheet.el.querySelector("#save-sheet-sub")!.textContent], ["Song 2", "Save to USB"]);
  assert.deepEqual([...sheet.el.querySelectorAll(".chip")].map((c) => c.textContent), ["MID", "KSO", "MP3", "WAV"]);
  assert.match(sheet.el.querySelector(".save-hint")!.textContent!, /takes as long as the song/);
  assert.equal(chip("MP3").disabled, false);
  (sheet.el.querySelector('[data-act="save-close"]') as HTMLElement).click();
  assert.equal(sheet.el.hasAttribute("open"), false);
  sheet.open(2, true);
  assert.deepEqual([sheet.el.querySelector("h3")!.textContent, sheet.el.querySelector("#save-sheet-sub")!.textContent], ["Saved: piano song 3", "Also save to USB"]);
});

test("no USB memory: the formats are disabled and the sheet says why", async () => {
  const { sheet, chip, status, store, sent } = await setup();
  store.fromWire({ fn: 0x10, v1: 0x51, v2: 0x2e, part: 0x7f, data: [0] }); // USBMemory: detached
  sheet.open(0);
  assert.deepEqual(["MID", "KSO", "MP3", "WAV"].map((f) => chip(f).disabled), [true, true, true, true]);
  assert.equal(status(), "Put a USB memory into the piano to save a file.");
  chip("MP3").click();
  await tick(20);
  assert.equal(sent.length, 0);
});

test("MID / KSO: 24 0A <song> <type> + SONG_nn, then the USB list is read again", async () => {
  const { sheet, chip, status, st, sent, out, counts } = await setup();
  sheet.open(1);
  chip("MID").click();
  await tick(60);
  assert.ok(sent.includes("F0 40 7F 24 08 02 0A 01 02 F7"));
  chip("KSO").click();
  await tick(60);
  assert.ok(sent.includes("F0 40 7F 24 08 02 0A 01 03 F7"));
  assert.deepEqual(st.ops, [[0x0a, "1/2", "\\SONG_02.MID"], [0x0a, "1/3", "\\SONG_02.KSO"]]);
  assert.deepEqual([status(), out.textContent], ["Saved to USB: SONG_02.KSO", "Saved to USB: SONG_02.KSO"]);
  assert.equal(counts().refreshed, 2);
  assert.ok(!sent.some((m) => m.includes(" 52 23 ")), "no audio recorder for a file the piano writes at once");
});

test("MP3: song + part in MusicMode 2, 10 52 23 (mode 3, MP3, convert, standby), Music 4, the song's end, rename of the take, RecMode 0", async () => {
  const { sheet, chip, status, cancel, st, sent, piano, ctx, counts, store } = await setup();
  ctx.usbFiles = ["SONG_02.MP3"]; // the name is taken: the piano would replace that file silently
  store.fromWire({ fn: 0x10, v1: 0x52, v2: 0x01, part: 0x7f, data: [0x7f] }); // both parts already
  sheet.open(1);
  chip("MP3").click();
  await tick(80);
  // A-B repeat off (with it on the song never ends: FUN_4016D928), the song, then the one compound frame
  order(sent, MODE2, "F0 40 7F 32 08 02 52 06 7F 00 F7", "F0 40 7F 10 08 02 52 00 7F 00 00 01 F7", "F0 40 7F 10 08 02 52 23 7F 03 01 02 7F 7F 7F 03 F7", REC);
  assert.ok(!sent.includes("F0 40 7F 10 08 02 52 01 7F 7F F7"), "the part is not sent again when it is both parts");
  assert.deepEqual([st.mode, st.fmt, st.recMode, st.state], [3, 1, 2, 4]);
  assert.equal(counts().quiet, 1, "what plays is stopped first");
  // the piano records: the time it reports (52 03, time form), Cancel instead of Close, no second format
  piano.send(note(0x03, 1, 0, 0, 0, 0x5d, 0x60)); // 12 000 ms
  await tick(20);
  assert.equal(status(), "Making MP3… 0:12");
  assert.deepEqual([cancel.hidden, chip("WAV").disabled, (sheet.el.querySelector('[data-act="save-close"]') as HTMLButtonElement).disabled], [false, true, true]);
  assert.equal(usbAnyRunning(usbPlayback(ctx)), false, "the recorded time is not a USB playback");
  assert.ok(!st.ops.length, "nothing renamed while the piano records");
  // the song ends: the piano stops its recorder by itself (FUN_4016D9B0 in RecMode 2)
  piano.send(note(0x08, 2));
  await tick(80);
  assert.deepEqual(st.ops, [[0x07, "\\kawairectempfile.tmp", "\\SONG_02_2.MP3"]]);
  // Reset before the rename (the piano loaded the take into its audio player; the stock panel does the same), and
  // a stop in the recorder mode at the very end
  order(sent, REC, STOP, "F0 40 7F 24 08 02 07 F7", RECMODE0, MODE2, STOP);
  assert.equal(status(), "Saved to USB: SONG_02_2.MP3");
  assert.deepEqual([cancel.hidden, chip("WAV").disabled, counts().refreshed], [true, false, 1]);
  assert.deepEqual([st.recMode, st.mode], [0, 2], "RecMode 2 left on would stop the audio recorder at every song's end");
});

test("WAV, cancelled: Music 2, the take is deleted (24 06 kawairectempfile.tmp), nothing is renamed", async () => {
  const { sheet, chip, status, cancel, st, sent } = await setup();
  sheet.open(0);
  chip("WAV").click();
  await tick(260);
  order(sent, "F0 40 7F 10 08 02 52 00 7F 00 00 00 F7", "F0 40 7F 10 08 02 52 01 7F 7F F7", "F0 40 7F 10 08 02 52 23 7F 03 00 02 7F 7F 7F 03 F7"); // both parts, format 0 = WAV
  assert.equal(st.state, 4);
  sheet.close();
  assert.equal(sheet.el.hasAttribute("open"), true, "not closed while the piano records");
  cancel.click();
  await tick(80);
  order(sent, REC, STOP, "F0 40 7F 24 08 02 06 F7", RECMODE0, MODE2, STOP); // the last stop: the piano song still played
  assert.deepEqual(st.ops, [[0x06, "\\kawairectempfile.tmp"]]);
  assert.equal(status(), "Cancelled. Nothing was saved.");
});

test("the piano has no USB memory after all (mode not changed, Result 0x21): no record command, the reason is shown", async () => {
  const { sheet, chip, status, sent } = await setup(false);
  sheet.open(0);
  chip("MP3").click();
  await tick(600);
  assert.equal(status(), "MP3 failed: No USB memory");
  assert.ok(!sent.includes(REC));
  order(sent, "F0 40 7F 10 08 02 52 23 7F 03 01 02 7F 7F 7F 03 F7", RECMODE0, MODE2);
});

test("field incident 2026-11-24: no USB memory and the internal recorder went to standby (mode 2) - never Music 4, the standby is stopped", async () => {
  const { sheet, chip, status, sent, st } = await setup(false, true);
  sheet.open(0);
  chip("MP3").click();
  await tick(600);
  assert.equal(status(), "MP3 failed: No USB memory");
  assert.ok(!sent.includes(REC), "no Music 4");
  assert.equal(st.internalRec, false);
  order(sent, "F0 40 7F 10 08 02 52 23 7F 03 01 02 7F 7F 7F 03 F7", STOP);
  assert.equal(st.state, 2, "the internal standby ended without a take");
});
