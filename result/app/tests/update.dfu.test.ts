// Feature 86 (panel UI / APK update from USB memory) and 87 (BLE-MIDI module
// DFU) — the web side against a fake Android bridge. The Kotlin side is
// covered by the Android build (assemblePhoneDebug / assemblePanelDebug).
import { byText, pianoPair, tick } from "./_dom.ts";
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";

const U = await import("../src/features/update.ts");
const D = await import("../src/features/dfu.ts");
const { infoScreen } = await import("../src/ui/info.ts");
const { createContext } = await import("../src/ui/context.ts");
const { ParamStore } = await import("../src/state/store.ts");
const { toHex } = await import("../src/protocol/sysex.ts");

type Native = { onUpdate(j: string): void; onDfu(j: string): void };
const native = () => (globalThis as unknown as { ca79ngNative: Native }).ca79ngNative;
const g = globalThis as unknown as { ca79ng?: Record<string, unknown> };
afterEach(() => delete g.ca79ng);

function fakeBridge(over: Record<string, unknown> = {}) {
  const calls: string[] = [];
  g.ca79ng = {
    listDevices: () => "[]", open: () => true, send() {}, close() {}, panel: () => '{"embedded":true}', setBacklight() {}, usbPath: () => "/mnt/usb", scanBle() {},
    appInfo: () => JSON.stringify({ version: "0.2.0", versionCode: 2, flavor: "panel" }),
    checkUpdate: () => JSON.stringify({ found: true, version: "0.3.0" }),
    installUpdate: () => { calls.push("install"); return true; },
    findDfuPackage: () => JSON.stringify({ path: "/mnt/usb/CA79NG/blemidi.zip", version: "2.2.0", source: "usb" }),
    startDfu: (p: string) => { calls.push(`dfu:${p}`); return true; },
    abortDfu() {},
    ...over,
  };
  return calls;
}

test("version comparison and manifest validation", () => {
  assert.equal(U.compareVersions("1.10.0", "1.9.3"), 1);
  assert.equal(U.compareVersions("1.0", "1.0.0"), 0);
  assert.equal(U.compareVersions("0.2.0", "0.3.0"), -1);
  const sha = "a".repeat(64);
  assert.deepEqual(U.parseManifest(JSON.stringify({ version: "1.2.0", sha256: sha })), { version: "1.2.0", sha256: sha });
  assert.equal(U.parseManifest(JSON.stringify({ version: "1.2.0", sha256: "abc" })), null);
  assert.equal(U.parseManifest(JSON.stringify({ version: "x", sha256: sha })), null);
  assert.equal(U.parseManifest(JSON.stringify({ version: "1.0", sha256: sha, file: "../evil.apk" })), null);
  assert.equal(U.parseManifest("{"), null);
});

test("no bridge: update reported unsupported, DFU not offered", () => {
  assert.deepEqual(U.checkApkUpdate(), { found: false, error: "unsupported" });
  assert.equal(D.dfuSupported(), false);
  const el = infoScreen(createContext(new ParamStore()));
  assert.equal(el.querySelector("#apk-update-btn"), null);
  assert.equal(el.querySelector("#dfu-btn"), null);
});

test("Information screen offers Update when the USB memory has a newer APK and reports install status", () => {
  const calls = fakeBridge();
  const r = U.checkApkUpdate();
  assert.deepEqual([r.found, r.version, r.current, r.newer], [true, "0.3.0", "0.2.0", true]);
  const el = infoScreen(createContext(new ParamStore()));
  document.body.append(el);
  assert.equal(el.querySelector("#shell-version")!.textContent, "0.2.0 (panel)");
  const btn = el.querySelector("#apk-update-btn") as HTMLButtonElement;
  assert.equal(btn.textContent, "Update to 0.3.0");
  btn.click();
  assert.deepEqual(calls, ["install"]);
  native().onUpdate(JSON.stringify({ state: "verifying" }));
  assert.equal(btn.nextElementSibling!.textContent, "Installing…");
  native().onUpdate(JSON.stringify({ state: "error", message: "sha256 mismatch" }));
  assert.equal(btn.nextElementSibling!.textContent, "Update failed: sha256 mismatch");
  assert.equal(btn.disabled, false);
  el.remove();
});

test("an older or equal APK on the USB memory is not offered", () => {
  fakeBridge({ checkUpdate: () => JSON.stringify({ found: true, version: "0.2.0" }) });
  const el = infoScreen(createContext(new ParamStore()));
  assert.equal(el.querySelector("#apk-update-btn"), null);
  assert.match(el.querySelector("#apk-update")!.textContent!, /not newer/);
});

async function modulePiano(version: string | null) {
  const p = await pianoPair();
  const moduleSent: string[] = [];
  p.piano.onMessage((m) => {
    if (m[1] !== 0x00) return;
    moduleSent.push(toHex(m));
    if (version && m[6] === 0x10 && m[7] === 0x04) p.piano.send(Uint8Array.from([0xf0, 0, 2, 8, 0x10, 0x55, 0x20, 0x04, ...[...version].map((c) => c.charCodeAt(0)), 0xf7]));
  });
  return { ...p, moduleSent };
}

