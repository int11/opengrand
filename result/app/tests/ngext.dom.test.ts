// Information screen: OpenGrand custom-firmware detection and the NOR dump control.
import { tick } from "./_dom.ts";
import { beforeEach, test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const { createContext } = await import("../src/ui/context.ts");
const { infoScreen } = await import("../src/ui/info.ts");
const { PianoSession } = await import("../src/protocol/session.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const i18n = await import("../src/i18n.ts");

beforeEach(() => {
  localStorage.clear();
  i18n.setLang("en");
});

/** opts.cave = null -> stock firmware (0x7D dropped silently). */
function pianoWithCave(opts: { features?: number | null } = {}) {
  const [app, piano] = LoopbackLink.pair();
  let state = 0;
  let chunk = 0;
  piano.onMessage((m) => {
    if (m[1] !== 0x7d || opts.features === null) return;
    const reply = (op: number, data: number[]) =>
      piano.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...data, 0xf7]));
    if (m[6] === 0x01) reply(0x01, [1, 2, 19, opts.features ?? 0x07, 0]);
    if (m[6] === 0x02) {
      state = 2;
      chunk = 48;
      reply(0x02, [0]);
    }
    if (m[6] === 0x03) reply(0x03, [state, (chunk >> 7) & 0x7f, chunk & 0x7f, 0]);
  });
  return new PianoSession(app, { handshake: false });
}

async function screenFor(session: InstanceType<typeof PianoSession> | null) {
  const el = infoScreen(createContext(new ParamStore(), { session: () => session }));
  document.body.append(el);
  return el;
}

test("Information screen reports a cave-equipped firmware and offers the dump", async () => {
  const el = await screenFor(pianoWithCave());
  (el.querySelector("#ng-check") as HTMLButtonElement).click();
  await tick(30);
  assert.match(el.querySelector("#ng-firmware")!.textContent!, /OpenGrand rev 1 on V2\.19/);
  assert.match(el.querySelector("#ng-firmware")!.textContent!, /nordump/);
  assert.equal((el.querySelector("#ng-dump") as HTMLElement).hidden, false);

  (el.querySelector("#ng-dump button") as HTMLButtonElement).click();
  await tick(1100);
  assert.match(el.querySelector("#ng-dump-status")!.textContent!, /Dumping… 50% \(48\/96 chunks\)/);
  el.remove();
});

test("stock firmware: no reply is reported as stock and the dump stays hidden", async () => {
  const el = await screenFor(pianoWithCave({ features: null }));
  (el.querySelector("#ng-check") as HTMLButtonElement).click();
  await tick(900);
  assert.equal(el.querySelector("#ng-firmware")!.textContent, "Stock firmware (no reply)");
  assert.equal((el.querySelector("#ng-dump") as HTMLElement).hidden, true);
  el.remove();
});

test("an identify-only cave hides the dump control", async () => {
  const el = await screenFor(pianoWithCave({ features: 0x03 }));
  (el.querySelector("#ng-check") as HTMLButtonElement).click();
  await tick(30);
  assert.match(el.querySelector("#ng-firmware")!.textContent!, /identify, status/);
  assert.equal((el.querySelector("#ng-dump") as HTMLElement).hidden, true);
  el.remove();
});

test("without a connection the check asks to connect first", async () => {
  const el = await screenFor(null);
  const btn = el.querySelector("#ng-check") as HTMLButtonElement;
  assert.equal(btn.disabled, true);
  el.remove();
});

