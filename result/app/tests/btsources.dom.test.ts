// BT audio source manager client (protocol/btport.ts 1D 04-09, 0F) and the 「블루투스」 page (ui/bluetooth.ts): a
// simulated piano that keeps a source table like result/firmware/src/bt/btsrc.c (field MACs of 2026-11-01/02).
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const i18n = await import("../src/i18n.ts");
const ng = await import("../src/protocol/ngext.ts");
const bt = await import("../src/protocol/btport.ts");
const { PianoSession } = await import("../src/protocol/session.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const { bluetoothPage, sourceText, switchText } = await import("../src/ui/bluetooth.ts");
const { noReplyText } = await import("../src/ui/btdiag.ts");

const PHONE = [0x62, 0x81, 0x81, 0x81, 0x11, 0xa1];
const PC = [0x02, 0x1a, 0x7d, 0xda, 0x13, 0x23];

type Dev = { addr: number[]; state: number; links: number; a2dp: number; streamS: number; state2: number; refusals: number; ps?: number };

/** a piano with the source manager: layout 2 (or 1 = `layout1`, or none = `old`); 1D 05 01 runs a scripted switch */
function srcPiano(opts: { old?: boolean; stuck?: boolean; bound?: boolean; layout1?: boolean; short?: boolean; l112?: boolean; noStrategy?: boolean; v5?: boolean; noClass?: boolean; noLevel?: boolean; noAvrcp?: boolean; noAudio?: boolean; x6?: boolean; x7?: boolean } = {}) {
  const [app, piano] = LoopbackLink.pair();
  const s = {
    devs: [{ addr: PC, state: 0x21 | 0x10 | 1, links: 0x7, a2dp: 0x3, streamS: 40, state2: 0x05, refusals: 0 },
      { addr: PHONE, state: 0x20, links: 0x38, a2dp: 0x18, streamS: 0, state2: 0x04, refusals: 0 }] as Dev[],
    policy: { mode: 2, flags: 0x04, hold: 15, sweep: 5, pref: null as number[] | null, strat: 0, recon: 2, store: 1, xopt: 0, xwait: 0 },
    sw: { phase: 0, result: 0, ds: 0 }, ac: { state: 0, op: 0, rc: 0, saved: 0 },
    // class of device (1D 0A): the module's value read live 2026-11-09; a write / read is read back on the next 1D 0A
    co: { state: 0, op: 0, rc: 0, saved: 0, cod: 0x001f00, orig: 0x001f00, want: 0x240414, pend: 0 }, coOps: [] as number[][],
    // AVRCP controller features (1D 0C): the module's line read live 2026-11-09 (`… CONTROLLER 1f`)
    av: { state: 0, op: 0, rc: 0, saved: 0, f: 0x1f, orig: 0x1f, want: 0x0f, pend: 0 }, avOps: [] as number[][],
    // audio routing (1D 0D): the stock factory line `SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE` (flags 2); route
    // = the reply's byte 10 / 11 (1 + link | 0x40 A2DP, device slot or 0x7f)
    au: { state: 0, op: 0, rc: 0, saved: 0, f: 2, orig: 2, want: 1, pend: 0, route: 0, rdev: 0x7f }, auOps: [] as number[][],
    health: { flags: 0, reboots: 0, since: 0xff }, swTried: 0, codec: { codec: 0, dev: 0xff, rate: 0 },
    actions: [] as number[][], acOps: [] as number[], passes: 100, restarts: 0, sent: [] as number[][],
    log: [] as { seq: number; t: number; flags: number; text: string }[],
    script: [] as (() => void)[],
    // 1D 0B: the DSP capture as the firmware reports it (peaks L / R, the DSP write count moving each scan)
    lvl: { peakL: 0, peakR: 0, moving: true, count: 0 },
  };
  const send = (op: number, d: number[]) => piano.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...d.map((b) => b & 0x7f), 0xf7]));
  const polOut = () => [s.policy.mode, s.policy.flags, s.policy.hold, s.policy.sweep, s.policy.pref ? 1 : 0, ...ng.pack7(s.policy.pref ?? [0, 0, 0, 0, 0, 0]),
    ...(opts.noStrategy ? [] : [s.policy.strat]), ...(opts.v5 ? [s.policy.recon, s.policy.store] : []), ...(opts.x6 ? [s.policy.xopt] : []), ...(opts.x7 ? [s.policy.xwait] : [])];
  piano.onMessage((m) => {
    if (m[1] !== 0x7d || m[6] !== 0x1d) return;
    const a = Array.from(m.slice(7, m.length - 1));
    s.sent.push(a);
    if (a[0] === 0x0f) {
      if (!opts.stuck) s.passes += 125;
      const raw = new Array(16).fill(0);
      raw[0] = 1; raw[1] = opts.stuck ? 1 : 0; raw[2] = s.passes & 0xff; raw[3] = s.passes >> 8; raw[6] = opts.stuck ? 0x00 : 0; raw[7] = opts.stuck ? 0xf8 : 0;
      send(0x1d, [0x0f, 0, ...ng.pack7(raw)]);
      return;
    }
    if (opts.stuck) return;   // task 24 stopped: every queued op is unanswered
    if (a[0] === 4 && !opts.old) {
      s.script.shift()?.();   // each read moves a running switch one step on
      const v2 = !opts.layout1, stride = v2 ? 14 : 12;
      const raw = new Array(v2 ? (opts.short ? 102 : opts.l112 ? 112 : opts.x7 ? 120 : opts.v5 ? 119 : 116) : 76).fill(0);
      raw[0] = v2 ? 2 : 1; raw[1] = s.policy.mode; raw[2] = s.policy.flags; raw[3] = s.policy.hold; raw[4] = s.policy.sweep; raw[5] = 3 | (opts.noStrategy ? 0 : (s.policy.strat + 1) << 4);
      raw[6] = s.devs.filter((d) => d.state & 1).length; raw[7] = 0;
      if (s.policy.pref) { raw[18] = 1; raw.splice(19, 6, ...s.policy.pref); }
      s.devs.forEach((d, i) => {
        const o = 28 + stride * i;
        raw[o] = d.state | 0x80; raw.splice(o + 1, 6, ...d.addr); raw[o + 7] = d.links; raw[o + 9] = d.a2dp; raw[o + 11] = d.streamS;
        if (v2) { raw[o + 12] = d.state2 | (s.policy.pref && d.addr.join() === s.policy.pref.join() ? 0 : 0); raw[o + 13] = d.refusals; }
      });
      if (v2) {
        const sel = s.devs.findIndex((d) => d.state2 & 1);
        raw[84] = sel < 0 ? 0xff : sel; raw[85] = s.sw.phase; raw[86] = s.sw.result; raw[87] = s.sw.ds;
        raw[88] = s.ac.state; raw[89] = s.ac.op; raw[90] = s.ac.rc;
        if (sel >= 0) { raw[95] = 1; raw.splice(96, 6, ...s.devs[sel]!.addr); }
        if (!opts.short) {
          raw[102] = s.health.flags; raw[103] = s.health.reboots & 0xff; raw[104] = s.health.reboots >> 8; raw[105] = s.health.since;
          raw[106] = s.swTried; raw[107] = s.codec.codec; raw[108] = s.codec.dev; raw[109] = s.codec.rate;
          if (!opts.l112) s.devs.forEach((d, i) => { raw[112 + i] = d.ps ?? 0; });
          if (opts.v5) { raw[116] = (s.policy.recon + 1) | (s.policy.store << 2) | (opts.x6 ? 0x40 | (s.policy.xopt & 1 ? 0x80 : 0) : 0); raw[117] = 2; raw[118] = 1; if (opts.x7) raw[119] = s.policy.xwait; }
        }
      }
      send(0x1d, [4, 0, ...ng.pack7(raw)]);
    } else if (a[0] === 5) {
      const addr = [...ng.unpack7(Uint8Array.from(a.slice(2)))].slice(0, 6);
      s.actions.push([a[1]!, ...addr]);
      const known = s.devs.some((d) => d.addr.join() === addr.join());
      const rc = opts.bound === false ? 2 : known ? 0 : 7;
      send(0x1d, [5, rc, a[1]!]);
      if (rc) return;
      if (a[1] === 3) s.devs = s.devs.filter((d) => d.addr.join() === addr.join());   // only = close the others
      if (a[1] === 1) {
        // the field case: the PC (Windows) keeps its stream after the pause -> the piano suspends it (strategy 0 - its
        // links stay, field 2026-11-03) -> the phone plays
        const pc = s.devs.find((d) => d.addr.join() !== addr.join())!, to = s.devs.find((d) => d.addr.join() === addr.join())!;
        pc.state2 &= ~1; to.state2 |= 1 | 8;
        s.sw = { phase: 1, result: 0, ds: 2 };
        s.script.push(() => {}, () => { s.sw = { phase: 2, result: 0, ds: 9 }; },
          () => { s.sw = { phase: 7, result: 0, ds: 12 }; pc.state &= ~1; },
          () => { s.sw = { phase: 0, result: 1, ds: 15 }; s.swTried = 1; to.state |= 1; to.state2 &= ~8; });
      }
    } else if (a[0] === 6) {
      if (a.length >= 13) s.policy = { mode: a[1]!, flags: a[2]!, hold: a[3]!, sweep: a[4]!, pref: a[5] ? [...ng.unpack7(Uint8Array.from(a.slice(6, 13)))].slice(0, 6) : null,
        strat: a.length >= 14 && !opts.noStrategy ? a[13]! : s.policy.strat,
        recon: a.length >= 15 && opts.v5 ? a[14]! : s.policy.recon, store: s.policy.store,
        xopt: a.length >= 16 && opts.x6 ? a[15]! : s.policy.xopt,
        xwait: a.length >= 17 && opts.x7 ? a[16]! : s.policy.xwait };
      for (const d of s.devs) d.state = (d.state & ~0x08) | (s.policy.pref && d.addr.join() === s.policy.pref.join() ? 0x08 : 0);
      send(0x1d, [6, 0, ...polOut()]);
    } else if (a[0] === 9) {
      const op = a[1] ?? 0;
      let rc = 0;
      if (op === 2 && s.ac.state !== 1) rc = 10;
      else if (op === 3 && s.ac.state !== 2) rc = 10;
      else if (op === 1) s.ac = { ...s.ac, state: 1, saved: 1 };
      else if (op === 2) s.ac = { ...s.ac, state: 2 };
      else if (op === 3) s.ac = { ...s.ac, state: 1 };
      if (op) s.acOps.push(op);
      const line = "SET CONTROL AUTOCALL 19 5000 A2DP";
      send(0x1d, [9, rc, op, s.ac.state, 0, 0, s.ac.saved, line.length, ...[...line].map((c) => c.charCodeAt(0))]);
    } else if (a[0] === 0x0b && !opts.old && !opts.noLevel) {
      const l = s.lvl, first = l.count === 0;
      if (l.moving || first) l.count = (l.count + 128) % 1152 || 128;
      const raw = new Array(16).fill(0);
      const put = (o: number, v: number) => { raw[o] = v & 0xff; raw[o + 1] = (v >> 8) & 0xff; };
      raw[0] = 1;
      raw[1] = first ? 0x02 : l.moving ? 0x01 : 0;
      put(2, l.peakL); put(4, l.peakR); put(10, 0x2d); put(12, 0x100); put(14, l.count);
      send(0x1d, [0x0b, 0, ...ng.pack7(raw)]);
    } else if (a[0] === 0x0a && !opts.old && !opts.noClass) {
      const op = a[1] ?? 0, c = s.co;
      if (c.op) {   // the read-back of the op before (the listing came)
        if (c.op === 2 || c.op === 3) c.cod = c.pend;
        c.state = 1; c.saved = 1; c.rc = 0; c.op = 0;
      }
      const given = a.length >= 6 ? ((a[2]! << 21) | (a[3]! << 14) | (a[4]! << 7) | a[5]!) : 0;
      let rc = 0;
      if (op > 3 || (given && (op !== 2 || !bt.codAudioSink(given)))) rc = 1;
      else if (op >= 2 && opts.bound === false) rc = 2;
      else if (op === 2 && (c.state !== 1 || c.cod === (given || 0x240414))) rc = 10;
      else if (op === 3 && (c.state !== 1 || c.cod === c.orig)) rc = 10;
      else if (op) {
        c.op = op;
        if (op === 2) { c.want = given || 0x240414; c.pend = c.want; }
        if (op === 3) c.pend = c.orig;
        s.coOps.push(a.slice(1));
      }
      const c7 = (v: number) => [(v >> 21) & 0x7f, (v >> 14) & 0x7f, (v >> 7) & 0x7f, v & 0x7f];
      send(0x1d, [0x0a, rc, op, c.state, c.op, c.rc, c.saved, ...c7(c.cod), ...c7(c.orig), ...c7(c.want)]);
    } else if (a[0] === 0x0c && !opts.old && !opts.noAvrcp) {
      const op = a[1] ?? 0, c = s.av;
      if (c.op) {   // the read-back of the op before (the listing came)
        if (c.op === 2 || c.op === 3) c.f = c.pend;
        c.state = 1; c.saved = 1; c.rc = 0; c.op = 0;
      }
      const given = a.length >= 3 ? a[2]! : 0;
      let rc = 0;
      if (op > 3 || (a.length >= 3 && (op !== 2 || given < 1 || given > 0x0f))) rc = 1;
      else if (op >= 2 && opts.bound === false) rc = 2;
      else if (op === 2 && (c.state !== 1 || c.f === (given || 0x0f))) rc = 10;
      else if (op === 3 && (c.state !== 1 || c.f === c.orig)) rc = 10;
      else if (op) {
        c.op = op;
        if (op === 2) { c.want = given || 0x0f; c.pend = c.want; }
        if (op === 3) c.pend = c.orig;
        s.avOps.push(a.slice(1));
      }
      send(0x1d, [0x0c, rc, op, c.state, c.op, c.rc, c.saved, c.f >> 7, c.f & 0x7f, c.orig >> 7, c.orig & 0x7f, c.want >> 7, c.want & 0x7f]);
    } else if (a[0] === 0x0d && !opts.old && !opts.noAudio) {
      const op = a[1] ?? 0, c = s.au;
      if (c.op) {   // the read-back of the op before (the listing came)
        if (c.op === 2 || c.op === 3) c.f = c.pend;
        c.state = 1; c.saved = 1; c.rc = 0; c.op = 0;
      }
      const given = a.length >= 3 ? a[2]! : 0;
      let rc = 0;
      if (op > 3 || (a.length >= 3 && (op !== 2 || (given !== 1 && given !== 3)))) rc = 1;
      else if (op >= 2 && opts.bound === false) rc = 2;
      else if (op === 2 && (c.state !== 1 || c.f === (given || 1))) rc = 10;
      else if (op === 3 && (c.state !== 1 || c.f === c.orig)) rc = 10;
      else if (op) {
        c.op = op;
        if (op === 2) { c.want = given || 1; c.pend = c.want; }
        if (op === 3) c.pend = c.orig;
        s.auOps.push(a.slice(1));
      }
      send(0x1d, [0x0d, rc, op, c.state, c.op, c.rc, c.saved, c.f, c.orig, c.want, c.route, c.rdev]);
    } else if (a[0] === 8 && !opts.old) {
      const from = ((a[1] ?? 0) << 7) | (a[2] ?? 0);
      const first = s.log.length ? s.log[0]!.seq : 0, next = s.log.length ? s.log[s.log.length - 1]!.seq + 1 : 0;
      const out = [8, 0, first >> 7, first & 0x7f, next >> 7, next & 0x7f, 0, 0, 0, 100];
      for (const r of s.log.filter((r) => r.seq >= from).slice(0, 3)) {
        out.push(r.seq >> 7, r.seq & 0x7f, 0, 0, (r.t / 10) >> 7, (r.t / 10) & 0x7f, r.flags, r.text.length, ...[...r.text].map((c) => c.charCodeAt(0)));
      }
      send(0x1d, out);
    } else if (a[0] === 7) {
      s.restarts++;
      send(0x1d, [7, 0]);
    }
  });
  return { app, s };
}

