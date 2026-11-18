// Phase B end-to-end against the firmware simulator (REMOTE_PROTOCOL.md §9):
// USB FileAccess (§9.5), user touch curve (§9.1), per-note VT (§9.2),
// Concert Magic settings/results (§9.4), song select (§9.3), recorder reset (§9.6).
import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { findParam } from "../src/protocol/codecs.ts";
import { FileAccessClient, FileAccessError } from "../src/protocol/fileaccess.ts";
import { PianoSession } from "../src/protocol/session.ts";
import { ParamStore } from "../src/state/store.ts";
import { PER_NOTE, PerNoteModel, TouchCurveModel } from "../src/features/vtedit.ts";
import { SIM_BIN, SimProcessLink } from "../scripts/sim-link.ts";
import { tinySmf } from "./fixtures/smf.ts";

const skip = !existsSync(SIM_BIN) && "firmware simulator not built (make -C ../firmware sim)";

async function boot(args: string[] = []) {
  const link = new SimProcessLink({ args });
  await link.ready;
  const session = new PianoSession(link, { ackTimeoutMs: 500 });
  const store = new ParamStore();
  store.attach(session);
  await session.connect(2000);
  return { link, session, store };
}

test("USB file manager: list, read, write, rename, delete, select, import, save-as-SMF, format (§9.5)", { skip }, async () => {
  const usb = mkdtempSync(join(tmpdir(), "ca79ng-usb-"));
  writeFileSync(join(usb, "B.MID"), tinySmf([60, 64]));
  writeFileSync(join(usb, "A.MID"), tinySmf([72], [48]));
  const { link, session, store } = await boot(["--usb", usb]);
  try {
    const fa = new FileAccessClient(session);
    // the simulator mounts the stick on its first USB poll; the piano notifies 10 51 2E 7F 01
    for (let i = 0; i < 40 && store.get(findParam("USBMemory"), 0x7f)?.[0] !== 1; i++) await new Promise((r) => setTimeout(r, 50));
    assert.equal(store.get(findParam("USBMemory"), 0x7f)?.[0], 1, "USBMemory attached");
    // the attach notice can beat the simulator's first directory scan under load (make check-full runs
    // ~30 emulators at once): a list right after it may answer "No data" (236) - retry briefly
    let first: string[] = [];
    for (let i = 0; i < 20; i++) {
      try { first = await fa.list(); break; } catch (e) { if ((e as { code?: number }).code !== 236 || i === 19) throw e; await new Promise((r) => setTimeout(r, 100)); }
    }
    assert.deepEqual(first, ["A.MID", "B.MID"], "sorted, id = position");
    assert.deepEqual([...(await fa.read("A.MID"))], [...tinySmf([72], [48])], "stop-and-wait read is byte exact");

    // a file larger than one packet group round-trips
    const big = tinySmf(Array.from({ length: 40 }, (_, i) => 40 + i));
    assert.equal(await fa.write("C.MID", big), 0);
    assert.deepEqual([...readFileSync(join(usb, "C.MID"))], [...big]);
    await assert.rejects(fa.write("C.MID", big), (e: unknown) => e instanceof FileAccessError && e.code === 0x55, "already exists");

    assert.equal(await fa.rename("C.MID", "D.MID"), 0);
    assert.equal(await fa.renameById(0, "AA.MID"), 0); // A.MID -> AA.MID by file-list id
    assert.deepEqual(await fa.list(), ["AA.MID", "B.MID", "D.MID"]);
    assert.equal(await fa.deleteById(1), 0); // B.MID
    await assert.rejects(fa.delete("nope.MID"), (e: unknown) => e instanceof FileAccessError && e.code === 0x56);
    assert.deepEqual(readdirSync(usb).sort(), ["AA.MID", "D.MID"]);

    // select into the USB slot (MusicTempo is notified), SongSelect numType 1 by id
    assert.equal(await fa.select("D.MID"), 0);
    assert.equal(await store.set(findParam("SongSelect")!, 0x7f, [1, 0, 0]), true);

    // import into internal song 1 (0-based 0), then save it back as SMF
    assert.equal(await store.set(findParam("SongSelect")!, 0x7f, [0, 0, 0]), true);
    assert.equal(await fa.load("AA.MID", 0), 0); // 24 09 00 02 (stock form)
    assert.equal(await fa.saveSongAsSmf(0), 0);
    assert.equal(await fa.saveSong(0, "S1.MID"), 0); // 24 0A 00 02 + name (stock form)
    assert.equal(readFileSync(join(usb, "S1.MID")).subarray(0, 4).toString(), "MThd");
    const saved = (await fa.list()).filter((n) => /^SONG_01/.test(n));
    assert.equal(saved.length, 1, "SONG_01.MID written");
    assert.equal(readFileSync(join(usb, saved[0])).subarray(0, 4).toString(), "MThd");

    // playing an SMF straight from the app (anonymous upload)
    assert.equal(await fa.playData(tinySmf([65])), 0);

    assert.equal(await fa.format(), 0);
    assert.deepEqual(await fa.list(), []);
    link.command("usb detach");
    await new Promise((r) => setTimeout(r, 300));
    assert.equal(store.get(findParam("USBMemory"), 0x7f)?.[0], 0, "detach notified");
    await assert.rejects(fa.list(), (e: unknown) => e instanceof FileAccessError && e.code === 0x21, "no media");
  } finally {
    link.close();
    rmSync(usb, { recursive: true, force: true });
  }
});

