// Piano songs (OpenGrand playback control, 0x7D op 0x1E - REMOTE_PROTOCOL.md section 10.9, firmware
// result/firmware/ports/cave/recctl_cave.c): the decoders, the
// NgRecorder op calls and the music tab box (shown only with Identify feat2 0x10, follows the piano's notices).
import { tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const ng = await import("../src/protocol/ngext.ts");
const { ParamStore } = await import("../src/state/store.ts");
const { createContext } = await import("../src/ui/context.ts");
const S = await import("../src/ui/screens.ts");

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
  let state = 2, song = 0;
  return {
    state: "ready",
    asked,
    push,
    onMidi(fn: (m: Uint8Array) => void) {
      listeners.push(fn);
      return () => listeners.splice(listeners.indexOf(fn), 1);
    },
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
        if (cmd === 1) state = 1;
        if (cmd === 4) song = arg;
        reply(0x1e, [0, 0, ...ng.pack7(statusRaw(state, song))]);
        reply(0x1e, [3, 1, cmd]);
      }
    },
  };
}

test("music tab: piano songs box - hidden without feat2 0x10, else song list, play, refusal text", async () => {
  const store = new ParamStore();
  const old = fakeSession(0x0f);
  const ctx0 = createContext(store, { session: () => old as never });
  const el0 = S.musicScreen(store, () => "", ctx0);
  document.body.append(el0);
  for (let i = 0; i < 20; i++) await tick();
  assert.equal((el0.querySelector("#piano-songs") as HTMLElement).hidden, true);
  assert.ok(!old.asked.some((a) => a[0] === 0x1e), "a payload without the feature is never asked op 0x1E");
  el0.remove();

  const s = fakeSession(0x1f);
  const ctx = createContext(store, { session: () => s as never });
  const el = S.musicScreen(store, () => "", ctx);
  document.body.append(el);
  for (let i = 0; i < 20; i++) await tick();
  const box = el.querySelector("#piano-songs") as HTMLElement;
  assert.equal(box.hidden, false);
  const opts = [...box.querySelectorAll("select option")].map((o) => o.textContent);
  assert.deepEqual(opts.slice(0, 3), ["Song 1 · 3 bars", "Song 2 · 5 bars", "Song 3 · empty"]);
  assert.equal(box.querySelector("#piano-songs-state")!.textContent, "Stopped");
  (box.querySelector("[data-action=piano-play]") as HTMLButtonElement).click();
  for (let i = 0; i < 20; i++) await tick();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 1, 0, 0]);
  assert.equal(box.querySelector("#piano-songs-state")!.textContent, "Playing · bar 0");
  // song 3 (empty): the piano refuses with rc 6
  const sel = box.querySelector("select") as HTMLSelectElement;
  sel.value = "2";
  sel.dispatchEvent(new sel.ownerDocument.defaultView!.Event("change"));
  for (let i = 0; i < 20; i++) await tick();
  assert.deepEqual(s.asked.at(-1), [0x1e, 0x02, 4, 0, 2]);
  (box.querySelector("[data-action=piano-play]") as HTMLButtonElement).click();
  for (let i = 0; i < 20; i++) await tick();
  assert.equal(box.querySelector("#piano-songs-note")!.textContent, "Nothing recorded in this song.");
  // an unsolicited notice (the panel stopped it)
  s.push(frame([0x00, 0x00, ...ng.pack7(statusRaw(2, 2))]));
  for (let i = 0; i < 5; i++) await tick();
  assert.equal(box.querySelector("#piano-songs-state")!.textContent, "Stopped");
  el.remove();
});
