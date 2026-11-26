// kwmcore transport (our web app inside the ORIGINAL panel shell) against a fake
// shell that behaves like the decompiled Java side: KWMCoreInterface.postMessage
// parsing, KWMCoreManager.resolvePromise/rejectPromise string escaping,
// KWM_onMidiMessage / KWM_onDeviceStateChange / KWMWebViewClient.onPageFinished.
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";

import { PianoSession } from "../src/protocol/session.ts";
import { decodeFrame, encodeFrame, toHex } from "../src/protocol/sysex.ts";
import {
  _resetKwmcoreForTests,
  autoLink,
  callNative,
  callPromise,
  embeddedMode,
  installKwm,
  installUiBundleFromUsb,
  kwmUrl,
  lcdBacklight,
  listKwmcore,
  setBacklight255,
  shellCallbackCounts,
  usbPath,
  versions,
} from "../src/transport/kwmcore.ts";
import { LoopbackLink } from "../src/transport/link.ts";

const g = globalThis as unknown as Record<string, unknown>;
const evalJs = (code: string): unknown => (0, eval)(code);
const tick = (ms = 5) => new Promise((r) => setTimeout(r, ms));

interface Dev {
  id: string;
  name: string;
  driver: string;
}

/** Fake KWMcore shell. Answers on a later tick, like evaluateJavascript. */
function fakeShell(opt: { devices?: Dev[]; silent?: string[]; backlight?: number } = {}) {
  const calls: { cmd: string; obj: Record<string, unknown> }[] = [];
  let devices: Dev[] = opt.devices ?? [{ id: "USB", name: "CA79", driver: "USB" }];
  let backlight = opt.backlight ?? 75;
  const [shellSide, piano] = LoopbackLink.pair();
  let detectKWM = false;
  shellSide.onMessage((m) => {
    if (detectKWM) evalJs(`KWM.onMidiMessage([${[...m].join(",")}],'USB','12.5')`);
  });
  const esc = (s: string) => s.replace(/\\/g, "\\\\").replace(/'/g, "\\'");
  const resolve = (cmd: string, p: string) => evalJs(`KWM.resolvePromise('${cmd}','${esc(p)}')`);
  const reject = (cmd: string, p: string) => evalJs(`KWM.rejectPromise('${cmd}','${esc(p)}')`);
  g.kwmcore = {
    postMessage(url: string) {
      // KWMCoreInterface.postMessage
      if (!url.startsWith("kwmcore://")) return;
      const i = url.indexOf("?");
      if (i === -1) return;
      const cmd = url.substring(10, i - 1);
      const obj = JSON.parse(url.substring(i + 1)) as Record<string, unknown>;
      calls.push({ cmd, obj });
      if (opt.silent?.includes(cmd)) return;
      setTimeout(() => {
        switch (cmd) {
          case "sendMidiMessage":
            shellSide.send(Uint8Array.from(obj.msg as number[]));
            break;
          case "getMidiDevices": {
            const j = JSON.stringify(devices);
            evalJs(`KWM.devices=${j}`);
            resolve(cmd, j);
            break;
          }
          case "getEmbeddedMode":
            resolve(cmd, "true");
            break;
          case "getLcdBacklight":
            resolve(cmd, `{"brightness":"${backlight}"}`);
            break;
          case "setLcdBacklight":
            backlight = obj.brightness as number;
            break;
          case "getUsbPath":
            resolve(cmd, `{"path":"/storage/USB'1"}`);
            break;
          case "getVersions":
            evalJs("KWM.localUiVersion='0.2.0'");
            evalJs("KWM.latestUiVersion='1.3.10'");
            evalJs("KWM.localAppVersion='3.4.12'");
            evalJs("KWM.latestAppVersion=''");
            evalJs("KWM.latestBleMidiVersion=''");
            resolve(cmd, "{}");
            break;
          case "downloadUiData":
          case "updateUiData":
            resolve(cmd, "{}");
            break;
          default:
            reject(cmd, "{}");
        }
      }, 1);
    },
  };
  return {
    calls,
    piano,
    get backlight() {
      return backlight;
    },
    /** KWMWebViewClient.onPageFinished */
    pageFinished() {
      if (evalJs("typeof KWM") !== "object") return false;
      detectKWM = true;
      evalJs(`KWM.devices=${JSON.stringify(devices)}`);
      evalJs(`KWM.dispatchEvent('deviceready',{"midi":true,"ble":false,"strageAccess":true})`);
      return true;
    },
    /** MidiManager.DeviceCallback → KWM_onDeviceStateChange */
    setDevices(d: Dev[]) {
      devices = d;
      evalJs("KWM.onDeviceStateChange()");
    },
  };
}

/** Piano side of the link: the replies a CA79 gives to the app's connect sequence. */
function fakePiano(link: LoopbackLink) {
  const tx = (fn: number, body: number[]) => link.send(encodeFrame({ dev: 0x00, fn, group: 0x04, machine: 0x1e, body }));
  const seen: string[] = [];
  link.onMessage((m) => {
    seen.push(toHex(m));
    const f = decodeFrame(m)!;
    if (f.fn === 0x7e && f.body[0] === 0x01 && f.body[1] === 0x01) tx(0x7e, [0x02, 0x00, 0x00]);
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) tx(0x7e, [0x05, 0x00, 0x00, 0x02]);
    if (f.fn === 0x7e && f.body[0] === 0x04 && f.body[1] === 0x41) tx(0x7e, [0x04, 0x01, ..."2.19".split("").map((c) => c.charCodeAt(0))]);
    if (f.fn === 0x02 && f.body[0] === 0x5f) {
      tx(0x10, [0x55, 0x00, 0x00, 95, 112, 0]);
      tx(0x03, [0x5f, 0x7f, 0x7f]);
    }
  });
  return seen;
}

