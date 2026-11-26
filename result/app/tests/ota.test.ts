// Remote cave update (REMOTE_PROTOCOL.md section 10.3): crypto against node:crypto, the client
// against a simulated ota_loader resident (tags checked with node:crypto, stop-and-wait asserted),
// and the Information screen section.
import { tick } from "./_dom.ts";
import { beforeEach, test } from "node:test";
import assert from "node:assert/strict";
import { createHash, createHmac } from "node:crypto";

const ota = await import("../src/protocol/ota.ts");
const { pack7, unpack7, crc32 } = await import("../src/protocol/ngext.ts");
const { PianoSession } = await import("../src/protocol/session.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const { ParamStore } = await import("../src/state/store.ts");
const { createContext } = await import("../src/ui/context.ts");
const { infoScreen } = await import("../src/ui/info.ts");
const i18n = await import("../src/i18n.ts");

const KEY = Uint8Array.from({ length: 32 }, (_, i) => (i * 37 + 11) & 0xff);
const hmac = (k: Uint8Array, m: Uint8Array) => new Uint8Array(createHmac("sha256", k).update(m).digest());
const le32 = (v: number) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
const u32 = (b: Uint8Array, o: number) => (b[o]! | (b[o + 1]! << 8) | (b[o + 2]! << 16) | (b[o + 3]! << 24)) >>> 0;
const keyIdRef = (k: Uint8Array) => u32(hmac(k, new TextEncoder().encode("C79 key id")), 0);

/** A .c79 file as tools/ota_pack.py builds it. */
function makeFile(payloadLen: number, key = KEY): Uint8Array {
  const payload = Uint8Array.from({ length: payloadLen }, (_, i) => (i * 13 + 7) & 0xff);
  payload.set([0x43, 0x37, 0x39, 0x45]);
  const hdr = new Uint8Array(64);
  hdr.set([0x43, 0x37, 0x39, 0x50, 1, 0, 64, 0, ...le32(payloadLen), ...le32(0x401c0000), ...le32(0), ...le32(crc32(payload)),
    ...le32(0x7085ad09), ...le32(1), ...le32(0x073b), ...le32(keyIdRef(key))]);
  const mac = hmac(key, Uint8Array.from([...hdr.slice(0, 0x30), ...payload])).slice(0, 16);
  hdr.set(mac, 0x30);
  return Uint8Array.from([...hdr, ...payload]);
}

/** Simulated resident: the ops, the tag check and the job states of cave/src/ota.c. */
function residentPiano(opts: { features1?: number; v2?: boolean; applyFails?: number; mangle?: (dataOp: number, m: Uint8Array) => Uint8Array } = {}) {
  const [app, piano] = LoopbackLink.pair();
  const s = {
    challenge: 0x12345678, job: 0, err: 0, active: 2, slot: 0, len: 0, crc: 0, seq: 0, rx: 0,
    staging: new Uint8Array(0x20000), slots: [{ seq: 0, flags: 0x40, tries: 0 }, { seq: 0, flags: 0x40, tries: 0 }],
    inFlight: 0, maxInFlight: 0, dataOps: 0, rejected: 0, polls: 0, frameMax: 0, begins: 0, commits: 0,
    apState: 0, apErr: 0, apTarget: 0, apCount: 0, apPolls: 0, applies: [] as number[], live: 0,
  };
  const reply = (op: number, raw: number[]) =>
    piano.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...pack7(raw), 0xf7]));
  const auth = (op: number, args: Uint8Array, tag: Uint8Array) => {
    const want = hmac(KEY, Uint8Array.from([0x43, 0x37, 0x39, 0x41, op, ...le32(s.challenge), ...args])).slice(0, 8);
    s.challenge = (s.challenge * 1103515245 + 12345) >>> 0;
    return want.every((b, i) => b === tag[i]);
  };
  piano.onMessage((m) => {
    if (m[1] !== 0x7d) return;
    if (m[6] === 0x12 && opts.mangle) m = opts.mangle(s.dataOps + 1, m);   // damage on the way (BLE, module, UART)
    s.frameMax = Math.max(s.frameMax, m.length);
    const op = m[6]!;
    const body = m.slice(7, m.length - 1);
    if (op === 0x01) piano.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, 0x41, 1, 2, 19, 0x3b, opts.features1 ?? 0x07, 0xf7]));
    if (op === 0x10) {
      if (s.job >= 2 && s.job < 8 && ++s.polls > 2) s.job = 8;
      if (s.apState >= 1 && s.apState <= 4 && ++s.apPolls > 2) {
        if (opts.applyFails) { s.apState = 6; s.apErr = opts.applyFails; }
        else { s.apState = 5; s.active = s.apTarget; s.live = 1; s.apCount++; }
      }
      const r = new Array(opts.v2 ? 72 : 48).fill(0);
      r.splice(0, 8, 1, s.job, s.err, 1, s.active, s.live ? 6 : 1, s.slot, 0);
      r.splice(8, 8, ...le32(s.rx), ...le32(s.len));
      r.splice(16, 8, ...le32(s.challenge), ...le32(keyIdRef(KEY)));
      s.slots.forEach((sl, i) => r.splice(24 + 6 * i, 6, ...le32(sl.seq), sl.flags, sl.tries));
      r[44] = 1;
      r[45] = 200;
      if (opts.v2) {
        r[0] = 2;
        r.splice(48, 6, 7, s.apState, s.apErr, s.apTarget, s.live, s.apCount);
        r.splice(54, 12, ...le32(0x55aaa55a), ...le32(0xffffffff), ...le32(0xffffffff));
        r.splice(66, 4, 0x40, 0, 0, 0);
      }
      reply(0x10, r);
    }
    if (op === 0x11) {
      const raw = unpack7(body);
      const ok = auth(op, raw.slice(0, 13), raw.slice(13));
      s.slot = raw[0]!; s.len = u32(raw, 1); s.crc = u32(raw, 5); s.seq = u32(raw, 9); s.rx = 0;
      const rc = !ok ? 2 : s.slot === s.active ? 4 : 0;
      if (rc === 0) { s.job = 1; s.err = 0; s.begins++; }
      reply(0x11, [rc, ...le32(s.challenge)]);
    }
    if (op === 0x12) {
      s.inFlight++;
      s.maxInFlight = Math.max(s.maxInFlight, s.inFlight);
      s.dataOps++;
      const off = (body[0]! << 14) | (body[1]! << 7) | body[2]!;
      const d = unpack7(body.slice(3));
      let rc = 0;
      if (off !== s.rx) rc = 7;
      else if (d.length > 200) rc = 1;
      else { s.staging.set(d, off); s.rx += d.length; }
      setTimeout(() => { s.inFlight--; reply(0x12, [rc, ...le32(s.rx)]); }, 1);
    }
    if (op === 0x13) {
      const raw = unpack7(body);
      const ok = auth(op, raw.slice(0, 1), raw.slice(1));
      const rc = !ok ? 2 : s.rx !== s.len ? 9 : 0;
      // the real resident checks the CRC in the job (J_VERIFY) and ends it with job 9 / err 10
      if (rc === 0 && crc32(s.staging.slice(0, s.len)) !== s.crc) { s.job = 9; s.err = 10; s.commits++; }
      else if (rc === 0) { s.job = 2; s.err = 0; s.polls = 0; s.commits++; s.slots[s.slot] = { seq: s.seq, flags: 0x41, tries: 0 }; }
      reply(0x13, [rc, ...le32(s.challenge)]);
    }
    if (op === 0x15 && opts.v2) {
      const raw = unpack7(body);
      const ok = auth(op, raw.slice(0, 1), raw.slice(1));
      let t = raw[0]!;
      if (t === 3) t = [0, 1].filter((i) => s.slots[i]!.flags & 1).sort((a, b) => s.slots[b]!.seq - s.slots[a]!.seq)[0] ?? 2;
      const rc = !ok ? 2 : t < 2 && !(s.slots[t]!.flags & 1) ? 4 : 0;
      if (rc === 0) { s.applies.push(t); s.apTarget = t; s.apState = 1; s.apErr = 0; s.apPolls = 0; }
      reply(0x15, [rc, ...le32(s.challenge)]);
    }
    if (op === 0x14) {
      const raw = unpack7(body);
      const ok = auth(op, raw.slice(0, 1), raw.slice(1));
      if (ok) s.rejected |= raw[0]!;
      reply(0x14, [ok ? 0 : 2, ...le32(s.challenge)]);
    }
  });
  return { session: new PianoSession(app, { handshake: false }), s };
}

