// OTA loader v4 (REMOTE_PROTOCOL.md section 10.12, cave/include/s0.h): status layout 3, slot info (op 0x0E), loader
// confirm (op 0x0F), loader targets of Begin / Commit / Reject, apply targets 2 and 4, rc 22-25, the upload plan
// (image type from the .c79 header, the slot and seq chosen by the app) and the firmware card's loader parts.
// Synthetic data only: buffers are built from the spec's byte offsets, the key is a counting pattern.
import { tick } from "./_dom.ts";
import { beforeEach, test } from "node:test";
import assert from "node:assert/strict";
import { createHmac } from "node:crypto";

const ota = await import("../src/protocol/ota.ts");
const L4 = await import("../src/ui/otaloader.ts");
const { pack7, unpack7, crc32 } = await import("../src/protocol/ngext.ts");
const { PianoSession } = await import("../src/protocol/session.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const { ParamStore } = await import("../src/state/store.ts");
const { createContext } = await import("../src/ui/context.ts");
const { infoScreen } = await import("../src/ui/info.ts");
const i18n = await import("../src/i18n.ts");

const KEY = Uint8Array.from({ length: 32 }, (_, i) => (i * 37 + 11) & 0xff);
const OTHER = Uint8Array.from({ length: 32 }, (_, i) => (i * 5 + 1) & 0xff);
const hmac = (k: Uint8Array, m: Uint8Array) => new Uint8Array(createHmac("sha256", k).update(m).digest());
const le32 = (v: number) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
const le16 = (v: number) => [v & 0xff, (v >>> 8) & 0xff];
const u32 = (b: Uint8Array, o: number) => (b[o]! | (b[o + 1]! << 8) | (b[o + 2]! << 16) | (b[o + 3]! << 24)) >>> 0;
const keyIdRef = (k: Uint8Array) => u32(hmac(k, new TextEncoder().encode("C79 key id")), 0);
const hexKey = (k: Uint8Array) => Array.from(k, (b) => b.toString(16).padStart(2, "0")).join("");

interface FileOpts { format?: number; type?: number; alg?: number; minApi?: number; version?: number; abi?: number; key?: Uint8Array; load?: number }

/** A .c79 as tools/ota_pack.py builds it: format 1 (main) or format 2 (0x28 type, 0x29 alg, 0x2A min API, 0x2C version). */
function makeFile(payloadLen: number, o: FileOpts = {}): Uint8Array {
  const key = o.key ?? KEY;
  const payload = Uint8Array.from({ length: payloadLen }, (_, i) => (i * 13 + 7) & 0xff);
  payload.set(o.type === 2 ? [0x43, 0x37, 0x39, 0x4c] : [0x43, 0x37, 0x39, 0x45]);
  const hdr = new Uint8Array(64);
  const loader = o.type === 2;
  hdr.set([0x43, 0x37, 0x39, 0x50, ...le16(o.format ?? 1), 64, 0, ...le32(payloadLen), ...le32(o.load ?? (loader ? 0x40176000 : 0x401c0000)),
    ...le32(0), ...le32(crc32(payload)), ...le32(0x7085ad09), ...le32(o.abi ?? 1), ...le32(loader ? 0 : 0x073b), ...le32(keyIdRef(key))]);
  if ((o.format ?? 1) >= 2) hdr.set([o.type ?? 1, o.alg ?? 1, o.minApi ?? 0, 0, ...le32(o.version ?? 0)], 0x28);
  hdr.set(hmac(key, Uint8Array.from([...hdr.slice(0, 0x30), ...payload])).slice(0, 16), 0x30);
  return Uint8Array.from([...hdr, ...payload]);
}
const loaderFile = (version = 7, o: FileOpts = {}) => makeFile(3000, { format: 2, type: 2, version, ...o });

interface Slot { seq: number; flags: number; tries: number }
const EMPTY = (): Slot => ({ seq: 0xffffffff, flags: 0x40, tries: 0 });

interface LoaderSt {
  ldr: [Slot, Slot]; running: number; s0Reason: number; s0Flags: number; caps2: number; api: number; version: number;
  rootKeyId: number; reselects: number; abiMin: number; abiMax: number;
}

/** Status layout 3 (112 raw bytes) from the section 10.12 table. */
function layout3(base: { job: number; err: number; active: number; reason: number; jobTarget: number; rej: number; rx: number; len: number;
  challenge: number; keyId: number; main: [Slot, Slot]; apState: number; apErr: number; apTarget: number; maxSeq: number }, l: LoaderSt, extra = 0): number[] {
  const r = new Array(112 + extra).fill(0);
  r.splice(0, 8, 3, base.job, base.err, 1, base.active, base.reason, base.jobTarget, base.rej);
  r.splice(8, 16, ...le32(base.rx), ...le32(base.len), ...le32(base.challenge), ...le32(base.keyId));
  base.main.forEach((sl, i) => r.splice(24 + 6 * i, 6, ...le32(sl.seq), sl.flags, sl.tries));
  r.splice(36, 4, 2, 0, 5, 0);
  r.splice(40, 4, ...le32(0x1fe00));
  r[44] = 1; r[45] = 200;
  r.splice(46, 2, ...le16(base.maxSeq & 0xffff));
  r.splice(48, 6, 0x1f, base.apState, base.apErr, base.apTarget, 0, 0);
  r.splice(54, 12, ...le32(0xffffffff), ...le32(0xffffffff), ...le32(0xffffffff));
  r[66] = 0x40;
  l.ldr.forEach((sl, i) => r.splice(72 + 6 * i, 6, ...le32(sl.seq), sl.flags, sl.tries));
  r.splice(84, 6, l.running, l.s0Reason, l.s0Flags, 1, l.api, l.caps2);
  r.splice(90, 12, ...le32(l.version), ...le32(l.rootKeyId), ...le32(base.keyId));
  r.splice(102, 2, l.abiMin, l.abiMax);
  r.splice(104, 4, ...le32(0xa040));
  r.splice(108, 2, ...le16(Math.max(0, ...l.ldr.filter((s) => s.flags & 1).map((s) => s.seq)) & 0xffff));
  r[110] = l.reselects;
  for (let i = 112; i < r.length; i++) r[i] = 0x5a;          // a later stage-1's appended bytes: ignored
  return r;
}

/** Simulated ota_loader_v4 stage-1 (cave/src/s1.c): the ops and interlocks of section 10.12. */
function v4Piano(opts: { safe?: boolean; ldrRunningConfirmed?: boolean; otherLoader?: Slot; layout2?: boolean; beginRc?: number } = {}) {
  const [app, piano] = LoopbackLink.pair();
  const s = {
    challenge: 0x2468ace0, job: 0, err: 0, active: 0, reason: 1, jobSlot: -1, len: 0, crc: 0, seq: 0, rx: 0,
    staging: new Uint8Array(0x20000),
    main: [{ seq: 1, flags: 0x4b, tries: 1 }, EMPTY()] as [Slot, Slot],
    ldr: [{ seq: 1, flags: 0x48 | 0x01 | (opts.ldrRunningConfirmed === false ? 0 : 0x02), tries: opts.ldrRunningConfirmed === false ? 1 : 0 }, opts.otherLoader ?? EMPTY()] as [Slot, Slot],
    running: 0, s0Flags: opts.safe ? 1 : 0, s0Reason: opts.safe ? 6 : 1, reselects: 0,
    apState: 0, apErr: 0, apTarget: 0, apPolls: 0, polls: 0, confReq: false,
    sent: [] as number[][], rejected: 0, confirms: [] as number[], applies: [] as number[], rcs: [] as [number, number][],
    headers: new Map<number, Uint8Array>(),
  };
  const reply = (op: number, raw: number[]) => piano.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...pack7(raw), 0xf7]));
  const rcReply = (op: number, rc: number) => { s.rcs.push([op, rc]); reply(op, [rc, ...le32(s.challenge)]); };
  const auth = (op: number, args: Uint8Array, tag: Uint8Array) => {
    const want = hmac(KEY, Uint8Array.from([0x43, 0x37, 0x39, 0x41, op, ...le32(s.challenge), ...args])).slice(0, 8);
    s.challenge = (s.challenge * 1103515245 + 12345) >>> 0;
    return want.every((b, i) => b === tag[i]);
  };
  const idx = (t: number) => (t <= 1 ? t : t === 0x10 || t === 0x11 ? t - 0x10 + 2 : -1);
  const slotOf = (i: number) => (i < 2 ? s.main[i]! : s.ldr[i - 2]!);
  const newestLdr = () => Math.max(0, ...s.ldr.filter((x) => x.flags & 1).map((x) => x.seq));
  const ldrOk = (x: Slot) => (x.flags & 0x07) === 0x03;
  /** stage-0's choice at a (re)select: newest bootable loader; an unconfirmed one spends a try */
  const select = () => {
    const cand = [0, 1].filter((i) => { const x = s.ldr[i]!; return (x.flags & 0x05) === 0x01 && (x.flags & 2 || x.tries < 3); })
      .sort((a, b) => s.ldr[b]!.seq - s.ldr[a]!.seq);
    s.ldr.forEach((x) => (x.flags &= ~0x08));
    const i = cand[0] ?? s.running;
    if (!(s.ldr[i]!.flags & 2)) s.ldr[i]!.tries++;
    s.ldr[i]!.flags |= 0x08;
    s.running = i;
  };
  piano.onMessage((m) => {
    if (m[1] !== 0x7d) return;
    s.sent.push([...m]);
    const op = m[6]!;
    const body = m.slice(7, m.length - 1);
    if (op === 0x01) piano.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, 0x41, 1, 2, 19, 0x3b, 0x07, 0xf7]));
    if (op === 0x10) {
      if (s.job >= 2 && s.job < 8 && ++s.polls > 2) {
        // J_VERIFY: the same header rules as stage-0 / main_hdr - type must fit the target, a main its API / ABI
        const h = s.staging;
        const fmt = h[4]! | (h[5]! << 8);
        const ldr = s.jobSlot >= 2;
        const type = fmt >= 2 ? h[0x28]! : 0;
        if (ldr !== (type === 2) || (ldr && fmt < 2)) { s.job = 9; s.err = 11; }
        else if (!ldr && fmt >= 2 && h[0x2a]! > 1) { s.job = 9; s.err = 25; }
        else {
          s.job = 8;
          const sl = slotOf(s.jobSlot);
          Object.assign(sl, { seq: s.seq, flags: 0x41, tries: 0 });
          s.headers.set(s.jobSlot, s.staging.slice(0, 48));
        }
      }
      if (s.confReq) { s.ldr[s.running]!.flags |= 0x02; s.confReq = false; }
      if (s.apState >= 1 && s.apState <= 4 && ++s.apPolls > 2) {
        if (s.apTarget === 4) {
          s.apState = 5;                                    // the old stage-1's last answer
        } else { s.apState = 5; s.active = s.apTarget === 3 ? 0 : s.apTarget; }
      } else if (s.apState === 5 && s.apTarget === 4) {
        // stage-0 dropped the old stage-1 and selected again: a fresh stage-1 answers
        select();
        s.reselects++;
        s.s0Flags |= 0x04;
        s.s0Reason = 7;
        s.apState = 0;
      }
      const base = { job: s.job, err: s.err, active: s.active, reason: s.reason, jobTarget: s.jobSlot >= 2 ? 0x10 + s.jobSlot - 2 : Math.max(0, s.jobSlot),
        rej: s.rejected, rx: s.rx, len: s.len, challenge: s.challenge, keyId: keyIdRef(KEY), main: s.main, apState: s.apState, apErr: s.apErr,
        apTarget: s.apTarget, maxSeq: Math.max(0, ...s.main.filter((x) => x.flags & 1).map((x) => x.seq)) };
      const r = layout3(base, { ldr: s.ldr, running: s.running, s0Reason: s0r(), s0Flags: s.s0Flags, caps2: 0x1f, api: 1, version: 12,
        rootKeyId: keyIdRef(KEY), reselects: s.reselects, abiMin: 1, abiMax: 1 }, 8);
      if (opts.layout2) { r[0] = 2; r.length = 72; }
      reply(0x10, r);
    }
    if (op === 0x0e) {
      const i = idx(body[0]!);
      if (i < 0) return;                                    // unknown target: silent
      const sl = slotOf(i);
      const hdr = s.headers.get(i) ?? new Uint8Array(48);
      reply(0x0e, [body[0]!, sl.flags, ...le32(sl.seq), sl.tries, 1, ...hdr]);
    }
    if (op === 0x11) {
      const raw = unpack7(body);
      const ok = auth(op, raw.slice(0, 13), raw.slice(13));
      const i = idx(raw[0]!);
      const len = u32(raw, 1), seq = u32(raw, 9);
      if (!ok) return rcReply(op, 2);
      if (opts.beginRc) return rcReply(op, opts.beginRc);
      if (i < 0 || (i < 2 && i === s.active) || (i >= 2 && i - 2 === s.running)) return rcReply(op, 4);
      if (i >= 2 && !(s.ldr[s.running]!.flags & 2)) return rcReply(op, 22);
      if (len > (i >= 2 ? 0xa040 : 0x1fe00)) return rcReply(op, 5);
      if (seq <= (i >= 2 ? newestLdr() : Math.max(0, ...s.main.filter((x) => x.flags & 1).map((x) => x.seq)))) return rcReply(op, 6);
      Object.assign(s, { jobSlot: i, len, crc: u32(raw, 5), seq, rx: 0, job: 1, err: 0 });
      return rcReply(op, 0);
    }
    if (op === 0x12) {
      const off = (body[0]! << 14) | (body[1]! << 7) | body[2]!;
      const d = unpack7(body.slice(3));
      let rc = 0;
      if (off !== s.rx) rc = 7;
      else { s.staging.set(d, off); s.rx += d.length; }
      setTimeout(() => reply(0x12, [rc, ...le32(s.rx)]), 1);
    }
    if (op === 0x13) {
      const raw = unpack7(body);
      if (!auth(op, raw.slice(0, 1), raw.slice(1))) return rcReply(op, 2);
      if (s.job !== 1 || idx(raw[0]!) !== s.jobSlot) return rcReply(op, 8);
      if (s.rx !== s.len) return rcReply(op, 9);
      s.job = 2; s.polls = 0;
      return rcReply(op, 0);
    }
    if (op === 0x14) {
      const raw = unpack7(body);
      if (!auth(op, raw.slice(0, 1), raw.slice(1))) return rcReply(op, 2);
      const mask = raw[0]!;
      if (!mask || mask & ~0x33) return rcReply(op, 1);
      if (mask & 0x30 && ![0, 1].some((i) => ldrOk(s.ldr[i]!) && !(mask & (0x10 << i)))) return rcReply(op, 23);
      s.rejected |= mask;
      [0, 1].forEach((i) => { if (mask & (1 << i)) s.main[i]!.flags |= 4; if (mask & (0x10 << i)) s.ldr[i]!.flags |= 4; });
      return rcReply(op, 0);
    }
    if (op === 0x0f) {
      const raw = unpack7(body);
      if (!auth(op, raw.slice(0, 1), raw.slice(1))) return rcReply(op, 2);
      s.confirms.push(raw[0]!);
      if (raw[0] !== 0x10 + s.running) return rcReply(op, 4);
      if (!(s.ldr[s.running]!.flags & 2)) s.confReq = true;
      return rcReply(op, 0);
    }
    if (op === 0x15) {
      const raw = unpack7(body);
      if (!auth(op, raw.slice(0, 1), raw.slice(1))) return rcReply(op, 2);
      const t = raw[0]!;
      s.applies.push(t);
      if (t > 4) return rcReply(op, 1);
      if (s.s0Flags & 1 && (t <= 1 || t === 3)) return rcReply(op, 24);
      Object.assign(s, { apTarget: t, apState: 1, apErr: 0, apPolls: 0 });
      return rcReply(op, 0);
    }
  });
  function s0r() { return s.s0Reason; }
  return { session: new PianoSession(app, { handshake: false }), s, select };
}