afterEach(() => {
  _resetKwmcoreForTests();
  delete g.kwmcore;
});

test("URL format is what KWMCoreInterface.postMessage parses", () => {
  const url = kwmUrl("sendMidiMessage", { msg: [0xf0, 0xf7], id: "USB" });
  assert.equal(url, 'kwmcore://sendMidiMessage/?{"msg":[240,247],"id":"USB"}');
  const i = url.indexOf("?");
  assert.equal(url.substring(10, i - 1), "sendMidiMessage");
  assert.equal(kwmUrl("getVersions"), "kwmcore://getVersions/?{}");
  assert.throws(() => kwmUrl("a?b"));
  assert.equal(callNative("log"), false, "no bridge outside the shell");
});

test("KWM global: typeof object for the shell, keeps the boot stub's object and deviceready", () => {
  const stub = { devices: [], __ready: { midi: true } } as Record<string, unknown>;
  g.KWM = stub;
  const k = installKwm();
  assert.equal(k, stub);
  assert.equal(evalJs("typeof KWM"), "object");
  assert.deepEqual(k.__ready, { midi: true });
  for (const f of ["resolvePromise", "rejectPromise", "callbackNative", "onMidiMessage", "onDeviceStateChange", "dispatchEvent", "onSmfMidiMessage"]) {
    assert.equal(typeof (k as unknown as Record<string, unknown>)[f], "function", f);
  }
});

test("shellCallbackCounts counts every callbackNative, registered or not, and the test reset clears it", () => {
  const k = installKwm();
  k.callbackNative("onMetronomeEvent", '{"beatNo":0}', true);
  k.callbackNative("onMetronomeEvent", '{"beatNo":1}', true);
  k.callbackNative("getTimeProgress", "{}", true);
  assert.deepEqual(shellCallbackCounts(), { onMetronomeEvent: 2, getTimeProgress: 1 });
  _resetKwmcoreForTests();
  assert.deepEqual(shellCallbackCounts(), {});
});

test("auto-connect waits for deviceready, picks the USB device, carries MIDI both ways", async () => {
  const shell = fakeShell({ devices: [{ id: "AA:BB", name: "phone BLE", driver: "BLE" }, { id: "USB", name: "CA79", driver: "USB" }] });
  installKwm();
  const pending = autoLink(1000);
  await tick(10);
  assert.equal(shell.calls.length, 0, "nothing sent before deviceready");
  assert.ok(shell.pageFinished());
  const link = await pending;
  assert.ok(link);
  assert.equal(link.id, "USB");
  assert.equal(link.kind, "kwmcore");

  const got: string[] = [];
  link.onMessage((m) => got.push(toHex(m)));
  shell.piano.onMessage((m) => shell.piano.send(m)); // echo
  link.send(Uint8Array.from([0xf0, 0x40, 0x7f, 0x7e, 0x08, 0x02, 0x01, 0x01, 0xf7]));
  await tick(10);
  const send = shell.calls.find((c) => c.cmd === "sendMidiMessage")!;
  assert.deepEqual(send.obj, { msg: [0xf0, 0x40, 0x7f, 0x7e, 0x08, 0x02, 0x01, 0x01, 0xf7], id: "USB" });
  assert.deepEqual(got, ["F0 40 7F 7E 08 02 01 01 F7"]);
  // other devices' traffic is not ours
  evalJs("KWM.onMidiMessage([144,60,100],'AA:BB','1.0')");
  assert.equal(got.length, 1);
});

