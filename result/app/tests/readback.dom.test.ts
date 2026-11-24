// Settings read-back: every control shows the
// piano's value once it is known and "unknown" before that; values the AllParameter dump does not
// carry (or a dump cut short) are read with a single get `02 v1 v2 part`; edits send a set; the
// panel-local display settings are shown as unknown on a phone/PC and shared with the original
// panel UI through user/uicache.json inside the KWMCore shell.
//
// The fake piano answers like the stock V2.19 (values from the golden trace, request matching of FUN_4015205C:
// v1 5F = every group, v2 7F/00 = whole group, part 7F = every stored part, end `03 v1 v2 7F`).
import { change, dom, field, tick } from "./_dom.ts";
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import { existsSync as fileExists, readFileSync } from "node:fs";

const { ParamStore } = await import("../src/state/store.ts");
const { bindStore } = await import("../src/ui/controls.ts");
const S = await import("../src/ui/screens.ts");
const { createContext } = await import("../src/ui/context.ts");
const { PianoSession } = await import("../src/protocol/session.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const { decodeFrame, encodeFrame, fromHex, toHex } = await import("../src/protocol/sysex.ts");
const PC = await import("../src/state/panelcache.ts");
const { _resetKwmcoreForTests } = await import("../src/transport/kwmcore.ts");

const TRACE = new URL("../../../target/captures/phone_pianoremote_1.1.5.txt", import.meta.url);
// The capture is private (not in the public repository): the tests built on the stock dump skip without it.
const noTrace = !fileExists(TRACE) && "stock capture target/captures/phone_pianoremote_1.1.5.txt absent (not in the public repository)";

/** fn 0x10 values of the stock AllParameter dump, in the stock order. */
function stockDump(): number[][] {
  const out: number[][] = [];
  for (const l of readFileSync(TRACE, "utf8").split("\n")) {
    const [, dir, ...b] = l.trim().split(/\s+/);
    if (dir !== "PI->PH") continue;
    const f = decodeFrame(fromHex(b.join(" ")));
    if (f && f.fn === 0x10) out.push(f.body);
    // SoftPedalDepth (50 12 00) was lost in the capture (btsnoop "F0 F7", golden.trace.test.ts);
    // the stock sends it right after 50 11 0B — put a value back so the model is the stock's
    if (f && f.fn === 0x10 && f.body[0] === 0x50 && f.body[1] === 0x11 && f.body[2] === 0x0b) out.push([0x50, 0x12, 0x00, 0x0d]);
  }
  return out;
}

/** Stock-like piano. `cut` drops dump values (a dump that lost its binding part-way). */
function stockPiano(opt: { cut?: (i: number) => boolean } = {}) {
  const [app, piano] = LoopbackLink.pair();
  const dump = stockDump();
  const requests: string[] = [];
  const sets: string[] = [];
  const send = (fn: number, body: number[]) => piano.send(encodeFrame({ dev: 0x7f, fn, group: 4, machine: 0x1e, body }));
  piano.onMessage((m) => {
    const f = decodeFrame(m);
    if (!f) return;
    const [a, b, part] = f.body;
    if (f.fn === 0x7e) {
      if (a === 0x01 && b === 0x01) send(0x7e, [0x02, 0x00, 0x40]);
      else if (a === 0x05 && b === 0x40) send(0x7e, [0x05, 0x00, 0x00, 0x02]);
      else if (a === 0x05 && b === 0x41) send(0x7e, [0x05, 0x01, 0x00, 0x01]);
      else if (a === 0x05 && b === 0x42) send(0x7e, [0x05, 0x02, 0x00]);
      else if (a === 0x04 && b === 0x41) send(0x7e, [0x04, 0x01, ..."V2.19".split("").map((c) => c.charCodeAt(0))]);
    } else if (f.fn === 0x02) {
      if (!(a === 0x5f && b === 0x7f)) requests.push(toHex(m));
      dump.forEach((v, i) => {
        if (a === 0x5f && opt.cut?.(i)) return;
        const groupOk = a === 0x5f ? !(v[0] === 0x56 && v[1] === 0x0b) : v[0] === a;
        const idOk = b === 0x7f || b === 0x00 || v[1] === b;
        const partOk = part === 0x7f || v[2] === part;
        if (groupOk && idOk && partOk) send(0x10, v);
      });
      send(0x03, [a, b, 0x7f]);
    } else if (f.fn === 0x10) {
      sets.push(toHex(m));
      send(0x07, f.body.slice(0, 3));
    }
  });
  return { app, requests, sets };
}

async function connected(opt: { cut?: (i: number) => boolean } = {}) {
  const p = stockPiano(opt);
  const store = new ParamStore();
  bindStore(store);
  const session = new PianoSession(p.app, { ackTimeoutMs: 50 });
  store.attach(session);
  const ctx = createContext(store, { session: () => session });
  // every screen is built before connecting, as in the app (controls register what they show)
  const screens = {
    sound: S.soundScreen(store, ctx),
    vt: S.vtScreen(store, ctx),
    metronome: S.metronomeScreen(store, new EventTarget()),
    music: S.musicScreen(store, ctx),
    settings: S.settingsScreen(store, ctx),
  };
  await session.connect(300);
  await tick(5);
  await store.readback(300);
  return { ...p, store, session, ctx, screens };
}

const out = (root: ParentNode, label: string) => field<HTMLOutputElement>(root, label).parentElement!.querySelector("output")!.textContent;
const row = (root: ParentNode, label: string) => field(root, label).closest(".row")!;

/** Stock V2.19 has no value for these (get answers only the end marker): OpenGrand extensions. */
const STOCK_UNREADABLE = ["MusicRepeat@127", "PlayMode@127", "SoundPreviewVolume@127", "TransposeValue@127"];

test("full stock dump: every control has its value; only the 4 stock-less values are asked with a single get", { skip: noTrace }, async () => {
  const { store, requests, screens } = await connected();
  assert.deepEqual(
    // 52 0D: the Concert Magic section's own request for its OpenGrand settings (not a read-back)
    requests.filter((r) => !r.includes(" 52 0D ")).sort(),
    ["F0 40 7F 02 08 02 50 1F 7F F7", "F0 40 7F 02 08 02 52 07 7F F7", "F0 40 7F 02 08 02 52 09 7F F7", "F0 40 7F 02 08 02 53 02 7F F7"],
  );
  assert.deepEqual(store.missing().sort(), STOCK_UNREADABLE);
  assert.deepEqual(store.unreadable().sort(), STOCK_UNREADABLE);
  // the values reported as unreadable on hardware (field 2026-10-31)
  assert.equal(out(screens.settings, "Transpose"), "0");
  assert.equal(out(screens.sound, "Split point"), "C4");
  assert.equal(out(screens.sound, "Layer octave"), "0");
  assert.equal(out(screens.sound, "Lower octave"), "0");
  assert.equal(out(screens.sound, "Layer dynamics"), "127");
  // metronome: tempo / beat / rhythm follow the dump (56 01 = 120, 56 02 = 4/4, 56 09 = 0)
  assert.equal(screens.metronome.querySelector(".tempo")!.textContent, "120");
  assert.equal(field<HTMLSelectElement>(screens.metronome, "Time signature").selectedOptions[0].textContent, "4/4");
  assert.equal(field<HTMLSelectElement>(screens.metronome, "Rhythm").selectedIndex, 0);
  // no row left in the "unknown" state except the stock-less ones
  const unknown = Object.values(screens).flatMap((s) => [...s.querySelectorAll(".row.unknown")].map((r) => r.querySelector("span")!.textContent));
  assert.deepEqual(unknown.sort(), ["Play mode", "Preview volume", "Repeat", "Song transpose"]);
});

test("dump cut short: the values it lost are read back one by one and shown", { skip: noTrace }, async () => {
  // drop the first 40 values (Rendering .. the 0x53 group): what a dump that lost the binding
  // early and was resumed by the stock request task would leave
  const { store, requests, screens } = await connected({ cut: (i) => i < 40 });
  assert.ok(requests.includes("F0 40 7F 02 08 02 53 02 7F F7"), requests.join("\n"));
  assert.ok(requests.includes("F0 40 7F 02 08 02 53 06 09 F7"), "single part get for the split point");
  assert.deepEqual(store.missing().sort(), STOCK_UNREADABLE);
  assert.equal(out(screens.settings, "Transpose"), "0");
  assert.equal(out(screens.sound, "Resonance Depth"), "6");
  assert.equal(out(screens.sound, "Split point"), "C4");
  assert.equal(out(screens.sound, "Layer octave"), "0");
  assert.equal(out(screens.sound, "Lower octave"), "0");
  assert.equal(out(screens.sound, "Layer dynamics"), "127");
  assert.equal(field<HTMLSelectElement>(screens.sound, "Mode").selectedOptions[0].textContent, "Single");
  assert.equal(row(screens.sound, "Split point").classList.contains("unknown"), false);
});

test("before any value: sliders show –, selects are blank, switches indeterminate (no defaults posing as values)", () => {
  const store = new ParamStore();
  bindStore(store);
  const el = S.soundScreen(store);
  assert.equal(out(el, "Split point"), "–");
  assert.ok(row(el, "Split point").classList.contains("unknown"));
  assert.equal(field<HTMLSelectElement>(el, "Mode").selectedIndex, -1);
  assert.equal(field(el, "Lower pedal").indeterminate, true);
  const m = S.metronomeScreen(store, new EventTarget());
  assert.equal(m.querySelector(".tempo")!.textContent, "–");
  assert.equal(field<HTMLSelectElement>(m, "Time signature").selectedIndex, -1);
  // a value arriving later is shown
  store.fromWire({ fn: 0x10, v1: 0x53, v2: 0x06, part: 0x09, data: [48] });
  store.fromWire({ fn: 0x10, v1: 0x50, v2: 0x21, part: 0x0f, data: [1] });
  assert.equal(out(el, "Split point"), "C3");
  assert.equal(field(el, "Lower pedal").indeterminate, false);
  assert.equal(field(el, "Lower pedal").checked, true);
});

test("edits send a set: key transpose on change (no animation frame needed), one frame per value", { skip: noTrace }, async () => {
  const { sets, screens } = await connected();
  const input = field(screens.settings, "Transpose");
  change(input, "3"); // change only (keyboard / test driver)
  await tick(20);
  assert.deepEqual(sets, ["F0 40 7F 10 08 02 53 02 0F 43 F7"]);
  // a drag: input events, then change at the end — still one frame for the final value
  change(input, "-2", "input");
  change(input, "-2", "change");
  await tick(20);
  assert.deepEqual(sets.slice(1), ["F0 40 7F 10 08 02 53 02 0F 3E F7"]);
  // Sound tab: split point / octave / dynamics
  change(field(screens.sound, "Split point"), "50");
  change(field(screens.sound, "Layer octave"), "1");
  change(field(screens.sound, "Layer dynamics"), "100");
  await tick(30);
  assert.deepEqual(sets.slice(2), ["F0 40 7F 10 08 02 53 06 09 32 F7", "F0 40 7F 10 08 02 53 03 08 41 F7", "F0 40 7F 10 08 02 53 05 08 64 F7"]);
});

test("phone/PC: LCD brightness and auto display off are panel-only and shown as unknown", () => {
  const store = new ParamStore();
  const el = S.settingsScreen(store, createContext(store, { panel: false }));
  for (const id of ["lcd-brightness", "auto-display-off"]) {
    const r = el.querySelector(`[data-panel-only="${id}"]`)!;
    assert.equal(r.querySelector("output")!.textContent, "Set on the panel · value unknown");
    assert.equal(r.querySelector("select, input"), null, "no control that could show a default");
  }
  // lock mode and auto power off are piano parameters: normal controls
  assert.equal(field<HTMLSelectElement>(el, "Lock mode").tagName, "SELECT");
  // on the panel the controls are there, with the panel's choices (Never, 30 s, 1–5 min)
  const p = S.settingsScreen(store, createContext(store, { panel: true }));
  assert.deepEqual([...field<HTMLSelectElement>(p, "Auto display off").options].map((o) => o.textContent), ["Off", "30 sec", "1 min", "2 min", "3 min", "4 min", "5 min"]);
});

// ---- panel cache inside the original KWMCore shell ----
const g = globalThis as unknown as Record<string, unknown>;
afterEach(() => {
  delete g.kwmcore;
  _resetKwmcoreForTests();
});

function fakeShell(files: Record<string, string>) {
  const saved: { name: string; json: Record<string, unknown> }[] = [];
  const esc = (s: string) => s.replace(/\\/g, "\\\\").replace(/'/g, "\\'");
  const evalJs = (code: string): unknown => (0, eval)(code);
  g.kwmcore = {
    postMessage(url: string) {
      const i = url.indexOf("?");
      const cmd = url.substring(10, i - 1);
      const obj = JSON.parse(url.substring(i + 1)) as { name: string; json?: Record<string, unknown> };
      setTimeout(() => {
        if (cmd === "loadJson") {
          if (obj.name in files) evalJs(`KWM.resolvePromise('loadJson','${esc(files[obj.name])}')`);
          else evalJs(`KWM.rejectPromise('loadJson','{}')`);
        } else if (cmd === "saveJson") {
          saved.push({ name: obj.name, json: obj.json! });
          files[obj.name] = JSON.stringify(obj.json);
          evalJs(`KWM.resolvePromise('saveJson','{}')`);
        }
      }, 1);
    },
  };
  return { saved };
}

test("panel cache: uicache.json <-> brightness / auto display off mapping", () => {
  assert.deepEqual(PC.fromPanelCache({ lcdContrast: 30, displayOff: 2 }), { brightness: 3, autoOffMin: 1 });
  assert.deepEqual(PC.fromPanelCache({ lcdContrast: 0, displayOff: 1 }), { brightness: 10, autoOffMin: 0.5 }); // original: 0 -> 100
  assert.deepEqual(PC.fromPanelCache({ displayOff: 9 }), {});
  assert.deepEqual(PC.toPanelCache({ brightness: 7, autoOffMin: 5 }), { lcdContrast: 70, displayOff: 6 });
});

test("panel cache: inside the original shell the panel's values are shown and changes are written back", async () => {
  const shell = fakeShell({ "user/uicache.json": JSON.stringify({ appVersion: "1.3.10", lcdContrast: 30, viewportScale: 1, displayOff: 2, defaultTab: 0 }) });
  const store = new ParamStore();
  let rerenders = 0;
  const ctx = createContext(store, { panel: true, rerender: () => rerenders++ });
  ctx.settings = { ...ctx.settings, lang: "en", brightness: 8, autoOffMin: 0 }; // the panel now starts in Korean; labels below are English
  assert.equal(await PC.attachPanelCache(ctx), true);
  assert.equal(ctx.settings.autoOffMin, 1, "panel Auto Display Off 1 min");
  assert.equal(ctx.settings.brightness, 3);
  assert.equal(rerenders, 1);
  const el = S.settingsScreen(store, ctx);
  assert.equal(field<HTMLSelectElement>(el, "Auto display off").selectedOptions[0].textContent, "1 min");
  change(field<HTMLSelectElement>(el, "Auto display off"), "5");
  await tick(10);
  assert.equal(shell.saved.length, 1);
  assert.deepEqual(shell.saved[0], { name: "user/uicache.json", json: { appVersion: "1.3.10", lcdContrast: 30, viewportScale: 1, displayOff: 6, defaultTab: 0 } });
  // other settings do not touch the panel file
  ctx.updateSettings({ scale: 110 });
  await tick(10);
  assert.equal(shell.saved.length, 1);
});

test("panel cache: no file -> nothing read, nothing written", async () => {
  const shell = fakeShell({});
  const store = new ParamStore();
  const ctx = createContext(store, { panel: true });
  assert.equal(await PC.attachPanelCache(ctx, 200), false);
  ctx.updateSettings({ autoOffMin: 3 });
  await tick(10);
  assert.equal(shell.saved.length, 0);
  void dom;
});

// ---- our firmware (ports/sim): the dump + read-back leave nothing unknown ----
const { existsSync } = await import("node:fs");
const { SIM_BIN, SimProcessLink } = await import("../scripts/sim-link.ts");
const noSim = !existsSync(SIM_BIN) && "firmware simulator not built (make -C ../firmware sim)";

test("OpenGrand firmware simulator: sync + read-back, the remaining unknowns are exactly the known firmware gap", { skip: noSim }, async () => {
  const link = new SimProcessLink();
  await link.ready;
  try {
    const session = new PianoSession(link, { ackTimeoutMs: 500 });
    const store = new ParamStore();
    bindStore(store);
    store.attach(session);
    const ctx = createContext(store, { session: () => session });
    const screens = [S.soundScreen(store, ctx), S.vtScreen(store, ctx), S.metronomeScreen(store, new EventTarget()), S.musicScreen(store, ctx), S.settingsScreen(store, ctx)];
    await session.connect(2000);
    await tick(5);
    await store.readback(1000);
    // Firmware gap: the 7 System values the stock sends (metronome tempo /
    // beat / rhythm / mode / volume, song tempo, song parts) now come with the dump;
    // left are the 4 values the stock has no value for either (NG
    // extensions), each asked once. Listed exactly so a fix (or a new gap) shows up here.
    const FW_GAP = [...STOCK_UNREADABLE].sort();
    assert.deepEqual(store.missing().sort(), FW_GAP, `not readable from our firmware: ${store.missing().join(", ")}`);
    assert.equal(store.readbackStats.requests, FW_GAP.length);
    assert.equal(store.readbackStats.answered, FW_GAP.length, "every get ends with 03 v1 v2 7F");
    // everything else the screens show came with the dump
    const unknown = screens.flatMap((s) => [...s.querySelectorAll(".row.unknown")].map((r) => r.querySelector("span")!.textContent));
    assert.deepEqual(unknown.sort(), ["Play mode", "Preview volume", "Repeat", "Song transpose"]);
  } finally {
    link.close();
  }
});