beforeEach(() => {
  localStorage.clear();
  i18n.setLang("en");
});

test("sha256 / HMAC-SHA256 / key id / tag match node:crypto", () => {
  for (const n of [0, 1, 55, 56, 63, 64, 65, 200, 1000]) {
    const m = Uint8Array.from({ length: n }, (_, i) => (i * 7) & 0xff);
    assert.deepEqual(ota.sha256(m), new Uint8Array(createHash("sha256").update(m).digest()), `sha256 ${n}`);
    assert.deepEqual(ota.hmacSha256(KEY, m), hmac(KEY, m), `hmac ${n}`);
  }
  assert.equal(ota.keyId(KEY), keyIdRef(KEY));
  const tag = ota.authTag(KEY, 0x13, 0xdeadbeef, [1]);
  assert.deepEqual(tag, Array.from(hmac(KEY, Uint8Array.from([0x43, 0x37, 0x39, 0x41, 0x13, 0xef, 0xbe, 0xad, 0xde, 1])).slice(0, 8)));
});

test("parseKey / parseOtaFile", () => {
  assert.equal(ota.parseKey("00".repeat(31)), null);
  assert.deepEqual(ota.parseKey(Array.from(KEY, (b) => b.toString(16).padStart(2, "0")).join(" ")), KEY);
  const f = makeFile(1000);
  assert.deepEqual(ota.parseOtaFile(f), { length: 1064, payload: 1000, feat0: 0x3b, feat1: 0x07, keyId: keyIdRef(KEY),
    format: 1, type: 0, kind: "main", alg: 1, minApi: 0, version: 0, abi: 1 });
  assert.equal(ota.parseOtaFile(f.slice(0, 500)), null);
});

