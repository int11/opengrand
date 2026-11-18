// Desktop app (Electron shell with the in-app Bluetooth list), owner 2026-11-19: 「연결」 opened the app's dialog and
// its Bluetooth row then opened a second window (the shell's chooser). Now the dialog itself searches as soon as it
// opens (requestDevice inside the click) and lists what the shell reports; a row answers the request with that
// device, closing the dialog or picking another kind of device cancels it — quietly, and before the other link opens.
// A shell without onDevices (older build) keeps the 「Bluetooth MIDI (pair new)…」 row.
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const i18n = await import("../src/i18n.ts");
const picker = await import("../src/ui/picker.ts");
const { desktopBleScan, desktopBleScanAvailable } = await import("../src/transport/desktop.ts");
type BtFound = import("../src/transport/desktop.ts").BtFound;
type LinkCandidate = import("../src/transport/link.ts").LinkCandidate;
type LinkChoice = import("../src/state/connection.ts").LinkChoice;

(dom.window.HTMLDialogElement.prototype as unknown as { showModal(): void }).showModal = function (this: HTMLDialogElement) { this.open = true; };
(dom.window.HTMLDialogElement.prototype as unknown as { close(): void }).close = function (this: HTMLDialogElement) {
  if (!this.open) return;
  this.open = false;
  this.dispatchEvent(new dom.window.Event("close"));
};

/**
 * The shell as the page sees it (preload.cjs) plus Web Bluetooth's requestDevice: a request stays open until the
 * shell answers it. Like Electron, the shell holds a request only once it has reported a device for it (`held`).
 */
function fakeDesktop(opt: { gesture?: boolean; settleMs?: number } = {}) {
  const log: string[] = [];
  const listeners = new Set<(l: BtFound[]) => void>();
  let open: { resolve(d: { id: string; name: string }): void; reject(e: Error): void } | null = null;
  let held = false;
  let list: BtFound[] = [];
  const settle = (f: () => void) => (opt.settleMs ? void setTimeout(f, opt.settleMs) : f());
  const g: Record<string, unknown> = {
    ca79Desktop: {
      shell: "electron",
      bluetooth: {
        remembered: null,
        reconnect: async () => "none",
        onDevices: (cb: (l: BtFound[]) => void) => {
          log.push("listen");
          listeners.add(cb);
          return () => { if (listeners.delete(cb)) log.push("unlisten"); };
        },
        choose: (id: string) => {
          log.push(`choose(${JSON.stringify(id)})`);
          const o = open;
          if (!o || !held) return;
          const d = list.find((x) => x.deviceId === id);
          if (id !== "" && !d) return;
          open = null;
          held = false;
          settle(() => {
            log.push(id ? "request resolved" : "request rejected");
            if (d) o.resolve({ id: `web-${d.deviceId}`, name: d.deviceName });
            else o.reject(Object.assign(new Error("User cancelled the requestDevice() chooser."), { name: "NotFoundError" }));
          });
        },
      },
    },
  };
  const request = () => {
    log.push("requestDevice");
    if (opt.gesture === false) return Promise.reject(Object.assign(new Error("Must be handling a user gesture to show a permission request."), { name: "SecurityError" }));
    return new Promise<{ id: string; name: string }>((resolve, reject) => { open = { resolve, reject }; });
  };
  const chosen: string[] = [];
  const deps = (over: Partial<import("../src/ui/picker.ts").PickerDeps> = {}): import("../src/ui/picker.ts").PickerDeps => ({
    shell: false, dev: false, simUrl: "ws://localhost:8079/midi",
    list: async () => [],
    inUseKey: () => null,
    choose: (c: LinkChoice) => { chosen.push(c.name); log.push(`connect ${c.name}`); },
    requestBle: { name: "Bluetooth MIDI", open: async () => { throw new Error("not in this test"); } },
    bleScan: desktopBleScanAvailable(g) ? (on) => {
      const s = desktopBleScan(request, on, g)!;
      return { found: s.found.then((d) => ({ name: d.name, open: async () => { throw new Error("not in this test"); } })), pick: s.pick, cancel: s.cancel };
    } : undefined,
    scanMs: 60, ...over,
  });
  return {
    g, log, chosen, deps,
    get listening() { return listeners.size; },
    get pending() { return open !== null; },
    /** the shell reports the devices of the running request (sorted there: piano-like names first) */
    report(l: BtFound[]) { list = l; held = true; for (const cb of [...listeners]) cb(l); },
    /** the request ends by itself (Chromium gave the scan up) */
    giveUp() { const o = open; open = null; held = false; o?.reject(Object.assign(new Error("no devices"), { name: "NotFoundError" })); },
  };
}