test("user touch curve: read chunks, edit points and name, save, discard (§9.1)", { skip }, async () => {
  const { link, store } = await boot();
  try {
    const tc = new TouchCurveModel(store);
    assert.equal(await tc.load(), true, "02 5A 7F 7F answered with 03 5A 7F 7F");
    assert.deepEqual(store.get(findParam("TouchCurve"), 0), [0x40], "TouchCurve = User");
    assert.deepEqual(tc.vel.slice(0, 4), [0, 1, 2, 3]);
    assert.equal(await tc.setPoint(64, 100), true);
    assert.equal(await tc.setName("Soft"), true);
    const fresh = new TouchCurveModel(store);
    await store.request(0x5a, 0x7f);
    assert.equal(fresh.name, "Soft");
    assert.deepEqual(fresh.points(), [64]);
    assert.deepEqual([fresh.vel[32], fresh.vel[64], fresh.vel[96]], [tc.vel[32], 100, tc.vel[96]], "piano interpolation = app interpolation");
    assert.equal(await tc.save(), true);
    await tc.setPoint(30, 90);
    await tc.exit(); // discard -> back to the saved curve
    assert.deepEqual(tc.points(), [64]);
    assert.equal(await tc.clear(), true);
    await store.request(0x5a, 0x7f);
    assert.deepEqual(fresh.points(), []);
  } finally {
    link.close();
  }
});

test("per-note voicing / key volume / tuning / temperament: read, edit by octave, clamp, save (§9.2)", { skip }, async () => {
  const { link, store } = await boot();
  try {
    for (const k of PER_NOTE) {
      const m = new PerNoteModel(store, k);
      assert.equal(await m.load(), true, `${k.id} table read`);
      const idx = k.size === 12 ? 4 : 60;
      await m.set(idx, 2);
      await m.set(idx + 1, k.limit + 9); // clamped
      const check = new PerNoteModel(store, k);
      assert.equal(await check.load(false), true);
      assert.deepEqual([check.values[idx], check.values[idx + 1]], [2, k.limit], k.id);
      assert.equal(await m.save(), true);
      await m.reset();
      await check.load(false);
      assert.equal(check.values[idx], 0, `${k.id} reset`);
    }
  } finally {
    link.close();
  }
});

test("Concert Magic settings round-trip and tap results are reported (§9.4)", { skip }, async () => {
  const usb = mkdtempSync(join(tmpdir(), "ca79ng-usb-"));
  writeFileSync(join(usb, "CM.MID"), tinySmf([60, 62, 64, 65, 67]));
  const { link, session, store } = await boot(["--usb", usb]);
  try {
    const results: number[][] = [];
    const cfg: number[][] = [];
    store.onMessage((m) => {
      if (m.v1 === 0x52 && m.v2 === 0x0b) results.push(m.data);
      if (m.v1 === 0x52 && m.v2 === 0x0d) cfg.push(m.data);
    });
    assert.equal(await store.request(0x52, 0x0d), true);
    assert.equal(cfg.length >= 1, true, "settings answered");
    assert.equal(await store.send({ v1: 0x52, v2: 0x0d, data: [0, 1, 0] }), true);
    cfg.length = 0;
    await store.request(0x52, 0x0d);
    assert.deepEqual(cfg.at(-1)?.slice(0, 3), [0, 1, 0]);

    await store.command(findParam("MusicMode", 0x31)!, 0x7f, [5]);
    assert.equal(await new FileAccessClient(session).select("CM.MID"), 0);
    await store.command(findParam("Music", 0x32)!, 0x7f, [1]);
    for (let i = 0; i < 3; i++) {
      link.command("key 60 90 100");
      link.command("wait 400");
    }
    await new Promise((r) => setTimeout(r, 1500));
    assert.ok(results.length >= 1, `Concert Magic results: ${JSON.stringify(results)}`);
    assert.ok(results.every((r) => r[0] >= 0 && r[0] <= 5));
  } finally {
    link.close();
    rmSync(usb, { recursive: true, force: true });
  }
});

test("delete all recorder songs reports ResetCompleteInfo (§9.6)", { skip }, async () => {
  const { link, store } = await boot();
  try {
    let done: number[] | null = null;
    store.onMessage((m) => {
      if (m.fn === 0x32 && m.v1 === 0x52 && m.v2 === 0x22) done = m.data;
    });
    await store.send({ fn: 0x32, v1: 0x52, v2: 0x21, data: [0x7f, 0x7f, 0x7f, 0x7f] });
    await new Promise((r) => setTimeout(r, 300));
    assert.deepEqual(done, [0, 0x7f, 0]);
  } finally {
    link.close();
  }
});

test("BLE module version through the main board (§9.7) and the Information screen path", { skip }, async () => {
  const { link, session } = await boot();
  try {
    const { queryModuleVersion } = await import("../src/features/dfu.ts");
    const v = await queryModuleVersion(session, 1500);
    // the simulator has no real module: either a version string or no answer, never a hang
    assert.ok(v === null || typeof v === "string");
    const id = session.identity!;
    assert.equal(id.model, "CA79");
  } finally {
    link.close();
  }
});