test("update: begin/data/commit with tags, strict stop-and-wait, frames <= 256 B, polled to done", async () => {
  const { session, s } = residentPiano();
  const file = makeFile(5000);
  const phases: string[] = [];
  const fin = await new ota.OtaClient(session).update(file, KEY, (p) => phases.push(p.phase), 5);
  assert.equal(fin.phase, "done", fin.message);
  assert.equal(s.slot, 0, "built-in running -> empty slot A");
  assert.equal(s.seq, 1);
  assert.deepEqual(s.staging.slice(0, file.length), file);
  assert.equal(s.dataOps, Math.ceil(file.length / ota.OTA_CHUNK));
  assert.equal(s.maxInFlight, 1, "never more than one data frame in flight");
  assert.ok(s.frameMax <= 256, `largest frame ${s.frameMax} B`);
  assert.ok(phases.includes("upload") && phases.includes("commit"));
});

test("update: a frame damaged on the way (byte / BLE packet lost, byte changed) is sent again from the start", async () => {
  // field 2026-11-16: ota_main 1ec47025 failed twice with "writing failed: CRC mismatch" after a full upload - the
  // resident stores a short frame as it comes and the old client followed its `received` off the frame grid
  const file = makeFile(20_000);
  const cases: [string, (k: number, m: Uint8Array) => Uint8Array, number][] = [
    ["byte lost", (k, m) => (k === 30 ? Uint8Array.from([...m.slice(0, 50), ...m.slice(51)]) : m), 1],
    ["BLE packet lost", (k, m) => (k === 30 ? Uint8Array.from([...m.slice(0, 40), ...m.slice(59)]) : m), 1],
    ["byte changed", (k, m) => (k === 30 ? Uint8Array.from(m, (b, i) => (i === 60 ? b ^ 1 : b)) : m), 1],
  ];
  for (const [name, mangle, restarts] of cases) {
    const { session, s } = residentPiano({ mangle });
    const msgs: string[] = [];
    const fin = await new ota.OtaClient(session).update(file, KEY, (p) => p.message && msgs.push(p.message), 1);
    assert.equal(fin.phase, "done", `${name}: ${fin.message}`);
    assert.deepEqual(s.staging.slice(0, file.length), file, name);
    assert.equal(s.begins, 1 + restarts, `${name}: begins`);
    assert.equal(fin.restarts, restarts, name);
    assert.ok(msgs.some((m) => /sending it again from the start/.test(m)), name);
  }
  // no resends allowed: the damaged frame is named at once (the old client went on to the CRC error); damage on every attempt gives up
  {
    const { session } = residentPiano({ mangle: cases[0]![1] });
    const fin = await new ota.OtaClient(session).update(file, KEY, () => {}, 1, 0);
    assert.equal(fin.phase, "error");
    assert.match(fin.message ?? "", /damaged frame: the piano holds 5999 bytes/);
  }
  {
    const { session, s } = residentPiano({ mangle: (k, m) => (k % 101 === 30 ? Uint8Array.from(m, (b, i) => (i === 60 ? b ^ 1 : b)) : m) });
    const fin = await new ota.OtaClient(session).update(file, KEY, () => {}, 1);
    assert.equal(fin.phase, "error");
    assert.match(fin.message ?? "", /writing failed: CRC mismatch \(after 2 resends\)/);
    assert.equal(s.commits, 3);
  }
});