const rows = (dlg: HTMLElement) => [...dlg.querySelectorAll<HTMLButtonElement>(".list button.item.dev")];
const head = (dlg: HTMLElement) => dlg.querySelector<HTMLElement>("#bt-scan")!;
const usb: LinkCandidate = { kind: "webmidi", id: "u1", name: "CA79 (USB-MIDI)", key: "webmidi:u1", open: async () => { throw new Error("not in this test"); } };

test("desktop picker: opening it starts the Bluetooth search at once; found devices are rows of the same dialog", async () => {
  i18n.setLang("ko");
  const d = fakeDesktop();
  const dlg = picker.openPicker(d.deps({ list: async () => [usb] }));
  assert.deepEqual(d.log, ["listen", "requestDevice"], "requestDevice inside openPicker (the click's gesture), the shell told first");
  assert.equal(head(dlg).dataset.state, "scanning");
  assert.equal(head(dlg).textContent, "블루투스 MIDI — 검색 중…");
  await tick(10);
  assert.equal(dlg.querySelector("#bt-pair"), null, "no 'pair new' row: nothing opens a second window");
  assert.equal(dlg.querySelector(".list")!.firstElementChild, head(dlg), "the Bluetooth row is first");
  assert.deepEqual(rows(dlg).map((b) => b.dataset.transport), ["usb"]);
  d.report([{ deviceId: "AA:01", deviceName: "Headphones" }]);
  d.report([{ deviceId: "BB:02", deviceName: "KAWAI CA79" }, { deviceId: "AA:01", deviceName: "Headphones" }, { deviceId: "CC:03", deviceName: "" }]);
  const got = rows(dlg).map((b) => [b.dataset.id ?? "", b.dataset.transport, b.querySelector(".dev-name")!.textContent, b.querySelector(".dev-sub")!.textContent, b.querySelector(".dev-go")!.textContent]);
  assert.deepEqual(got, [
    ["BB:02", "ble", "KAWAI CA79", "블루투스 MIDI", "연결"],
    ["AA:01", "ble", "Headphones", "블루투스 MIDI", "연결"],
    ["CC:03", "ble", "CC:03", "블루투스 MIDI", "연결"], // no name: its id
    ["", "usb", "CA79", "USB MIDI", "연결"],
  ], "the shell's order (piano first), then the other candidates");
  assert.equal(head(dlg).dataset.state, "scanning", "still searching while the request is open");
  assert.equal(dlg.querySelector("#bt-hint"), null);
  dlg.close();
  i18n.setLang("en");
});

test("desktop picker: a row answers the request with that device → the dialog closes and the app connects to it", async () => {
  const d = fakeDesktop();
  const dlg = picker.openPicker(d.deps());
  await tick(10);
  d.report([{ deviceId: "BB:02", deviceName: "KAWAI CA79" }, { deviceId: "AA:01", deviceName: "Headphones" }]);
  rows(dlg)[0]!.click();
  assert.equal(d.log.at(-2), 'choose("BB:02")');
  await tick(10);
  assert.deepEqual(d.chosen, ["KAWAI CA79"]);
  assert.equal(dom.window.document.querySelector("#picker"), null, "closed");
  assert.equal(d.listening, 0);
  assert.ok(!d.log.includes('choose("")'), "nothing was cancelled");
});

test("desktop picker: Close cancels the search (choose \"\") and connects nothing", async () => {
  const d = fakeDesktop();
  const dlg = picker.openPicker(d.deps());
  await tick(10);
  d.report([{ deviceId: "BB:02", deviceName: "KAWAI CA79" }]);
  [...dlg.querySelectorAll("button")].find((b) => b.textContent === "Close")!.click();
  await tick(10);
  assert.deepEqual(d.log.slice(2), ['choose("")', "request rejected", "unlisten"]);
  assert.equal(d.pending, false, "the request is over");
  assert.deepEqual(d.chosen, []);
  assert.equal(dom.window.document.querySelector("#picker"), null);

  // closed before the shell reported anything: it is told all the same (it cancels that request when it shows up)
  const e = fakeDesktop();
  const dlg2 = picker.openPicker(e.deps());
  dlg2.close();
  await tick(10);
  assert.deepEqual(e.log, ["listen", "requestDevice", 'choose("")', "unlisten"]);
  assert.deepEqual(e.chosen, []);
});