beforeEach(() => {
  localStorage.clear();
  i18n.setLang("en");
});

const BASE = { job: 0, err: 0, active: 2, reason: 7, jobTarget: 0x11, rej: 0x20, rx: 0, len: 0, challenge: 0x01020304, keyId: 0x0badcafe,
  main: [{ seq: 4, flags: 0x41, tries: 0 }, { seq: 5, flags: 0x43, tries: 1 }] as [Slot, Slot], apState: 0, apErr: 0, apTarget: 0, maxSeq: 5 };
const LDR: LoaderSt = { ldr: [{ seq: 2, flags: 0x4b, tries: 0 }, { seq: 3, flags: 0x61, tries: 3 }], running: 0, s0Reason: 6, s0Flags: 0x05,
  caps2: 0x1f, api: 1, version: 0x01020304, rootKeyId: 0x11223344, reselects: 9, abiMin: 1, abiMax: 1 };

test("layout 3: every appended field from section 10.12; bytes 0..71 as layout 2; later bytes ignored", () => {
  const st = ota.decodeOtaStatus(Uint8Array.from(pack7(layout3(BASE, LDR, 16))))!;
  assert.equal(st.layout, 3);
  assert.equal(st.active, 2);
  assert.equal(ota.activeName(st), "no main", "value 2 on v4 = no main");
  assert.equal(st.reason, 7);
  assert.equal(ota.OTA_REASON[st.reason], "SAFE mode (no main)");
  assert.equal(st.jobSlot, 0x11, "job target = the target byte");
  assert.equal(st.rejectPending, 0x20);
  assert.equal(st.keyId, 0x0badcafe);
  assert.deepEqual(st.slots, BASE.main, "24..35 = the main slots");
  assert.equal(st.caps, 0x1f);
  assert.notEqual(st.diag, null);
  assert.deepEqual(st.loader, {
    slots: LDR.ldr, running: 0, s0Reason: 6, s0Flags: 0x05, s0Version: 1, api: 1, caps2: 0x1f, version: 0x01020304,
    rootKeyId: 0x11223344, workKeyId: 0x0badcafe, mainAbiMin: 1, mainAbiMax: 1, imageMax: 0xa040, maxSeq16: 3, reselects: 9, safe: true,
  });
  // layout byte 3 but cut short: the loader part is not trusted, the rest still reads
  const short = ota.decodeOtaStatus(Uint8Array.from(pack7(layout3(BASE, LDR).slice(0, 100))))!;
  assert.equal(short.loader, null);
  assert.equal(short.caps, 0x1f);
  // layouts 1 / 2 never show loader parts, and value 2 stays "built-in"
  const two = ota.decodeOtaStatus(Uint8Array.from(pack7([2, ...layout3(BASE, LDR).slice(1, 72)])))!;
  assert.equal(two.loader, null);
  assert.equal(ota.activeName(two), "built-in");
  const one = ota.decodeOtaStatus(Uint8Array.from(pack7([1, ...layout3(BASE, LDR).slice(1, 48)])))!;
  assert.equal(one.loader, null);
  assert.equal(one.diag, null);
});