test("update refuses a file signed with another key, and a piano with another key", async () => {
  const { session } = residentPiano();
  const other = Uint8Array.from({ length: 32 }, () => 7);
  const r1 = await new ota.OtaClient(session).update(makeFile(300, other), KEY);
  assert.match(r1.message ?? "", /different key/);
  const r2 = await new ota.OtaClient(session).update(makeFile(300, other), other);
  assert.match(r2.message ?? "", /piano has a different key/);
});

test("reject: authenticated, a wrong key is refused", async () => {
  const { session, s } = residentPiano();
  const c = new ota.OtaClient(session);
  assert.equal(await c.reject(Uint8Array.from({ length: 32 }, () => 1), 1), 2);
  assert.equal(s.rejected, 0);
  assert.equal(await c.reject(KEY, 2), 0);
  assert.equal(s.rejected, 2);
});

test("one firmware job per link: a second upload while one runs is refused before it sends anything (no second Begin)", async () => {
  // the Information screen is rebuilt on every tab switch / reconnect with a fresh, enabled upload button: a second
  // Begin would reset the piano's reception under the first upload
  const { session, s } = residentPiano();
  const file = makeFile(3000);
  const first = new ota.OtaClient(session).update(file, KEY, () => {}, 2);
  await tick(5);
  const second = await new ota.OtaClient(session).update(file, KEY, () => {}, 2);
  assert.equal(second.phase, "error");
  assert.equal(second.rc, 3, second.message);
  const fin = await first;
  assert.equal(fin.phase, "done", fin.message);
  assert.equal(s.begins, 1, "one Begin on the wire");
  assert.deepEqual(s.staging.slice(0, file.length), file);
  // released at the end: the next upload runs
  assert.equal((await new ota.OtaClient(session).update(makeFile(400), KEY, () => {}, 2)).phase, "done");
});

test("one firmware job per link: overlapping authenticated ops do not race for the challenge (second refused, rc 3)", async () => {
  // every authenticated op changes the challenge: two status + op pairs in flight made one fail as "authentication
  // failed (wrong key?)" and could fail a Commit after a whole upload
  const { session, s } = residentPiano();
  const [a, b] = await Promise.all([new ota.OtaClient(session).reject(KEY, 1), new ota.OtaClient(session).reject(KEY, 2)]);
  assert.deepEqual([a, b], [0, 3]);
  assert.equal(s.rejected, 1);
  // during an upload: a roll back is refused, the upload's Commit still authenticates
  const up = new ota.OtaClient(session).update(makeFile(2000), KEY, () => {}, 2);
  await tick(5);
  assert.equal(await new ota.OtaClient(session).reject(KEY, 2), 3);
  assert.equal((await up).phase, "done");
  assert.equal(s.rejected, 1);
});