test("a full PianoSession connect runs over the shell bridge", async () => {
  const shell = fakeShell();
  const seen = fakePiano(shell.piano);
  installKwm();
  shell.pageFinished();
  const link = await autoLink(500);
  const s = new PianoSession(link!);
  const id = await s.connect(300);
  assert.equal(id.model, "CA79");
  assert.equal(id.version, "2.19");
  assert.equal(s.state, "ready");
  assert.equal(seen[0], "F0 40 7F 7E 08 02 01 01 F7");
  assert.ok(shell.calls.filter((c) => c.cmd === "sendMidiMessage").length >= 6);
});

test("device removed → link closes; listing returns the shell's devices", async () => {
  const shell = fakeShell();
  installKwm();
  shell.pageFinished();
  const link = (await autoLink(500))!;
  let closed = 0;
  link.onClose(() => closed++);
  shell.setDevices([]);
  await tick(20);
  assert.equal(closed, 1);
  link.send(Uint8Array.from([0xfe])); // no-op after close
  assert.equal(shell.calls.filter((c) => c.cmd === "sendMidiMessage").length, 0);
  shell.setDevices([{ id: "USB", name: "CA79", driver: "USB" }]);
  const c = await listKwmcore();
  assert.deepEqual(c.map((x) => [x.kind, x.id]), [["kwmcore", "USB"]]);
});

test("no device: autoLink resolves null (caller retries on device change)", async () => {
  const shell = fakeShell({ devices: [] });
  installKwm();
  shell.pageFinished();
  assert.equal(await autoLink(200), null);
});

test("promises are keyed by command name: overlapping calls are serialised, timeouts free the queue", async () => {
  const shell = fakeShell({ silent: ["getJniVersion"] });
  installKwm();
  const a = lcdBacklight();
  const b = lcdBacklight();
  assert.equal(shell.calls.filter((c) => c.cmd === "getLcdBacklight").length, 1, "second call waits for the first answer");
  assert.equal(await a, 75);
  assert.equal(await b, 75);
  assert.equal(shell.calls.filter((c) => c.cmd === "getLcdBacklight").length, 2);
  await assert.rejects(callPromise("getJniVersion", null, 20), /rejected/);
  await assert.rejects(callPromise("unknownCmd", null, 200), /rejected/);
});

test("panel services: embedded mode, backlight scale, USB path, versions, UI install", async () => {
  const shell = fakeShell();
  installKwm();
  assert.equal(await embeddedMode(), true);
  setBacklight255(255);
  await tick(5);
  assert.equal(shell.backlight, 100);
  setBacklight255(64);
  await tick(5);
  assert.equal(shell.backlight, 25);
  assert.equal(await usbPath(), "/storage/USB'1", "quote survives the shell's escaping");
  assert.deepEqual(await versions(), { localUi: "0.2.0", usbUi: "1.3.10", localApp: "3.4.12", usbApp: null, usbBleMidi: null });
  await installUiBundleFromUsb();
  assert.deepEqual(shell.calls.filter((c) => /UiData/.test(c.cmd)).map((c) => c.cmd), ["downloadUiData", "updateUiData"]);
});

test("deviceready that arrived before the app loaded is not lost", async () => {
  const shell = fakeShell();
  // the boot stub in ng/index.html records deviceready on KWM.__ready
  g.KWM = { devices: [], dispatchEvent(id: string, arg: unknown) { if (id === "deviceready") (g.KWM as Record<string, unknown>).__ready = arg; } };
  shell.pageFinished();
  installKwm();
  const t0 = Date.now();
  const link = await autoLink(2000);
  assert.ok(link);
  assert.ok(Date.now() - t0 < 500, "did not wait for the fallback timeout");
});