test("slot info (op 0x0E): flags, seq, tries, state version and the header fields", () => {
  const f = loaderFile(42);
  const raw = [0x11, 0x41, ...le32(6), 2, 1, ...f.slice(0, 48)];
  const inf = ota.decodeSlotInfo(Uint8Array.from(pack7(raw)))!;
  assert.deepEqual(inf, { target: 0x11, flags: 0x41, seq: 6, tries: 2, stateVersion: 1,
    image: { format: 2, type: 2, payload: 3000, keyId: keyIdRef(KEY), version: 42, minApi: 0, abi: 1 } });
  const empty = ota.decodeSlotInfo(Uint8Array.from(pack7([0x01, 0x40, ...le32(0xffffffff), 0, 0, ...new Array(48).fill(0)])))!;
  assert.equal(empty.image, null);
  assert.equal(ota.decodeSlotInfo(Uint8Array.from(pack7([0x10, 1, 2]))), null);
});

test(".c79 header: format 1 = main, format 2 type byte 0x28 = main / loader / unknown", () => {
  const m1 = ota.parseOtaFile(makeFile(500))!;
  assert.equal(m1.format, 1);
  assert.equal(m1.kind, "main");
  assert.equal(m1.alg, 1, "format 1 reads HMAC");
  const m2 = ota.parseOtaFile(makeFile(500, { format: 2, type: 1, minApi: 1, version: 9 }))!;
  assert.deepEqual([m2.kind, m2.type, m2.minApi, m2.version], ["main", 1, 1, 9]);
  const ld = ota.parseOtaFile(loaderFile(5))!;
  assert.deepEqual([ld.kind, ld.type, ld.version, ld.alg], ["loader", 2, 5, 1]);
  assert.equal(ota.parseOtaFile(makeFile(500, { format: 2, type: 9 }))!.kind, null);
  assert.equal(ota.parseOtaFile(makeFile(500, { format: 0 })), null);
});