test("desktop picker: another row cancels the search first; its connection starts only after that request is over", async () => {
  const d = fakeDesktop({ settleMs: 30 });
  const dlg = picker.openPicker(d.deps({ list: async () => [usb] }));
  await tick(10);
  d.report([{ deviceId: "BB:02", deviceName: "KAWAI CA79" }]);
  rows(dlg).find((b) => b.dataset.transport === "usb")!.click();
  assert.equal(dom.window.document.querySelector("#picker"), null, "the dialog closes at once");
  assert.deepEqual(d.chosen, [], "not connecting yet: the Bluetooth request is still ending");
  await tick(80);
  assert.deepEqual(d.log.slice(2), ['choose("")', "unlisten", "request rejected", "connect CA79 (USB-MIDI)"]);

  // nothing reported yet = the shell holds no request it could answer: no wait
  const e = fakeDesktop({ settleMs: 30 });
  const dlg2 = picker.openPicker(e.deps({ list: async () => [usb] }));
  await tick(10);
  rows(dlg2)[0]!.click();
  await tick(5);
  assert.deepEqual(e.log.slice(2), ['choose("")', "unlisten", "connect CA79 (USB-MIDI)"]);
});

test("desktop picker: a search that ends by itself, or cannot start without a gesture → 'not found' row + Search again", async () => {
  i18n.setLang("ko");
  const d = fakeDesktop();
  const dlg = picker.openPicker(d.deps());
  await tick(100);
  assert.match(dlg.querySelector("#bt-hint")!.textContent!, /다른 기기\(PC · 폰\)가 피아노에 연결되어 있을 수 있습니다/, "nothing after a while: the usual reason, still searching");
  assert.equal(head(dlg).dataset.state, "scanning");
  d.giveUp();
  await tick(10);
  assert.equal(head(dlg).dataset.state, "none");
  assert.equal(d.listening, 0);
  dlg.querySelector<HTMLButtonElement>("#bt-rescan")!.click();
  assert.equal(head(dlg).dataset.state, "scanning");
  assert.deepEqual(d.log, ["listen", "requestDevice", "unlisten", "listen", "requestDevice"]);
  d.report([{ deviceId: "BB:02", deviceName: "KAWAI CA79" }]);
  assert.equal(rows(dlg).length, 1);
  dlg.close();
  await tick(10);

  // the dialog opened by itself (remembered piano did not come back): requestDevice refuses without a gesture
  const e = fakeDesktop({ gesture: false });
  const dlg2 = picker.openPicker(e.deps());
  await tick(10);
  assert.equal(head(dlg2).dataset.state, "none");
  assert.ok(dlg2.querySelector("#bt-rescan"));
  assert.equal(dlg2.querySelector("#bt-pair"), null);
  dlg2.close();
  assert.ok(!e.log.includes('choose("")'), "nothing left to cancel");
  i18n.setLang("en");
});

test("desktop shell without onDevices (older build): the 'pair new' row as before, no search of its own", async () => {
  const d = fakeDesktop();
  const bt = (d.g.ca79Desktop as { bluetooth: Record<string, unknown> }).bluetooth;
  delete bt.onDevices;
  assert.equal(desktopBleScanAvailable(d.g), false);
  assert.equal(desktopBleScan(async () => 1, () => {}, d.g), null);
  let asked = 0;
  const dlg = picker.openPicker(d.deps({ requestBle: { name: "Bluetooth MIDI", open: async () => { throw new Error("x"); } }, choose: () => { asked++; } }));
  await tick(10);
  assert.equal(dlg.querySelector("#bt-scan"), null);
  assert.deepEqual(d.log, []);
  dlg.querySelector<HTMLButtonElement>("#bt-pair")!.click();
  assert.equal(asked, 1, "chosen synchronously (the browser chooser needs the click's gesture)");
  assert.deepEqual(d.log, []);
});
