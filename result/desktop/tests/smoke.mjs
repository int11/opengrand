// Smoke test of the desktop shell with Playwright's Electron driver (the same path automated device tests use).
//   cd result/desktop && npm run smoke        (builds result/app first)
// Needs a display: Windows, or WSL with WSLg (DISPLAY=:0), or Linux under xvfb-run.
// Checks: app:// origin + secure context + ES module app booted, Web Bluetooth / Web MIDI present,
// MIDI SysEx permission granted, no service worker registered by the app (and the scheme allows one),
// the app's own connection dialog lists the Bluetooth devices the main process reports (in-app list; the chooser
// window only when the page does not listen; automation: only a name matching CA79|KAWAI), and CDP attach
// with --cdp=PORT.
// Windows build: CA79_ELECTRON_EXE=C:\\…\\OpenGrand\\OpenGrand.exe node tests/smoke.mjs (Windows Node).
import { _electron as electron, chromium } from "playwright-core";
import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { createRequire } from "node:module";
import assert from "node:assert/strict";

const here = dirname(fileURLToPath(import.meta.url));
const desktop = join(here, "..");
// CA79_ELECTRON_EXE=<packaged OpenGrand.exe>: test the Windows build instead of the dev Electron
const packaged = process.env.CA79_ELECTRON_EXE || null;
const electronPath = packaged || createRequire(import.meta.url)("electron");
const appArgs = packaged ? [] : [desktop];
const results = [];
// VS Code terminals set ELECTRON_RUN_AS_NODE; Electron would then start as plain Node
const baseEnv = { ...process.env };
delete baseEnv.ELECTRON_RUN_AS_NODE;
const ok = (name) => {
  results.push(name);
  console.log(`ok - ${name}`);
};

function profile() {
  return mkdtempSync(join(tmpdir(), "ca79-desktop-"));
}
// Windows: Electron's helper processes may still hold the profile for a moment after the app exits
function dropProfile(dir) {
  try {
    rmSync(dir, { recursive: true, force: true, maxRetries: 10, retryDelay: 300 });
  } catch (e) {
    console.log(`  (profile left behind: ${dir} - ${e.code})`);
  }
}

async function launch(extraEnv = {}, args = []) {
  const userData = profile();
  const app = await electron.launch({
    executablePath: electronPath,
    args: [...appArgs, ...args],
    env: { ...baseEnv, CA79_USER_DATA: userData, CA79_QUERY: "log&tab=info", ...extraEnv },
    timeout: 60000,
  });
  const out = [];
  app.process().stdout?.on("data", (d) => out.push(String(d)));
  app.mainLog = () => out.join("");
  const page = await app.firstWindow();
  await page.waitForSelector("#connect", { timeout: 30000 });
  return { app, page, userData };
}

// Fire the main process's select-bluetooth-device handler with a fake device list, as Electron does
// while Web Bluetooth scans. Returns what the handler passed to Electron's callback (null = not yet).
async function fakeScan(app, devices) {
  return app.evaluate(({ BrowserWindow }, devices) => {
    const win = BrowserWindow.getAllWindows().find((w) => w.webContents.getURL().startsWith("app://"));
    globalThis.__picked ??= [];
    let prevented = false;
    win.webContents.emit("select-bluetooth-device", { preventDefault: () => (prevented = true) }, devices, (id) => globalThis.__picked.push(id));
    return { prevented };
  }, devices);
}
const picked = (app) => app.evaluate(() => globalThis.__picked ?? []);