/** A decoded layout-3 status with the given loader slots (synthetic). */
function v4Status(over: Partial<LoaderSt> = {}, base: Partial<typeof BASE> = {}) {
  return ota.decodeOtaStatus(Uint8Array.from(pack7(layout3({ ...BASE, active: 0, keyId: keyIdRef(KEY), ...base },
    { ...LDR, s0Flags: 0, rootKeyId: keyIdRef(KEY), ...over }))))!;
}

test("upload plan: the slot and seq of each image type, and the interlocks the firmware would refuse", () => {
  const ld = ota.parseOtaFile(loaderFile())!;
  const main = ota.parseOtaFile(makeFile(800))!;
  // loader A runs (confirmed), B holds an older given-up loader: B, seq = newest loader + 1
  assert.deepEqual(ota.planUpload(ld, v4Status()), { ok: true, kind: "loader", slot: 1, target: 0x11, seq: 4 });
  // loader B runs: A
  const runB = v4Status({ ldr: [{ seq: 2, flags: 0x43, tries: 0 }, { seq: 3, flags: 0x4b, tries: 0 }], running: 1 });
  assert.deepEqual(ota.planUpload(ld, runB), { ok: true, kind: "loader", slot: 0, target: 0x10, seq: 4 });
  // mains: as before (main A runs -> B; seq over the mains only, loaders count separately)
  assert.deepEqual(ota.planUpload(main, v4Status()), { ok: true, kind: "main", slot: 1, target: 1, seq: 6 });
  // rc 22: the running loader is unconfirmed
  const unconf = v4Status({ ldr: [{ seq: 2, flags: 0x49, tries: 1 }, EMPTY()] });
  assert.equal((ota.planUpload(ld, unconf) as { rc?: number }).rc, 22);
  assert.equal(ota.planUpload(main, unconf).ok, true, "a main upload is not affected");
  // rc 25: a format-2 main that needs a newer stage-1 API, or another ABI
  assert.equal((ota.planUpload(ota.parseOtaFile(makeFile(800, { format: 2, type: 1, minApi: 2 }))!, v4Status()) as { rc?: number }).rc, 25);
  assert.equal((ota.planUpload(ota.parseOtaFile(makeFile(800, { abi: 2 }))!, v4Status()) as { rc?: number }).rc, 25);
  assert.equal(ota.planUpload(ota.parseOtaFile(makeFile(800, { format: 2, type: 1, minApi: 1 }))!, v4Status()).ok, true);
  // app-only refusals
  const refusal = (p: ReturnType<typeof ota.planUpload>) => (p as { refusal?: string }).refusal;
  assert.equal(refusal(ota.planUpload(ota.parseOtaFile(loaderFile(1, { alg: 2 }))!, v4Status())), "signature");
  assert.equal(refusal(ota.planUpload(ota.parseOtaFile(makeFile(800, { format: 2, type: 7 }))!, v4Status())), "unknownType");
  assert.equal(refusal(ota.planUpload(ld, v4Status({ rootKeyId: 0x01010101 }))), "loaderKey");
  assert.equal(refusal(ota.planUpload(ota.parseOtaFile(makeFile(0xa100, { format: 2, type: 2 }))!, v4Status())), "tooLarge");
  // an older loader (layout 2): no loader uploads, no format-2 mains; a format-1 main exactly as before
  const v3 = ota.decodeOtaStatus(Uint8Array.from(pack7([2, ...layout3({ ...BASE, active: 1, keyId: keyIdRef(KEY) }, LDR).slice(1, 72)])))!;
  assert.equal(refusal(ota.planUpload(ld, v3)), "noLoaderOta");
  assert.equal(refusal(ota.planUpload(ota.parseOtaFile(makeFile(800, { format: 2, type: 1 }))!, v3)), "needV4");
  assert.deepEqual(ota.planUpload(main, v3), { ok: true, kind: "main", slot: 0, target: 0, seq: 6 });
});

