// "Internal songs" against the OpenGrand host core (ports/sim): the core answers SongSelect / IntSongPart the
// way the stock does (0xEC for an empty song or part, then MaxTime in bars — firmware src/system/music.c
// song_report), so the same viewer works on both.
import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync } from "node:fs";

import { InternalSongs } from "../src/protocol/songs.ts";
import { PianoSession } from "../src/protocol/session.ts";
import { ParamStore } from "../src/state/store.ts";
import { SIM_BIN, SimProcessLink } from "../scripts/sim-link.ts";

const skip = !existsSync(SIM_BIN) && "firmware simulator not built (make -C ../firmware sim)";
const wait = (ms: number) => new Promise((r) => setTimeout(r, ms));

test("sim: empty recorder scans as 10 empty songs; a recorded Part 1 shows up; delete empties it", { skip }, async () => {
  const link = new SimProcessLink();
  await link.ready;
  try {
    const session = new PianoSession(link, { ackTimeoutMs: 500 });
    const store = new ParamStore();
    store.attach(session);
    await session.connect(2000);
    const songs = new InternalSongs(session, 800);

    let all = await songs.scan();
    assert.deepEqual(all.map((i) => [i.hasData, i.bars]), Array.from({ length: 10 }, () => [false, 0]));

    // record song 2, part 1: MusicMode 2, SongSelect 1, IntSongPart 0, Rec, play a few keys, stop
    await session.sendParam({ fn: 0x31, v1: 0x52, v2: 0x1d, part: 0x7f, data: [2] });
    await session.sendParam({ v1: 0x52, v2: 0x00, part: 0x7f, data: [0, 0, 1] });
    await session.sendParam({ v1: 0x52, v2: 0x01, part: 0x7f, data: [0] });
    await session.sendParam({ fn: 0x32, v1: 0x52, v2: 0x08, part: 0x7f, data: [4] });
    for (const k of [60, 62, 64]) link.command(`key ${k} 100 150`);
    await wait(700);
    await session.sendParam({ fn: 0x32, v1: 0x52, v2: 0x08, part: 0x7f, data: [2] });
    await wait(100);

    all = await songs.scan();
    assert.equal(all[1].hasData, true, "song 2 recorded");
    assert.ok((all[1].bars ?? 0) >= 1);
    assert.deepEqual(all[1].parts, [true, false], "Part 1 only");
    assert.equal(all.filter((i) => i.hasData).length, 1);

    assert.equal(await songs.delete(1, 0x7f), true, "ResetCompleteInfo");
    const s2 = await songs.info(1);
    assert.equal(s2.hasData, false);
  } finally {
    link.close();
  }
});
