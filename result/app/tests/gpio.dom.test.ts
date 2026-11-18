// Settings > Power > Developer: 「GPIO 읽기」 (0x7D op 0x17, read only - REMOTE_PROTOCOL.md section 10.11) against a scripted piano: the snapshot decoded per named pin, the change
// log, change notices on / off, an older payload that stays silent, and the ko / ja strings.
import { change, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const { PianoSession } = await import("../src/protocol/session.ts");
const { ParamStore } = await import("../src/state/store.ts");
const { createContext } = await import("../src/ui/context.ts");
const { powerDevSection } = await import("../src/ui/power.ts");
const ng = await import("../src/protocol/ngext.ts");
const { fromHex, decodeFrame } = await import("../src/protocol/sysex.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const i18n = await import("../src/i18n.ts");
const { gpioEventText } = await import("../src/ui/gpio.ts");

async function until(cond: () => boolean, ms = 3000): Promise<void> {
  for (let t = 0; t < ms && !cond(); t += 20) await tick(20);
}
const hex = (b: number) => b.toString(16).padStart(2, "0");
const le32 = (v: number) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];

/** A snapshot record (gpiodiag.h gd_encode_snapshot): the BootProgram DSP-stage table values + a few live bits. */
function snapRaw(): number[] {
  const raw = new Array(130).fill(0);
  raw[0] = 1; raw[1] = 0x02; raw[2] = 0x7f; raw[3] = 5;
  const words = [
    0xfb2bbc00, 0x04440000 | 0x4000, 0x00020400,  // 23: DIR, OUT (GP3[10], GP3[6], GP3[2] and GP2[14] high), IN (GP3[1] released, GP2[10] ready)
    0x51a94f60, 0x8000a000, 0x40000000,           // 45: OUT GP5[15], GP4[15], GP4[13]; IN GP5[14] (headphones in)
    0x88f80000, 0x00000800, 0x00000800,           // 67: OUT GP6[11] only (headphones in: amp off, unmuted)
  ];
  words.forEach((w, i) => raw.splice(5 + 4 * i, 4, ...le32(w)));
  raw[41] = 1; raw[42] = 0; raw[43] = 0; raw[44] = 0x21; raw[45] = 0xdd;
  for (let i = 0; i < 20; i++) raw.splice(46 + 4 * i, 4, ...le32(0x11111111 * (i % 10)));
  raw.splice(126, 4, ...le32(65432));
  return raw;
}

function ev(seq: number, ms: number, sig: number, level: number): number[] {
  return [seq & 0xff, seq >> 8, ...le32(ms), sig, level];
}

test("codec: snapshot / log / notice decode (gpiodiag.h layout 1)", () => {
  const s = ng.decodeGpioSnapshot(Uint8Array.from(snapRaw()))!;
  assert.equal(s.started, true);
  assert.equal(s.notify, false);
  assert.equal(s.noticePort, null);
  assert.equal(s.dir[23], 0xfb2bbc00);
  assert.equal(s.in[45], 0x40000000);
  assert.equal(s.chipA, 0xdd);
  assert.equal(s.ms, 65432);
  assert.equal(s.pinmux[3], 0x33333333);
  const pins = Object.fromEntries(ng.gpioPins(s).map((p) => [p.name, p]));
  // DIR bit 1 = input: GP3[8] / GP3[1] / GP5[14] / GP2[10] inputs, the rest outputs
  assert.deepEqual(["GP3[8]", "GP3[1]", "GP5[14]", "GP2[10]"].map((n) => pins[n]!.dir), ["in", "in", "in", "in"]);
  assert.deepEqual(["GP6[15]", "GP6[11]", "GP6[12]", "GP6[13]", "GP7[2]", "GP7[12]", "GP3[10]", "GP2[14]", "GP4[15]", "GP4[13]", "GP5[15]", "GP3[2]", "GP3[6]"]
    .map((n) => pins[n]!.dir), new Array(13).fill("out"));
  assert.equal(pins["GP3[8]"]!.level, 0);
  assert.equal(pins["GP5[14]"]!.level, 1);
  assert.equal(pins["GP6[15]"]!.level, 0);
  assert.equal(pins["GP6[11]"]!.level, 1);
  assert.equal(pins["GP3[10]"]!.level, 1);
  assert.equal(pins["GP2[14]"]!.level, 1);
  assert.equal(pins["GP5[15]"]!.level, 1);
  assert.equal(pins["GP3[1]"]!.level, 1);
  assert.equal(ng.decodeGpioSnapshot(new Uint8Array(129)), null);
  const log = ng.decodeGpioLog(Uint8Array.from([1, 2, 7, 0, ...ev(6, 1000, 0, 1), ...ev(7, 1008, 3, 0)]))!;
  assert.equal(log.seq, 7);
  assert.deepEqual(log.events.map((e) => [e.pin, e.level, e.ms]), [["GP3[8]", 1, 1000], ["GP6[15]", 0, 1008]]);
  const msg = fromHex(`F0 7D 43 41 37 39 57 7F ${ng.pack7([1, 9, 0, ...ev(9, 2000, 2, 0)]).map(hex).join(" ")} F7`);
  assert.deepEqual(ng.parseGpioNotice(msg)!.events.map((e) => [e.seq, e.pin, e.level]), [[9, "GP5[14]", 0]]);
  assert.equal(ng.parseGpioNotice(fromHex("F0 7D 43 41 37 39 57 00 00 F7")), null);
  // 17 02 01 refused on an unbound port: the shared not-bound detector knows it
  assert.deepEqual(ng.ngNotBound(fromHex("F0 7D 43 41 37 39 57 02 02 02 7F F7")), { op: 0x17, sub: 0x02 });
  i18n.setLang("en");
  assert.equal(gpioEventText(log.events[0]!), "1.000 s  GP3[8] (Mute request input) → 1");
});

function piano(opts: { gpio: boolean }) {
  const [app, p] = LoopbackLink.pair();
  const out = (h: string) => p.send(fromHex(h));
  const asked: number[][] = [];
  const polHex = ng.pack7(ng.encodePowerPolicy(ng.POWER_POLICY_DEFAULT)).map(hex).join(" ");
  p.onMessage((m) => {
    if (m[1] === 0x7d) {
      const op = m[6]!, sub = m[7] ?? 0;
      if (op === 0x01) out("F0 7D 43 41 37 39 41 01 02 13 3B 77 7F 38 F7");
      if (op === 0x18) out(`F0 7D 43 41 37 39 58 ${hex(sub)} 00 ${polHex} F7`);
      if (op === 0x19 && sub === 0x00) out("F0 7D 43 41 37 39 59 00 00 00 F7");
      if (op === 0x17 && opts.gpio) {
        asked.push([...m.slice(7, m.length - 1)]);
        if (m.length === 8 || sub === 0x00) out(`F0 7D 43 41 37 39 57 00 ${ng.pack7(snapRaw()).map(hex).join(" ")} F7`);
        if (sub === 0x01) out(`F0 7D 43 41 37 39 57 01 ${ng.pack7([1, 2, 5, 0, ...ev(4, 30000, 2, 1), ...ev(5, 30000, 3, 0)]).map(hex).join(" ")} F7`);
        if (sub === 0x02) out(`F0 7D 43 41 37 39 57 02 00 ${m[8] ? "03 02" : "02 7F"} F7`);
      }
      return;
    }
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01) out("F0 40 7F 7E 04 1E 02 00 40 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) out("F0 40 7F 7E 04 1E 05 00 00 02 F7");
    if (f.fn === 0x02) out("F0 40 7F 03 04 1E 5F 7F 7F F7");
  });
  return { app, out, asked };
}

test("Info > Developer > Power: Read GPIO lists every named pin, the log, change notices", async () => {
  i18n.setLang("en");
  const { app, out, asked } = piano({ gpio: true });
  const session = new PianoSession(app, { ackTimeoutMs: 50 });
  await session.connect(100);
  const store = new ParamStore();
  const sec = powerDevSection(createContext(store, { session: () => session }));
  await until(() => !!sec.querySelector("[data-action=gpio-read]"));
  // owner 2026-11-16: in the Info tab's developer card (system.dom checks the fold)
  assert.equal(sec.id, "power-dev");
  assert.ok(sec.querySelector("#gpio-diag"));
  await until(() => !sec.hidden);
  assert.equal(sec.hidden, false);
  sec.querySelector<HTMLButtonElement>("[data-action=gpio-read]")!.click();
  await until(() => sec.querySelectorAll(".gpio-event").length === 2);
  const row = (n: string) => sec.querySelector<HTMLElement>(`.gpio-pin[data-pin="${n}"]`)!;
  assert.equal(sec.querySelectorAll(".gpio-pin").length, ng.GPIO_PINS.length);
  assert.equal(row("GP3[8]").dataset.level, "0");
  assert.equal(row("GP3[8]").dataset.dir, "in");
  assert.match(row("GP3[8]").textContent!, /GP3\[8\] Mute request input \*input · now 0 \(OUT 0 · IN 0\)/);
  assert.equal(row("GP5[14]").dataset.level, "1");
  assert.equal(row("GP6[15]").dataset.level, "0");
  assert.equal(row("GP6[11]").dataset.level, "1");
  assert.equal(row("GP3[10]").dataset.dir, "out");
  assert.match(sec.querySelector("#gpio-stock")!.textContent!, /headphones 1 · GP3\[8\] sample 0 · GP3\[8\] event 0 · model byte 0x21 · chip A base 0xDD/);
  assert.match(sec.querySelector("#gpio-raw")!.textContent!, /23: DIR 0xFB2BBC00 OUT 0x04444000 IN 0x00020400/);
  assert.match(sec.querySelector("#gpio-status")!.textContent!, /65\.4 s/);
  // newest first
  assert.deepEqual([...sec.querySelectorAll<HTMLElement>(".gpio-event")].map((e) => e.dataset.signal), ["GP6[15]", "GP5[14]"]);
  // change notices: on (bound), a notice appears on top, off
  const box = sec.querySelector<HTMLInputElement>("[data-action=gpio-notify]")!;
  change(box, true);
  await until(() => /Change notices on/.test(sec.querySelector("#gpio-status")!.textContent!));
  out(`F0 7D 43 41 37 39 57 7F ${ng.pack7([1, 6, 0, ...ev(6, 31000, 0, 1)]).map(hex).join(" ")} F7`);
  await until(() => sec.querySelectorAll(".gpio-event").length === 3);
  const top = sec.querySelector<HTMLElement>(".gpio-event")!;
  assert.equal(top.dataset.signal, "GP3[8]");
  assert.match(top.textContent!, /● 31\.000 s {2}GP3\[8\] \(Mute request input\) → 1/);
  change(box, false);
  await until(() => /Change notices off/.test(sec.querySelector("#gpio-status")!.textContent!));
  out(`F0 7D 43 41 37 39 57 7F ${ng.pack7([1, 7, 0, ...ev(7, 32000, 0, 0)]).map(hex).join(" ")} F7`);
  await tick(100);
  assert.equal(sec.querySelectorAll(".gpio-event").length, 3, "off: notices are not listed");
  assert.deepEqual(asked, [[0x00], [0x01], [0x02, 0x01], [0x02, 0x00]]);
  // the strings exist in Korean and Japanese
  for (const lang of ["ko", "ja"] as const) {
    i18n.setLang(lang);
    for (const p of ng.GPIO_PINS) assert.notEqual(i18n.t(p.role), p.role, `${lang}: ${p.role}`);
    for (const k of ["Read GPIO", "GPIO (read only)", "Change notices", "input", "output", "Change notices on: pin changes appear below as they happen.",
      "No answer from the piano (this firmware may not have the GPIO diagnostic).", "{dir} · now {level} (OUT {out} · IN {in})", "{s} s  {pin} ({role}) → {level}"])
      assert.notEqual(i18n.t(k), k, `${lang}: ${k}`);
  }
  i18n.setLang("ko");
  assert.equal(i18n.t("Read GPIO"), "GPIO 읽기");
  i18n.setLang("en");
});

test("Info > Developer > Power: an older payload does not answer op 0x17", async () => {
  i18n.setLang("en");
  const { app } = piano({ gpio: false });
  const session = new PianoSession(app, { ackTimeoutMs: 50 });
  await session.connect(100);
  const store = new ParamStore();
  const sec = powerDevSection(createContext(store, { session: () => session }));
  await until(() => !!sec.querySelector("[data-action=gpio-read]"));
  sec.querySelector<HTMLButtonElement>("[data-action=gpio-read]")!.click();
  await until(() => /may not have the GPIO diagnostic/.test(sec.querySelector("#gpio-status")!.textContent!), 3000);
  assert.match(sec.querySelector("#gpio-status")!.textContent!, /No answer from the piano/);
  assert.equal(sec.querySelectorAll(".gpio-pin").length, 0);
});