test("loader rollback mask: bit 4 / 5 of the running loader, refused (rc 23) without another confirmed loader", () => {
  const both = v4Status({ ldr: [{ seq: 2, flags: 0x43, tries: 0 }, { seq: 3, flags: 0x4b, tries: 0 }], running: 1 });
  assert.deepEqual(ota.loaderRollbackMask(both), { ok: true, mask: 0x20 });
  const bothA = v4Status({ ldr: [{ seq: 2, flags: 0x4b, tries: 0 }, { seq: 3, flags: 0x43, tries: 0 }], running: 0 });
  assert.deepEqual(ota.loaderRollbackMask(bothA), { ok: true, mask: 0x10 });
  assert.deepEqual(ota.loaderRollbackMask(v4Status()), { ok: false, rc: 23 }, "B is not confirmed");
  const rejected = v4Status({ ldr: [{ seq: 2, flags: 0x4b, tries: 0 }, { seq: 3, flags: 0x47, tries: 0 }] });
  assert.deepEqual(ota.loaderRollbackMask(rejected), { ok: false, rc: 23 }, "B is rejected");
});

test("op encoders: slot info, loader confirm (tag over the target byte), reject loader bits, apply 2 / 4", async () => {
  const { session, s } = v4Piano({ otherLoader: { seq: 3, flags: 0x43, tries: 0 } });
  const c = new ota.OtaClient(session);
  await c.info(0x11, 200);
  assert.deepEqual(s.sent.at(-1), [0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, 0x0e, 0x11, 0xf7]);
  assert.equal(await c.info(0x05, 50), null, "unknown target: no answer");
  // confirm: 0F <pack7: target · HMAC("C79A" · 0F · challenge · target)[:8]>
  s.ldr[0]!.flags &= ~0x02;
  const ch = s.challenge;
  const st = await c.status();
  assert.equal(st!.challenge, ch);
  const r = await c.confirmLoader(KEY, 5);
  assert.equal(r.ok, true, r.message);
  const frame = s.sent.find((m) => m[6] === 0x0f)!;
  const tag = Array.from(hmac(KEY, Uint8Array.from([0x43, 0x37, 0x39, 0x41, 0x0f, ...le32(ch), 0x10])).slice(0, 8));
  assert.deepEqual(frame.slice(7, -1), pack7([0x10, ...tag]));
  assert.equal(s.ldr[0]!.flags & 0x02, 0x02, "polled until the loader shows confirmed");
  assert.deepEqual(s.confirms, [0x10]);
  // reject: the loader bits pass (bits 4-5), nothing else above bit 1
  assert.equal(await c.reject(KEY, 0x20 | 0x40 | 0x04), 0);
  assert.equal(s.rejected, 0x20);
  // apply 2 = no main
  const a = await c.apply(KEY, ota.ApplyTarget.NoMain, () => {}, 5);
  assert.equal(a.ok, true, a.message);
  assert.equal(a.message, "no main");
  assert.deepEqual(s.applies, [2]);
});

