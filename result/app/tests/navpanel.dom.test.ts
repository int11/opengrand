// Inside the original panel shell (kwmcore) the panel is always connected to its piano (user 2026-11-02): no connect button, no 「연결 끊기」 / 「다른 기기에 연결…」, only the state 「피아노 연결됨」;
// the shell's auto-connect still links it. Settings → 「앱 · 화면」 has the UI chooser and the Kawai UI here.
// A fake shell forwards the page's MIDI to the firmware simulator's panel port (like npm run panelsim).
import { dom, tick } from "./_dom.ts";
import { after, test } from "node:test";
import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { SIM_BIN, SimProcessLink } from "../scripts/sim-link.ts";

const skip = !existsSync(SIM_BIN) && "firmware simulator not built (make -C ../firmware sim)";
const doc = dom.window.document;
type Kwm = { resolvePromise(c: string, j: string): void; rejectPromise(c: string, j: string): void; onMidiMessage(m: number[], id: string, t: string): void; dispatchEvent(id: string, a: unknown): void };
const g = globalThis as unknown as Record<string, unknown> & { KWM?: Kwm };

let sim: SimProcessLink | null = null;
after(() => sim?.close());

async function bootInShell(): Promise<void> {
  sim = new SimProcessLink({ port: "panel" });
  await sim.ready;
  sim.onMessage((m) => g.KWM?.onMidiMessage([...m], "USB", "0"));
  const devices = JSON.stringify([{ id: "USB", name: "CA79", driver: "USB" }]);
  g.kwmcore = {
    postMessage(url: string) {
      const i = url.indexOf("?");
      const cmd = url.substring(10, i - 1);
      const obj = JSON.parse(url.substring(i + 1)) as { msg?: number[] };
      setTimeout(() => {
        if (cmd === "sendMidiMessage") sim!.send(Uint8Array.from(obj.msg!));
        else if (cmd === "getMidiDevices") g.KWM!.resolvePromise(cmd, devices);
        else g.KWM!.rejectPromise(cmd, "{}");
      }, 0);
    },
  };
  // ?embedded=0: no 30 s "never connected → chooser" timer in this test process; kwmShell is what matters here
  dom.reconfigure({ url: "http://localhost/ng/index.html?embedded=0" });
  (globalThis as unknown as { location: Location }).location = dom.window.location;
  (globalThis as unknown as { addEventListener: unknown }).addEventListener = dom.window.addEventListener.bind(dom.window);
  (dom.window.HTMLDialogElement.prototype as unknown as { showModal(): void }).showModal = function (this: HTMLDialogElement) { this.open = true; };
  localStorage.setItem("ca79ng.settings", JSON.stringify({ lang: "ko" }));
  g.KWM = { __ready: {} } as unknown as Kwm; // deviceready already fired (KWMWebViewClient.onPageFinished), kept by installKwm
  await import("../src/main.ts");
  for (let i = 0; i < 300 && doc.querySelector("header")!.getAttribute("data-phase") !== "ready"; i++) await tick(50);
}

test("panel shell: auto-connects, header shows only 「피아노 연결됨」 — no connect button or menu", { skip }, async () => {
  await bootInShell();
  const header = doc.querySelector("header")!;
  assert.equal(header.getAttribute("data-phase"), "ready", "the shell's auto-connect linked the piano");
  assert.equal(header.querySelector(".status")!.textContent, "피아노 연결됨");
  assert.equal(doc.querySelector("#connect"), null, "no connect button");
  // the Bluetooth chip and the power button - no connect button
  assert.deepEqual([...header.querySelectorAll("button")].map((b) => b.id), ["bt-audio", "power-btn"]);
  // no way to the connected menu anywhere: walk every tab and look for its items
  for (const b of doc.querySelectorAll<HTMLButtonElement>("nav #tabbar button[data-tab]")) {
    b.click();
    await tick(5);
    assert.equal(doc.querySelector("#conn-menu, [data-action=disconnect], [data-action=other-device]"), null, `tab ${b.dataset.tab}`);
    assert.ok(![...doc.querySelectorAll("button")].some((x) => !x.hidden && /연결 끊기|다른 기기에 연결/.test(x.textContent ?? "")), `tab ${b.dataset.tab}`);
  }
  doc.querySelector<HTMLButtonElement>("nav button[data-tab=bluetooth]")!.click();
  assert.equal(doc.querySelector<HTMLElement>("#bt-midi-connect")!.hidden, true, "the Bluetooth tab's connect-menu button is hidden too");
});

test("panel shell: Settings → 「앱 · 화면」 has language, MIDI log, UI chooser and the Kawai UI", { skip }, () => {
  doc.querySelector<HTMLButtonElement>("nav button[data-tab=settings]")!.click();
  const card = doc.querySelector<HTMLElement>("main #app-settings")!;
  assert.ok(card);
  assert.deepEqual([...card.querySelectorAll(".row > span")].map((s) => s.textContent), ["언어", "시작 화면", "MIDI 로그"]);
  assert.equal(card.querySelector("#ui-chooser")!.textContent, "UI 선택");
  assert.equal(card.querySelector("#kawai-ui")!.textContent, "Kawai 원본 UI");
});