function mount(p: ReturnType<typeof srcPiano>, opts: Parameters<typeof bluetoothPage>[1] = {}) {
  const port = new bt.BtPort(new ng.NgClient(new PianoSession(p.app)));
  const page = bluetoothPage(() => port, { refreshMs: 30, ...opts });
  dom.window.document.body.append(page);
  return page;
}

const until = async (cond: () => boolean, n = 150) => { for (let i = 0; i < n && !cond(); i++) await tick(10); };

test("1D 04 decode: layout 2 (selection, switch, AUTOCALL) and layout 1; MAC shown as its last two bytes", async () => {
  const p = srcPiano();
  const port = new bt.BtPort(new ng.NgClient(new PianoSession(p.app)));
  const src = await port.sources();
  assert.ok(src);
  assert.equal(src!.devices.length, 2);
  assert.deepEqual(src!.devices[0]!.addr, PC);
  assert.deepEqual(src!.selected, PC);
  assert.deepEqual(src!.sw, { phase: 0, result: 0, ms: 0 });
  assert.deepEqual(src!.autocall, { state: 0, op: 0, rc: 0 });
  assert.equal(src!.devices[0]!.state2 & bt.SRC_DEV2.selected, bt.SRC_DEV2.selected);
  assert.equal(bt.macShort(PHONE), "…11:a1");
  i18n.setLang("ko");
  assert.equal(sourceText(src!.devices[0]!), "…13:23 · 재생 중");
  assert.equal(sourceText(src!.devices[1]!), "…11:a1 · 연결됨");
  assert.equal(sourceText({ ...src!.devices[1]!, a2dp: 0 }), "…11:a1 · 연결됨 · 소리 연결 없음");
  assert.equal(sourceText({ ...src!.devices[1]!, state2: 0x05 }), "…11:a1 · 재생 기다림", "selected, silent: no 「재생을 눌러」 yet");
  const ready = { ...src!, sw: { phase: 0, result: bt.SW_RESULT.ready, ms: 4000 } };
  assert.equal(sourceText({ ...src!.devices[1]!, state2: 0x05 }, ready, 1), "…11:a1 · 재생 기다림 — 그 기기에서 재생을 한 번 눌러 주세요",
    "only after the piano's PLAY / START / reopen ended \"ready\"");
  assert.equal(sourceText({ ...src!.devices[0]!, a2dp: 0, links: 0, state: 0x20, state2: 0x20 }), "…13:23 · 피아노가 소리 연결을 닫아 둠 — 「이 기기로 듣기」를 누르면 다시 연결");
  const aac = { ...src!, codec: { codec: 2, dev: 0, rate: 0 } };
  assert.equal(sourceText(src!.devices[0]!, aac, 0), "…13:23 · 재생 중 · AAC");
  assert.deepEqual(src!.health, { flags: 0, reboots: 0, sinceS: null });
  assert.equal(src!.swTried, 0);
  assert.equal(src!.codec, null);
  i18n.setLang("en");
  assert.equal(await port.sourceAction(bt.SRC_ACT.close, [1, 2, 3, 4, 5, 6]), 7);
  const pol = await port.setPolicy({ mode: 2, flags: 0x14, holdDs: 20, sweepS: 9, pref: PC });
  assert.deepEqual(pol, { rc: 0, policy: { mode: 2, flags: 0x14, holdDs: 20, sweepS: 9, pref: PC, strategy: 0 } }, "no strategy sent: it stays");
  // an old strategy in the policy: the app writes 0 - one way to switch since the strategies were removed
  const pol3 = await port.setPolicy({ mode: 2, flags: 0x14, holdDs: 20, sweepS: 9, pref: PC, strategy: 3 });
  assert.equal(p.s.sent.filter((a) => a[0] === 6).at(-1)![13], 0, "1D 06 byte 13 = 0 whatever the policy held");
  assert.equal(pol3?.policy.strategy, 0);
  assert.equal((await port.sources())?.policy.strategy, 0, "1D 04 byte 5 bits 4-6 = strategy + 1");
  assert.equal(await port.restartModule(), 0);
  assert.deepEqual(await port.autocall(bt.AC_OP.state), { rc: 0, op: 0, state: 0, busy: 0, lastRc: 0, saved: false, line: "SET CONTROL AUTOCALL 19 5000 A2DP" });
  const old = srcPiano({ layout1: true });
  const s1 = await new bt.BtPort(new ng.NgClient(new PianoSession(old.app))).sources();
  assert.ok(s1 && s1.devices.length === 2 && s1.sw === null && s1.selected === null && s1.devices[1]!.state2 === 0);
  // result 1 texts: PLAY sent (tries bit 0x01, also 0x10 = after the stream was up) / no PLAY (no AVRCP, or a firmware
  // without the tries byte) - never 「소리가 납니다」: the module cannot tell sound from silence
  i18n.setLang("ko");
  const done = (tried: number) => switchText({ ...src!, sw: { phase: 0, result: bt.SW_RESULT.playing, ms: 1700 }, swTried: tried }, "…13:23");
  assert.equal(done(bt.SW_TRIED.fresh | bt.SW_TRIED.play | bt.SW_TRIED.playUp), "…13:23(으)로 바꿨습니다(1.7초 걸림). 소리가 안 나면 그 기기에서 재생을 누르세요");
  assert.equal(done(bt.SW_TRIED.fresh), "…13:23(으)로 바꿨습니다(1.7초 걸림). 소리가 안 나면 그 기기에서 재생을 누르세요");
  assert.equal(done(bt.SW_TRIED.start), "…13:23(으)로 바꿨습니다(1.7초 걸림). 소리가 안 나면 그 기기에서 재생을 누르세요 · 피아노가 소리 연결을 다시 엶");
  i18n.setLang("en");
  assert.equal(done(bt.SW_TRIED.play), "Asked …13:23 to play (1.7 s) — if you hear nothing, press play on it");
  // no PLAY on purpose (firmware after 9e6c8c32 - Windows takes PLAY as play/pause): what was done, said plainly
  i18n.setLang("ko");
  assert.equal(done(bt.SW_TRIED.kept), "…13:23(으)로 바꿨습니다(1.7초 걸림). 이미 재생 중이었습니다");
  assert.equal(done(bt.SW_TRIED.fresh | bt.SW_TRIED.notOurs),
    "…13:23(으)로 바꿨습니다(1.7초 걸림). 소리가 안 나면 그 기기에서 재생을 누르세요");
  assert.equal(done(bt.SW_TRIED.kept | bt.SW_TRIED.start), "…13:23(으)로 바꿨습니다(1.7초 걸림). 이미 재생 중이었습니다 · 피아노가 소리 연결을 다시 엶");
  i18n.setLang("en");
  assert.equal(done(bt.SW_TRIED.kept), "…13:23 was already playing, so it was left as it is (1.7 s)");
  const short = srcPiano({ short: true });   // the 102-byte layout 2 (ota_main cf3f2f3b): no health / tries / codec
  const s2 = await new bt.BtPort(new ng.NgClient(new PianoSession(short.app))).sources();
  assert.ok(s2 && s2.sw && s2.health === null && s2.codec === null && s2.swTried === 0);
});