async function normalRun() {
  const { app, page, userData } = await launch();
  try {
    const env = await page.evaluate(async () => ({
      origin: location.origin,
      secure: isSecureContext,
      bluetooth: typeof navigator.bluetooth?.requestDevice,
      getDevices: typeof navigator.bluetooth?.getDevices,
      midi: typeof navigator.requestMIDIAccess,
      usb: typeof navigator.usb?.requestDevice,
      shell: window.ca79Desktop?.shell,
      electron: window.ca79Desktop?.electron,
      booted: document.getElementById("app")?.childElementCount ?? 0,
      sysex: (await navigator.permissions.query({ name: "midi", sysex: true })).state,
      swBefore: (await navigator.serviceWorker.getRegistrations()).length,
    }));
    console.log("  page:", JSON.stringify(env));
    assert.equal(env.origin, "app://ca79ng");
    assert.equal(env.secure, true);
    assert.ok(env.booted > 0, "app rendered (ES module ran)");
    ok("app:// origin, secure context, app booted");
    // field 2026-11-03: minimized window → throttled timers → OTA ~7 min instead of ~83 s
    const throttle = await app.evaluate(({ app: a, BrowserWindow }) => {
      const win = BrowserWindow.getAllWindows().find((w) => w.webContents.getURL().startsWith("app://"));
      return { bg: win.webContents.getBackgroundThrottling(), sw: a.commandLine.hasSwitch("disable-background-timer-throttling") && a.commandLine.hasSwitch("disable-renderer-backgrounding") };
    });
    assert.deepEqual(throttle, { bg: false, sw: true });
    ok("app window not throttled while hidden (backgroundThrottling false + switches)");
    assert.equal(env.bluetooth, "function");
    assert.equal(env.midi, "function");
    assert.equal(env.shell, "electron");
    ok("Web Bluetooth + Web MIDI APIs present, preload flag");
    assert.equal(env.sysex, "granted");
    ok("MIDI SysEx permission granted to the app origin");
    const midi = await page.evaluate(() =>
      Promise.race([
        navigator.requestMIDIAccess({ sysex: true }).then((a) => `ok inputs=${a.inputs.size} outputs=${a.outputs.size}`, (e) => `rejected ${e.name}: ${e.message}`),
        new Promise((r) => setTimeout(() => r("timeout"), 5000)),
      ]),
    );
    console.log("  requestMIDIAccess:", midi);
    // not a permission refusal (SecurityError / NotAllowedError). WSL has no ALSA sequencer, so there
    // Chromium's MIDI backend fails to start (InvalidStateError) - that is the platform, not the shell.
    const noAlsa = process.platform === "linux" && midi.startsWith("rejected InvalidStateError");
    assert.ok(midi.startsWith("ok") || noAlsa, midi);
    ok(noAlsa ? "requestMIDIAccess({ sysex: true }) not refused (no ALSA sequencer here)" : "requestMIDIAccess({ sysex: true }) resolves");
    assert.equal(env.swBefore, 0);
    const sw = await page.evaluate(async () => {
      const r = await navigator.serviceWorker.register("sw.js");
      const scope = r.scope;
      await r.unregister();
      return scope;
    });
    assert.equal(sw, "app://ca79ng/");
    ok("the app registers no service worker; the app:// scheme would allow one");

    // the app's own connection dialog searches for Bluetooth devices itself and lists what the main process reports
    // (in-app list: preload onDevices / choose) - no chooser window. requestDevice is stubbed (never settles), so
    // the lists below are the only ones, whatever Bluetooth adapter this machine has.
    await page.evaluate(() => {
      navigator.bluetooth.requestDevice = () => new Promise(() => {});
    });
    await page.click("#connect");
    await page.waitForSelector('#picker #bt-scan[data-state="scanning"]', { timeout: 10000 });
    assert.equal(await page.$("#picker #bt-pair"), null);
    ok("connection dialog searches for Bluetooth MIDI by itself (no 'pair new' row)");
    const r1 = await fakeScan(app, [{ deviceId: "AA:01", deviceName: "Headphones" }]);
    assert.equal(r1.prevented, true);
    await fakeScan(app, [
      { deviceId: "AA:01", deviceName: "Headphones" },
      { deviceId: "BB:02", deviceName: "KAWAI CA79" },
    ]);
    await page.waitForSelector('#picker button.item.dev[data-id="BB:02"]', { timeout: 10000 });
    const order = await page.$$eval("#picker button.item.dev[data-id]", (bs) => bs.map((b) => b.dataset.id));
    assert.deepEqual(order, ["BB:02", "AA:01"]);
    assert.deepEqual(await picked(app), []);
    assert.ok(!app.windows().some((w) => w.url().endsWith("picker.html")), "no chooser window");
    ok("in-app list: devices are rows of the app's dialog (piano first), no chooser window, nothing picked by itself");
    // an id that was never listed is not accepted
    await page.evaluate(() => window.ca79Desktop.bluetooth.choose("ZZ:99"));
    await page.waitForTimeout(200);
    assert.deepEqual(await picked(app), []);
    await page.click('#picker button.item.dev[data-id="BB:02"]');
    await page.waitForTimeout(300);
    assert.deepEqual(await picked(app), ["BB:02"]);
    ok("in-app list: the user's click on a row selects the device; an unlisted id is ignored");
    // cancel path: a new request, Esc closes the dialog -> ""
    await fakeScan(app, [{ deviceId: "BB:02", deviceName: "KAWAI CA79" }]);
    await page.keyboard.press("Escape");
    await page.waitForSelector("#picker", { state: "detached", timeout: 10000 });
    await page.waitForTimeout(300);
    assert.deepEqual(await picked(app), ["BB:02", ""]);
    ok("in-app list: closing the dialog answers the request with no device");

    // nobody listens (dialog closed; an app build without the in-app list): the chooser window, as before -
    // it opens, nothing is picked until the user clicks
    await fakeScan(app, [{ deviceId: "AA:01", deviceName: "Headphones" }]);
    const chooser = await app.waitForEvent("window", { predicate: (w) => w.url().endsWith("picker.html"), timeout: 10000 });
    await fakeScan(app, [
      { deviceId: "AA:01", deviceName: "Headphones" },
      { deviceId: "BB:02", deviceName: "KAWAI CA79" },
    ]);
    await chooser.waitForSelector('button[data-id="BB:02"]', { timeout: 10000 });
    assert.deepEqual(await chooser.$$eval("#list button", (bs) => bs.map((b) => b.dataset.id)), ["BB:02", "AA:01"]);
    assert.deepEqual(await picked(app), ["BB:02", ""]);
    ok("fallback: chooser window lists devices (piano first), picks nothing by itself");
    await chooser.click('button[data-id="BB:02"]');
    await page.waitForTimeout(300);
    assert.deepEqual(await picked(app), ["BB:02", "", "BB:02"]);
    ok("fallback: the user's click selects the device");
    // cancel path: a new request, close with Cancel → ""
    await fakeScan(app, [{ deviceId: "BB:02", deviceName: "KAWAI CA79" }]);
    const chooser2 = await app.waitForEvent("window", { predicate: (w) => w.url().endsWith("picker.html"), timeout: 10000 });
    await chooser2.click("#cancel");
    await page.waitForTimeout(300);
    assert.deepEqual(await picked(app), ["BB:02", "", "BB:02", ""]);
    ok("fallback: Cancel answers the request with no device");

    await reconnectChecks(app, page, userData);
  } finally {
    await app.close();
    dropProfile(userData);
  }
}

