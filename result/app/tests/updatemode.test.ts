// USB firmware update mode on the built-in panel:
// the updater's frames as read with REA in the BootProgram, the watch that recognises them, and the model
// used by the panel simulator.
import "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

import { MAIN_END, MAIN_START, UpdaterModel, UpdaterWatch, addressFrame, bootEndFrame, decodeUpdaterFrame, fileFrame, isNormalMode, modeFrame, resultFrame, stoppedFrame } from "../src/protocol/updatemode.ts";
import { fromHex, toHex } from "../src/protocol/sysex.ts";

const CONNECT = fromHex("F0 40 7F 7E 08 02 01 01 F7");

test("frames: the bytes FUN_40015734 / 4001598C / 40015A3C / 40015798 / 40015AC4 / 40015824 build", () => {
  assert.equal(toHex(modeFrame()), toHex(fromHex("F0 40 7F 7E 04 1E 02 01 04 F7")));
  assert.equal(toHex(bootEndFrame()), toHex(fromHex("F0 40 7F 7E 04 1E 03 00 01 7D F7")));
  assert.equal(toHex(resultFrame(false)), toHex(fromHex("F0 40 7F 7E 04 1E 03 00 01 01 F7")));
  assert.equal(toHex(addressFrame(0x123456)), toHex(fromHex("F0 40 7F 7E 04 1E 04 03 56 68 48 00 00 F7")));
  const f = fileFrame("CAx9_P06SYS");
  assert.equal(f.length, 26);
  assert.deepEqual(decodeUpdaterFrame(f), { kind: "file", name: "CAx9_P06SYS" });
  assert.deepEqual(decodeUpdaterFrame(modeFrame()), { kind: "mode", sub: 4 });
  assert.deepEqual(decodeUpdaterFrame(addressFrame(0x3fffff)), { kind: "address", offset: 0x3fffff });
  assert.deepEqual(decodeUpdaterFrame(stoppedFrame(0x21)), { kind: "stopped", why: "nousb", code: 0x21 });
  assert.deepEqual(decodeUpdaterFrame(stoppedFrame(0x56)), { kind: "stopped", why: "nofile", code: 0x56 });
  assert.deepEqual(decodeUpdaterFrame(bootEndFrame()), { kind: "bootEnd" });
  // a broken checksum, the running piano's Mode and an ordinary 7E 03 reply are not updater frames
  const bad = fileFrame("X"); bad[24] ^= 1;
  assert.equal(decodeUpdaterFrame(bad), null);
  assert.equal(decodeUpdaterFrame(fromHex("F0 40 00 7E 04 1E 02 00 00 F7")), null);
  assert.ok(isNormalMode(fromHex("F0 40 00 7E 04 1E 02 00 40 F7")));
  assert.equal(decodeUpdaterFrame(fromHex("F0 40 00 7E 04 1E 03 00 01 6C F7")), null);
});

test("model: only the first exact CONNECT is answered (FUN_40015420 flag), nothing goes out before it", () => {
  const out: Uint8Array[] = [];
  const u = new UpdaterModel((m) => out.push(m), "CAx9_P06SYS", MAIN_START, 0x30000);
  u.step(); u.step();
  assert.equal(out.length, 0, "silent until a CONNECT");
  u.receive(fromHex("F0 40 00 7E 08 02 01 01 F7")); // other device id: not the frame it compares
  u.step();
  assert.equal(out.length, 0);
  u.receive(CONNECT);
  u.step();
  assert.equal(toHex(out[0]), toHex(modeFrame()));
  const n = out.length;
  u.receive(CONNECT); // a second page's CONNECT: no second Mode reply
  while (!u.done) u.step();
  assert.equal(out.slice(n).filter((m) => toHex(m) === toHex(modeFrame())).length, 0);
  assert.equal(toHex(out[out.length - 1]), toHex(bootEndFrame()));
});

test("watch: Mode reply → active, progress to Boot End; the session never sees those frames", () => {
  const w = new UpdaterWatch();
  const seen: string[] = [];
  w.onChange((s) => seen.push(s.phase));
  const u = new UpdaterModel((m) => { if (!w.feed(m)) assert.fail(`not consumed ${toHex(m)}`); });
  u.receive(CONNECT);
  u.step();
  assert.ok(w.state.active);
  assert.equal(w.state.via, "mode");
  for (let i = 0; i < 10; i++) u.step();
  assert.equal(w.state.files.length, 1, "Sync 2 replay + first block = one file");
  assert.equal(w.state.files[0].name, "CAx9_P06SYS");
  assert.ok(w.state.progress! > 0 && w.state.progress! < 0.5);
  while (!u.done) u.step();
  assert.equal(w.state.phase, "done");
  assert.equal(w.state.progress, 1);
  assert.equal(w.state.files[0].result, "ok");
  assert.equal(w.state.files[0].offset, MAIN_END - 1);
  assert.ok(seen.length > 5);
});

test("watch: without the Mode reply (an earlier page took it) the progress frames alone count — but not once the piano answered as itself", () => {
  const w = new UpdaterWatch();
  assert.equal(w.feed(resultFrame(true)), false, "a result alone is an ordinary 7E 03 shape");
  assert.equal(w.feed(addressFrame(0x80000)), true);
  assert.equal(w.state.via, "progress");
  const w2 = new UpdaterWatch();
  w2.feed(fromHex("F0 40 00 7E 04 1E 02 00 40 F7")); // normal Mode reply
  assert.equal(w2.feed(addressFrame(0x80000)), false);
  assert.equal(w2.state.active, false);
  // factory modes (CheckSum / Destination / Model) are not shown
  const w3 = new UpdaterWatch();
  assert.equal(w3.feed(fromHex("F0 40 7F 7E 04 1E 02 01 02 F7")), false);
});

test("watch: no USB memory / write error", () => {
  const w = new UpdaterWatch();
  w.feed(modeFrame());
  w.feed(stoppedFrame(0x21));
  assert.equal(w.state.phase, "nousb");
  const e = new UpdaterWatch();
  e.feed(modeFrame()); e.feed(fileFrame("CAx9_P06SYS")); e.feed(addressFrame(MAIN_START)); e.feed(resultFrame(false)); e.feed(bootEndFrame());
  assert.equal(e.state.phase, "error");
  assert.equal(e.state.files[0].result, "error");
});

test("screen: warning while writing, Boot End message, Korean", async () => {
  const { updateScreen } = await import("../src/ui/updatemode.ts");
  const i18n = await import("../src/i18n.ts");
  i18n.setLang("ko");
  try {
    let kawai = 0;
    const sc = updateScreen({ kawai: () => kawai++, dismiss: () => {} });
    const w = new UpdaterWatch();
    w.onChange((s) => sc.render(s));
    sc.render(w.state);
    assert.equal(sc.el.hidden, true);
    w.feed(modeFrame()); w.feed(fileFrame("CAx9_P06SYS")); w.feed(addressFrame(0x200000));
    assert.equal(sc.el.hidden, false);
    assert.match(sc.el.textContent!, /전원을 끄지 마세요/);
    assert.match(sc.el.textContent!, /CAx9_P06SYS/);
    w.feed(addressFrame(MAIN_END - 1)); w.feed(resultFrame(true)); w.feed(bootEndFrame());
    assert.match(sc.el.textContent!, /Boot End/);
    assert.match(sc.el.textContent!, /100 %/);
    (sc.el.querySelector(".upd-btns button") as HTMLButtonElement).click();
    assert.equal(kawai, 1);
  } finally {
    i18n.setLang("en");
  }
});