test("update: a loader file goes to the loader slot not running (Begin / Commit target 0x11), seq = newest loader + 1", async () => {
  const { session, s } = v4Piano();
  const file = loaderFile(8);
  const fin = await new ota.OtaClient(session).update(file, KEY, () => {}, 2);
  assert.equal(fin.phase, "done", fin.message);
  assert.deepEqual([fin.kind, fin.target, fin.seq, fin.message], ["loader", 0x11, 2, "loader slot B"]);
  const begin = unpack7(Uint8Array.from(s.sent.find((m) => m[6] === 0x11)!.slice(7, -1)));
  assert.equal(begin[0], 0x11);
  assert.equal(u32(begin, 1), file.length);
  assert.equal(u32(begin, 9), 2);
  assert.equal(unpack7(Uint8Array.from(s.sent.find((m) => m[6] === 0x13)!.slice(7, -1)))[0], 0x11);
  assert.deepEqual(s.ldr[1], { seq: 2, flags: 0x41, tries: 0 });
  assert.deepEqual(s.main[1], EMPTY(), "main slots untouched");
  const inf = await new ota.OtaClient(session).info(0x11, 200);
  assert.equal(inf?.image?.version, 8);
  // a main next: main slot B, its own seq
  const fm = await new ota.OtaClient(session).update(makeFile(900), KEY, () => {}, 2);
  assert.deepEqual([fm.phase, fm.kind, fm.target, fm.seq], ["done", "main", 1, 2]);
});

test("update refuses locally what stage-1 refuses: unconfirmed running loader (22), incompatible main (25) - nothing sent", async () => {
  const { session, s } = v4Piano({ ldrRunningConfirmed: false });
  const fin = await new ota.OtaClient(session).update(loaderFile(), KEY, () => {}, 2);
  assert.equal(fin.phase, "error");
  assert.equal(fin.rc, 22);
  assert.equal(s.sent.filter((m) => m[6] === 0x11).length, 0, "no Begin");
  const f25 = await new ota.OtaClient(v4Piano().session).update(makeFile(900, { format: 2, type: 1, minApi: 3 }), KEY, () => {}, 2);
  assert.equal(f25.rc, 25);
  // the piano's own refusal comes through the same way (e.g. it saw the loader differently than its last status)
  const p = v4Piano({ beginRc: 22 });
  const f22 = await new ota.OtaClient(p.session).update(loaderFile(), KEY, () => {}, 2);
  assert.deepEqual([f22.phase, f22.rc, f22.message], ["error", 22, "begin refused: the running loader is not confirmed"]);
  assert.equal(L4.refusalText(f22), ota.OTA_RC_HELP[22]);
});

test("apply: SAFE mode refuses mains (rc 24) without sending; RESELECT (4) waits for the new stage-1", async () => {
  const safe = v4Piano({ safe: true });
  const r24 = await new ota.OtaClient(safe.session).apply(KEY, ota.ApplyTarget.Newest, () => {}, 5);
  assert.equal(r24.ok, false);
  assert.equal(r24.rc, 24);
  assert.deepEqual(safe.s.applies, []);
  // reselect after a loader upload: stage-0 picks the newer (unconfirmed) loader B, one try spent
  const { session, s } = v4Piano();
  s.ldr[1] = { seq: 2, flags: 0x41, tries: 0 };
  session.state = "ready";
  const why: string[] = [];
  session.onRebind((w) => why.push(w));
  const r = await new ota.OtaClient(session).apply(KEY, ota.ApplyTarget.Reselect, () => {}, 5);
  assert.equal(r.ok, true, r.message);
  assert.equal(r.message, "loader slot B");
  assert.equal(s.running, 1);
  assert.equal(s.ldr[1]!.tries, 1);
  assert.equal(r.status!.loader!.reselects, 1);
  assert.deepEqual(why, ["apply"]);
  // a layout-2 loader has no target 4
  const old = v4Piano({ layout2: true });
  const r4 = await new ota.OtaClient(old.session).apply(KEY, ota.ApplyTarget.Reselect, () => {}, 5);
  assert.equal(r4.ok, false);
  assert.deepEqual(old.s.applies, []);
});