test("Information screen: OTA section with the ota bit or when the resident loader answers; key import, upload, rollback", async () => {
  const off = residentPiano({ features1: 0x03 });
  let el = infoScreen(createContext(new ParamStore(), { session: () => off.session }));
  document.body.append(el);
  (el.querySelector("#ng-check") as HTMLButtonElement).click();
  await tick(60);
  // a payload without the "ota" bit (e.g. ota_hookdemo) still runs on the resident loader,
  // which answers OTA status - the section must stay reachable so the payload can be rolled back
  assert.equal((el.querySelector("#ng-ota") as HTMLElement).hidden, false);
  el.remove();

  const { session, s } = residentPiano();
  el = infoScreen(createContext(new ParamStore(), { session: () => session }));
  document.body.append(el);
  (el.querySelector("#ng-check") as HTMLButtonElement).click();
  await tick(80);
  assert.equal((el.querySelector("#ng-ota") as HTMLElement).hidden, false);
  assert.match(el.querySelector("#ota-state")!.textContent!, /built-in .* A: empty .* B: empty/);
  assert.equal(el.querySelector("#ota-key")!.textContent, "No key imported.");
  (el.querySelector("#ota-upload") as HTMLButtonElement).click();
  await tick(10);
  assert.equal(el.querySelector("#ota-status")!.textContent, "Import the key first.");
  (el.querySelector("#ota-key-text") as HTMLInputElement).value = Array.from(KEY, (b) => b.toString(16).padStart(2, "0")).join("");
  (el.querySelector("#ota-key-save") as HTMLButtonElement).click();
  assert.match(el.querySelector("#ota-key")!.textContent!, new RegExp(`Key ${keyIdRef(KEY).toString(16).padStart(8, "0")} imported`));
  (el.querySelector("#ng-ota") as HTMLElement & { setOtaFile: (d: Uint8Array) => void }).setOtaFile(makeFile(900));
  (el.querySelector("#ota-upload") as HTMLButtonElement).click();
  for (let i = 0; i < 100 && !/Written to/.test(el.querySelector("#ota-status")!.textContent!); i++) await tick(50);
  assert.match(el.querySelector("#ota-status")!.textContent!, /Written to slot A/);
  assert.equal((el.querySelector("#ota-progress") as HTMLProgressElement).value, 100);
  s.active = 0;                                      // as after the next power-on
  (el.querySelector("#ota-rollback") as HTMLButtonElement).click();
  await tick(60);
  assert.equal(s.rejected, 1);
  assert.match(el.querySelector("#ota-status")!.textContent!, /Slot rejected/);
  el.remove();
});

test("status layout 1 (the flashed ota_loader) vs layout 2 (ota_loader_v2)", async () => {
  const one = await new ota.OtaClient(residentPiano().session).status();
  assert.equal(one?.layout, 1);
  assert.equal(one?.caps, 0);
  assert.equal(one?.diag, null);
  const two = await new ota.OtaClient(residentPiano({ v2: true }).session).status();
  assert.equal(two?.layout, 2);
  assert.equal(two?.caps & ota.OTA_CAP.applyNow, 1);
  assert.deepEqual(two?.diag, { mbox0: 0x55aaa55a, mbox1: 0xffffffff, copy: 0xffffffff, esc: 0x40, keysDown: 0, keysLow: 0, escWait: 0 });
});