// Reconnect on start (Electron has no getDevices()): the main process remembers the chosen device and,
// only for the app's own reconnect request, answers the chooser with exactly that device.
// Runs after normalRun's clicks picked BB:02 "KAWAI CA79" (the last answer was a Cancel).
async function reconnectChecks(app, page, userData) {
  const stored = JSON.parse(readFileSync(join(userData, "last-bluetooth.json"), "utf8"));
  assert.deepEqual(stored, { deviceId: "BB:02", deviceName: "KAWAI CA79" });
  ok("reconnect: the chosen device is remembered in userData (a Cancel does not forget it)");

  // a reload = what the app sees after a restart: the preload reports the remembered name
  await page.reload();
  await page.waitForSelector("#connect", { timeout: 30000 });
  assert.equal(await page.evaluate(() => window.ca79Desktop.bluetooth.remembered), "KAWAI CA79");

  // the app's reconnect: the hook runs with a user gesture, the chooser answers only the remembered id
  const r = await page.evaluate(async () => {
    globalThis.__hookRuns = [];
    globalThis.__ca79ngDesktopReconnect = () => globalThis.__hookRuns.push(navigator.userActivation?.isActive ?? null);
    return window.ca79Desktop.bluetooth.reconnect();
  });
  assert.equal(r, "started");
  assert.deepEqual(await page.evaluate(() => globalThis.__hookRuns), [true], "hook ran once, with user activation");
  const before = (await picked(app)).length;
  await fakeScan(app, [{ deviceId: "AA:01", deviceName: "Headphones" }, { deviceId: "CC:03", deviceName: "KAWAI CA79" }]);
  await page.waitForTimeout(300);
  assert.equal((await picked(app)).length, before, "same name, other id: not picked");
  assert.ok(!app.windows().some((w) => w.url().endsWith("picker.html")), "no chooser window for the reconnect");
  await fakeScan(app, [{ deviceId: "CC:03", deviceName: "KAWAI CA79" }, { deviceId: "BB:02", deviceName: "KAWAI CA79" }]);
  await page.waitForTimeout(300);
  assert.deepEqual((await picked(app)).slice(before), ["BB:02"]);
  ok("reconnect: hook runs with a user gesture; only the remembered device id is picked, no chooser window");

  // without the app's request, the next chooser is the normal one again (no silent pick)
  await fakeScan(app, [{ deviceId: "BB:02", deviceName: "KAWAI CA79" }]);
  const chooser = await app.waitForEvent("window", { predicate: (w) => w.url().endsWith("picker.html"), timeout: 10000 });
  await chooser.click("#cancel");
  await page.waitForTimeout(300);
  assert.deepEqual((await picked(app)).slice(before), ["BB:02", ""]);
  ok("reconnect: a request the app did not start gets the normal chooser");

  // the app itself on start: a remembered BLE piano → reconnect through the shell (requestDevice with a
  // gesture, not refused for a missing one), no Chrome-flag note; not found here → the app's picker
  await page.evaluate(() => localStorage.setItem("ca79ng.lastLink", JSON.stringify({ key: "ble:old", name: "KAWAI CA79" })));
  await page.reload();
  await page.waitForSelector("#picker", { state: "attached", timeout: 30000 });
  const after = await page.evaluate(() => ({
    note: document.getElementById("reconnect-note")?.hidden,
    log: document.getElementById("midi-log")?.textContent ?? "",
  }));
  assert.equal(after.note, true, "no reconnect note: the desktop app reconnects by itself");
  console.log("  app start:", after.log.split("\n").filter((l) => /open|reconnect/i.test(l)).join(" | ") || "(log hidden)");
  assert.doesNotMatch(after.log, /user gesture/i);
  assert.match(app.mainLog(), /bluetooth: reconnect to KAWAI CA79 started/);
  ok("reconnect: the app asks the shell on start; piano not found → the app's connection picker");
}