test("page: one tap 「이 기기로 듣기」 switches — progress, then the result; nothing disconnected; the selected badge moves", async () => {
  i18n.setLang("ko");
  const p = srcPiano();
  const page = mount(p);
  await until(() => page.querySelectorAll("[data-mac]").length === 2);
  const row = (mac: number[]) => page.querySelector<HTMLElement>(`[data-mac="${bt.macText(mac)}"]`)!;
  assert.equal(row(PC).querySelector<HTMLElement>(".badge")!.hidden, false, "the PC is the selected source");
  assert.equal(row(PC).querySelector<HTMLElement>("[data-action=use]")!.hidden, true, "selected and playing: no 「이 기기로 듣기」");
  assert.equal(row(PHONE).querySelector<HTMLElement>(".badge")!.hidden, true);
  assert.ok(page.textContent!.includes("이 기기로 듣기") && page.textContent!.includes("이 기기 연결 끊기") && page.textContent!.includes("우선 기기로"));
  assert.equal(page.querySelector("[data-action=pause]"), null, "the separate 「일시정지」 button is gone (merged into the switch)");
  const sw = page.querySelector<HTMLElement>("#bt-sw")!;
  row(PHONE).querySelector<HTMLButtonElement>("[data-action=use]")!.click();
  assert.equal(sw.hidden, false);
  assert.match(sw.textContent!, /…11:a1\(으\)로 바꾸는 중/, "progress at once");
  await until(() => p.s.actions.length > 0);
  assert.deepEqual(p.s.actions[0], [1, ...PHONE], "「이 기기로 듣기」 = 1D 05 01");
  const seen: string[] = [];
  await until(() => { seen.push(sw.textContent!); return sw.dataset.result === "1"; }, 300);
  assert.ok(seen.some((x) => /전환 중… …13:23 일시정지/.test(x)), `pause step shown: ${[...new Set(seen)].join(" | ")}`);
  assert.ok(seen.some((x) => /소리 멈추는 중/.test(x)), "suspend step shown (no CLOSE any more - field 2026-11-03)");
  assert.match(sw.textContent!, /…11:a1\(으\)로 바꿨습니다\(1\.5초 걸림\)\. 소리가 안 나면 그 기기에서 재생을 누르세요/,
    "stream up + PLAY sent: what was done, not 「소리가 납니다」 (field 2026-11-02 afternoon: Windows streamed silence)");
  assert.ok(!/소리가 피아노에서 납니다/.test(sw.textContent!));
  await until(() => !/재생 중/.test(row(PC).textContent!.split(" · ")[1] ?? ""));
  assert.match(row(PC).textContent!, /^…13:23 · 연결됨/, "the PC stays listed with its sound link (suspended, not parked)");
  await until(() => !row(PHONE).querySelector<HTMLElement>(".badge")!.hidden);
  assert.equal(row(PHONE).querySelector<HTMLElement>(".badge")!.hidden, false, "the badge moved to the phone");
  assert.equal(row(PC).querySelector<HTMLElement>(".badge")!.hidden, true);
  assert.equal(page.querySelectorAll("[data-mac]").length, 2, "nothing is disconnected (the PC keeps its row)");
  assert.equal(p.s.actions.filter((a) => a[0] === 0).length, 0);
  // policy: 「고른 기기만」 / 「재생한 기기로」 (follow)
  assert.ok(page.querySelector("[data-mode=select]")!.classList.contains("on"));
  page.querySelector<HTMLButtonElement>("[data-mode=follow]")!.click();
  await until(() => !!(p.s.policy.flags & bt.SRC_FLAG.follow));
  assert.equal(p.s.policy.mode, 2);
  assert.equal(p.s.policy.flags & bt.SRC_FLAG.follow, bt.SRC_FLAG.follow);
  // 선호 기기 toggles
  row(PHONE).querySelector<HTMLButtonElement>("[data-action=pref]")!.click();
  await until(() => !!p.s.policy.pref);
  assert.deepEqual(p.s.policy.pref, PHONE);
  await until(() => row(PHONE).querySelector("[data-action=pref]")!.textContent === "우선 해제");
  row(PHONE).querySelector<HTMLButtonElement>("[data-action=pref]")!.click();
  await until(() => !p.s.policy.pref);
  assert.equal(p.s.policy.pref, null);
  page.querySelector<HTMLButtonElement>("#bt-src-restart")!.click();
  await until(() => p.s.restarts > 0);
  assert.equal(p.s.restarts, 1);
  page.remove();
  i18n.setLang("en");
});

test("page: AUTOCALL is changed only through an explicit confirmation; read → off → restore", async () => {
  i18n.setLang("ko");
  const p = srcPiano();
  const page = mount(p);
  await until(() => !page.querySelector<HTMLElement>("#bt-ac-card")!.hidden);
  const st = page.querySelector<HTMLElement>("#bt-ac-state")!;
  assert.match(st.textContent!, /아직 읽지 않았습니다/);
  assert.equal(page.querySelector<HTMLElement>("#bt-ac-ask")!.hidden, true, "no write action before a read");
  page.querySelector<HTMLButtonElement>("#bt-ac-read")!.click();
  await until(() => /켜짐/.test(st.textContent!));
  assert.match(st.textContent!, /SET CONTROL AUTOCALL 19 5000 A2DP/);
  const ask = page.querySelector<HTMLButtonElement>("#bt-ac-ask")!;
  assert.equal(ask.hidden, false);
  assert.match(ask.textContent!, /끄기/);
  ask.click();
  assert.equal(p.s.acOps.filter((o) => o !== 1).length, 0, "the first tap writes nothing");
  const conf = page.querySelector<HTMLElement>("#bt-ac-confirm")!;
  assert.equal(conf.hidden, false);
  assert.match(conf.textContent!, /모듈에 저장되는 설정/);
  assert.match(conf.textContent!, /SET CONTROL AUTOCALL 19 5000 A2DP/, "the exact restore line is shown");
  page.querySelector<HTMLButtonElement>("#bt-ac-no")!.click();
  await tick(80);
  assert.equal(p.s.acOps.filter((o) => o !== 1).length, 0, "cancel writes nothing");
  ask.click();
  page.querySelector<HTMLButtonElement>("#bt-ac-yes")!.click();
  await until(() => /꺼짐/.test(st.textContent!));
  assert.deepEqual(p.s.acOps, [1, 2]);
  assert.match(page.querySelector<HTMLButtonElement>("#bt-ac-ask")!.textContent!, /되돌리기/);
  page.querySelector<HTMLButtonElement>("#bt-ac-ask")!.click();
  page.querySelector<HTMLButtonElement>("#bt-ac-yes")!.click();
  await until(() => /켜짐/.test(st.textContent!));
  assert.deepEqual(p.s.acOps, [1, 2, 3]);
  page.remove();
  i18n.setLang("en");
});