test("DFU flow: version query -> package -> enter DFU -> scan -> progress -> done", async () => {
  const calls = fakeBridge();
  const { session, moduleSent } = await modulePiano("2.1.0");
  const states: string[] = [];
  const run = D.runDfu(session, (s) => states.push(s.phase + (s.progress !== undefined ? `:${s.progress}` : "")));
  await tick(20);
  assert.deepEqual(moduleSent, ["F0 00 02 08 10 55 10 04 00 F7", "F0 00 02 08 10 55 7F 00 01 F7"], "requestFirmware then enterDfu");
  assert.deepEqual(calls, ["dfu:/mnt/usb/CA79NG/blemidi.zip"]);
  native().onDfu(JSON.stringify({ state: "scan" }));
  native().onDfu(JSON.stringify({ progress: 40 }));
  native().onDfu(JSON.stringify({ progress: 100, complete: 1 }));
  const final = await run;
  assert.equal(final.phase, "done");
  assert.equal(final.moduleVersion, "2.1.0");
  assert.deepEqual(states, ["query", "package", "enter", "scan", "upload:40", "done:100"]);
});

test("DFU: one at a time - a second run while one runs (a rebuilt screen's button) sends nothing and starts nothing", async () => {
  const calls = fakeBridge();
  const { session, moduleSent } = await modulePiano("2.1.0");
  const first = D.runDfu(session, () => {});
  const states: string[] = [];
  const secondRun = D.runDfu(session, (s) => states.push(s.phase));
  await tick(20);
  const sentEnter = moduleSent.filter((m) => m.endsWith("7F 00 01 F7")).length;
  const started = [...calls];
  native().onDfu(JSON.stringify({ progress: 100, complete: 1 }));
  const second = await secondRun;
  assert.equal((await first).phase, "done");
  assert.equal(sentEnter, 1, "one enter-DFU");
  assert.deepEqual(started, ["dfu:/mnt/usb/CA79NG/blemidi.zip"], "one startDfu");
  assert.deepEqual([second.phase, second.message], ["error", "busy"]);
  assert.deepEqual(states, ["error"]);
  // released: the next run goes through (up to date now)
  fakeBridge({ findDfuPackage: () => JSON.stringify({ path: "/x/blemidi.zip", version: "2.1.0" }) });
  assert.equal((await D.runDfu(session, () => {})).phase, "uptodate");
});

test("DFU: up-to-date module is not flashed; missing package and bootloader errors are reported", async () => {
  let calls = fakeBridge({ findDfuPackage: () => JSON.stringify({ path: "/x/blemidi.zip", version: "2.1.0" }) });
  let p = await modulePiano("2.1.0");
  assert.equal((await D.runDfu(p.session, () => {})).phase, "uptodate");
  assert.equal(p.moduleSent.length, 1, "no enter-DFU sent");
  assert.deepEqual(calls, []);

  fakeBridge({ findDfuPackage: () => "{}" });
  p = await modulePiano("2.1.0");
  const r = await D.runDfu(p.session, () => {});
  assert.deepEqual([r.phase, r.message], ["error", "no package"]);

  // module silent (old firmware) -> still updates; the shell reports a scan timeout
  calls = fakeBridge();
  p = await modulePiano(null);
  const run = D.runDfu(p.session, () => {}, { queryTimeoutMs: 30 });
  await tick(60);
  native().onDfu(JSON.stringify({ error: "bootloader DfuTarg/DFUMI1P not found" }));
  const e = await run;
  assert.equal(e.phase, "error");
  assert.equal(e.moduleVersion, null);
  assert.match(e.message!, /not found/);
  assert.equal(calls.length, 1);
});

test("Information screen drives the DFU and shows progress", async () => {
  fakeBridge();
  const { session } = await modulePiano("2.0.0");
  const el = infoScreen(createContext(new ParamStore(), { session: () => session }));
  document.body.append(el);
  (el.querySelector("#dfu-btn") as HTMLButtonElement).click();
  await tick(20);
  native().onDfu(JSON.stringify({ progress: 55 }));
  assert.equal(el.querySelector("#dfu-status")!.textContent, "Updating… 55%");
  assert.equal((el.querySelector("#dfu-progress") as HTMLProgressElement).value, 55);
  assert.equal(el.querySelector("#ble-version")!.textContent, "2.0.0");
  native().onDfu(JSON.stringify({ progress: 100, complete: 1 }));
  await tick(5);
  assert.equal(el.querySelector("#dfu-status")!.textContent, "Module updated.");
  void byText;
  el.remove();
});

test("scripts/mkupdate.mjs writes a package whose manifest the app accepts", async () => {
  const { mkdtempSync, writeFileSync, readFileSync, rmSync } = await import("node:fs");
  const { tmpdir } = await import("node:os");
  const { join } = await import("node:path");
  const { execFileSync } = await import("node:child_process");
  const { createHash } = await import("node:crypto");
  const dir = mkdtempSync(join(tmpdir(), "ca79ng-upd-"));
  try {
    writeFileSync(join(dir, "fake.apk"), "not really an apk");
    execFileSync(process.execPath, ["scripts/mkupdate.mjs", join(dir, "fake.apk"), join(dir, "usb"), "9.8.7"]);
    const m = U.parseManifest(readFileSync(join(dir, "usb/CA79NG/manifest.json"), "utf8"));
    assert.ok(m);
    assert.equal(m.version, "9.8.7");
    assert.equal(m.sha256, createHash("sha256").update(readFileSync(join(dir, "usb/CA79NG/app.apk"))).digest("hex"));
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