async function automationRun() {
  const { app, userData } = await launch({ CA79_AUTOMATION: "1" });
  try {
    await fakeScan(app, [{ deviceId: "AA:01", deviceName: "Headphones" }, { deviceId: "CC:03", deviceName: "" }]);
    await new Promise((r) => setTimeout(r, 300));
    assert.deepEqual(await picked(app), []);
    const windows = app.windows().map((w) => w.url());
    assert.ok(!windows.some((u) => u.endsWith("picker.html")), "no chooser window in automation");
    await fakeScan(app, [{ deviceId: "AA:01", deviceName: "Headphones" }, { deviceId: "BB:02", deviceName: "CA79 MIDI" }]);
    await new Promise((r) => setTimeout(r, 300));
    assert.deepEqual(await picked(app), ["BB:02"]);
    ok("automation: waits through non-matching devices, picks the CA79|KAWAI name");
  } finally {
    await app.close();
    dropProfile(userData);
  }
}

async function cdpRun() {
  const port = 9339;
  const userData = profile();
  const child = spawn(electronPath, [...appArgs, `--cdp=${port}`], {
    env: { ...baseEnv, CA79_USER_DATA: userData },
    stdio: ["ignore", "pipe", "pipe"],
  });
  child.stdout.on("data", (d) => process.stdout.write(`  [electron] ${d}`));
  try {
    let version = null;
    for (let i = 0; i < 60 && !version; i++) {
      await new Promise((r) => setTimeout(r, 500));
      version = await fetch(`http://127.0.0.1:${port}/json/version`).then((r) => r.json(), () => null);
    }
    assert.ok(version, "CDP endpoint answers");
    const browser = await chromium.connectOverCDP(`http://127.0.0.1:${port}`);
    let page = null;
    for (let i = 0; i < 40 && !page; i++) {
      page = browser.contexts().flatMap((c) => c.pages()).find((p) => p.url().startsWith("app://ca79ng/")) ?? null;
      if (!page) await new Promise((r) => setTimeout(r, 250));
    }
    assert.ok(page, "app page visible over CDP");
    await page.waitForSelector("#connect", { timeout: 30000 });
    const origin = await page.evaluate(() => location.origin);
    assert.equal(origin, "app://ca79ng");
    ok(`--cdp=${port}: connectOverCDP finds and drives the app page (${version.Browser})`);
    await browser.close().catch(() => {});
  } finally {
    child.kill();
    await new Promise((r) => child.once("exit", r));
    dropProfile(userData);
  }
}

await normalRun();
await automationRun();
await cdpRun();
console.log(`smoke: ${results.length} checks passed`);
