// Fast OTA upload (REMOTE_PROTOCOL.md 10.3 Data v2, cave/include/ota.h "fast upload"): the LZ encoder against the
// reference decoder, and OtaClient.update against a simulated stage-1 that offers v2 frames in its Begin reply (CRC per
// frame, LZ, window, cumulative ACK) - faults on the way, the fallback to 200 B stop-and-wait for older loaders, and the
// RESELECT completion rule of 10.12.
import { test } from "node:test";
import assert from "node:assert/strict";
import { createHash, createHmac, randomBytes } from "node:crypto";

const ota = await import("../src/protocol/ota.ts");
const { pack7, unpack7, crc32 } = await import("../src/protocol/ngext.ts");
const { PianoSession } = await import("../src/protocol/session.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");

const KEY = Uint8Array.from({ length: 32 }, (_, i) => (i * 37 + 11) & 0xff);
const hmac = (k: Uint8Array, m: Uint8Array) => new Uint8Array(createHmac("sha256", k).update(m).digest());
const le32 = (v: number) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
const u32 = (b: Uint8Array, o: number) => (b[o]! | (b[o + 1]! << 8) | (b[o + 2]! << 16) | (b[o + 3]! << 24)) >>> 0;
const keyIdRef = (k: Uint8Array) => u32(hmac(k, new TextEncoder().encode("C79 key id")), 0);

/** A .c79 (format 1 main); `code` = a payload with repeats, as real ARM/Thumb code has (LZ gets ~0.6-0.8). */
function makeFile(payloadLen: number, code = true): Uint8Array {
  let x = 12345;
  const rnd = () => ((x = (x * 1103515245 + 12345) >>> 0) >>> 16) & 0xff;
  const payload = new Uint8Array(payloadLen);
  const noise = randomBytes(payloadLen);
  for (let i = 0; i < payloadLen;) {
    if (code && i >= 64 && rnd() < 100) {             // a repeat of 4..19 bytes from up to 2 KB back
      const d = 1 + (((rnd() << 8) | rnd()) % Math.min(i, 2048));
      for (let k = 4 + (rnd() & 15); k && i < payloadLen; k--, i++) payload[i] = payload[i - d]!;
    } else payload[i] = noise[i++]!;
  }
  payload.set([0x43, 0x37, 0x39, 0x45]);
  const hdr = new Uint8Array(64);
  hdr.set([0x43, 0x37, 0x39, 0x50, 1, 0, 64, 0, ...le32(payloadLen), ...le32(0x401c0000), ...le32(0), ...le32(crc32(payload)),
    ...le32(0x7085ad09), ...le32(1), ...le32(0x073b), ...le32(keyIdRef(KEY))]);
  hdr.set(hmac(KEY, Uint8Array.from([...hdr.slice(0, 0x30), ...payload])).slice(0, 16), 0x30);
  return Uint8Array.from([...hdr, ...payload]);
}

interface FastOpts {
  /** Begin reply: null = an older loader (5 bytes) */
  offer?: { caps: number; window: number; max: number } | null;
  /** damage on the way: k = data frame number (1-based) -> the messages that arrive */
  mangle?: (k: number, m: Uint8Array) => Uint8Array[];
  /** data replies (1-based) lost on the way back */
  dropReply?: Set<number>;
}