test("rc 22-25 and the app's refusals have user texts in en, ja and ko", () => {
  for (const rc of [22, 23, 24, 25]) {
    assert.ok(ota.OTA_ERRORS[rc], `short name ${rc}`);
    assert.equal(L4.rcText(rc), ota.OTA_RC_HELP[rc]);
    assert.equal(L4.refusalText({ rc }), ota.OTA_RC_HELP[rc]);
  }
  assert.equal(L4.rcText(10), "CRC mismatch", "older codes keep the protocol name");
  assert.match(L4.refusalText({ refusal: "needV4" }), /OTA loader v4/);
  assert.equal(L4.refusalText({ rc: 3, message: "begin refused: busy" }), "Update failed: begin refused: busy");
  i18n.setLang("ko");
  for (const rc of [22, 23, 24, 25]) assert.match(L4.rcText(rc), /[가-힣]/, `ko ${rc}`);
  for (const k of Object.keys(ota.OTA_LOCAL_HELP) as (keyof typeof ota.OTA_LOCAL_HELP)[]) assert.match(L4.refusalText({ refusal: k }), /[가-힣]/, `ko ${k}`);
  assert.match(L4.rcText(22), /확인/);
  i18n.setLang("ja");
  for (const rc of [22, 23, 24, 25]) assert.notEqual(L4.rcText(rc), ota.OTA_RC_HELP[rc], `ja ${rc}`);
});

test("confirm on connect: sends nothing without a remembered upload; confirms the remembered loader once it runs", async () => {
  localStorage.setItem(ota.OTA_KEY_STORAGE, hexKey(KEY));
  const idle = v4Piano({ ldrRunningConfirmed: false });
  assert.equal(await L4.confirmLoaderOnConnect(idle.session), "");
  assert.equal(idle.s.sent.length, 0, "no remembered upload: not even a status");
  // remembered B #2, but A still runs (no power cycle yet): wait, keep it remembered
  const p = v4Piano();
  p.s.ldr[1] = { seq: 2, flags: 0x41, tries: 0 };
  L4.rememberLoaderUpload({ keyId: keyIdRef(KEY), seq: 2, slot: 1 });
  assert.equal(await L4.confirmLoaderOnConnect(p.session), "");
  assert.notEqual(L4.pendingLoader(), null);
  assert.deepEqual(p.s.confirms, []);
  // power cycle: stage-0 runs B on trial
  p.select();
  assert.equal(p.s.running, 1);
  assert.match(await L4.confirmLoaderOnConnect(p.session), /Loader confirmed \(slot B\)/);
  assert.deepEqual(p.s.confirms, [0x11]);
  assert.equal(p.s.ldr[1]!.flags & 2, 2);
  assert.equal(L4.pendingLoader(), null);
  // remembered, but stage-0 gave it up after three unconfirmed starts: say so, forget it
  const g = v4Piano();
  g.s.ldr[1] = { seq: 2, flags: 0x61, tries: 3 };
  L4.rememberLoaderUpload({ keyId: keyIdRef(KEY), seq: 2, slot: 1 });
  assert.match(await L4.confirmLoaderOnConnect(g.session), /previous loader/);
  assert.equal(L4.pendingLoader(), null);
  assert.deepEqual(g.s.confirms, []);
  // another key's upload is not ours to confirm
  L4.rememberLoaderUpload({ keyId: keyIdRef(OTHER), seq: 2, slot: 1 });
  assert.equal(await L4.confirmLoaderOnConnect(v4Piano().session), "");
});

/** Firmware card on a connected piano, the key imported. */
async function card(session: InstanceType<typeof PianoSession>, key = true) {
  if (key) localStorage.setItem(ota.OTA_KEY_STORAGE, hexKey(KEY));
  const el = infoScreen(createContext(new ParamStore(), { session: () => session }));
  document.body.append(el);
  (el.querySelector("#ng-check") as HTMLButtonElement).click();
  for (let i = 0; i < 100 && el.querySelector("#ota-state")!.textContent === "–"; i++) await tick(10);
  await tick(30);
  return el;
}
const q = <T extends HTMLElement = HTMLElement>(el: Element, sel: string) => el.querySelector(sel) as T;
const until = async (cond: () => boolean, n = 200) => { for (let i = 0; i < n && !cond(); i++) await tick(20); };

