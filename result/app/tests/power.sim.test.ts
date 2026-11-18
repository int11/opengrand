// Settings > Power (OpenGrand power policy, 0x7D op 0x18 / 0x19) against the real firmware core
// (ports/sim), and against a stock piano that does not answer 0x7D (section stays hidden).
import { change, tapTwice, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const { SIM_BIN, SimProcessLink } = await import("../scripts/sim-link.ts");
const { PianoSession } = await import("../src/protocol/session.ts");
const { ParamStore } = await import("../src/state/store.ts");
const { createContext } = await import("../src/ui/context.ts");
const { settingsScreen } = await import("../src/ui/screens.ts");
const { powerDevSection } = await import("../src/ui/power.ts");
const ng = await import("../src/protocol/ngext.ts");
const { fromHex, decodeFrame } = await import("../src/protocol/sysex.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const i18n = await import("../src/i18n.ts");

const skip = !existsSync(SIM_BIN) && "firmware simulator not built (make -C ../firmware sim)";

async function until(cond: () => boolean, ms = 3000): Promise<void> {
  for (let t = 0; t < ms && !cond(); t += 20) await tick(20);
}

async function boot(flash: string) {
  const link = new SimProcessLink({ args: ["--flash", flash] });
  await link.ready;
  const session = new PianoSession(link, { ackTimeoutMs: 500 });
  const store = new ParamStore();
  store.attach(session);
  await session.connect(2000);
  return { link, session, store };
}

test("codec: policy wire form matches the firmware (power.h)", () => {
  assert.deepEqual(ng.encodePowerPolicy(ng.POWER_POLICY_DEFAULT), [1, 1, 0x2c, 1, 0xe0, 1, 0, 0]);
  assert.deepEqual(ng.decodePowerPolicy(Uint8Array.from([1, 2, 0xff, 0xff, 30, 0, 0, 0])), { apoStandby: false, holdConnected: true, maxHoldMin: ng.HOLD_UNLIMITED, standbyCapMin: 30, btWake: true });
  // TV wake (07_tv_wake): flag bit 2 = standby switches Bluetooth audio off; the default record keeps ota_power's bytes
  assert.deepEqual(ng.encodePowerPolicy({ ...ng.POWER_POLICY_DEFAULT, btWake: false }), [1, 5, 0x2c, 1, 0xe0, 1, 0, 0]);
  assert.equal(ng.decodePowerPolicy(Uint8Array.from([1, 5, 120, 0, 0xe0, 1, 0, 0]))!.btWake, false);
  assert.equal(ng.decodePowerPolicy(Uint8Array.from([2, 0, 0, 0, 30, 0])), null);
  assert.equal(ng.parseStandbyNotice(fromHex("F0 7D 43 41 37 39 59 7F 00 01 F7")), "standby");
  assert.equal(ng.parseStandbyNotice(fromHex("F0 7D 43 41 37 39 59 00 00 01 F7")), null);
  // op 0x1A record (ports/cave/power_cave.c pwc_diag): layout 1, hooks 0x0F, standby, silenced
  const raw = new Uint8Array(48);
  raw.set([1, 0x0f, 1, 0x81, 2, 2, 1, 4]);
  raw.set([0xc8, 0xaf, 0, 0], 8); // counter 45000
  raw.set([3, 0], 36);
  raw[47] = 1;
  const d = ng.decodePowerDiag(raw)!;
  assert.equal(d.hooks, 0x0f);
  assert.equal(d.state, "standby");
  assert.equal(d.silenced, true);
  assert.equal(d.btAudio, "streaming");
  assert.equal(d.store, "mounted");
  assert.equal(d.counter, 45000);
  assert.equal(d.hpSkips, 3);
  assert.equal(d.relight, 1);
  assert.equal(d.wake, 4);
  assert.equal(d.tv, undefined);
  assert.equal(ng.decodePowerDiag(new Uint8Array(47)), null);
  // layout 2 (ota_power2): wake cause 5 = Bluetooth stream start, BT module state, latch / ring-reset counters
  const raw2 = new Uint8Array(52);
  raw2.set(raw);
  raw2[0] = 2;
  raw2[7] = 5;
  raw2.set([0x03, 0x00, 1, 2], 48);
  const d2 = ng.decodePowerDiag(raw2)!;
  assert.equal(d2.wake, 5);
  assert.deepEqual(d2.tv, { btModuleOn: true, armed: true, latched: false, rxFlag: 0, unlatches: 1, ringResets: 2 });
  assert.equal(ng.decodePowerDiag(raw2.slice(0, 48)), null);
});

test("Settings > Power: shown for OpenGrand, edits are saved and survive a reboot of the piano", { skip }, async () => {
  i18n.setLang("en");
  const dir = mkdtempSync(join(tmpdir(), "ca79ng-policy-"));
  const flash = join(dir, "flash.bin");
  let { link, session, store } = await boot(flash);
  try {
    const ctx = createContext(store, { session: () => session });
    const el = settingsScreen(store, ctx);
    const sec = el.querySelector<HTMLElement>("#power-policy")!;
    assert.ok(sec);
    await until(() => !sec.hidden);
    assert.equal(sec.hidden, false, "OpenGrand reports feat1 0x10");
    const sel = (name: string) => sec.querySelector<HTMLSelectElement>(`select[name=${name}]`)!;
    // owner defaults: standby at APO, 8 h limit, 5 h hold (2026-11-02), connected-only does not hold
    assert.equal(sel("apoAction").value, "1");
    assert.equal(sel("standbyCap").value, "480");
    assert.equal(sel("maxHold").value, "300");
    assert.deepEqual([...sel("maxHold").options].map((o) => o.value), ["0", "30", "60", "120", "240", "300", "480", "65535"]);
    // the rule in a few words under the setting, the full rule behind 「자세히」
    assert.equal(sec.querySelector("#hold-rule")?.textContent, "While Bluetooth audio plays, the piano stays on for up to 5 h.");
    assert.match(sec.querySelector("#hold-detail")?.textContent ?? "", /up to 5 h; .* after 5 h it switches off right away/);
    // a device that reports paused does not hold
    assert.match(sec.querySelector("#hold-detail")?.textContent ?? "", /reports it is paused counts as stopped\.$/);
    // the power card is the Settings tab's 「전원」 card, with the stock auto power-off row; the Bluetooth hint is gone
    assert.ok(el.querySelector("#power-settings")!.contains(sec));
    assert.equal(el.querySelector("#power-settings h3")!.textContent, "Power");
    assert.equal(sec.querySelector("#bt-audio-hint"), null);
    // owner 2026-11-15: no standby / wake / off on the card (standby / wake = the header's power button)
    assert.equal(sec.querySelector("[data-action=standby-now], [data-action=wake]"), null);
    assert.equal(sec.querySelector("#power-actions"), null);
    assert.equal(sec.querySelector<HTMLInputElement>("input[name=holdConnected]")!.checked, false);
    // TV wake (feat1 0x40): shown, on by default
    assert.equal(sec.querySelector<HTMLInputElement>("input[name=btWake]")!.checked, true);

    change(sel("apoAction"), "0");
    await until(() => /Saved/.test(sec.querySelector("#power-status")?.textContent ?? ""));
    change(sel("maxHold"), "60");
    await until(() => sel("maxHold")?.value === "60" && /Saved/.test(sec.querySelector("#power-status")?.textContent ?? ""));
    change(sel("standbyCap"), "240");
    await until(() => sel("standbyCap")?.value === "240" && /Saved/.test(sec.querySelector("#power-status")?.textContent ?? ""));
    change(sec.querySelector<HTMLInputElement>("input[name=holdConnected]")!, true);
    await tick(300);
    change(sec.querySelector<HTMLInputElement>("input[name=btWake]")!, false);
    await tick(300);
    const got = await new ng.NgPower(new ng.NgClient(session)).get();
    assert.deepEqual(got?.policy, { apoStandby: false, holdConnected: true, maxHoldMin: 60, standbyCapMin: 240, btWake: false });

    const lines: string[] = [];
    link.onLine((l) => lines.push(l));
    link.command("apo");
    await until(() => lines.some((l) => l.startsWith("apo ")));
    assert.match(lines.find((l) => l.startsWith("apo "))!, /policy=off\/3600000\/1\/14400000/);

    // reboot the piano (new simulator process, same settings flash)
    link.close();
    await tick(300);
    ({ link, session, store } = await boot(flash));
    const power = new ng.NgPower(new ng.NgClient(session));
    const after = await power.get();
    assert.equal(after?.rc, 0);
    assert.deepEqual(after?.policy, { apoStandby: false, holdConnected: true, maxHoldMin: 60, standbyCapMin: 240, btWake: false });

    // bad value: rejected, unchanged
    const bad = await power.set({ ...after!.policy, standbyCapMin: 10 });
    assert.equal(bad?.rc, ng.PolicyResult.BadValue);
    assert.equal(bad?.policy.standbyCapMin, 240);

    // standby by op, the notice, wake by op
    const notices: string[] = [];
    session.onMidi((m) => {
      const n = ng.parseStandbyNotice(m);
      if (n) notices.push(n);
    });
    // the header's power button (owner 2026-11-15): 「To standby」 = 19 01, 「Wake」 = 19 02
    const { powerButton } = await import("../src/ui/powerbtn.ts");
    const listeners: ((s: { phase: string; gen: number }) => void)[] = [];
    const conn = { state: { phase: "ready", gen: 1 }, onChange: (fn: (s: { phase: string; gen: number }) => void) => listeners.push(fn) };
    const seen: string[] = [];
    const pb = powerButton(() => session, conn as never, { changed: (st) => seen.push(st) });
    listeners.forEach((fn) => fn(conn.state));
    await until(() => !pb.el.hidden && pb.state() === "on");
    assert.equal(pb.el.id, "power-btn");
    assert.equal(pb.el.dataset.action, "standby-now");
    assert.equal(pb.el.textContent, "To standby");
    pb.el.click();
    await until(() => pb.state() === "standby");
    assert.equal((await power.standby("query"))?.state, "standby");
    assert.equal(pb.el.dataset.action, "wake");
    assert.equal(pb.el.textContent, "Wake");
    assert.match(pb.el.title, /in standby/);
    // the Settings card shows the state line and no button of its own
    const sec3 = settingsScreen(store, createContext(store, { session: () => session })).querySelector<HTMLElement>("#power-policy")!;
    await until(() => !!sec3.querySelector("#standby-state"));
    assert.equal(sec3.querySelector("button[data-action=wake], button[data-action=standby-now]"), null);
    assert.equal(sec3.querySelector("#power-diag, #power-dev, #apo-test"), null, "the test tools: Info › 개발자용");
    pb.el.click();
    await until(() => pb.state() === "on" || /waking|on/.test(notices.join()));
    for (let i = 0; i < 20 && (await power.standby("query"))?.state !== "on"; i++) await tick(50);
    assert.notEqual((await power.standby("query"))?.state, "standby");
    // another way into standby (op from elsewhere): refresh() asks 19 00 again (the notice: the next test)
    assert.deepEqual(await power.standby("enter"), { rc: 0, state: "standby" });
    pb.refresh();
    await until(() => pb.state() === "standby");
    assert.equal(pb.el.dataset.action, "wake");
    assert.ok(seen.includes("standby") && seen.includes("on"));
    assert.equal((await power.standby("wake"))?.rc, 0);

    // defaults back
    const def = await power.reset();
    assert.deepEqual(def?.policy, ng.POWER_POLICY_DEFAULT);
  } finally {
    link.close();
    rmSync(dir, { recursive: true, force: true });
  }
});

test("Settings > Power: hidden on a stock piano (no 0x7D answer)", async () => {
  i18n.setLang("en");
  const [app, piano] = LoopbackLink.pair();
  const out = (h: string) => piano.send(fromHex(h));
  let ngSeen = 0;
  piano.onMessage((m) => {
    if (m[1] === 0x7d) return void ngSeen++; // stock V2.19: dropped silently
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01) out("F0 40 7F 7E 04 1E 02 00 40 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) out("F0 40 7F 7E 04 1E 05 00 00 02 F7");
    if (f.fn === 0x02) out("F0 40 7F 03 04 1E 5F 7F 7F F7");
  });
  const session = new PianoSession(app, { ackTimeoutMs: 50 });
  await session.connect(100);
  const store = new ParamStore();
  const ctx = createContext(store, { session: () => session });
  const sec = settingsScreen(store, ctx).querySelector<HTMLElement>("#power-policy")!;
  await tick(900); // identify times out after 700 ms
  assert.equal(ngSeen, 1, "one Identify, nothing else");
  assert.equal(sec.hidden, true);
  assert.equal(sec.children.length, 0);
});

// "no limit" standby cap (0xFFFF) and the app start-up wake
test("codec: standby cap 0xFFFF = no limit", () => {
  assert.equal(ng.CAP_NONE, 0xffff);
  assert.deepEqual(ng.encodePowerPolicy({ ...ng.POWER_POLICY_DEFAULT, standbyCapMin: ng.CAP_NONE }), [1, 1, 0x2c, 1, 0xff, 0xff, 0, 0]);
  assert.equal(ng.decodePowerPolicy(Uint8Array.from([1, 1, 120, 0, 0xff, 0xff, 0, 0]))!.standbyCapMin, ng.CAP_NONE);
});

test("Settings > Power: 'No limit' is saved, survives a reboot; opening the app wakes a piano in standby", { skip }, async () => {
  i18n.setLang("en");
  const { wakeOnConnect } = await import("../src/ui/power.ts");
  const dir = mkdtempSync(join(tmpdir(), "ca79ng-nocap-"));
  const flash = join(dir, "flash.bin");
  let { link, session, store } = await boot(flash);
  try {
    const ctx = createContext(store, { session: () => session });
    const sec = settingsScreen(store, ctx).querySelector<HTMLElement>("#power-policy")!;
    await until(() => !sec.hidden);
    const sel = () => sec.querySelector<HTMLSelectElement>("select[name=standbyCap]")!;
    assert.ok([...sel().options].some((o) => o.value === String(ng.CAP_NONE) && o.textContent === "No limit (until woken)"));
    // the app-local start-up wake toggle: shown (not the panel), on by default, kept in the app settings
    const aw = sec.querySelector<HTMLInputElement>("input[name=autoWake]")!;
    assert.equal(aw.checked, true);
    change(aw, false);
    assert.equal(ctx.settings.autoWake, false);
    change(aw, true);
    assert.equal(ctx.settings.autoWake, true);

    change(sel(), String(ng.CAP_NONE));
    await until(() => sel()?.value === String(ng.CAP_NONE) && /Saved/.test(sec.querySelector("#power-status")?.textContent ?? ""));
    assert.equal(sel().value, String(ng.CAP_NONE));
    assert.match(sec.textContent ?? "", /No standby limit/);
    const lines: string[] = [];
    link.onLine((l) => lines.push(l));
    link.command("apo");
    await until(() => lines.some((l) => l.startsWith("apo ")));
    assert.match(lines.find((l) => l.startsWith("apo "))!, /policy=standby\/18000000\/0\/0$/, "core: cap_ms 0 = no cap");

    // reboot: still no limit
    link.close();
    await tick(300);
    ({ link, session, store } = await boot(flash));
    const power = new ng.NgPower(new ng.NgClient(session));
    assert.equal((await power.get())?.policy.standbyCapMin, ng.CAP_NONE);

    // awake piano: nothing to do
    assert.equal(await wakeOnConnect(session), false);
    // in standby: 19 00 -> standby, 19 02 -> woken
    assert.deepEqual(await power.standby("enter"), { rc: 0, state: "standby" });
    assert.equal(await wakeOnConnect(session), true);
    assert.notEqual((await power.standby("query"))?.state, "standby");

    // another controller took the binding (this app is displaced) and the piano is in standby: the
    // wake still works (19 00 / 19 02 answer on any port). The host core keeps every CONNECTed port
    // bound, so the stock 7E 01 00 is injected as if the piano had sent it.
    for (let i = 0; i < 20 && (await power.standby("query"))?.state !== "on"; i++) await tick(50);
    (link as unknown as { emit(m: Uint8Array): void }).emit(fromHex("F0 40 7F 7E 04 1E 01 00 F7"));
    await until(() => session.displaced);
    assert.equal(session.displaced, true);
    link.command("standby enter");
    await tick(200);
    assert.equal((await power.standby("query"))?.state, "standby");
    assert.equal(await wakeOnConnect(session), true);
    assert.notEqual((await power.standby("query"))?.state, "standby");
  } finally {
    link.close();
    rmSync(dir, { recursive: true, force: true });
  }
});

test("Settings > Power: an older payload rejects 'No limit' with a clear note", async () => {
  i18n.setLang("en");
  const [app, piano] = LoopbackLink.pair();
  const out = (h: string) => piano.send(fromHex(h));
  const hex = (b: number) => b.toString(16).padStart(2, "0");
  const polHex = ng.pack7([1, 1, 120, 0, 0x3c, 0, 0, 0]).map(hex).join(" "); // ota_power2: 1 h cap, no 0xFFFF
  piano.onMessage((m) => {
    if (m[1] === 0x7d) {
      const op = m[6]!, sub = m[7] ?? 0;
      if (op === 0x01) out("F0 7D 43 41 37 39 41 01 02 13 3B 57 F7");
      if (op === 0x18) out(`F0 7D 43 41 37 39 58 ${hex(sub)} ${sub === 1 ? "01" : "00"} ${polHex} F7`);
      if (op === 0x19) out("F0 7D 43 41 37 39 59 00 00 00 F7");
      return;
    }
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01) out("F0 40 7F 7E 04 1E 02 00 40 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) out("F0 40 7F 7E 04 1E 05 00 00 02 F7");
    if (f.fn === 0x02) out("F0 40 7F 03 04 1E 5F 7F 7F F7");
  });
  const session = new PianoSession(app, { ackTimeoutMs: 50 });
  await session.connect(100);
  const store = new ParamStore();
  const sec = settingsScreen(store, createContext(store, { session: () => session })).querySelector<HTMLElement>("#power-policy")!;
  await until(() => !sec.hidden && !!sec.querySelector("select[name=standbyCap]"));
  assert.equal(sec.querySelector<HTMLSelectElement>("select[name=standbyCap]")!.value, "60");
  change(sec.querySelector<HTMLSelectElement>("select[name=standbyCap]")!, String(ng.CAP_NONE));
  await until(() => /ota_power3/.test(sec.querySelector("#power-status")?.textContent ?? ""));
  assert.match(sec.querySelector("#power-status")!.textContent!, /needs ota_power3/);
  assert.equal(sec.querySelector<HTMLSelectElement>("select[name=standbyCap]")!.value, "60", "redrawn with the piano's policy");
});

// feature 2: remote off (19 03) - the simulator's host core answers the op
// byte-equal to the cave port; the card shows the button only for feat2 0x20 and asks first
test("remote off 19 03 against the firmware core: standby the way the APO goes, again -> not on", { skip }, async () => {
  const dir = mkdtempSync(join(tmpdir(), "ca79ng-off-"));
  const { link, session } = await boot(join(dir, "flash.bin"));
  try {
    const power = new ng.NgPower(new ng.NgClient(session));
    assert.deepEqual(await power.remoteOff(), { rc: 0, state: "standby", why: 0 });
    assert.deepEqual(await power.remoteOff(), { rc: ng.PolicyResult.BadState, state: "standby", why: 0 });
    assert.equal((await power.standby("wake"))?.rc, 0);
  } finally {
    link.close();
    rmSync(dir, { recursive: true, force: true });
  }
});

test("Settings > Power: 'Turn off' (feat2 0x20) asks first, then shows why the piano refused", async () => {
  i18n.setLang("en");
  const [app, piano] = LoopbackLink.pair();
  const out = (h: string) => piano.send(fromHex(h));
  const hex = (b: number) => b.toString(16).padStart(2, "0");
  const polHex = ng.pack7(ng.encodePowerPolicy(ng.POWER_POLICY_DEFAULT)).map(hex).join(" ");
  const offs: number[] = [];
  let answer = "06 00 01"; // refused: recording
  piano.onMessage((m) => {
    if (m[1] === 0x7d) {
      const op = m[6]!, sub = m[7] ?? 0;
      if (op === 0x01) out("F0 7D 43 41 37 39 41 01 02 13 3B 77 3F F7");
      if (op === 0x18) out(`F0 7D 43 41 37 39 58 ${hex(sub)} 00 ${polHex} F7`);
      if (op === 0x19 && sub === 0x00) out("F0 7D 43 41 37 39 59 00 00 00 F7");
      if (op === 0x19 && sub === 0x03) {
        offs.push(sub);
        out(`F0 7D 43 41 37 39 59 03 ${answer} F7`);
      }
      return;
    }
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01) out("F0 40 7F 7E 04 1E 02 00 40 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) out("F0 40 7F 7E 04 1E 05 00 00 02 F7");
    if (f.fn === 0x02) out("F0 40 7F 03 04 1E 5F 7F 7F F7");
  });
  const session = new PianoSession(app, { ackTimeoutMs: 50 });
  await session.connect(100);
  const store = new ParamStore();
  const { CONFIRM } = await import("../src/ui/confirm2.ts");
  const ms = CONFIRM.ms;
  CONFIRM.ms = 60;
  try {
    const ctx = createContext(store, { session: () => session });
    const settings = settingsScreen(store, ctx).querySelector<HTMLElement>("#power-policy")!;
    const sec = powerDevSection(ctx);
    await until(() => !!sec.querySelector("[data-action=remote-off]"));
    const btn = () => sec.querySelector<HTMLButtonElement>("[data-action=remote-off]")!;
    assert.equal(btn().textContent, "Turn off (standby)");
    // owner 2026-11-15 / 11-16: the remote off is only a test tool - Info › 개발자용, not Settings
    assert.ok(sec.querySelector("#remote-off-row")!.contains(btn()));
    await until(() => !settings.hidden);
    assert.equal(settings.querySelector("[data-action=remote-off]"), null);
    btn().click(); // the in-page question; not tapped again → disarmed, nothing sent
    assert.match(sec.querySelector("[data-confirm=ask]")!.textContent!, /standby/);
    await tick(100);
    assert.equal(sec.querySelector("[data-confirm=ask]"), null);
    assert.equal(btn().textContent, "Turn off (standby)");
    assert.deepEqual(offs, []);
    tapTwice(btn());
    await until(() => /recording/.test(sec.querySelector("#power-dev-status")?.textContent ?? ""));
    assert.deepEqual(offs, [3]);
    assert.match(sec.querySelector("#power-dev-status")!.textContent!, /Not now: the piano is recording/);
    answer = "00 01 00";
    tapTwice(btn());
    await until(() => /in standby/.test(sec.querySelector("#power-dev-status")?.textContent ?? ""));
    assert.equal(sec.querySelector("[data-action=wake], [data-action=standby-now]"), null, "standby / wake: the header's power button only");
    assert.equal(sec.querySelector("[data-action=remote-off]"), null, "redrawn in standby: no off row");
  } finally {
    CONFIRM.ms = ms;
  }
});

// APO test shortcut 19 04 - the simulator's host core answers like the cave
test("APO test 19 04 against the firmware core: rc 0 echo, 5 s refused, 0 = the full menu time", { skip }, async () => {
  const dir = mkdtempSync(join(tmpdir(), "ca79ng-apot-"));
  const { link, session } = await boot(join(dir, "flash.bin"));
  try {
    const power = new ng.NgPower(new ng.NgClient(session));
    const r = await power.apoTest(60);
    assert.ok(r);
    // the simulator boots with the menu APO from its flash: rc 0 when it counts, rc 7 when it is Off
    if (r.setting === 0) assert.equal(r.rc, ng.PolicyResult.ApoOff);
    else assert.deepEqual(r, { rc: 0, state: "on", secs: 60, setting: r.setting, held: false });
    const bad = await power.apoTest(5);
    assert.equal(bad?.rc, r.setting === 0 ? ng.PolicyResult.ApoOff : ng.PolicyResult.BadValue);
    assert.equal((await power.apoTest(0))?.secs, 0);
  } finally {
    link.close();
    rmSync(dir, { recursive: true, force: true });
  }
});

test("Settings > Power: auto power-off test 1 / 5 min / cancel (feat2 0x20), seconds left from op 1A", async () => {
  i18n.setLang("en");
  const [app, piano] = LoopbackLink.pair();
  const out = (h: string) => piano.send(fromHex(h));
  const hex = (b: number) => b.toString(16).padStart(2, "0");
  const polHex = ng.pack7(ng.encodePowerPolicy(ng.POWER_POLICY_DEFAULT)).map(hex).join(" ");
  const asked: number[] = [];
  let rc = 0;
  // op 1A: layout 2, ON, APO setting 1 (15 min), counter 45000 - 2950 = 42050 -> 59 s left
  const diag = new Uint8Array(60);
  diag[0] = 2; diag[6] = 1;
  new DataView(diag.buffer).setUint32(8, 45000 - 2950, true);
  const diagHex = ng.pack7([...diag]).map(hex).join(" ");
  piano.onMessage((m) => {
    if (m[1] === 0x7d) {
      const op = m[6]!, sub = m[7] ?? 0;
      if (op === 0x01) out("F0 7D 43 41 37 39 41 01 02 13 3B 77 3F F7");
      if (op === 0x18) out(`F0 7D 43 41 37 39 58 ${hex(sub)} 00 ${polHex} F7`);
      if (op === 0x19 && sub === 0x00) out("F0 7D 43 41 37 39 59 00 00 00 F7");
      if (op === 0x19 && sub === 0x04) {
        const secs = (m[8]! << 7) | m[9]!;
        asked.push(secs);
        out(`F0 7D 43 41 37 39 59 04 ${hex(rc)} 00 ${hex(m[8]!)} ${hex(m[9]!)} ${hex(rc === 7 ? 0 : 1)} 00 F7`);
      }
      if (op === 0x1a) out(`F0 7D 43 41 37 39 5A ${diagHex} F7`);
      return;
    }
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01) out("F0 40 7F 7E 04 1E 02 00 40 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) out("F0 40 7F 7E 04 1E 05 00 00 02 F7");
    if (f.fn === 0x02) out("F0 40 7F 03 04 1E 5F 7F 7F F7");
  });
  const session = new PianoSession(app, { ackTimeoutMs: 50 });
  await session.connect(100);
  const store = new ParamStore();
  const sec = powerDevSection(createContext(store, { session: () => session }));
  await until(() => !!sec.querySelector("[data-action=apo-test-60]"));
  // (owner 2026-11-16): the test tools and the remote off that only goes to standby (APO =
  // standby here) sit in the Info tab's developer card; the header's power button stays the one visible standby action
  assert.equal(sec.id, "power-dev");
  assert.ok(sec.querySelector("#apo-test") && sec.querySelector("#power-diag"));
  assert.equal(sec.querySelector("#remote-off-row [data-action=remote-off]")!.textContent, "Turn off (standby)");
  assert.equal(sec.querySelector("[data-action=standby-now], [data-action=wake]"), null, "standby / wake: the header's power button");
  await until(() => /59 s left/.test(sec.querySelector("#apo-test-left")?.textContent ?? ""));
  assert.match(sec.querySelector("#apo-test-left")!.textContent!, /59 s left/);
  sec.querySelector<HTMLButtonElement>("[data-action=apo-test-60]")!.click();
  await until(() => /in 60 s/.test(sec.querySelector("#power-dev-status")?.textContent ?? ""));
  sec.querySelector<HTMLButtonElement>("[data-action=apo-test-300]")!.click();
  await until(() => /in 300 s/.test(sec.querySelector("#power-dev-status")?.textContent ?? ""));
  sec.querySelector<HTMLButtonElement>("[data-action=apo-test-cancel]")!.click();
  await until(() => /full menu time/.test(sec.querySelector("#power-dev-status")?.textContent ?? ""));
  assert.deepEqual(asked, [60, 300, 0]);
  rc = 7;
  sec.querySelector<HTMLButtonElement>("[data-action=apo-test-60]")!.click();
  await until(() => /set to Off/.test(sec.querySelector("#power-dev-status")?.textContent ?? ""));
  assert.match(sec.querySelector("#power-dev-status")!.textContent!, /Auto power off is set to Off/);
  // the header's power button follows the piano's standby notice 59 7F 00 <state> (APO, a key, another app)
  const { powerButton } = await import("../src/ui/powerbtn.ts");
  const fns: ((s: { phase: string; gen: number }) => void)[] = [];
  const conn = { state: { phase: "ready", gen: 1 }, onChange: (fn: (s: { phase: string; gen: number }) => void) => fns.push(fn) };
  const pb = powerButton(() => session, conn as never);
  fns.forEach((fn) => fn(conn.state));
  await until(() => !pb.el.hidden && pb.state() === "on");
  out("F0 7D 43 41 37 39 59 7F 00 01 F7");   // standby entered
  await until(() => pb.state() === "standby");
  assert.equal(pb.el.dataset.action, "wake");
  out("F0 7D 43 41 37 39 59 7F 00 00 F7");   // on again
  await until(() => pb.state() === "on");
  assert.equal(pb.el.dataset.action, "standby-now");
  // lost connection: the button goes away
  conn.state = { phase: "lost", gen: 1 };
  fns.forEach((fn) => fn(conn.state));
  await until(() => pb.el.hidden);
  assert.equal(pb.el.hidden, true);
  // the Korean and Japanese strings exist
  for (const lang of ["ko", "ja"] as const) {
    i18n.setLang(lang);
    assert.notEqual(i18n.t("Auto power-off test"), "Auto power-off test");
    assert.notEqual(i18n.t("Auto power off is set to Off on the piano."), "Auto power off is set to Off on the piano.");
    for (const k of ["To standby", "Wake", "Waking…", "Piano: on — press to put it into standby", "Piano: in standby — press to wake it", "Wake it with the power button at the top."])
      assert.notEqual(i18n.t(k), k, `${lang}: ${k}`);
  }
  i18n.setLang("ko");
  assert.equal(i18n.t("To standby"), "대기로");
  assert.equal(i18n.t("Wake"), "깨우기");
  i18n.setLang("en");
});

test("header power button: hidden on a stock piano (no 0x7D answer)", async () => {
  const { powerButton } = await import("../src/ui/powerbtn.ts");
  const [app, piano] = LoopbackLink.pair();
  piano.onMessage((m) => {
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01) piano.send(fromHex("F0 40 7F 7E 04 1E 02 00 40 F7"));
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) piano.send(fromHex("F0 40 7F 7E 04 1E 05 00 00 02 F7"));
    if (f.fn === 0x02) piano.send(fromHex("F0 40 7F 03 04 1E 5F 7F 7F F7"));
  });
  const session = new PianoSession(app, { ackTimeoutMs: 50 });
  await session.connect(100);
  const fns: ((s: { phase: string; gen: number }) => void)[] = [];
  const conn = { state: { phase: "ready", gen: 1 }, onChange: (fn: (s: { phase: string; gen: number }) => void) => fns.push(fn) };
  let queries = 0; // 19 00 frames that reached the piano
  piano.onMessage((m) => { if (m[1] === 0x7d && m[6] === 0x19 && m[7] === 0x00) queries++; });
  const pb = powerButton(() => session, conn as never, { pollMs: 30 });
  fns.forEach((fn) => fn(conn.state));
  await tick(1200);
  assert.equal(pb.el.hidden, true);
  // stock firmware (no power feature): no 10 s poll, and refresh() does not send 19 00 either
  assert.equal(pb.polling(), false);
  pb.refresh();
  await tick(150);
  assert.equal(queries, 0, "no 19 00 to a stock piano");
});