test("apply: authenticated op 0x15, polled to done; refused on the old loader; errors surface", async () => {
  const old = residentPiano();
  const r0 = await new ota.OtaClient(old.session).apply(KEY, ota.ApplyTarget.Newest, () => {}, 5);
  assert.equal(r0.ok, false);
  assert.match(r0.message, /cannot apply live/);
  const { session, s } = residentPiano({ v2: true });
  s.slots[0] = { seq: 1, flags: 0x41, tries: 0 };
  s.slots[1] = { seq: 2, flags: 0x41, tries: 0 };
  const c = new ota.OtaClient(session);
  session.state = "ready"; // as after a connect: the swap is reported to the connection manager (field 2026-11-09)
  const why: string[] = [];
  session.onRebind((w) => why.push(w));
  assert.equal((await c.apply(Uint8Array.from({ length: 32 }, () => 3), 0, () => {}, 5)).ok, false, "wrong key");
  assert.deepEqual(why, [], "refused: nothing swapped");
  assert.deepEqual(s.applies, []);
  const r = await c.apply(KEY, ota.ApplyTarget.Newest, () => {}, 5);
  assert.equal(r.ok, true, r.message);
  assert.deepEqual(s.applies, [1], "newest = B (seq 2)");
  assert.equal(r.status?.active, 1);
  assert.equal(r.status?.live, true);
  assert.deepEqual(why, ["apply"], "apply done: the new payload's core has an empty binding table - CONNECT again");
  const bad = residentPiano({ v2: true, applyFails: 12 });
  bad.s.slots[0] = { seq: 1, flags: 0x41, tries: 0 };
  const r2 = await new ota.OtaClient(bad.session).apply(KEY, 0, () => {}, 5);
  assert.equal(r2.ok, false);
  assert.match(r2.message, /signature/);
});

test("Information screen: Apply now only on ota_loader_v2; upload can apply right after writing", async () => {
  const old = residentPiano();
  let el = infoScreen(createContext(new ParamStore(), { session: () => old.session }));
  document.body.append(el);
  (el.querySelector("#ng-check") as HTMLButtonElement).click();
  await tick(80);
  assert.equal((el.querySelector("#ota-apply") as HTMLButtonElement).hidden, true, "layout 1: no apply button");
  assert.equal((el.querySelector("#ota-diag") as HTMLElement).hidden, true);
  el.remove();

  const { session, s } = residentPiano({ v2: true });
  localStorage.setItem(ota.OTA_KEY_STORAGE, Array.from(KEY, (b) => b.toString(16).padStart(2, "0")).join(""));
  el = infoScreen(createContext(new ParamStore(), { session: () => session }));
  document.body.append(el);
  (el.querySelector("#ng-check") as HTMLButtonElement).click();
  await tick(80);
  assert.equal((el.querySelector("#ota-apply") as HTMLButtonElement).hidden, false);
  assert.match(el.querySelector("#ota-diag")!.textContent!, /mailbox 55aaa55a ffffffff/);
  (el.querySelector("#ng-ota") as HTMLElement & { setOtaFile: (d: Uint8Array) => void }).setOtaFile(makeFile(700));
  (el.querySelector("#ota-apply-after") as HTMLInputElement).checked = true;
  (el.querySelector("#ota-upload") as HTMLButtonElement).click();
  for (let i = 0; i < 200 && !/Now running|failed/.test(el.querySelector("#ota-status")!.textContent!); i++) await tick(50);
  assert.match(el.querySelector("#ota-status")!.textContent!, /Now running: slot A/);
  assert.deepEqual(s.applies, [0], "written to A, then applied (newest = A)");
  assert.match(el.querySelector("#ota-state")!.textContent!, /slot A — applied live/);
  s.slots[1] = { seq: 5, flags: 0x41, tries: 0 };
  (el.querySelector("#ota-apply") as HTMLButtonElement).click();
  for (let i = 0; i < 200 && s.applies.length < 2; i++) await tick(20);
  for (let i = 0; i < 100 && !/slot B\./.test(el.querySelector("#ota-status")!.textContent!); i++) await tick(50);
  assert.deepEqual(s.applies, [0, 1]);
  assert.match(el.querySelector("#ota-status")!.textContent!, /Now running: slot B/);
  el.remove();
});

/** The link goes quiet for status reads once `op` was sent: every status read then takes its whole timeout (~1 s). */
function quietStatusAfter(session: InstanceType<typeof PianoSession>, op: number): void {
  const real = session.sendRaw.bind(session);
  let quiet = false;
  session.sendRaw = (m: Uint8Array) => {
    if (m[1] === 0x7d && m[6] === 0x10 && quiet) return;
    if (m[1] === 0x7d && m[6] === op) quiet = true;
    real(m);
  };
}

