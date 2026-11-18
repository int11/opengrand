// Device picker (ui/picker.ts) and BLE-MIDI handoff (state/handoff.ts):
// on the Android shell a Bluetooth MIDI row is always first (scan countdown -> found pianos under it -> nothing found:
// "another device may be connected" + Search again); the simulator only in development; "in use" only for a live
// link; a remembered piano that does not come back opens the picker; a retry with nothing to reopen opens it too.
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const i18n = await import("../src/i18n.ts");
const picker = await import("../src/ui/picker.ts");
const { installHandoff, HANDOFF_HIDDEN_MS } = await import("../src/state/handoff.ts");
const { ConnectionManager, connView, NO_LINK } = await import("../src/state/connection.ts");
type LinkCandidate = import("../src/transport/link.ts").LinkCandidate;

(dom.window.HTMLDialogElement.prototype as unknown as { showModal(): void }).showModal = function (this: HTMLDialogElement) { this.open = true; };
(dom.window.HTMLDialogElement.prototype as unknown as { close(): void }).close = function (this: HTMLDialogElement) {
  this.open = false;
  this.dispatchEvent(new dom.window.Event("close"));
};

const cand = (name: string, key: string): LinkCandidate => ({ kind: "android", id: key, name, key, open: async () => { throw new Error("not in this test"); } });

/** a fake shell: scan() starts, devices appear when the test says so */
function fakeShell() {
  const found: LinkCandidate[] = [];
  let onChange: (() => void) | null = null;
  let scans = 0;
  return {
    found, get scans() { return scans; },
    deps: (over: Partial<import("../src/ui/picker.ts").PickerDeps> = {}) => ({
      shell: true, dev: false, simUrl: "ws://appassets.androidplatform.net:8079/midi",
      list: async () => [...found],
      scan: (fn: () => void) => { scans++; onChange = fn; return () => { onChange = null; }; },
      inUseKey: () => null, choose: () => {},
      openSim: (url: string) => ({ key: `websocket:${url}`, name: url, open: async () => { throw new Error("x"); } }),
      scanMs: 120, tickMs: 20, ...over,
    }),
    appear(c: LinkCandidate) { found.push(c); onChange?.(); },
  };
}

test("shell picker: Bluetooth MIDI row first — scanning countdown, pianos under it, USB after; no simulator", async () => {
  i18n.setLang("ko");
  const sh = fakeShell();
  const usb = cand("CA79 (USB-MIDI)", "android:usb1");
  sh.found.push(usb);
  const dlg = picker.openPicker(sh.deps());
  await tick(10);
  const head = () => dlg.querySelector<HTMLElement>("#bt-scan")!;
  assert.equal(head().dataset.state, "scanning");
  assert.match(head().textContent!, /^블루투스 MIDI 검색 중… \d초$/);
  assert.equal(dlg.querySelector(".list")!.firstElementChild, head(), "the Bluetooth row is the first row");
  sh.appear(cand("CA79 (BLE-MIDI)", "android:aa:bb"));
  await tick(10);
  const rows = [...dlg.querySelectorAll<HTMLButtonElement>(".list button.item.dev")];
  const items = rows.map((b) => [b.querySelector(".dev-name")!.textContent, b.querySelector(".dev-sub")!.textContent, b.querySelector(".dev-go")!.textContent]);
  assert.deepEqual(items, [["CA79", "블루투스 MIDI", "연결"], ["CA79", "USB MIDI", "연결"]], "BLE under the row, USB after — device rows, not settings buttons");
  assert.deepEqual(rows.map((b) => b.dataset.transport), ["ble", "usb"]);
  assert.ok(rows.every((b) => b.querySelector(".dev-ico svg")), "each row has its transport icon");
  assert.equal(dlg.querySelector("#sim-item"), null, "no simulator on the shell");
  await tick(200);
  assert.equal(head().dataset.state, "found");
  assert.equal(head().textContent, i18n.t("Bluetooth MIDI"));
  dlg.close();
  i18n.setLang("en");
});