test("1D 0A decode / encode: class of device (state, read-back, the classes); a write of another audio class carries it", async () => {
  assert.deepEqual(bt.encodeBtClass(bt.CO_OP.state), [0x0a, 0]);
  assert.deepEqual(bt.encodeBtClass(bt.CO_OP.write, 0x240418), [0x0a, 2, 0x01, 0x10, 0x08, 0x18]);
  assert.equal(bt.decodeBtClass(Uint8Array.from([0x0a, 0, 0])), null, "short reply");
  assert.deepEqual(bt.decodeBtClass(Uint8Array.from([0x0a, 0, 1, 1, 1, 0, 1, 0, 0, 0x3e, 0, 0, 0, 0x3e, 0, 1, 0x10, 0x08, 0x14])),
    { rc: 0, op: 1, state: 1, busy: 1, lastRc: 0, saved: true, cod: 0x001f00, restore: 0x001f00, write: 0x240414 });
  assert.ok(bt.codAudioSink(bt.COD_AUDIO_SINK));
  assert.ok(!bt.codAudioSink(0x001f00), "the field value: major uncategorized, no service bit");
  assert.ok(!bt.codAudioSink(0x200414), "no Rendering bit");
  assert.ok(!bt.codAudioSink(0x240514), "major Peripheral");
  assert.equal(bt.codHex(0x1f00), "001f00");
  const p = srcPiano();
  const port = new bt.BtPort(new ng.NgClient(new PianoSession(p.app)));
  const st = await port.deviceClass(bt.CO_OP.state);
  assert.deepEqual(st, { rc: 0, op: 0, state: 0, busy: 0, lastRc: 0, saved: false, cod: 0x001f00, restore: 0x001f00, write: 0x240414 });
  assert.equal((await port.deviceClass(bt.CO_OP.write))?.rc, 10, "write before a read");
  assert.equal((await port.deviceClass(bt.CO_OP.write, 0x001f00))?.rc, 1, "not an audio sink class");
  assert.equal((await port.deviceClass(bt.CO_OP.read))?.busy, bt.CO_OP.read);
  const w = await port.deviceClass(bt.CO_OP.write, 0x240418);
  assert.ok(w && w.rc === 0 && w.state === bt.CO_STATE.known && w.write === 0x240418);
  assert.equal((await port.deviceClass(bt.CO_OP.state))?.cod, 0x240418, "read back");
  assert.deepEqual(p.s.coOps, [[1], [2, 0x01, 0x10, 0x08, 0x18]]);
  assert.equal(await new bt.BtPort(new ng.NgClient(new PianoSession(srcPiano({ noClass: true }).app))).deviceClass(0, undefined, 100), null);
});

test("1D 0C decode / encode: AVRCP controller features (state, read-back); a write of other features carries them", async () => {
  assert.deepEqual(bt.encodeBtAvrcpFeatures(bt.AVF_OP.state), [0x0c, 0]);
  assert.deepEqual(bt.encodeBtAvrcpFeatures(bt.AVF_OP.write, 0x01), [0x0c, 2, 0x01]);
  assert.equal(bt.decodeBtAvrcpFeatures(Uint8Array.from([0x0c, 0, 0])), null, "short reply");
  assert.deepEqual(bt.decodeBtAvrcpFeatures(Uint8Array.from([0x0c, 0, 1, 1, 1, 0, 1, 0, 0x1f, 0, 0x1f, 0, 0x0f])),
    { rc: 0, op: 1, state: 1, busy: 1, lastRc: 0, saved: true, features: 0x1f, restore: 0x1f, write: 0x0f });
  assert.equal(bt.avfHex(0x0f), "0f");
  const p = srcPiano();
  const port = new bt.BtPort(new ng.NgClient(new PianoSession(p.app)));
  assert.deepEqual(await port.avrcpFeatures(bt.AVF_OP.state), { rc: 0, op: 0, state: 0, busy: 0, lastRc: 0, saved: false, features: 0x1f, restore: 0x1f, write: 0x0f });
  assert.equal((await port.avrcpFeatures(bt.AVF_OP.write))?.rc, 10, "write before a read");
  assert.equal((await port.avrcpFeatures(bt.AVF_OP.write, 0x1f))?.rc, 1, "the browsing bit");
  assert.equal((await port.avrcpFeatures(bt.AVF_OP.read))?.busy, bt.AVF_OP.read);
  const w = await port.avrcpFeatures(bt.AVF_OP.write, 0x01);
  assert.ok(w && w.rc === 0 && w.state === bt.AVF_STATE.controller && w.write === 0x01);
  assert.equal((await port.avrcpFeatures(bt.AVF_OP.state))?.features, 0x01, "read back");
  assert.deepEqual(p.s.avOps, [[1], [2, 0x01]]);
  assert.equal(await new bt.BtPort(new ng.NgClient(new PianoSession(srcPiano({ noAvrcp: true }).app))).avrcpFeatures(0, undefined, 100), null);
});

test("1D 0D decode / encode: audio routing (state, flags, route event); the module lines it writes", async () => {
  assert.deepEqual(bt.encodeBtAudio(bt.AUD_OP.state), [0x0d, 0]);
  assert.deepEqual(bt.encodeBtAudio(bt.AUD_OP.write, 3), [0x0d, 2, 3]);
  assert.equal(bt.decodeBtAudio(Uint8Array.from([0x0d, 0, 0])), null, "short reply");
  assert.deepEqual(bt.decodeBtAudio(Uint8Array.from([0x0d, 0, 1, 1, 0, 0, 1, 1, 2, 1, 0x40 | 5, 1])),
    { rc: 0, op: 1, state: 1, busy: 0, lastRc: 0, saved: true, flags: 1, restore: 2, write: 1, route: { link: 4, a2dp: true, dev: 1 } });
  assert.deepEqual(bt.decodeBtAudio(Uint8Array.from([0x0d, 0, 0, 1, 0, 0, 1, 1, 2, 1, 2, 0x7f]))?.route, { link: 1, a2dp: false, dev: null });
  assert.equal(bt.decodeBtAudio(Uint8Array.from([0x0d, 0, 0, 1, 0, 0, 1, 1, 2, 1, 0, 0x7f]))?.route, null);
  assert.equal(bt.audLine(bt.AUD_FACTORY), "SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE");
  assert.equal(bt.audLine(bt.AUD_WRITES[0]), "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT");
  assert.equal(bt.audLine(bt.AUD_WRITES[1]), "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT KEEPALIVE");
  const p = srcPiano();
  const port = new bt.BtPort(new ng.NgClient(new PianoSession(p.app)));
  assert.deepEqual(await port.audio(bt.AUD_OP.state), { rc: 0, op: 0, state: 0, busy: 0, lastRc: 0, saved: false, flags: 2, restore: 2, write: 1, route: null });
  assert.equal((await port.audio(bt.AUD_OP.write))?.rc, 10, "write before a read");
  assert.equal((await port.audio(bt.AUD_OP.write, 2))?.rc, 1, "KEEPALIVE alone is no write variant");
  assert.equal((await port.audio(bt.AUD_OP.read))?.busy, bt.AUD_OP.read);
  const w = await port.audio(bt.AUD_OP.write, 3);
  assert.ok(w && w.rc === 0 && w.state === bt.AUD_STATE.plain && w.write === 3);
  assert.equal((await port.audio(bt.AUD_OP.state))?.flags, 3, "read back");
  assert.deepEqual(p.s.auOps, [[1], [2, 3]]);
  assert.equal(await new bt.BtPort(new ng.NgClient(new PianoSession(srcPiano({ noAudio: true }).app))).audio(0, undefined, 100), null);
});

test("page: no module-setting cards (the firmware keeps the decided values); the page asks no 1D 0A / 0C / 0D; health bit pin shown", async () => {
  i18n.setLang("ko");
  const p = srcPiano();
  const page = mount(p);
  await until(() => !page.querySelector<HTMLElement>("#bt-ac-card")!.hidden);
  await tick(300);
  for (const id of ["#bt-co-card", "#bt-av-card", "#bt-au-card", "#bt-co-read", "#bt-av-read", "#bt-au-read", "#bt-av-restart", "#bt-au-restart"])
    assert.equal(page.querySelector(id), null, id);
  assert.equal(p.s.sent.filter((a) => a[0] === 0x0a || a[0] === 0x0c || a[0] === 0x0d).length, 0, "no module-setting op from the page");
  assert.ok(page.querySelector("#bt-src-restart"), "the module restart button stays");
  assert.ok(page.querySelector("#bt-ac-read"), "the AUTOCALL card stays");
  const health = page.querySelector<HTMLElement>("#bt-health")!;
  assert.equal(health.hidden, true);
  p.s.health.flags = bt.MOD_HEALTH.pin;   // the firmware could not keep the decided settings this power-on
  await until(() => !health.hidden);
  assert.match(page.querySelector("#bt-health-text")!.textContent!, /모듈 설정\(기기 종류 · PC 연결 · 소리 경로\)을 맞추지 못했습니다/);
  assert.equal(health.classList.contains("warn"), false);
  page.remove();
  i18n.setLang("en");
});