/** stage-1 (s1.c ota_op / data2) as far as the upload goes. */
function fastPiano(o: FastOpts = {}) {
  const offer = o.offer === undefined ? { caps: 3, window: 4, max: 1536 } : o.offer;
  const [app, piano] = LoopbackLink.pair();
  const s = {
    challenge: 0x13572468, job: 0, err: 0, len: 0, crc: 0, rx: 0, staging: new Uint8Array(0x20000),
    dataOps: 0, v1Ops: 0, v2Ops: 0, inFlight: 0, maxInFlight: 0, frameMax: 0, replies: 0, begins: 0, polls: 0,
    flags: new Set<number>(), rcs: [] as number[],
  };
  const reply = (op: number, raw: number[]) => piano.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...pack7(raw), 0xf7]));
  const auth = (op: number, args: Uint8Array, tag: Uint8Array) => {
    const want = hmac(KEY, Uint8Array.from([0x43, 0x37, 0x39, 0x41, op, ...le32(s.challenge), ...args])).slice(0, 8);
    s.challenge = (s.challenge * 1103515245 + 12345) >>> 0;
    return want.every((b, i) => b === tag[i]);
  };
  const data2 = (body: Uint8Array): number[] => {
    const off = ((body[0]! & 0x3f) << 14) | (body[1]! << 7) | body[2]!;
    let rc = 0;
    if (s.job !== 1) rc = 8;
    else if (off > s.rx) rc = 7;
    else if (off === s.rx) {
      const u = unpack7(body.slice(3));
      if (u.length > 5 + offer!.max) rc = 1;
      else if (u.length < 5 || crc32(u.slice(4)) !== u32(u, 0)) rc = 10;
      else if (u.length < 6 || u[4]! > 1) rc = 1;
      else if (u[4] === 1) {
        const end = ota.lzDecode(u.slice(5), s.staging, off, s.len);
        if (end <= off) rc = 1;
        else s.rx = end;
      } else if (u.length - 5 > s.len - off) rc = 5;
      else { s.staging.set(u.slice(5), off); s.rx = off + u.length - 5; }
      if (rc === 0) s.flags.add(u[4]!);
    }
    s.rcs.push(rc);
    return [rc, ...le32(s.rx), ...le32(off)];
  };
  piano.onMessage((m0) => {
    if (m0[1] !== 0x7d) return;
    const msgs = m0[6] === 0x12 && o.mangle ? o.mangle(++s.dataOps, m0) : [m0];
    if (m0[6] === 0x12 && !o.mangle) s.dataOps++;
    for (const m of msgs) {
      s.frameMax = Math.max(s.frameMax, m.length);
      const op = m[6]!;
      const body = m.slice(7, m.length - 1);
      if (op === 0x10) {
        if (s.job >= 2 && s.job < 8 && ++s.polls > 2) s.job = 8;
        const r = new Array(48).fill(0);
        r.splice(0, 8, 1, s.job, s.err, 1, 2, 2, 0, 0);
        r.splice(8, 8, ...le32(s.rx), ...le32(s.len));
        r.splice(16, 8, ...le32(s.challenge), ...le32(keyIdRef(KEY)));
        r.splice(24, 12, 0, 0, 0, 0, 0x40, 0, 0, 0, 0, 0, 0x40, 0);
        r[44] = 1;
        r[45] = 200;
        reply(0x10, r);
      }
      if (op === 0x11) {
        const raw = unpack7(body);
        const ok = auth(op, raw.slice(0, 13), raw.slice(13));
        s.len = u32(raw, 1); s.crc = u32(raw, 5); s.rx = 0;
        if (ok) { s.job = 1; s.begins++; }
        const rc = ok ? 0 : 2;
        reply(0x11, offer ? [rc, ...le32(s.challenge), offer.caps, offer.window, offer.max & 0xff, offer.max >> 8] : [rc, ...le32(s.challenge)]);
      }
      if (op === 0x12) {
        s.inFlight++;
        s.maxInFlight = Math.max(s.maxInFlight, s.inFlight);
        let r: number[];
        if (body[0]! & 0x40 && offer) { s.v2Ops++; r = data2(body); }
        else {
          s.v1Ops++;
          const off = (body[0]! << 14) | (body[1]! << 7) | body[2]!;
          const d = unpack7(body.slice(3));
          let rc = 0;
          if (off !== s.rx) rc = 7;
          else if (d.length > 200) rc = 1;
          else { s.staging.set(d, off); s.rx += d.length; }
          r = [rc, ...le32(s.rx)];
        }
        const n = ++s.replies;
        setTimeout(() => { s.inFlight--; if (!o.dropReply?.has(n)) reply(0x12, r); }, 2);
      }
      if (op === 0x13) {
        const raw = unpack7(body);
        const ok = auth(op, raw.slice(0, 1), raw.slice(1));
        const rc = !ok ? 2 : s.rx !== s.len ? 9 : 0;
        if (rc === 0 && crc32(s.staging.slice(0, s.len)) !== s.crc) { s.job = 9; s.err = 10; }
        else if (rc === 0) { s.job = 2; s.err = 0; s.polls = 0; }
        reply(0x13, [rc, ...le32(s.challenge)]);
      }
    }
  });
  return { session: new PianoSession(app, { handshake: false }), s };
}