test("shell picker: nothing found → 'another device may be connected' + Search again restarts the scan", async () => {
  i18n.setLang("ko");
  const sh = fakeShell();
  const dlg = picker.openPicker(sh.deps());
  await tick(220);
  const head = dlg.querySelector<HTMLElement>("#bt-scan")!;
  assert.equal(head.dataset.state, "none");
  assert.match(head.textContent!, /피아노를 찾지 못했습니다 — 다른 기기\(PC · 폰\)가 피아노에 연결되어 있을 수 있습니다/);
  assert.equal(sh.scans, 1);
  dlg.querySelector<HTMLButtonElement>("#bt-rescan")!.click();
  await tick(5);
  assert.equal(sh.scans, 2);
  assert.equal(dlg.querySelector<HTMLElement>("#bt-scan")!.dataset.state, "scanning");
  sh.appear(cand("CA79 (BLE-MIDI)", "android:aa:bb"));
  await tick(220);
  assert.equal(dlg.querySelector<HTMLElement>("#bt-scan")!.dataset.state, "found");
  dlg.close();
  i18n.setLang("en");
});

test("picker: 'in use' only for the live link; simulator only in development; browsers get the Web Bluetooth entry", async () => {
  const sh = fakeShell();
  sh.found.push(cand("CA79 (BLE-MIDI)", "android:aa:bb"));
  let live: string | null = null;
  const dlg = picker.openPicker(sh.deps({ inUseKey: () => live }));
  await tick(10);
  assert.ok(!dlg.textContent!.includes("in use"), "a failed / lost link is not 'in use'");
  dlg.close();
  live = "android:aa:bb";
  const dlg2 = picker.openPicker(sh.deps({ inUseKey: () => live }));
  await tick(10);
  const row = dlg2.querySelector<HTMLButtonElement>("button.item.dev")!;
  assert.ok(row.classList.contains("on"));
  assert.equal(row.querySelector(".dev-go")!.textContent, "✓ in use", "the pill says in use instead of Connect");
  dlg2.close();
  const web = picker.openPicker(sh.deps({ shell: false, scan: undefined, dev: true, simUrl: "ws://localhost:8079/midi", requestBle: { name: "Bluetooth MIDI", open: async () => { throw new Error("x"); } } }));
  await tick(10);
  assert.ok(web.querySelector("#bt-pair"), "Web Bluetooth entry");
  assert.ok(web.querySelector("#sim-item"), "simulator in development");
  assert.equal(web.querySelector("#bt-scan"), null);
  web.close();
  // where the simulator may show
  const env = { shell: false, kwm: false, embedded: false, hostname: "localhost", search: "" };
  assert.equal(picker.devSimulator(env), true);
  assert.equal(picker.devSimulator({ ...env, shell: true }), false, "Android shell");
  assert.equal(picker.devSimulator({ ...env, hostname: "appassets.androidplatform.net" }), false);
  assert.equal(picker.devSimulator({ ...env, kwm: true }), false, "panel");
  assert.equal(picker.devSimulator({ ...env, hostname: "example.github.io" }), false);
  assert.equal(picker.devSimulator({ ...env, hostname: "example.github.io", search: "?sim" }), true);
});

test("a stored simulator link is dropped outside development; a remembered piano that fails opens the picker", async () => {
  const store = new Map<string, string>();
  const st = { getItem: (k: string) => store.get(k) ?? null, removeItem: (k: string) => void store.delete(k) };
  store.set("last", JSON.stringify({ key: "websocket:ws://appassets.androidplatform.net:8079/midi", name: "Simulator" }));
  assert.equal(picker.dropStoredSim(st, "last", true), false);
  assert.ok(store.has("last"));
  assert.equal(picker.dropStoredSim(st, "last", false), true);
  assert.ok(!store.has("last"));
  store.set("last", JSON.stringify({ key: "android:aa:bb", name: "CA79" }));
  assert.equal(picker.dropStoredSim(st, "last", false), false);
  let picked = 0;
  const failing = { phase: "idle", choose: async () => { failing.phase = "failed"; return "failed"; } };
  await picker.reopenRemembered(failing, { name: "CA79", open: async () => { throw new Error("x"); } }, () => picked++);
  assert.equal(picked, 1);
  const ok = { phase: "idle", choose: async () => { ok.phase = "ready"; return "ready"; } };
  await picker.reopenRemembered(ok, { name: "CA79", open: async () => { throw new Error("x"); } }, () => picked++);
  assert.equal(picked, 1);
});

test("retry with nothing to reopen: a clear message and the picker (field 'no link to open')", async () => {
  let pickers = 0;
  const conn = new ConnectionManager({ makeSession: () => { throw new Error("no session in this test"); }, openPicker: () => pickers++ });
  // the field case: the simulator was picked on the phone, its websocket never opened
  await conn.choose({ key: "websocket:ws://appassets.androidplatform.net:8079/midi", name: "Simulator", open: async () => { throw new Error("websocket error"); } });
  assert.equal(conn.state.phase, "failed");
  conn.retry();
  await tick(5);
  assert.equal(conn.state.phase, "failed");
  assert.equal(conn.state.error, NO_LINK);
  assert.ok(pickers >= 1);
  i18n.setLang("ko");
  const v = connView(conn.state);
  assert.equal(i18n.t(...v.status), "연결 실패: 피아노를 다시 골라 주세요");
  i18n.setLang("en");
});