test("a probe cave shows the diagnostic box and prints op results", async () => {
  const { pack7 } = await import("../src/protocol/ngext.ts");
  const [app, piano] = LoopbackLink.pair();
  piano.onMessage((m) => {
    if (m[1] !== 0x7d) return;
    const reply = (op: number, data: number[]) =>
      piano.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...data, 0xf7]));
    if (m[6] === 0x01) reply(0x01, [1, 2, 19, 0x7b, 0x01]);
    if (m[6] === 0x03) reply(0x03, [0, 0, 0, 0]);
    if (m[6] === 0x05) reply(0x05, pack7([2, 3, 0, 0, 0, 0, 8, 0]));
    if (m[6] === 0x09 && m[7] === 0) reply(0x09, [0, ...pack7([2, 1, 0, 0, 0, 0x40, 0x53, 0x17, 0x40, 0x40, 0x53, 0x17, 0x40, 0, 0x53, 0x17, 0x40, 0, 0, 0x20, 0x40, 9, 0, 0, 0])]);
  });
  const el = await screenFor(new PianoSession(app, { handshake: false }));
  (el.querySelector("#ng-check") as HTMLButtonElement).click();
  await tick(30);
  assert.equal((el.querySelector("#ng-probe") as HTMLElement).hidden, false);
  assert.equal((el.querySelector("#ng-dump") as HTMLElement).hidden, true);

  (el.querySelector("#ng-probe-port") as HTMLButtonElement).click();
  await tick(30);
  assert.equal(el.querySelector("#ng-probe-out")!.textContent, "port 2 · binding 3 · port3 forwarding 0 · length 8");

  (el.querySelector("#ng-probe-canary") as HTMLButtonElement).click();
  await tick(30);
  assert.match(el.querySelector("#ng-probe-out")!.textContent!, /canary armed · 0x40175300–0x40200000 · 9 ticks/);
  assert.match(el.querySelector("#ng-probe-out")!.textContent!, /changed words 1 · first 0x40175340/);
  el.remove();
});

test("the baseline cave (no canary bit) shows the probe ops but hides the canary buttons", async () => {
  const [app, piano] = LoopbackLink.pair();
  piano.onMessage((m) => {
    if (m[1] !== 0x7d) return;
    const reply = (op: number, data: number[]) =>
      piano.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...data, 0xf7]));
    if (m[6] === 0x01) reply(0x01, [1, 2, 19, 0x3b, 0x01]);
    if (m[6] === 0x03) reply(0x03, [0, 0, 0, 0]);
  });
  const el = await screenFor(new PianoSession(app, { handshake: false }));
  (el.querySelector("#ng-check") as HTMLButtonElement).click();
  await tick(30);
  const hidden = (id: string) => (el.querySelector(id) as HTMLElement).hidden;
  assert.equal(hidden("#ng-probe"), false);
  for (const id of ["#ng-probe-port", "#ng-probe-link", "#ng-probe-apo", "#ng-probe-btlog"]) assert.equal(hidden(id), false, id);
  assert.equal(hidden("#ng-probe-arm"), true);
  assert.equal(hidden("#ng-probe-canary"), true);
  el.remove();
});

test("the dump-only cave keeps the probe box hidden", async () => {
  const el = await screenFor(pianoWithCave());
  (el.querySelector("#ng-check") as HTMLButtonElement).click();
  await tick(30);
  assert.equal((el.querySelector("#ng-probe") as HTMLElement).hidden, true);
  el.remove();
});

test("core_probe: the core self-test button decodes the cave's reply", async () => {
  // reply bytes of op 0x0A as the core_probe cave produced them in the emulator
  // (the core_probe round trip, first tick after boot; the image is no longer built - the bytes stay a fixture)
  const CORE_REPLY = [0x34, 0x01, 0x09, 0x7f, 0x01, 0x0f, 0x47, 0x49, 0x56, 0x10, 0x29, 0x13, 0x5d, 0x6a, 0x57, 0x29,
    0x02, 0x11, 0x79, 0x41, 0x00, 0x06, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x4c,
    0x4c, 0x00];
  const [app, piano] = LoopbackLink.pair();
  piano.onMessage((m) => {
    if (m[1] !== 0x7d) return;
    const reply = (op: number, data: number[]) =>
      piano.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...data, 0xf7]));
    if (m[6] === 0x01) reply(0x01, [1, 2, 19, 0x3b, 0x03]);
    if (m[6] === 0x03) reply(0x03, [0, 0, 0, 0]);
    if (m[6] === 0x0a) reply(0x0a, CORE_REPLY);
  });
  const el = await screenFor(new PianoSession(app, { handshake: false }));
  (el.querySelector("#ng-check") as HTMLButtonElement).click();
  await tick(30);
  assert.match(el.querySelector("#ng-firmware")!.textContent!, /core/);
  assert.equal((el.querySelector("#ng-probe-core") as HTMLElement).hidden, false);
  assert.equal((el.querySelector("#ng-probe-canary") as HTMLElement).hidden, true);
  (el.querySelector("#ng-probe-core") as HTMLButtonElement).click();
  await tick(30);
  const out = el.querySelector("#ng-probe-out")!.textContent!;
  assert.match(out, /core self-test 9\/9 passed/);
  assert.match(out, /store CRC 0x1049C78F -> 0xEA5D93A9 · kproto out 6 frames 65 B/);
  assert.match(out, /core ticks 1 · 0 ms · time 5000200 us · R1 guards ok/);
  el.remove();
});

