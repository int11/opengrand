// End-to-end: the app's protocol stack talking to the real firmware core
// (ports/sim) over the PANEL port — the same bytes a USB-MIDI link carries.
import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync } from "node:fs";

import { bpm, findParam, rangeEncode } from "../src/protocol/codecs.ts";
import { PianoSession } from "../src/protocol/session.ts";
import { ParamStore } from "../src/state/store.ts";
import { SIM_BIN, SimProcessLink } from "../scripts/sim-link.ts";

const skip = !existsSync(SIM_BIN) && "firmware simulator not built (make -C ../firmware sim)";

function nextStatus(link: SimProcessLink): Promise<string> {
  return new Promise((resolve) => {
    link.onLine((l) => {
      if (l.startsWith("status")) resolve(l);
    });
    link.command("status");
  });
}

test("app connects to the firmware, syncs, edits and controls it", { skip }, async () => {
  const link = new SimProcessLink();
  await link.ready;
  try {
    const session = new PianoSession(link, { ackTimeoutMs: 500 });
    const store = new ParamStore();
    store.attach(session);

    const id = await session.connect(2000);
    assert.equal(id.model, "CA79");
    assert.equal(id.version, "1.0.0");
    assert.ok(store.size() > 150, `dump delivered ${store.size()} values`);
    assert.deepEqual(store.get(findParam("Sound"), 0), [95, 112, 0]); // SK-EX Rendering

    // edit: reverb type (ACKed 0x10), resonance depth (range codec), master tuning
    assert.equal(await store.set(findParam("ReverbType")!, 0, [5]), true);
    const rd = findParam("ResonanceDepth")!;
    assert.equal(await store.set(rd, 0, [rangeEncode(rd, 8)]), true);
    assert.equal(await store.set(findParam("Tuning")!, 0x7f, [68]), true); // 442 Hz
    // select Jazz Organ, Dual mode
    assert.equal(await store.set(findParam("Sound")!, 0, [121, 0, 17]), true);
    assert.equal(await store.set(findParam("KeyboardMode")!, 0x0f, [1]), true);

    let st = await nextStatus(link);
    assert.match(st, /mode=1/);
    assert.match(st, /sound=121\/0\/17/);
    assert.match(st, /tune=442\.0/);

    // metronome: tempo then run (remote command, fire-and-forget)
    await store.set(findParam("MetronomeTempo")!, 0x7f, bpm.encode(180));
    await store.command(findParam("Metronome", 0x32)!, 0x7f, [1]);
    const beats: number[] = [];
    session.onParam((p) => {
      if (p.v1 === 0x56 && p.v2 === 0x0b) beats.push(p.data[0]);
    });
    await new Promise((r) => setTimeout(r, 1100));
    st = await nextStatus(link);
    assert.match(st, /metro=1 bpm=180/);
    assert.ok(beats.length >= 2, `beat counter notifications: ${beats.length}`);

    // the piano rejects out-of-range data with NACK -> store rolls back
    const ch = findParam("MIDIChannel")!;
    if (ch.dataMax[0] < 127) {
      assert.equal(await store.set(ch, 0x7f, [127]), false);
    }
    assert.equal(session.stats.failed, ch.dataMax[0] < 127 ? 1 : 0);
  } finally {
    link.close();
  }
});

test("two controllers stay in sync (panel + phone)", { skip }, async () => {
  const panel = new SimProcessLink({ port: "panel" });
  await panel.ready;
  try {
    // second controller on the BLE port of the same simulator process
    const lines: string[] = [];
    panel.onLine((l) => lines.push(l));
    const sp = new PianoSession(panel, { ackTimeoutMs: 500 });
    const store = new ParamStore();
    store.attach(sp);
    await sp.connect(2000);
    // REMOTE_PROTOCOL.md §9.8: Bluetooth MIDI now defaults to Off and the BLE port is
    // blocked until it is switched on (from the panel, as a user would)
    assert.equal(await store.set(findParam("BluetoothMIDI")!, 0x7f, [1]), true);
    panel.command("midi ble F0 40 7F 7E 08 02 01 01 F7");
    panel.command("midi ble F0 40 00 10 08 02 55 08 00 01 F7"); // phone: ReverbType = Lounge
    await new Promise((r) => setTimeout(r, 300));
    assert.deepEqual(store.get(findParam("ReverbType"), 0), [1], "panel learned the phone's change");
    assert.ok(lines.some((l) => l === "tx ble F0 40 7F 07 04 1E 55 08 00 F7"), "phone got its ACK");
  } finally {
    panel.close();
  }
});

test("parameters sent by the new app controls are accepted by the firmware (features 11, 52, 81, 82)", { skip }, async () => {
  const link = new SimProcessLink();
  await link.ready;
  try {
    const session = new PianoSession(link, { ackTimeoutMs: 300, maxRetries: 2 });
    const store = new ParamStore();
    store.attach(session);
    await session.connect(2000);
    const sends: [string, number, number[]][] = [
      ["SoundPreviewStart", 0x7f, [3]], // 81
      ["SoundPreviewStop", 0x7f, [3]],
      ["SoundPreviewVolume", 0x7f, [90]],
      ["MusicTempo", 0x7f, bpm.encode(96)], // 52
      ["IntSongPart", 0x7f, [0]],
      ["TransposeValue", 0x7f, [61]], // 11 song transpose (System part)
      ["SongSelect", 0x7f, [0, 0, 3]], // 3-byte form
    ];
    for (const [name, part, data] of sends) assert.equal(await store.set(findParam(name)!, part, data), true, `${name} ACKed`);
    // 82: Panel lock (§9.9) — from the panel port everything but LockMode is refused
    assert.equal(await store.set(findParam("LockMode")!, 0x7f, [1, 0]), true);
    assert.equal(await store.set(findParam("ReverbType")!, 0, [4]), false, "locked panel edit NACKed");
    assert.equal(await store.set(findParam("LockMode")!, 0x7f, [0, 0]), true, "unlock accepted");
    assert.equal(await store.set(findParam("ReverbType")!, 0, [4]), true);
  } finally {
    link.close();
  }
});