test("handoff: hidden for HANDOFF_HIDDEN_MS lets the piano go, back in front reconnects, pagehide at once", () => {
  const calls: string[] = [];
  const timers: { fn: () => void; ms: number; live: boolean }[] = [];
  const doc = { hidden: false, fns: [] as (() => void)[], addEventListener(_t: "visibilitychange", fn: () => void) { this.fns.push(fn); } };
  const win = { fns: [] as (() => void)[], addEventListener(_t: "pagehide", fn: () => void) { this.fns.push(fn); } };
  let shellFn: ((s: "background" | "foreground") => void) | null = null;
  installHandoff({ suspend: () => calls.push("suspend"), resume: () => calls.push("resume") }, {
    doc, win, onShell: (fn) => { shellFn = fn; },
    setTimer: (fn, ms) => { const t = { fn, ms, live: true }; timers.push(t); return t; },
    clearTimer: (h) => { if (h) (h as { live: boolean }).live = false; },
  });
  assert.equal(HANDOFF_HIDDEN_MS, 60000);
  doc.hidden = true;
  doc.fns[0]!();
  assert.equal(timers[0]!.ms, HANDOFF_HIDDEN_MS);
  doc.hidden = false;
  doc.fns[0]!();                       // back before the grace: the link stays
  assert.deepEqual(calls, ["resume"]);
  assert.equal(timers[0]!.live, false);
  doc.hidden = true;
  doc.fns[0]!();
  timers[1]!.fn();                     // a minute in the background
  assert.deepEqual(calls, ["resume", "suspend"]);
  doc.hidden = false;
  doc.fns[0]!();
  win.fns[0]!();
  shellFn!("background");
  assert.deepEqual(calls, ["resume", "suspend", "resume", "suspend", "suspend"]);
});

test("handoff: the desktop app keeps the piano while minimized (keepWhenHidden), closing still lets it go", () => {
  const calls: string[] = [];
  const timers: unknown[] = [];
  const doc = { hidden: false, fns: [] as (() => void)[], addEventListener(_t: "visibilitychange", fn: () => void) { this.fns.push(fn); } };
  const win = { fns: [] as (() => void)[], addEventListener(_t: "pagehide", fn: () => void) { this.fns.push(fn); } };
  installHandoff({ suspend: () => calls.push("suspend"), resume: () => calls.push("resume") }, {
    doc, win, keepWhenHidden: true,
    setTimer: (fn) => { timers.push(fn); return fn; },
    clearTimer: () => {},
  });
  doc.hidden = true;
  doc.fns[0]!();
  doc.hidden = false;
  doc.fns[0]!();
  assert.equal(timers.length, 0);      // no hidden-grace timer at all
  assert.deepEqual(calls, []);
  win.fns[0]!();                       // the app window closes
  assert.deepEqual(calls, ["suspend"]);
});

test("picker rows: the driver suffix leaves the name, the transport goes to the second line", () => {
  const c = (name: string, kind: LinkCandidate["kind"] = "android"): LinkCandidate => ({ kind, id: name, name, open: async () => { throw new Error("x"); } });
  assert.equal(picker.deviceName(c("CA79 (BLE-MIDI)")), "CA79");
  assert.equal(picker.deviceName(c("CA79 (USB-MIDI)")), "CA79");
  assert.equal(picker.deviceName(c("CA79 (KWMcore usb)", "kwmcore")), "CA79");
  assert.equal(picker.deviceName(c("CA79 MIDI 1", "webmidi")), "CA79 MIDI 1");
  assert.equal(picker.deviceName(c("(BLE-MIDI)")), "(BLE-MIDI)", "never an empty name");
  assert.equal(picker.transportOf(c("CA79 (BLE-MIDI)")), "ble");
  assert.equal(picker.transportOf(c("CA79", "ble")), "ble");
  assert.equal(picker.transportOf(c("CA79 (USB-MIDI)")), "usb");
  assert.equal(picker.transportOf(c("ws://x", "websocket")), "sim");
  assert.equal(picker.transportOf(c("CA79 MIDI 1", "webmidi")), "midi");
});