test("LZ frames: decode back to the file, whole tokens per frame, each within the budget", () => {
  for (const [len, code] of [[0, true], [4, true], [5, false], [300, true], [5000, false], [40_000, true]] as const) {
    const f = len ? makeFile(len, code) : Uint8Array.of(7);
    for (const budget of [16, 200, 1024, 1536]) {
      const frames = ota.lzFrames(f, budget);
      const out = new Uint8Array(f.length);
      let pos = 0;
      for (const fr of frames) {
        assert.equal(fr.off, pos, "frames follow each other");
        assert.ok(fr.data.length <= budget && fr.data.length > 0, `budget ${budget}: ${fr.data.length}`);
        pos = ota.lzDecode(fr.data, out, pos, f.length);
        assert.equal(pos, fr.end, `frame [${fr.off}, ${fr.end}) decodes whole`);
      }
      assert.equal(pos, f.length);
      assert.deepEqual(out, f, `len ${len} budget ${budget}`);
    }
  }
  const code = makeFile(40_000);
  const lz = ota.lzFrames(code, 1024).reduce((a, x) => a + x.data.length, 0);
  assert.ok(lz < code.length * 0.8, `code-like data compresses (${lz} / ${code.length})`);
  // the reference decoder refuses what stage-1 refuses
  const out = new Uint8Array(100);
  assert.equal(ota.lzDecode(Uint8Array.from([0x10, 0xaa, 0x05, 0x00]), out, 0, 100), -1, "distance beyond the bytes so far");
  assert.equal(ota.lzDecode(Uint8Array.from([0x10, 0xaa, 0x00, 0x00]), out, 0, 100), -1, "distance 0");
  assert.equal(ota.lzDecode(Uint8Array.from([0x1f, 0xaa, 0x01, 0x00, 0xff, 0xff]), out, 0, 100), -1, "past the end");
  assert.equal(ota.lzDecode(Uint8Array.from([0xf0, 0xff]), out, 0, 100), -1, "truncated length");
  assert.equal(ota.lzDecode(Uint8Array.from([0x13, 0xaa, 0x01, 0x00]), out, 0, 100), 8, "overlapping run");
  assert.deepEqual([...out.slice(0, 8)], new Array(8).fill(0xaa));
});

test("LZ frames = tools/ota_frames.py byte for byte (the emulator round trip feeds those to the real stage-1)", () => {
  const syn = Uint8Array.from({ length: 20000 }, (_, i) => (((i * i * 7) >> 3) ^ (i >> 6)) & 0xff);
  const h = createHash("sha256");
  for (const f of ota.lzFrames(syn, 1024)) h.update(Uint8Array.from([...le32(f.off), ...le32(f.end), ...f.data]));
  assert.equal(h.digest("hex").slice(0, 16), "22f6d521b3baf8b1");   // roundtrip_ota_window.py LZ_VECTOR
});

test("d2Message: flag bit, 20-bit offset, CRC over flags || data, <= 0x800 B at the loader's max", () => {
  const data = Uint8Array.from({ length: 1536 }, (_, i) => (i * 31) & 0xff);
  const m = ota.d2Message({ off: 0x1fe00, end: 0x1fe00 + 1536, flags: 0, data });
  assert.equal(m.length, 1773);
  assert.ok(m.length <= 0x800);
  assert.deepEqual([...m.slice(0, 10)], [0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, 0x12, 0x40 | 7, 0x7c, 0]);
  const u = unpack7(m.slice(10, -1));
  assert.equal(u32(u, 0), crc32(Uint8Array.from([0, ...data])));
  assert.equal(ota.decodeBeginOffer(Uint8Array.from([0, 1, 2, 3, 4])), null, "an older loader's 5-byte Begin reply");
  assert.deepEqual(ota.decodeBeginOffer(Uint8Array.from([0, 1, 2, 3, 4, 3, 4, 0, 6])), { caps: 3, window: 4, max: 1536 });
});