test("bug 2026-11-01 evening: one tap on 「끊기」 disconnects nothing; rows stay the same nodes across refreshes", async () => {
  // Field: the first time the sheet was opened, `1D 05 00` (disconnect) went to the playing PC ~3 s later. The rows
  // were rebuilt on every 2 s refresh (buttons replaced under the pointer) and one tap on 「끊기」 cut a source off.
  i18n.setLang("ko");
  const p = srcPiano();
  const page = mount(p, { refreshMs: 20, confirmMs: 150 });
  await until(() => page.querySelectorAll("[data-mac]").length === 2);
  const pcRow = () => page.querySelector<HTMLElement>(`[data-mac="${bt.macText(PC)}"]`)!;
  const close0 = pcRow().querySelector<HTMLButtonElement>("[data-action=close]")!;
  const use0 = pcRow().querySelector<HTMLButtonElement>("[data-action=use]")!;
  const reads = () => p.s.sent.filter((a) => a[0] === 4).length;
  const r0 = reads();
  await until(() => reads() >= r0 + 4);
  assert.ok(reads() >= r0 + 4, "several refreshes happened");
  assert.equal(pcRow().querySelector("[data-action=close]"), close0, "the same 「끊기」 node after refreshes (not rebuilt)");
  assert.equal(pcRow().querySelector("[data-action=use]"), use0);
  assert.equal(p.s.sent.filter((a) => a[0] === 5).length, 0, "opening + refreshing sends no 1D 05");
  close0.click();
  await tick(60);
  assert.equal(p.s.actions.length, 0, "one tap only arms 「끊기」");
  assert.equal(close0.textContent, "한 번 더 누르면 끊습니다");
  assert.equal(pcRow().querySelector("[data-action=close]"), close0, "a refresh keeps the armed button");
  await tick(200);
  assert.equal(close0.textContent, "이 기기 연결 끊기", "disarmed after the confirm window");
  close0.click();
  await tick(60);
  assert.equal(p.s.actions.length, 0);
  close0.click();
  await until(() => p.s.actions.length > 0);
  assert.deepEqual(p.s.actions, [[0, ...PC]], "the second tap within the window disconnects");
  page.remove();
  const n = p.s.sent.length;
  await tick(120);
  assert.ok(p.s.sent.length <= n + 1, "a page taken out of the document stops polling");
  i18n.setLang("en");
});

test("developer view: the module lines (1D 08) — module lines ←, commands sent →, read on from the last sequence", async () => {
  const p = srcPiano();
  p.s.log.push({ seq: 0, t: 1000, flags: 4, text: "module on" }, { seq: 1, t: 1520, flags: 0, text: "A2DP STREAMING START 4" },
    { seq: 2, t: 2000, flags: 0, text: "RING 0 62:81:81:81:11:a1 19 A2DP" }, { seq: 3, t: 2500, flags: 1, text: "AVRCP PAUSE" });
  const page = mount(p);
  await until(() => !!page.querySelector("[data-mac]"));
  assert.equal(p.s.sent.filter((a) => a[0] === 8).length, 0, "closed: not read");
  const box = page.querySelector<HTMLDetailsElement>("#bt-src-dev")!;
  box.open = true;
  box.dispatchEvent(new dom.window.Event("toggle"));
  const pre = page.querySelector("#bt-src-log")!;
  await until(() => pre.textContent!.includes("AVRCP PAUSE"));
  const lines = pre.textContent!.split("\n");
  assert.deepEqual(lines, ["    1.00 · module on", "    1.52 ← A2DP STREAMING START 4", "    2.00 ← RING 0 62:81:81:81:11:a1 19 A2DP", "    2.50 → AVRCP PAUSE"]);
  p.s.log.push({ seq: 4, t: 3000, flags: 0, text: "A2DP STREAMING STOP 4" });
  await until(() => pre.textContent!.includes("STOP"));
  assert.equal(pre.textContent!.split("\n").length, 5, "only the new line is added");
  assert.ok(p.s.sent.filter((a) => a[0] === 8).some((a) => a[2] === 4), "read on from sequence 4");
  // the EVENT lines of `SET CONTROL AUDIO … EVENT` (D74): shown as they came, nothing filtered
  p.s.log.push({ seq: 5, t: 3100, flags: 0, text: "A2DP CODEC APT-X JOINT_STEREO 44100" },
    { seq: 6, t: 3200, flags: 0, text: "AUDIO ROUTE 4 A2DP LEFT RIGHT" });
  await until(() => pre.textContent!.includes("AUDIO ROUTE"));
  assert.deepEqual(pre.textContent!.split("\n").slice(-2), ["    3.10 ← A2DP CODEC APT-X JOINT_STEREO 44100", "    3.20 ← AUDIO ROUTE 4 A2DP LEFT RIGHT"]);
  page.remove();
});