test("probe caves without the core bit hide the core self-test button", async () => {
  const el = await screenFor(pianoWithCave({ features: 0x3b }));
  (el.querySelector("#ng-check") as HTMLButtonElement).click();
  await tick(30);
  assert.equal((el.querySelector("#ng-probe-core") as HTMLElement).hidden, true);
  el.remove();
});

// Startup settings card (ui/startup.ts, op 0x1C): the card
// says the mode covers every piano setting except Bluetooth pairing / the module's own settings, and tells when a mode
// or snapshot of an older firmware still leaves our own settings as they are.
const { pack7: ngPack7 } = await import("../src/protocol/ngext/wire.ts");
const { startupSection } = await import("../src/ui/startup.ts");

function pianoWithStartup(status: number[]) {
  const [app, piano] = LoopbackLink.pair();
  piano.onMessage((m) => {
    if (m[1] !== 0x7d) return;
    const reply = (op: number, data: number[]) =>
      piano.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...data, 0xf7]));
    if (m[6] === 0x01) reply(0x01, [1, 2, 19, 0x3b, 0x77, 0x7f]);
    if (m[6] === 0x1c) reply(0x1c, [m[7] === 0xf7 ? 0 : m[7]!, 0, ...ngPack7(Uint8Array.from(status))]);
  });
  const s = new PianoSession(app, { handshake: false });
  (s as unknown as { state: string }).state = "ready";   // the card needs a synced session (no Kawai handshake here)
  return s;
}

function startupRaw(mode: number, flags: number, ng: number[] | null): number[] {
  const r = new Array<number>(ng ? 40 : 36).fill(0);
  r[0] = 1; r[1] = mode; r[2] = 0x01 | 0x08 | flags; r[3] = 3;
  if (ng) for (let i = 0; i < 4; i++) r[36 + i] = ng[i]!;
  return r;
}

test("startup card: covers every setting; an older firmware's mode / snapshot is pointed out (ko)", async () => {
  i18n.setLang("ko");
  const card = async (raw: number[]) => {
    const el = startupSection(createContext(new ParamStore(), { session: () => pianoWithStartup(raw) }));
    document.body.append(el);
    for (let i = 0; i < 50 && el.querySelector<HTMLElement>("#startup-ng")!.hidden; i++) await tick(10);
    return el;
  };
  let el = await card(startupRaw(2, 0x04, [0x01 | 0x04, 2, 3, 21]));   // power-off, P with our settings
  assert.equal(el.hidden, false);
  assert.match(el.querySelector("#startup-scope")!.textContent!, /모든 설정.*블루투스 기기 · 자동 재연결 · 자동 전원 꺼짐.*블루투스 페어링과 블루투스 모듈 자체 설정만 빠집니다/);
  assert.equal(el.querySelector("#startup-older"), null);
  el.remove();
  el = await card(startupRaw(2, 0x04, [0, 0, 0, 0]));                  // P written by a firmware before §7
  assert.match(el.querySelector("#startup-older")!.textContent!, /다음 변경부터/);
  el.remove();
  el = await card(startupRaw(0, 0, [0, 0, 0, 0]));                     // stock chosen by a firmware before §7
  assert.match(el.querySelector("#startup-older")!.textContent!, /모드를 다시 고를 때까지/);
  el.remove();
  el = await card(startupRaw(0, 0, [0x01, 1, 3, 0]));                  // stock chosen now: factory for everything else
  assert.equal(el.querySelector("#startup-older"), null);
  assert.match(el.textContent!, /나머지는 모두 공장 초기값/);
  el.remove();
  el = await card(startupRaw(1, 0x02, null));                          // a firmware before §7 (36-byte status): no note
  assert.equal(el.querySelector("#startup-older"), null);
  el.remove();
});