test("fast upload: v2 frames, two in flight, LZ when smaller; progress up to the end; done", async () => {
  const { session, s } = fastPiano();
  const file = makeFile(30_000);
  const prog: number[] = [];
  const fin = await new ota.OtaClient(session).update(file, KEY, (p) => p.phase === "upload" && prog.push(p.sent), 2);
  assert.equal(fin.phase, "done", fin.message);
  assert.deepEqual(s.staging.slice(0, file.length), file);
  assert.deepEqual(fin.framing, { v2: true, chunk: 1024, window: 2, lz: true });
  assert.equal(fin.resends, 0);
  assert.equal(s.v1Ops, 0);
  assert.ok(s.maxInFlight === 2, `max in flight ${s.maxInFlight}`);
  assert.ok(s.frameMax <= 0x800, `frame ${s.frameMax}`);
  assert.deepEqual([...s.flags], [1]);
  assert.ok(prog.every((v, i) => !i || v > prog[i - 1]!) && prog.at(-1) === file.length, "progress grows to the end");
  assert.ok(s.v2Ops < Math.ceil(file.length / 1024), `LZ needs fewer frames (${s.v2Ops})`);
});

test("fast upload: options and the loader's limits; raw frames when LZ is not offered or does not pay", async () => {
  {
    const { session, s } = fastPiano({ offer: { caps: 1, window: 1, max: 600 } });
    const fin = await new ota.OtaClient(session).update(makeFile(9000), KEY, () => {}, 2, 2, { chunk: 1536, window: 4 });
    assert.equal(fin.phase, "done", fin.message);
    assert.deepEqual(fin.framing, { v2: true, chunk: 600, window: 1, lz: false });
    assert.equal(s.maxInFlight, 1);
    assert.deepEqual([...s.flags], [0]);
  }
  {
    const { session, s } = fastPiano();
    const file = makeFile(9000, false);              // random: LZ does not pay
    const fin = await new ota.OtaClient(session).update(file, KEY, () => {}, 2);
    assert.equal(fin.phase, "done", fin.message);
    assert.equal(fin.framing!.lz, false);
    assert.deepEqual(s.staging.slice(0, file.length), file);
  }
  {
    const { session, s } = fastPiano();
    const fin = await new ota.OtaClient(session).update(makeFile(3000), KEY, () => {}, 2, 2, { fast: false });
    assert.equal(fin.phase, "done", fin.message);
    assert.equal(fin.framing!.v2, false);
    assert.equal(s.v2Ops, 0);
  }
});

test("fallback: a loader without the offer (5-byte Begin reply: v3, v4 before stage-1 version 2) gets 200 B stop-and-wait", async () => {
  const { session, s } = fastPiano({ offer: null });
  const file = makeFile(5000);
  const fin = await new ota.OtaClient(session).update(file, KEY, () => {}, 2);
  assert.equal(fin.phase, "done", fin.message);
  assert.deepEqual(fin.framing, { v2: false, chunk: 200, window: 1, lz: false });
  assert.equal(s.v2Ops, 0);
  assert.equal(s.v1Ops, Math.ceil(file.length / 200));
  assert.equal(s.maxInFlight, 1);
  assert.ok(s.frameMax <= 256);
  assert.deepEqual(s.staging.slice(0, file.length), file);
});

test("fast upload: frames lost / damaged / cut / duplicated on the way and lost replies - only the gap is sent again", async () => {
  const file = makeFile(20_000);
  const flip = (m: Uint8Array) => Uint8Array.from(m, (b, i) => (i === 40 ? b ^ 1 : b));
  type Fin = Awaited<ReturnType<InstanceType<typeof ota.OtaClient>["update"]>>;
  const cases: [string, FastOpts, (r: Fin, s: ReturnType<typeof fastPiano>["s"]) => boolean][] = [
    ["frame lost", { mangle: (k, m) => (k === 4 ? [] : [m]) }, (r, s) => r.resends === 1 && s.rcs.includes(7)],
    ["byte changed", { mangle: (k, m) => (k === 4 ? [flip(m)] : [m]) }, (r, s) => r.resends === 1 && s.rcs.includes(10)],
    ["tail cut", { mangle: (k, m) => (k === 4 ? [Uint8Array.from([...m.slice(0, 100), 0xf7])] : [m]) }, (r, s) => r.resends === 1 && s.rcs.includes(10)],
    ["duplicated", { mangle: (k, m) => (k === 4 ? [m, m] : [m]) }, (r) => r.resends === 0],
    ["reply lost", { dropReply: new Set([4]) }, (r) => r.resends === 0],
  ];
  for (const [name, o, cond] of cases) {
    for (const lz of [false, true]) {
      const { session, s } = fastPiano(o);
      const fin = await new ota.OtaClient(session).update(file, KEY, () => {}, 2, 2, { lz, window: 3 });
      assert.equal(fin.phase, "done", `${name}: ${fin.message}`);
      assert.deepEqual(s.staging.slice(0, file.length), file, name);
      assert.equal(s.begins, 1, `${name}: no restart from Begin`);
      assert.ok(cond(fin, s), `${name} lz=${lz}: resends ${fin.resends}, rcs ${[...new Set(s.rcs)]}`);
    }
  }
});