test("page on an older payload: no list, the switching hint; BLE-MIDI card shows this app's link and opens the connect menu", async () => {
  const p = srcPiano({ old: true });
  let opened = 0;
  const page = mount(p, { refreshMs: 5000, link: () => ({ kind: "ble", name: "CA79" }), coBound: () => "Panel also connected", openConnect: () => { opened++; } });
  await until(() => page.textContent!.includes("cannot list"));
  assert.ok(page.textContent!.includes("This piano firmware cannot list the Bluetooth audio devices."));
  assert.ok(page.textContent!.includes("disconnect Bluetooth on the device playing now"));
  assert.match(page.querySelector("#bt-midi-link")!.textContent!, /holds the piano's Bluetooth MIDI link \(CA79\)/);
  assert.equal(page.querySelector("#bt-midi-co")!.textContent, "Panel also connected");
  page.querySelector<HTMLButtonElement>("#bt-midi-connect")!.click();
  assert.equal(opened, 1);
  page.remove();
});

test("no reply to 1D 01: the task-19 heartbeat tells a stopped engine loop from a lost answer (field 2026-11-01)", async () => {
  i18n.setLang("ko");
  const stuck = srcPiano({ stuck: true });
  const txt = await noReplyText(new bt.BtPort(new ng.NgClient(new PianoSession(stuck.app))), 1, async () => {});
  assert.match(txt, /엔진 루프가 멈췄습니다\(단계 1, 송신 개수 63488\)/);
  const alive = srcPiano();
  assert.equal(await noReplyText(new bt.BtPort(new ng.NgClient(new PianoSession(alive.app))), 1, async () => {}), "응답이 없습니다.");
  i18n.setLang("en");
  const hb = bt.decodeBtHeartbeat(Uint8Array.from([1, 1, 5, 0, 0, 0, 0, 0xf8, 0, 0, 1, 0, 2, 0x0b, 3, 4]))!;
  assert.deepEqual([hb.passes, hb.stage, hb.txCount, hb.ier, hb.pending, hb.sending, hb.querying, hb.moduleOn], [5, 1, 0xf800, 2, true, true, false, true]);
  const raw = new Array(40).fill(0);
  raw[0] = 1; raw[28] = 0x10; raw[32] = 2; raw[34] = 9; raw[36] = 1; raw[38] = 1; raw[39] = 2;
  assert.deepEqual(bt.decodeBtStatus(Uint8Array.from(raw))!.loop, { passes: 16, txDrops: 2, reqSeen: 9, reqBusy: 1, stage: 1, ier: 2 });
});

test("page: the switch's own restart steps; 「재생을 한 번 눌러」 only after them; the reboot-loop warning offers AUTOCALL off behind its confirmation", async () => {
  i18n.setLang("ko");
  const p = srcPiano();
  const page = mount(p);
  await until(() => page.querySelectorAll("[data-mac]").length === 2);
  const row = (mac: number[]) => page.querySelector<HTMLElement>(`[data-mac="${bt.macText(mac)}"]`)!;
  // phone -> PC that cannot be started: PLAY, the piano's START, reopen, then "ready"
  p.s.devs[0]!.state &= ~1;
  p.s.devs[0]!.state2 &= ~1;
  p.s.devs[1]!.state2 |= 1;
  p.s.devs[1]!.state |= 1;
  await until(() => !row(PHONE).querySelector<HTMLElement>(".badge")!.hidden);
  const sw = page.querySelector<HTMLElement>("#bt-sw")!;
  row(PC).querySelector<HTMLButtonElement>("[data-action=use]")!.click();
  await until(() => p.s.actions.length > 0);
  p.s.script.length = 0;
  const pc = p.s.devs[0]!, ph = p.s.devs[1]!;
  ph.state2 &= ~1; pc.state2 |= 1 | 8;
  s_steps(p, [
    () => { p.s.sw = { phase: 7, result: 0, ds: 8 }; ph.state &= ~1; },
    () => { p.s.sw = { phase: 8, result: 0, ds: 20 }; p.s.swTried = 1 | 2; },
    () => { p.s.sw = { phase: 9, result: 0, ds: 31 }; p.s.swTried = 1 | 2 | 4; },
    () => { p.s.sw = { phase: 0, result: 2, ds: 60 }; pc.state2 &= ~8; },
  ]);
  const seen: string[] = [];
  await until(() => { seen.push(sw.textContent! + " | " + row(PC).textContent!); return sw.dataset.result === "2"; }, 300);
  assert.ok(seen.some((x) => /…13:23 소리 켜는 중/.test(x)), `START step: ${[...new Set(seen)].join(" / ")}`);
  assert.ok(seen.some((x) => /…13:23 소리 연결 다시 여는 중/.test(x)), "reopen step");
  assert.ok(!seen.slice(0, -1).some((x) => /한 번 눌러/.test(x)), "「재생을 한 번 눌러」 not while the piano still tries");
  assert.match(sw.textContent!, /피아노가 소리를 켜지 못했습니다\. 그 기기에서 재생을 한 번 눌러 주세요/);
  await until(() => /한 번 눌러/.test(row(PC).textContent!));
  assert.match(row(PC).textContent!, /…13:23 · 재생 기다림 — 그 기기에서 재생을 한 번 눌러 주세요/);
  // the reboot loop: warning + 「AUTOCALL 끄기…」 opens the same confirmation; nothing is written by the warning itself
  const health = page.querySelector<HTMLElement>("#bt-health")!;
  assert.equal(health.hidden, true);
  p.s.health = { flags: 3, reboots: 4, since: 5 };
  await until(() => !health.hidden);
  assert.match(health.textContent!, /계속 저절로 다시 시작됩니다/);
  assert.match(health.textContent!, /4회/);
  assert.match(health.textContent!, /지우고 다시 페어링/);
  const ac = page.querySelector<HTMLButtonElement>("#bt-health-ac")!;
  assert.equal(ac.hidden, false);
  assert.equal(ac.textContent, "모듈 자동 재연결 끄기…");
  assert.equal(page.querySelector<HTMLDetailsElement>("#bt-dev-fold")!.open, false);
  ac.click();                                     // AUTOCALL not read yet: a read, then the confirmation
  assert.equal(page.querySelector<HTMLDetailsElement>("#bt-dev-fold")!.open, true, "the warning opens the fold that holds the confirmation");
  await until(() => !page.querySelector<HTMLElement>("#bt-ac-confirm")!.hidden);
  assert.equal(page.querySelector<HTMLElement>("#bt-ac-confirm")!.hidden, false);
  assert.deepEqual(p.s.acOps, [1], "only the read: nothing written before 「끄기」");
  page.querySelector<HTMLButtonElement>("#bt-ac-yes")!.click();
  await until(() => p.s.acOps.length === 2);
  assert.deepEqual(p.s.acOps, [1, 2]);
  await until(() => page.querySelector<HTMLElement>("#bt-health-ac")!.hidden);
  assert.equal(page.querySelector<HTMLElement>("#bt-health-ac")!.hidden, true, "AUTOCALL off: no action any more");
  // one reboot a moment ago: a note only
  p.s.health = { flags: 2, reboots: 1, since: 30 };
  await until(() => /30초 전/.test(health.textContent!));
  assert.ok(!/반복/.test(health.textContent!));
  page.remove();
  i18n.setLang("en");
});

function s_steps(p: ReturnType<typeof srcPiano>, steps: (() => void)[]) {
  p.s.script.push(...steps);
}

test("field 2026-11-02: a hidden window (minimized desktop app) sends no 1D 04 / 1D 08; back in view the page reads at once", async () => {
  const p = srcPiano();
  const page = mount(p, { refreshMs: 20 });
  await until(() => page.querySelectorAll("[data-mac]").length === 2);
  const reads = () => p.s.sent.filter((a) => a[0] === 4 || a[0] === 8).length;
  const doc = dom.window.document;
  const setVis = (v: string) => {
    Object.defineProperty(doc, "visibilityState", { configurable: true, get: () => v });
    doc.dispatchEvent(new dom.window.Event("visibilitychange"));
  };
  try {
    setVis("hidden");
    await tick(80);
    const r0 = reads();
    await tick(200);
    assert.equal(reads(), r0, "nothing asked while hidden");
    setVis("visible");
    await until(() => reads() > r0);
    assert.ok(reads() > r0, "visible again: polling resumes");
  } finally {
    delete (doc as unknown as Record<string, unknown>).visibilityState;
    page.remove();
  }
});

test("play status (1D 04 bytes 112..115, firmware after ota_main 9e6c8c32): decoded per slot, shown on the device line; 112-byte replies have none", async () => {
  const p = srcPiano();
  p.s.devs[0]!.ps = bt.PLAY_STATUS.playing | bt.PS_FLAG.live;
  p.s.devs[1]!.ps = bt.PLAY_STATUS.paused | bt.PS_FLAG.live | bt.PS_FLAG.ours;
  const src = await new bt.BtPort(new ng.NgClient(new PianoSession(p.app))).sources();
  assert.ok(src);
  assert.equal(src!.devices[0]!.ps, 0x0a);
  assert.equal(src!.devices[1]!.ps, 0x2b);
  i18n.setLang("ko");
  assert.equal(sourceText(src!.devices[0]!), "…13:23 · 재생 중", "streaming and playing: said once");
  assert.equal(sourceText(src!.devices[1]!), "…11:a1 · 연결됨 · 기기에서 일시정지");
  assert.equal(sourceText({ ...src!.devices[0]!, ps: bt.PLAY_STATUS.paused }), "…13:23 · 재생 중 · 기기에서 일시정지",
    "Windows streams while paused: both facts");
  assert.equal(sourceText({ ...src!.devices[1]!, ps: bt.PS_FLAG.none }), "…11:a1 · 연결됨", "not reported: nothing");
  assert.equal(sourceText({ ...src!.devices[1]!, ps: bt.PLAY_STATUS.stopped }), "…11:a1 · 연결됨 · 기기에서 정지");
  i18n.setLang("en");
  assert.equal(sourceText(src!.devices[1]!), "…11:a1 · connected · player: paused");
  const old = srcPiano({ l112: true });
  const s112 = await new bt.BtPort(new ng.NgClient(new PianoSession(old.app))).sources();
  assert.ok(s112 && s112.devices.every((d) => d.ps === null) && s112.health !== null);
  assert.equal(sourceText(s112!.devices[1]!), "…11:a1 · connected");
});

test("page: no switching-strategy selector; a policy write sends strategy 0 - also to an older firmware left on another one; no strategy byte to a firmware without it", async () => {
  i18n.setLang("ko");
  const p = srcPiano();
  const page = mount(p);
  await until(() => page.querySelectorAll("[data-mac]").length === 2);
  await until(() => !!page.querySelector("[data-mode=follow]"));
  assert.equal(page.querySelector("#bt-src-strategy"), null, "no strategy box");
  assert.equal(page.querySelectorAll("[data-strategy]").length, 0);
  assert.doesNotMatch(page.textContent!, /전환 방식 \(실험\)/);
  assert.equal(page.querySelector<HTMLElement>("#bt-src-late")!.hidden, true, "a firmware without the late stop: nothing there");
  page.remove();
  // an older firmware left on strategy 2 (RAM, set by an older app): the next policy write puts it back to 0
  const left = srcPiano();
  left.s.policy.strat = 2;
  const page1 = mount(left);
  await until(() => !!page1.querySelector("[data-mode=follow]"));
  page1.querySelector<HTMLButtonElement>("[data-mode=follow]")!.click();
  await until(() => left.s.policy.strat === 0);
  assert.equal(left.s.sent.filter((a) => a[0] === 6 && a.length >= 14).at(-1)![13], 0, "1D 06 ... <strategy 0>");
  page1.remove();
  const old = srcPiano({ noStrategy: true });
  const page2 = mount(old);
  await until(() => page2.querySelectorAll("[data-mac]").length === 2);
  page2.querySelector<HTMLButtonElement>("[data-mode=follow]")!.click();
  await until(() => old.s.sent.some((a) => a[0] === 6 && a.length >= 13));
  assert.ok(old.s.sent.filter((a) => a[0] === 6 && a.length >= 13).every((a) => a.length === 13), "no strategy byte to an older firmware");
  page2.remove();
  i18n.setLang("en");
});

test("page: the late-stop option (1D 06 byte 15) - off by default, toggled in the developer section, ko / ja; hidden on older firmware", async () => {
  const p = srcPiano({ v5: true, x6: true });
  const port = new bt.BtPort(new ng.NgClient(new PianoSession(p.app)));
  assert.equal((await port.sources())?.policy.options, 0, "1D 04 byte 116 bit 6 = known, bit 7 = on");
  assert.equal((await port.policy())?.options, 0);
  i18n.setLang("ko");
  const page = mount(p);
  await until(() => !!page.querySelector("#bt-src-latestop"));
  const box = page.querySelector<HTMLElement>("#bt-src-late")!;
  assert.equal(box.hidden, false);
  assert.ok(page.querySelector("#bt-src-dev")!.parentElement!.contains(box), "in the developer section, next to the module lines");
  assert.match(box.textContent!, /다른 기기로 바꿀 때/);
  const ls = box.querySelector<HTMLElement>("#bt-src-latestop")!;
  assert.ok(ls.querySelector("[data-latestop='0']")!.classList.contains("on"), "default off");
  assert.match(ls.textContent!, /PC가 소리 연결을 늦게 닫으면 피아노가 닫음\(실험\)/);
  assert.match(box.querySelector("#bt-src-latestop-why")!.textContent!, /^끔: /);
  ls.querySelector<HTMLButtonElement>("[data-latestop='1']")!.click();
  await until(() => p.s.policy.xopt === 1);
  const set = p.s.sent.filter((a) => a[0] === 6 && a.length >= 16).at(-1)!;
  assert.equal(set.length, 16);
  assert.deepEqual([set[13], set[14], set[15]], [0, 2, 1], "1D 06 ... <strategy> <reconnect> <options>");
  await until(() => page.querySelector("#bt-src-latestop [data-latestop='1']")?.classList.contains("on") === true);
  assert.match(page.querySelector("#bt-src-latestop-why")!.textContent!, /4\.5초/);
  assert.match(page.querySelector("#bt-src-latestop-why")!.textContent!, /블루투스 오디오 모듈을 다시 시작/);
  page.querySelector<HTMLButtonElement>("[data-mode=follow]")!.click();   // another policy write keeps the option
  await until(() => !!(p.s.policy.flags & bt.SRC_FLAG.follow));
  assert.equal(p.s.sent.filter((a) => a[0] === 6 && a.length >= 16).at(-1)![15], 1);
  i18n.setLang("ja");
  page.querySelector<HTMLButtonElement>("#bt-src-latestop [data-latestop='0']")!.click();
  await until(() => p.s.policy.xopt === 0);
  await until(() => /^オフ：/.test(page.querySelector("#bt-src-latestop-why")?.textContent ?? ""));
  page.remove();
  const old = srcPiano({ v5: true });
  const page2 = mount(old);
  await until(() => !!page2.querySelector("[data-mode=follow]"));
  assert.equal(page2.querySelector("#bt-src-latestop"), null, "older firmware: no toggle");
  assert.equal(page2.querySelector<HTMLElement>("#bt-src-late")!.hidden, true);
  page2.querySelector<HTMLButtonElement>("[data-mode=follow]")!.click();
  await until(() => !!(old.s.policy.flags & bt.SRC_FLAG.follow));
  assert.ok(old.s.sent.filter((a) => a[0] === 6 && a.length >= 13).every((a) => a.length <= 15), "no options byte to an older firmware");
  page2.remove();
  i18n.setLang("en");
});

test("page: the late stop's wait (1D 06 byte 16) - 0.5 / 1 / 2 / 4.5 s next to the toggle, default 1 s, ko / ja; hidden and never sent to older firmware", async () => {
  const p = srcPiano({ v5: true, x6: true, x7: true });
  const port = new bt.BtPort(new ng.NgClient(new PianoSession(p.app)));
  assert.equal((await port.sources())?.policy.lateWait, 0, "1D 04 byte 119");
  assert.equal((await port.policy())?.lateWait, 0, "1D 06 reply byte 16");
  i18n.setLang("ko");
  const page = mount(p);
  await until(() => !!page.querySelector("#bt-src-latewait"));
  const lw = page.querySelector<HTMLElement>("#bt-src-latewait")!;
  assert.deepEqual([...lw.querySelectorAll<HTMLElement>("[data-latewait]")].map((b) => b.textContent), ["0.1초", "0.5초", "1초", "2초", "4.5초"]);
  assert.ok(lw.querySelector("[data-latewait='10']")!.classList.contains("on"), "0 = the firmware default 1 s");
  assert.match(page.querySelector("#bt-src-late")!.textContent!, /일시정지를 알린 뒤 기다림/);
  page.querySelector<HTMLButtonElement>("#bt-src-latestop [data-latestop='1']")!.click();
  await until(() => p.s.policy.xopt === 1);
  let set = p.s.sent.filter((a) => a[0] === 6 && a.length >= 16).at(-1)!;
  assert.equal(set.length, 17);
  assert.deepEqual([set[13], set[14], set[15], set[16]], [0, 2, 1, 0], "1D 06 ... <strategy> <reconnect> <options> <wait>");
  await until(() => /1초 뒤까지/.test(page.querySelector("#bt-src-latestop-why")?.textContent ?? ""));
  page.querySelector<HTMLButtonElement>("#bt-src-latewait [data-latewait='5']")!.click();
  await until(() => p.s.policy.xwait === 5);
  set = p.s.sent.filter((a) => a[0] === 6 && a.length >= 16).at(-1)!;
  assert.deepEqual([set[15], set[16]], [1, 5], "the option stays on");
  await until(() => page.querySelector("#bt-src-latewait [data-latewait='5']")?.classList.contains("on") === true);
  assert.match(page.querySelector("#bt-src-latestop-why")!.textContent!, /0\.5초 뒤까지/);
  page.querySelector<HTMLButtonElement>("[data-mode=follow]")!.click();   // another policy write keeps the wait
  await until(() => !!(p.s.policy.flags & bt.SRC_FLAG.follow));
  assert.equal(p.s.sent.filter((a) => a[0] === 6 && a.length >= 16).at(-1)![16], 5);
  i18n.setLang("ja");
  page.querySelector<HTMLButtonElement>("#bt-src-latewait [data-latewait='45']")!.click();
  await until(() => p.s.policy.xwait === 45);
  await until(() => /4\.5秒後/.test(page.querySelector("#bt-src-latestop-why")?.textContent ?? ""));
  assert.match(page.querySelector("#bt-src-latewait")!.textContent!, /0\.5秒/);
  page.remove();
  // the firmware with the option but no wait (753d97fb): no selector, never a 17th byte, the text says 4.5 s
  const old = srcPiano({ v5: true, x6: true });
  i18n.setLang("ko");
  const page2 = mount(old);
  await until(() => !!page2.querySelector("#bt-src-latestop"));
  assert.equal(page2.querySelector("#bt-src-latewait"), null, "older firmware: no wait selector");
  page2.querySelector<HTMLButtonElement>("#bt-src-latestop [data-latestop='1']")!.click();
  await until(() => old.s.policy.xopt === 1);
  await until(() => /4\.5초 뒤까지/.test(page2.querySelector("#bt-src-latestop-why")?.textContent ?? ""));
  assert.ok(old.s.sent.filter((a) => a[0] === 6 && a.length >= 13).every((a) => a.length <= 16), "no wait byte to an older firmware");
  page2.remove();
  i18n.setLang("en");
});

test("page: the piano's own reconnect (1D 06 byte 14, kept on NOR) - three choices with one line each, ko / ja; an older firmware's select-only BUSY gets the plain text; hidden on older firmware", async () => {
  const p = srcPiano({ v5: true });
  const port = new bt.BtPort(new ng.NgClient(new PianoSession(p.app)));
  const src = await port.sources();
  assert.equal(src?.policy.reconnect, bt.SRC_RECONNECT.prefLast, "1D 04 byte 116 bits 0-1 = value + 1");
  assert.equal(src?.policy.store, bt.POLICY_STORE.kept);
  assert.deepEqual(src?.lineFixes, { garbled: 2, recovered: 1 });
  const pol = await port.policy();
  assert.equal(pol?.reconnect, bt.SRC_RECONNECT.prefLast);
  i18n.setLang("ko");
  const page = mount(p);
  await until(() => !page.querySelector<HTMLElement>("#bt-reconnect")!.hidden);
  const box = page.querySelector<HTMLElement>("#bt-reconnect")!;
  // An everyday choice, so in the 「기기 고르기 · 다시 연결」 card - not with AUTOCALL in the fold
  assert.ok(page.querySelector("#bt-policy-card")!.contains(box), "in the policy card");
  assert.equal(page.querySelector("#bt-dev-fold")!.contains(box), false, "not in the developer fold");
  assert.match(box.textContent!, /켤 때 다시 연결할 기기/);
  assert.doesNotMatch(box.textContent!, /AUTOCALL/);
  assert.deepEqual([...box.querySelectorAll("button")].map((b) => b.textContent), ["끔", "우선 기기만", "우선 · 마지막 기기"]);
  assert.ok(box.querySelector("[data-reconnect='2']")!.classList.contains("on"), "default: preferred, else last");
  assert.match(box.textContent!, /마지막으로 고른 기기/);
  // user decision 2026-11-14: it applies at power-on and at the wake from a standby that
  // switched the module off - one plain sentence
  assert.equal(box.querySelector("#bt-rc-store")!.textContent, "피아노를 켤 때, 그리고 블루투스 오디오를 끈 대기에서 깰 때 적용됩니다.");
  assert.doesNotMatch(page.textContent!, /피아노를 꺼도 유지/);
  // At power-on the reconnect choice follows the startup setting (no longer "kept") - said once
  // for the whole card
  assert.equal(page.querySelector("#bt-src-store")!.textContent, "우선 기기 · 자동 재연결 · 위의 선택은 시작 설정을 따릅니다(설정 → 시작 설정).");
  assert.equal(page.querySelector<HTMLElement>("#bt-rc-ac")!.hidden, true, "AUTOCALL off: the choice is in use, no note");
  box.querySelector<HTMLButtonElement>("[data-reconnect='1']")!.click();
  await until(() => p.s.policy.recon === 1);
  const set = p.s.sent.filter((a) => a[0] === 6 && a.length >= 15).at(-1)!;
  assert.equal(set.length, 15);
  assert.equal(set[13], 0, "the strategy as it was");
  assert.equal(set[14], 1, "1D 06 ... <strategy> <reconnect>");
  await until(() => box.querySelector("[data-reconnect='1']")!.classList.contains("on"));
  assert.match(box.querySelector("#bt-rc-why")!.textContent!, /우선 기기에만 연결을 시도합니다/);
  i18n.setLang("ja");
  box.querySelector<HTMLButtonElement>("[data-reconnect='0']")!.click();
  await until(() => box.querySelector("[data-reconnect='0']")!.classList.contains("on"));
  assert.match(box.querySelector("#bt-rc-why")!.textContent!, /呼び出しません/);
  page.remove();
  i18n.setLang("en");
  // an older firmware still on 「선택만」 (strategy 4, since removed): its BUSY gets the plain text
  const busy = { ...src!, policy: { ...src!.policy, strategy: 4 }, sw: { phase: 0, result: bt.SW_RESULT.busy, ms: 1500 },
    devices: src!.devices.map((d) => d.state2 & bt.SRC_DEV2.selected ? d : { ...d, state: d.state | bt.SRC_DEV.stream }) };   // the other still streams
  assert.match(switchText(busy, "…11:a1"), /The other device would not let go of the sound — stop it there, or disconnect it/);
  // The piano never stops another source - one streaming without AVRCP is the user's to pause
  const held = (phase: number, result: number) => ({
    ...src!, sw: { phase, result, ms: 12000 },
    devices: src!.devices.map((d) => d.state2 & bt.SRC_DEV2.selected ? d
      : { ...d, state: d.state | bt.SRC_DEV.stream, state2: d.state2 & ~bt.SRC_DEV2.avrcp }),
  });
  assert.match(switchText(held(bt.SW_PHASE.pause, 0), "…11:a1"), /the piano cannot pause .* — pause it on that device/);
  assert.match(switchText(held(0, bt.SW_RESULT.busy), "…11:a1"), /The piano cannot pause .*no remote control/);
  i18n.setLang("ko");
  assert.match(switchText(held(0, bt.SW_RESULT.busy), "…11:a1"), /…11:a1: 피아노에서 일시정지할 수 없습니다\. /);
  i18n.setLang("en");
  // (field 2026-11-10 night): a BUSY whose blocking stream has closed since is stale - not 「…놓지 않습니다」
  const letGo = { ...src!, sw: { phase: 0, result: bt.SW_RESULT.busy, ms: 30000 },
    devices: src!.devices.map((d) => ({ ...d, state: d.state & ~bt.SRC_DEV.stream })) };
  assert.match(switchText(letGo, "…11:a1"), /has let go of the sound — press “Listen on this device” again/);
  assert.doesNotMatch(switchText(letGo, "…11:a1"), /would not let go|cannot pause/);
  i18n.setLang("ko");
  assert.match(switchText(letGo, "…11:a1"), /다른 기기가 소리 연결을 닫았습니다\. 「이 기기로 듣기」를 다시 누르세요/);
  i18n.setLang("en");
  // The piano never starts the sound of a selected device without AVRCP - READY says play on it there
  const noRcTarget = (tried: number, avrcp: boolean) => ({
    ...src!, sw: { phase: 0, result: bt.SW_RESULT.ready, ms: 800 }, swTried: tried,
    devices: src!.devices.map((d) => !(d.state2 & bt.SRC_DEV2.selected) ? d
      : { ...d, state2: avrcp ? d.state2 | bt.SRC_DEV2.avrcp : d.state2 & ~bt.SRC_DEV2.avrcp }),
  });
  assert.match(switchText(noRcTarget(0, false), "…13:23"), /…13:23 is selected — the piano has no remote control of it, so press play on that device/);
  assert.match(switchText(noRcTarget(0, true), "…13:23"), /the piano could not start it; press play on it once/);
  assert.match(switchText(noRcTarget(bt.SW_TRIED.start, false), "…13:23"), /the piano could not start it/);   // an older firmware's START
  i18n.setLang("ko");
  assert.match(switchText(noRcTarget(0, false), "…13:23"), /…13:23 선택됨 — 이 기기는 피아노에서 재생을 시킬 수 없습니다\. 그 기기에서 재생을 누르세요/);
  i18n.setLang("en");
  // an older firmware (15 B policy, 116 B list): no selector, no reconnect byte written
  const old = srcPiano();
  const page2 = mount(old);
  await until(() => page2.querySelectorAll("[data-mac]").length === 2);
  assert.equal(page2.querySelector<HTMLElement>("#bt-reconnect")!.hidden, true);
  page2.querySelector<HTMLButtonElement>("[data-mode=follow]")!.click();
  await until(() => old.s.sent.some((a) => a[0] === 6 && a.length >= 14));
  assert.ok(old.s.sent.filter((a) => a[0] === 6 && a.length >= 13).every((a) => a.length <= 14), "no reconnect byte to an older firmware");
  page2.remove();
});

test("1D 0B decode: verdict signal / silent / unknown (first scan, DSP not moving, pointer outside), dBFS", () => {
  const raw = (flags: number, l: number, r: number) => Uint8Array.from([1, flags, l & 0xff, l >> 8, r & 0xff, r >> 8, 0, 0, 0, 0, 0x2d, 0, 0, 1, 0x80, 0]);
  const a = bt.decodeBtLevel(raw(1, 0x0800, 3))!;
  assert.deepEqual([a.peakL, a.peakR, a.gain, a.pos, a.count], [0x800, 3, 0x2d, 0x100, 0x80]);
  assert.equal(bt.levelVerdict(a), "signal");
  assert.equal(bt.levelDb(a.peakL), -24);
  assert.equal(bt.levelVerdict(bt.decodeBtLevel(raw(1, 15, 9))!), "silent", "under the floor 16");
  assert.equal(bt.levelVerdict(bt.decodeBtLevel(raw(2, 0x800, 0))!), "unknown", "first scan: MOVED not known");
  assert.equal(bt.levelVerdict(bt.decodeBtLevel(raw(0, 0x800, 0))!), "unknown", "the DSP did not move");
  assert.equal(bt.levelVerdict(bt.decodeBtLevel(raw(9, 0x800, 0))!), "unknown", "pointer outside the buffers");
  assert.equal(bt.decodeBtLevel(Uint8Array.from([2, ...new Array(15).fill(0)])), null, "another layout");
  assert.equal(bt.levelDb(0), -Infinity);
});

test("page: 「피아노 BT 입력」 follows 1D 0B (signal / no signal), hidden on a payload without it", async () => {
  i18n.setLang("ko");
  const p = srcPiano();
  const page = mount(p, { refreshMs: 20 });
  const line = page.querySelector<HTMLElement>("#bt-level")!;
  await until(() => !line.hidden && /없음/.test(line.textContent!));
  assert.equal(line.textContent, "피아노에 들어오는 소리: 없음");
  p.s.lvl.peakL = 0x0800;
  await until(() => /있음/.test(line.textContent!));
  // 15_ko_labels (approved 2026-11-15): 있음 / 없음 only, the level in the tooltip
  assert.equal(line.textContent, "피아노에 들어오는 소리: 있음");
  assert.equal(line.title, "-24 dBFS");
  assert.equal(line.dataset.verdict, "signal");
  p.s.lvl.moving = false;   // the DSP stopped moving: not a verdict
  await until(() => /확인 중/.test(line.textContent!));
  page.remove();
  const old = srcPiano({ noLevel: true });
  const page2 = mount(old);
  await until(() => !page2.querySelector<HTMLElement>("#bt-ac-card")!.hidden);
  await tick(900);
  assert.equal(page2.querySelector<HTMLElement>("#bt-level")!.hidden, true);
  page2.remove();
  i18n.setLang("en");
});

// (owner 2026-11-14): everyday cards first, one closed developer fold
// at the bottom, nothing about power on this page, and the device that plays now said at the top
test("page layout: what plays now on top; piano settings, choosing / reconnecting, BLE MIDI; developer items in one closed fold; no power actions", async () => {
  i18n.setLang("ko");
  const { ParamStore } = await import("../src/state/store.ts");
  const p = srcPiano({ v5: true, x6: true, x7: true });
  const page = mount(p, { store: new ParamStore(), openConnect: () => {} });
  await until(() => page.querySelectorAll("[data-mac]").length === 2 && !page.querySelector<HTMLElement>("#bt-policy-card")!.hidden);
  // the cards in order
  const cards = [...page.querySelectorAll<HTMLElement>(":scope > section")].map((s) => s.id);
  assert.deepEqual(cards, ["bt-audio-card", "bt-piano-card", "bt-policy-card", "bt-midi-card", "bt-dev-card"]);
  assert.equal(page.querySelector("#bt-dev-card")!, page.lastElementChild, "the developer fold is last");
  assert.equal(page.querySelector("#bt-policy-card h3")!.textContent, "기기 고르기 · 다시 연결");
  // the developer fold: closed, and it holds the module restart, AUTOCALL, the late stop and the module lines
  const fold = page.querySelector<HTMLDetailsElement>("#bt-dev-card > details#bt-dev-fold")!;
  assert.equal(fold.open, false);
  assert.equal(fold.querySelector("summary")!.textContent, "고급 · 개발자용");
  for (const id of ["#bt-src-restart", "#bt-ac-card", "#bt-ac-read", "#bt-src-late", "#bt-src-dev", "#bt-src-log"]) assert.ok(fold.querySelector(id), id);
  await until(() => !!page.querySelector("#bt-src-latestop"));
  assert.ok(fold.contains(page.querySelector("#bt-src-latestop")), "the late-stop experiment is in the fold");
  assert.equal(page.querySelector<HTMLElement>("#bt-restart-box")!.hidden, false, "source manager: the restart is offered");
  // outside the fold: no AUTOCALL wording, no module lines, no experiment (the reboot-loop warning, hidden here, may
  // offer AUTOCALL off)
  const shown = (el: Element) => { const c = el.cloneNode(true) as Element; c.querySelectorAll("[hidden]").forEach((x) => x.remove()); return c.textContent!; };
  for (const s of page.querySelectorAll<HTMLElement>(":scope > section:not(#bt-dev-card)")) assert.doesNotMatch(shown(s), /AUTOCALL|모듈 줄|실험/, s.id);
  // power belongs to Settings → 전원: no standby / off / wake on this page
  assert.equal(page.querySelector("[data-action=standby-now], [data-action=remote-off], [data-action=wake], select[name=standbyCap]"), null);
  assert.ok(![...page.querySelectorAll("button")].some((b) => /지금 대기|끄기\(대기\)|대기 해제/.test(b.textContent ?? "")));
  // the piano's own Bluetooth settings: second card
  assert.deepEqual([...page.querySelectorAll("#bt-piano-card .row > span:first-child")].map((s) => s.textContent), ["블루투스 오디오", "블루투스 오디오 음량", "블루투스 MIDI"]);
  // what plays now: the selected, streaming PC; then the phone picked but silent
  const now = page.querySelector<HTMLElement>("#bt-now")!;
  await until(() => !now.hidden);
  assert.equal(page.querySelector("#bt-audio-card h3")!.nextElementSibling, now, "first line of the card");
  assert.equal(now.textContent, "지금 피아노로 듣는 기기: …13:23");
  assert.equal(now.dataset.playing, "1");
  p.s.devs[0]!.state &= ~1; p.s.devs[0]!.state2 &= ~1;
  p.s.devs[1]!.state2 |= 1;
  await until(() => /고른 기기/.test(now.textContent!));
  assert.equal(now.textContent, "고른 기기: …11:a1 — 아직 소리가 오지 않음");
  p.s.devs[1]!.state2 &= ~1;
  await until(() => /아직 고른 기기가 없습니다/.test(now.textContent!));
  // AUTOCALL on: the piano's reconnect choice says it is not used now
  const rcAc = page.querySelector<HTMLElement>("#bt-rc-ac")!;
  assert.equal(rcAc.hidden, true);
  p.s.ac.state = 1;
  await until(() => !rcAc.hidden);
  assert.match(rcAc.textContent!, /지금은 쓰이지 않습니다/);
  page.remove();
  i18n.setLang("en");
});