test("firmware card (layout 3): loader row, stage-0 line, SAFE note; layout 2 shows none of it", async () => {
  const { session } = v4Piano({ safe: true, otherLoader: { seq: 3, flags: 0x61, tries: 3 } });
  let el = await card(session);
  assert.equal(q(el, "#ota-loader-row").hidden, false);
  assert.equal(q(el, "#ota-loader-state").textContent, "A: #1 (running, confirmed) · B: #3 (3 of 3 tries, given up)");
  assert.match(q(el, "#ota-s0").textContent!, /stage-0 v1: SAFE mode \(E2\); loader API 1, v12; key [0-9a-f]{8} · SAFE mode · main: slot A/);
  assert.equal(q(el, "#ota-safe").hidden, false);
  assert.equal(q(el, "#ota-loader-confirm-row").hidden, true, "running loader confirmed: no confirm offer");
  assert.equal(q(el, "#ota-loader-rollback").hidden, false);
  el.remove();

  const old = v4Piano({ layout2: true });
  el = await card(old.session);
  assert.equal(q(el, "#ota-loader-row").hidden, true);
  assert.equal(q(el, "#ota-s0").hidden, true);
  assert.equal(q(el, "#ota-safe").hidden, true);
  assert.equal(q(el, "#ota-loader-confirm-row").hidden, true);
  assert.equal(q(el, "#ota-loader-rollback").hidden, true);
  assert.equal(q(el, "#ota-reselect").hidden, true);
  assert.match(q(el, "#ota-state").textContent!, /^slot A — /);
  assert.equal(old.s.sent.filter((m) => m[6] === 0x0e).length, 0, "no slot info below layout 3");
  el.remove();
});

test("firmware card: an unconfirmed running loader is offered for confirm (op 0x0F), with the 3 power-ons explained", async () => {
  const { session, s } = v4Piano({ ldrRunningConfirmed: false });
  const el = await card(session);
  assert.equal(q(el, "#ota-loader-confirm-row").hidden, false);
  assert.match(q(el, "#ota-loader-note").textContent!, /slot A, #1\) is not confirmed.*after 3 power-ons/);
  assert.match(q(el, "#ota-loader-state").textContent!, /A: #1 \(running, 1 of 3 tries\)/);
  // a loader file is refused before anything is sent (rc 22), with the rc text
  (q(el, "#ng-ota") as HTMLElement & { setOtaFile: (d: Uint8Array) => void }).setOtaFile(loaderFile());
  q<HTMLButtonElement>(el, "#ota-upload").click();
  await until(() => /not confirmed yet/.test(q(el, "#ota-status").textContent!));
  assert.equal(q(el, "#ota-status").textContent, ota.OTA_RC_HELP[22]);
  assert.equal(s.sent.filter((m) => m[6] === 0x11).length, 0);
  q<HTMLButtonElement>(el, "#ota-loader-confirm").click();
  await until(() => /Loader confirmed/.test(q(el, "#ota-status").textContent!));
  assert.equal(q(el, "#ota-status").textContent, "Loader confirmed (slot A).");
  assert.deepEqual(s.confirms, [0x10]);
  assert.equal(q(el, "#ota-loader-confirm-row").hidden, true);
  // rolling back the only confirmed loader: refused here (rc 23 text), nothing sent
  q<HTMLButtonElement>(el, "#ota-loader-rollback").click();
  await until(() => /one confirmed loader must stay/.test(q(el, "#ota-status").textContent!));
  assert.equal(q(el, "#ota-status").textContent, ota.OTA_RC_HELP[23]);
  assert.equal(s.sent.filter((m) => m[6] === 0x14).length, 0);
  el.remove();
});

test("firmware card: loader upload -> instructions + switch loader now -> the new loader runs and is confirmed", async () => {
  const { session, s } = v4Piano();
  const el = await card(session);
  assert.equal(q(el, "#ota-reselect").hidden, true);
  (q(el, "#ng-ota") as HTMLElement & { setOtaFile: (d: Uint8Array) => void }).setOtaFile(loaderFile(13));
  q<HTMLInputElement>(el, "#ota-apply-after").checked = true;   // applies the newest main: never after a loader
  q<HTMLButtonElement>(el, "#ota-upload").click();
  await until(() => /Loader written|failed/.test(q(el, "#ota-status").textContent!));
  await until(() => !q<HTMLButtonElement>(el, "#ota-upload").disabled);
  assert.match(q(el, "#ota-status").textContent!, /^Loader written to slot B \(#2\)\. .*after 3 power-ons\.$/);
  assert.deepEqual(s.applies, [], "no apply after a loader upload");
  assert.deepEqual(L4.pendingLoader(), { keyId: keyIdRef(KEY), seq: 2, slot: 1 });
  assert.equal(q(el, "#ota-reselect").hidden, false);
  assert.deepEqual(s.confirms, [], "the old loader still runs: nothing to confirm yet");
  assert.match(q(el, "#ota-loader-state").textContent!, /B: #2 v13 \(0 of 3 tries\)/);
  q<HTMLButtonElement>(el, "#ota-reselect").click();
  await until(() => /Loader confirmed|failed/.test(q(el, "#ota-status").textContent!));
  assert.equal(q(el, "#ota-status").textContent, "Now running loader slot B. Loader confirmed (slot B).");
  assert.deepEqual(s.applies, [4]);
  assert.deepEqual(s.confirms, [0x11]);
  assert.equal(L4.pendingLoader(), null);
  assert.equal(q(el, "#ota-reselect").hidden, true);
  assert.match(q(el, "#ota-loader-state").textContent!, /B: #2 v13 \(running, confirmed\)/);
  el.remove();
});

test("firmware card in Korean: the loader texts are translated", async () => {
  const { session } = v4Piano({ ldrRunningConfirmed: false });
  i18n.setLang("ko");
  const el = await card(session);
  assert.match(q(el, "#ota-loader-note").textContent!, /확인되지 않았습니다/);
  assert.equal(q(el, "#ota-loader-confirm").textContent, "이 로더 확인");
  assert.match(q(el, "#ota-loader-state").textContent!, /실행 중, 시도 1\/3/);
  el.remove();
});