// the 10 s 19 00 poll (field 2026-11-15) runs only while the button is shown; a lost link stops it
test("header power button: the 19 00 poll runs only while connected to a piano with the power feature", async () => {
  const { powerButton } = await import("../src/ui/powerbtn.ts");
  const [app, piano] = LoopbackLink.pair();
  const out = (h: string) => piano.send(fromHex(h));
  let queries = 0;
  piano.onMessage((m) => {
    if (m[1] === 0x7d) {
      const op = m[6]!, sub = m[7] ?? 0;
      if (op === 0x01) out("F0 7D 43 41 37 39 41 01 02 13 3B 77 3F F7"); // Identify: feat1 0x13 (power)
      if (op === 0x19 && sub === 0x00) { queries++; out("F0 7D 43 41 37 39 59 00 00 00 F7"); }
      return;
    }
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01) out("F0 40 7F 7E 04 1E 02 00 40 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) out("F0 40 7F 7E 04 1E 05 00 00 02 F7");
    if (f.fn === 0x02) out("F0 40 7F 03 04 1E 5F 7F 7F F7");
  });
  const session = new PianoSession(app, { ackTimeoutMs: 50 });
  await session.connect(100);
  const fns: ((s: { phase: string; gen: number }) => void)[] = [];
  const conn = { state: { phase: "ready", gen: 1 }, onChange: (fn: (s: { phase: string; gen: number }) => void) => fns.push(fn) };
  const pb = powerButton(() => session, conn as never, { pollMs: 30 });
  fns.forEach((fn) => fn(conn.state));
  await until(() => !pb.el.hidden && pb.state() === "on");
  assert.equal(pb.polling(), true);
  const q0 = queries;
  await until(() => queries >= q0 + 3); // polled
  // lost: the poll stops, the button goes, refresh() sends nothing
  conn.state = { phase: "lost", gen: 1 };
  fns.forEach((fn) => fn(conn.state));
  await until(() => pb.el.hidden);
  assert.equal(pb.polling(), false);
  await tick(20);
  const q1 = queries;
  pb.refresh();
  await tick(200);
  assert.equal(queries, q1, "no 19 00 while disconnected");
  // reconnected (new generation): polling again
  conn.state = { phase: "ready", gen: 2 };
  fns.forEach((fn) => fn(conn.state));
  await until(() => !pb.el.hidden && pb.polling());
  await until(() => queries >= q1 + 2);
  // a standby notice arrives (fan-out build): no more 19 00 polling on this connection
  out("F0 7D 43 41 37 39 59 7F 00 01 F7");
  await until(() => !pb.polling());
  assert.equal(pb.state(), "standby");
  conn.state = { phase: "idle", gen: 2 };
  fns.forEach((fn) => fn(conn.state));
  await until(() => !pb.polling());
});

test("remote off: reason texts", async () => {
  const { remoteOffText } = await import("../src/ui/power.ts");
  i18n.setLang("en");
  assert.match(remoteOffText({ rc: 6, state: "on", why: ng.OffReason.FileOp }), /file/);
  assert.match(remoteOffText({ rc: 6, state: "on", why: ng.OffReason.Flash }), /saving/);
  assert.match(remoteOffText({ rc: 0, state: "off", why: 0 }), /switching off/);
  assert.match(remoteOffText({ rc: 2, state: "on", why: 0 }), /another device/);
  assert.match(remoteOffText(null), /No answer/);
});