test("apply waits by time, not by attempts: on a quiet link (a status read takes ~1 s) the timeout still bounds the wait", async () => {
  const { session, s } = residentPiano({ v2: true });
  s.slots[0] = { seq: 1, flags: 0x41, tries: 0 };
  quietStatusAfter(session, 0x15);
  const t0 = Date.now();
  const r = await new ota.OtaClient(session).apply(KEY, ota.ApplyTarget.Newest, () => {}, 50, 300);
  const applyMs = Date.now() - t0;
  assert.equal(r.ok, false);
  assert.match(r.message, /in time/);
  assert.ok(applyMs < 2000, `apply waited ${applyMs} ms for a 300 ms timeout`);
});

test("the commit wait is by time, not by attempts: on a quiet link (a status read takes ~1 s) the timeout still bounds it", async () => {
  const c = residentPiano();
  quietStatusAfter(c.session, 0x13);
  const t0 = Date.now();
  const fin = await Promise.race([
    new ota.OtaClient(c.session).update(makeFile(400), KEY, () => {}, 50, 2, { commitTimeoutMs: 300 }),
    tick(4000).then(() => null),
  ]);
  const commitMs = Date.now() - t0;
  assert.ok(fin, `the commit wait did not end within 4 s for a 300 ms timeout`);
  assert.equal(fin.phase, "error");
  assert.match(fin.message!, /did not finish writing/);
  assert.ok(commitMs < 2500, `the commit wait took ${commitMs} ms for a 300 ms timeout`);
});

test("Information screen rebuilt during an upload (tab switch, language change, Reconnect on the same session): it follows the running upload and the apply after it, buttons off, then shows the result", async () => {
  const { session, s } = residentPiano({ v2: true });
  localStorage.setItem(ota.OTA_KEY_STORAGE, Array.from(KEY, (b) => b.toString(16).padStart(2, "0")).join(""));
  const ctx = createContext(new ParamStore(), { session: () => session });
  const a = infoScreen(ctx);
  document.body.append(a);
  (a.querySelector("#ng-check") as HTMLButtonElement).click();
  await tick(80);
  const file = makeFile(12000);
  (a.querySelector("#ng-ota") as HTMLElement & { setOtaFile: (d: Uint8Array) => void }).setOtaFile(file);
  (a.querySelector("#ota-apply-after") as HTMLInputElement).checked = true;
  (a.querySelector("#ota-upload") as HTMLButtonElement).click();
  for (let i = 0; i < 200 && !/Uploading/.test(a.querySelector("#ota-status")!.textContent!); i++) await tick(2);
  a.remove();
  const b = infoScreen(ctx);                         // main.ts show() builds a new screen
  document.body.append(b);
  const q = <T extends HTMLElement = HTMLButtonElement>(sel: string) => b.querySelector(sel) as T;
  const buttons = ["#ota-upload", "#ota-rollback", "#ota-apply", "#ota-loader-rollback", "#ota-reselect", "#ota-loader-confirm"];
  assert.equal(q("#ng-ota").hidden, false, "the firmware card is shown for the running job");
  assert.match(q("#ota-status").textContent!, new RegExp(`Uploading… \\d+/${file.length} bytes`));
  assert.equal(q("#ota-progress").hidden, false);
  for (const sel of buttons) assert.equal(q(sel).disabled, true, `${sel} off during the upload`);
  for (let i = 0; i < 300 && !/Now running|failed|Rejected/.test(q("#ota-status").textContent!); i++) await tick(20);
  assert.match(q("#ota-status").textContent!, /Now running: slot A/, "the apply after the upload shows on the rebuilt screen too");
  await tick(80);
  assert.equal(s.begins, 1);
  assert.deepEqual(s.applies, [0]);
  for (const sel of buttons) assert.equal(q(sel).disabled, false, `${sel} on again after the job`);
  assert.match(q("#ota-status").textContent!, /Now running: slot A/);
  b.remove();
});
