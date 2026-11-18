// Unit tests for the desktop shell's pure decisions (no Electron needed): node --test
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const path = require("node:path");
const p = require("../src/policy.cjs");

test("remote debugging and automation are off by default", () => {
  const o = p.parseOptions([], {});
  assert.equal(o.cdpPort, null);
  assert.equal(o.automation, false);
  assert.equal(o.btPattern, "CA79|KAWAI");
  assert.equal(o.query, "");
});

test("flags and env turn them on", () => {
  assert.equal(p.parseOptions(["--cdp"], {}).cdpPort, "9222");
  assert.equal(p.parseOptions(["--cdp=9333"], {}).cdpPort, "9333");
  assert.equal(p.parseOptions([], { CA79_CDP_PORT: "9444" }).cdpPort, "9444");
  assert.equal(p.parseOptions(["--automation"], {}).automation, true);
  assert.equal(p.parseOptions([], { CA79_AUTOMATION: "1" }).automation, true);
  assert.equal(p.parseOptions([], { CA79_AUTOMATION: "0" }).automation, false);
  assert.equal(p.parseOptions([], { PW_BT_DEVICE: "Piano" }).btPattern, "Piano");
  assert.equal(p.parseOptions(["--query=?log&tab=info"], {}).query, "log&tab=info");
  assert.throws(() => p.parseOptions(["--cdp=abc"], {}));
});

test("permissions: only the app origin, only the listed ones", () => {
  for (const perm of ["midi", "midiSysex", "usb"]) assert.ok(p.permissionAllowed(perm, "app://ca79ng/index.html"), perm);
  assert.ok(!p.permissionAllowed("geolocation", "app://ca79ng/"));
  assert.ok(!p.permissionAllowed("media", "app://ca79ng/"));
  assert.ok(!p.permissionAllowed("midiSysex", "https://example.com/"));
  assert.ok(!p.permissionAllowed("midiSysex", "app://other/"));
  assert.ok(!p.permissionAllowed("midiSysex", "file:///C:/x.html"));
  assert.ok(!p.permissionAllowed("midiSysex", ""));
  assert.ok(p.devicePermissionAllowed("usb", "app://ca79ng"));
  assert.ok(!p.devicePermissionAllowed("hid", "app://ca79ng"));
  assert.ok(!p.devicePermissionAllowed("usb", "https://example.com"));
});

test("app:// requests stay inside the app folder", () => {
  const root = path.resolve("/srv/dist");
  assert.equal(p.resolveAppFile(root, "app://ca79ng/"), path.join(root, "index.html"));
  assert.equal(p.resolveAppFile(root, "app://ca79ng/index.html?log"), path.join(root, "index.html"));
  assert.equal(p.resolveAppFile(root, "app://ca79ng/app-X.js"), path.join(root, "app-X.js"));
  assert.equal(p.resolveAppFile(root, "app://ca79ng/a%20b.png"), path.join(root, "a b.png"));
  assert.equal(p.resolveAppFile(root, "app://ca79ng/%2e%2e/secret"), path.join(root, "secret")); // URL parser folds it
  assert.equal(p.resolveAppFile(root, "app://ca79ng/..%2fsecret"), null);
  assert.equal(p.resolveAppFile(root, "app://other/index.html"), null);
  assert.equal(p.resolveAppFile(root, "https://ca79ng/index.html"), null);
  assert.equal(p.mimeType("x/app-1.js"), "text/javascript; charset=utf-8");
  assert.equal(p.mimeType("manifest.webmanifest"), "application/manifest+json");
});

test("automation picks only a matching name, never an arbitrary device", () => {
  const devs = [
    { deviceId: "a", deviceName: "" },
    { deviceId: "b", deviceName: "Headphones" },
    { deviceId: "c", deviceName: "KAWAI CA79 MIDI" },
  ];
  assert.equal(p.autoPick(devs, "CA79|KAWAI"), "c");
  assert.equal(p.autoPick(devs.slice(0, 2), "CA79|KAWAI"), null);
  assert.equal(p.autoPick([], "CA79|KAWAI"), null);
  assert.equal(p.autoPick(devs, "head"), "b");
  assert.deepEqual(p.sortDevices(devs).map((d) => d.deviceId), ["c", "b", "a"]);
});

test("reconnect picks only the remembered device id; the remembered device survives a restart as json", () => {
  const rem = p.lastBluetoothOf("BB:02", [{ deviceId: "AA:01", deviceName: "Headphones" }, { deviceId: "BB:02", deviceName: "KAWAI CA79" }]);
  assert.deepEqual(rem, { deviceId: "BB:02", deviceName: "KAWAI CA79" });
  assert.equal(p.lastBluetoothOf("", []), null, "a cancel stores nothing");
  assert.deepEqual(p.parseLastBluetooth(JSON.stringify(rem)), rem);
  assert.equal(p.parseLastBluetooth("{"), null);
  assert.equal(p.parseLastBluetooth(JSON.stringify({ deviceName: "CA79" })), null);
  // same name, another id (a second CA79, a re-paired module): not picked
  assert.equal(p.reconnectPick([{ deviceId: "CC:03", deviceName: "KAWAI CA79" }], rem), null);
  assert.equal(p.reconnectPick([{ deviceId: "AA:01", deviceName: "Headphones" }, { deviceId: "BB:02", deviceName: "" }], rem), "BB:02");
  assert.equal(p.reconnectPick([], rem), null);
  assert.equal(p.reconnectPick([{ deviceId: "BB:02", deviceName: "x" }], null), null);
});

test("the app window is never throttled while minimized (field 2026-11-03: OTA ~7 min instead of ~83 s)", () => {
  const w = p.appWebPreferences("/x/preload.cjs");
  assert.equal(w.backgroundThrottling, false);
  assert.deepEqual([w.contextIsolation, w.sandbox, w.nodeIntegration, w.preload], [true, true, false, "/x/preload.cjs"]);
  for (const sw of ["disable-background-timer-throttling", "disable-renderer-backgrounding", "disable-backgrounding-occluded-windows"]) assert.ok(p.NO_THROTTLE_SWITCHES.includes(sw), sw);
});