test("fast upload: the last frame's reply lost -> timeout, one frame in flight from then on, still done", async () => {
  const file = makeFile(3000, false);
  const { session, s } = fastPiano({ dropReply: new Set([3]) });
  const t0 = Date.now();
  const fin = await new ota.OtaClient(session).update(file, KEY, () => {}, 2, 2, { chunk: 1024, lz: false });
  assert.equal(fin.phase, "done", fin.message);
  assert.equal(fin.resends, 1);
  assert.deepEqual(s.staging.slice(0, file.length), file);
  assert.ok(Date.now() - t0 >= 1500, "waited for the timeout");
});

test("fast upload: a refusal other than 7 / 10 (here rc 1, unknown frame flags) stops the upload with the rc's name", async () => {
  const unknownFlags = (k: number, m: Uint8Array) => {
    if (k !== 2) return [m];
    const off = ((m[7]! & 0x3f) << 14) | (m[8]! << 7) | m[9]!;
    const u = unpack7(m.slice(10, -1));
    return [ota.d2Message({ off, end: off, flags: 2, data: u.slice(5) })];
  };
  const { session } = fastPiano({ mangle: unknownFlags });
  const fin = await new ota.OtaClient(session).update(makeFile(3000), KEY, () => {}, 2, 2, { lz: false });
  assert.equal(fin.phase, "error");
  assert.match(fin.message ?? "", /upload stopped at 1024 B: bad arguments/);
});

test("apply RESELECT with the counter already at 255: done only once the apply state is idle (0), not while quiescing (3)", async () => {
  const [app, piano] = LoopbackLink.pair();
  let challenge = 0x11111111, polls = 0;
  const reply = (op: number, raw: number[]) => piano.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...pack7(raw), 0xf7]));
  const seen: number[] = [];
  piano.onMessage((m) => {
    if (m[1] !== 0x7d) return;
    if (m[6] === 0x15) { challenge++; reply(0x15, [0, ...le32(challenge)]); polls = 0; return; }
    if (m[6] !== 0x10) return;
    // before the request: counter 255, reselected flag from an earlier RESELECT, idle. After it: the old stage-1
    // quiesces (3) for two polls - with byte 86 bit 2 still set from before - then the new one answers idle (0)
    const after = seen.length > 1;
    const ap = !after ? 0 : polls++ < 2 ? 3 : 0;
    seen.push(ap);
    const r = new Array(112).fill(0);
    r.splice(0, 6, 3, 0, 0, 1, 0, 1);
    r.splice(16, 8, ...le32(challenge), ...le32(keyIdRef(KEY)));
    r[48] = 0x1f; r[49] = ap;
    r.splice(72, 12, ...le32(1), 0x4b, 0, ...le32(2), 0x43, 0);
    r[84] = 0; r[86] = 0x04; r[87] = 1; r[88] = 1; r[89] = 0x1f; r[110] = 255;
    reply(0x10, r);
  });
  const session = new PianoSession(app, { handshake: false });
  const res = await new ota.OtaClient(session).apply(KEY, ota.ApplyTarget.Reselect, () => {}, 2);
  assert.equal(res.ok, true, res.message);
  assert.equal(res.status!.applyState, 0);
  assert.ok(seen.filter((v) => v === 3).length === 2, `quiescing polls passed over: ${seen}`);
});
