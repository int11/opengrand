// Info > Developer: 「RAM 카나리」 (0x7D op 0x20, ngver 4 - REMOTE_PROTOCOL.md section 10.13) against a scripted piano: the status
// and region records decoded, arm (default T1-A + T1-B) / read / disarm, the not-bound answer, an older payload that
// stays silent, the testhooks helper, and the ko / ja strings.
import { tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const { PianoSession } = await import("../src/protocol/session.ts");
const ng = await import("../src/protocol/ngext.ts");
const { fromHex, decodeFrame } = await import("../src/protocol/sysex.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const i18n = await import("../src/i18n.ts");
const { ramCanarySection, canaryRegionText } = await import("../src/ui/ramcanary.ts");

async function until(cond: () => boolean, ms = 3000): Promise<void> {
  for (let t = 0; t < ms && !cond(); t += 20) await tick(20);
}
const hex = (b: number) => b.toString(16).padStart(2, "0");
const le16 = (v: number) => [v & 0xff, (v >>> 8) & 0xff];
const le32 = (v: number) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];

function statusRaw(phase: number, armed: number, dirty: number): number[] {
  return [1, 8, phase, 0, ...le16(armed), ...le16(phase === 2 ? armed : 0), ...le16(dirty), ...le16(2048), ...le32(1990),
    ...le32(2024448), ...le32(2048000), ...le32(180), ...le32(231)];
}
function regionRaw(i: number, armed: boolean, changed: boolean): number[] {
  const r = ng.RAM_CANARY_REGIONS[i]!;
  return [armed ? 2 : 0, armed ? 1 : 0, 0, 0, ...le32(r.lo), ...le32(r.hi), ...le32(512), ...le32(armed ? 3 : 0),
    ...le32(changed ? 2 : 0), ...le32(changed ? 3 : 0), ...le32(changed ? r.lo + 0x100 : 0), ...le32(changed ? r.lo + 0x20000 : 0),
    ...le32(changed ? r.lo + 0x100 : 0), ...le32(changed ? 0x12345678 : 0), ...le32(changed ? 2 : 0)];
}

test("codec: status / region records (ramcanary.h layout 1), not-bound arm", () => {
  const s = ng.decodeRamCanaryStatus(Uint8Array.from(statusRaw(2, 3, 2)))!;
  assert.deepEqual([s.regions, s.phase, s.armed, s.filled, s.dirty, s.budget, s.passes, s.filledWords, s.verifiedWords, s.lastUs, s.maxUs],
    [8, "verifying", 3, 3, 2, 2048, 1990, 2024448, 2048000, 180, 231]);
  assert.equal(ng.decodeRamCanaryStatus(new Uint8Array(31)), null);
  const r = ng.decodeRamCanaryRegion(1, Uint8Array.from(regionRaw(1, true, true)))!;
  assert.deepEqual([r.state, r.armed, r.lo, r.hi, r.sweeps, r.changed, r.changedMax, r.firstAddr, r.firstValue, r.firstSweep, r.maxAddr],
    ["verifying", true, 0x41944000, 0x41cff000, 3, 2, 3, 0x41944100, 0x12345678, 2, 0x41964000]);
  // the eight blocks: 4 KB guards in, R1 (0x40173A14-0x401FFFFF) never in
  for (const b of ng.RAM_CANARY_REGIONS) {
    assert.equal(b.lo % 0x1000, 0);
    assert.ok(b.hi <= 0x40173a14 || b.lo >= 0x40200000, b.name);
  }
  assert.deepEqual(ng.ngNotBound(fromHex("F0 7D 43 41 37 39 60 01 02 00 00 F7")), { op: 0x20, sub: 0x01 });
  i18n.setLang("en");
  assert.equal(canaryRegionText(r), "T1-B: checking · 3 sweeps · changed now 2 (most 3) · first 0x41944100 = 0x12345678 (sweep 2) · 0x41944100–0x41964000");
  assert.equal(canaryRegionText(ng.decodeRamCanaryRegion(5, Uint8Array.from(regionRaw(5, false, false)))!), "T2-a: not armed");
});

function piano(opts: { canary: boolean; bound?: boolean }) {
  const [app, p] = LoopbackLink.pair();
  const out = (h: string) => p.send(fromHex(h));
  const asked: number[][] = [];
  let armed = 0;
  p.onMessage((m) => {
    if (m[1] === 0x7d) {
      const op = m[6]!, sub = m.length > 8 ? m[7]! : 0;
      if (op === 0x01) out("F0 7D 43 41 37 39 41 04 02 13 3B 77 7F 3F F7");
      if (op === 0x20 && opts.canary) {
        asked.push([...m.slice(7, m.length - 1)]);
        if (sub === 0x00) out(`F0 7D 43 41 37 39 60 00 ${ng.pack7(statusRaw(armed ? 2 : 0, armed, armed & 2)).map(hex).join(" ")} F7`);
        if (sub === 0x02) out(`F0 7D 43 41 37 39 60 02 ${hex(m[8]!)} ${ng.pack7(regionRaw(m[8]!, !!(armed & (1 << m[8]!)), m[8] === 1)).map(hex).join(" ")} F7`);
        if (sub === 0x01) {
          if (opts.bound !== false) armed = m[8]! | (m[9]! << 7);
          out(`F0 7D 43 41 37 39 60 01 ${opts.bound === false ? "02" : "00"} ${hex(armed & 0x7f)} ${hex(armed >> 7)} F7`);
        }
        if (sub === 0x03) {
          armed = 0;
          out("F0 7D 43 41 37 39 60 03 00 00 00 F7");
        }
      }
      return;
    }
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01) out("F0 40 7F 7E 04 1E 02 00 40 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) out("F0 40 7F 7E 04 1E 05 00 00 02 F7");
    if (f.fn === 0x02) out("F0 40 7F 03 04 1E 5F 7F 7F F7");
  });
  return { app, asked };
}

test("Info > Developer: RAM canary arm (T1-A + T1-B by default) / read / disarm", async () => {
  i18n.setLang("en");
  const { app, asked } = piano({ canary: true });
  const session = new PianoSession(app, { ackTimeoutMs: 50 });
  await session.connect(100);
  const sec = ramCanarySection(() => session);
  assert.equal(sec.id, "ram-canary");
  const boxes = [...sec.querySelectorAll<HTMLInputElement>("input[data-region]")];
  assert.deepEqual(boxes.map((b) => b.checked), [true, true, false, false, false, false, false, false]);
  sec.querySelector<HTMLButtonElement>("[data-action=canary-arm]")!.click();
  await until(() => sec.querySelectorAll(".canary-region").length === 8);
  assert.match(sec.querySelector("#canary-status")!.textContent!, /Changed words found|Armed/);
  const row = (i: number) => sec.querySelector<HTMLElement>(`.canary-region[data-region="${i}"]`)!;
  assert.equal(row(0).dataset.state, "verifying");
  assert.equal(row(1).dataset.changed, "2");
  assert.equal(row(1).dataset.first, "0x41944100");
  assert.equal(row(2).dataset.state, "off");
  assert.match(sec.querySelector("#canary-summary")!.textContent!, /checking \(T1-A \+ T1-B\) · 1990 passes · filled 2024448 · checked 2048000 words · step 180 µs \(longest 231 µs\)/);
  assert.equal(sec.querySelector<HTMLElement>("#canary-summary")!.dataset.dirty, "2");
  // all eight unticked: refused locally, nothing sent
  boxes.forEach((b) => (b.checked = false));
  const n = asked.length;
  sec.querySelector<HTMLButtonElement>("[data-action=canary-arm]")!.click();
  await until(() => /Pick at least one block/.test(sec.querySelector("#canary-status")!.textContent!));
  assert.equal(asked.length, n);
  sec.querySelector<HTMLButtonElement>("[data-action=canary-disarm]")!.click();
  await until(() => /Disarmed/.test(sec.querySelector("#canary-status")!.textContent!));
  await until(() => row(0)?.dataset.state === "off");
  assert.deepEqual(asked.slice(0, 2), [[0x01, 0x03, 0x00], [0x00]]);
  assert.deepEqual(asked.slice(2, 10), [0, 1, 2, 3, 4, 5, 6, 7].map((i) => [0x02, i]));
  assert.ok(asked.some((a) => a.length === 1 && a[0] === 0x03));
  for (const lang of ["ko", "ja"] as const) {
    i18n.setLang(lang);
    for (const k of ["RAM canary", "Arm", "Disarm", "filling", "checking", "Pick at least one block.", "Read.", "{name}: not armed", "{name}: no answer",
      "{phase} ({armed}) · {passes} passes · filled {filled} · checked {checked} words · step {last} µs (longest {max} µs)",
      "{name}: {state} · {sweeps} sweeps · changed now {changed} (most {max})", "first {addr} = {value} (sweep {sweep}) · {lo}–{hi}",
      "No answer from the piano (this firmware may not have the RAM canary).", "Changed words found: those blocks are written by something else.",
      "Armed: {regions}. The piano fills them, then keeps checking.", "Disarmed. The results stay readable until the next arm.",
      "Fills unused RAM blocks with a pattern and watches whether anything else writes there - the field test before OpenGrand uses them. Nothing is written until you arm it; switching the piano off and on ends it."])
      assert.notEqual(i18n.t(k), k, `${lang}: ${k}`);
  }
  i18n.setLang("ko");
  assert.equal(i18n.t("RAM canary"), "RAM 카나리");
  i18n.setLang("en");
});

test("Info > Developer: RAM canary arm from an unbound app, and an older payload that stays silent", async () => {
  i18n.setLang("en");
  {
    const { app } = piano({ canary: true, bound: false });
    const session = new PianoSession(app, { ackTimeoutMs: 50 });
    await session.connect(100);
    const sec = ramCanarySection(() => session);
    sec.querySelector<HTMLButtonElement>("[data-action=canary-arm]")!.click();
    await until(() => /controlled by another device/.test(sec.querySelector("#canary-status")!.textContent!));
    assert.equal(sec.querySelectorAll(".canary-region").length, 0);
  }
  {
    const { app } = piano({ canary: false });
    const session = new PianoSession(app, { ackTimeoutMs: 50 });
    await session.connect(100);
    const sec = ramCanarySection(() => session);
    sec.querySelector<HTMLButtonElement>("[data-action=canary-read]")!.click();
    await until(() => /may not have the RAM canary/.test(sec.querySelector("#canary-status")!.textContent!), 3000);
    assert.equal(sec.querySelectorAll(".canary-region").length, 0);
  }
});
