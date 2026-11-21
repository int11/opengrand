// Tabs and the app's own items on a phone / PC (user feedback 2026-11-02): every tab in
// one row (narrow: a bottom row that scrolls sideways, wide: the side list — same buttons, styles.css lays them
// out), no 「더 보기」, and language / UI tools / MIDI log in Settings → 「앱 · 화면」. Outside the panel shell the
// connect button and its menu stay.
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const doc = dom.window.document;
(globalThis as unknown as { location: Location }).location = dom.window.location;
(globalThis as unknown as { addEventListener: unknown }).addEventListener = dom.window.addEventListener.bind(dom.window);
(dom.window.HTMLDialogElement.prototype as unknown as { showModal(): void }).showModal = function (this: HTMLDialogElement) { this.open = true; };
(dom.window.HTMLDialogElement.prototype as unknown as { close(): void }).close = function (this: HTMLDialogElement) { this.open = false; };
localStorage.setItem("ca79ng.settings", JSON.stringify({ lang: "ko" }));
const { TAB_IDS } = await import("../src/state/settings.ts");
await import("../src/main.ts");
await tick(20);

const tabs = () => [...doc.querySelectorAll<HTMLButtonElement>("nav #tabbar button[data-tab]")];

test("tab row: all nine tabs, in order, each with icon and label — no 「더 보기」, no 「피아노」 (in 「음색」 since 2026-11-09)", () => {
  assert.deepEqual(tabs().map((b) => b.dataset.tab), [...TAB_IDS]);
  assert.deepEqual(tabs().map((b) => b.querySelector(".lbl")!.textContent),
    ["음색", "뮤직", "메트로놈", "테크니션", "코드", "레코더", "블루투스", "설정", "정보"]);
  for (const b of tabs()) assert.ok(b.querySelector(".ico svg"), `${b.dataset.tab} has its icon`);
  assert.equal(doc.querySelector("#nav-more"), null);
  assert.equal(doc.querySelector("header #more"), null, "no header ⋯ either");
  assert.equal(doc.querySelector("nav .secondary"), null, "no tab hidden on narrow screens");
});

test("tab row: the open tab is lit and scrolled into view; the fade shows the side with more tabs", () => {
  const bar = doc.querySelector<HTMLElement>("#tabbar")!;
  const nav = doc.querySelector("nav")!;
  // jsdom has no layout: give the row the panel's sizes (480 px wide, nine 76 px tabs)
  Object.defineProperty(bar, "clientWidth", { value: 480, configurable: true });
  Object.defineProperty(bar, "scrollWidth", { value: 684, configurable: true });
  tabs().forEach((b, i) => {
    Object.defineProperty(b, "offsetLeft", { value: i * 76, configurable: true });
    Object.defineProperty(b, "offsetWidth", { value: 76, configurable: true });
  });
  bar.scrollLeft = 0;
  tabs()[0].click();
  assert.equal(bar.scrollLeft, 0);
  assert.equal(nav.classList.contains("fade-l"), false);
  assert.equal(nav.classList.contains("fade-r"), true, "more tabs to the right");
  tabs()[8].click(); // 정보, the last one
  assert.equal(tabs()[8].classList.contains("on"), true);
  assert.equal(tabs().filter((b) => b.classList.contains("on")).length, 1);
  // centred on 8*76+38 = 646 → 406 (the browser clamps to scrollWidth - clientWidth = 204)
  assert.ok(bar.scrollLeft > 0, "scrolled towards the open tab");
  tabs()[0].click();
  assert.equal(doc.querySelector("main #app-settings"), null);
});

test("Settings → 「앱 · 화면」 first: language, startup screen, MIDI log; no panel-only items outside the panel shell", async () => {
  tabs().find((b) => b.dataset.tab === "settings")!.click();
  const card = doc.querySelector<HTMLElement>("main #app-settings")!;
  assert.ok(card, "the card is there");
  assert.equal(card, doc.querySelector("main .screen > section.card"), "first card on the tab");
  assert.equal(card.querySelector("h3")!.textContent, "앱 · 화면");
  const labels = [...card.querySelectorAll(".row > span")].map((s) => s.textContent);
  assert.deepEqual(labels, ["언어", "시작 화면", "MIDI 로그"]);
  assert.equal(card.querySelector("#ui-chooser"), null, "UI chooser only inside the panel shell");
  assert.equal(card.querySelector("#kawai-ui"), null, "Kawai UI only inside the panel shell");
  assert.equal([...doc.querySelectorAll("#system-settings .row > span")].some((s) => s.textContent === "언어"), false, "language moved out of 시스템");
  // MIDI log switch opens / closes the log and follows its floating close button
  const sw = card.querySelector<HTMLInputElement>("#midi-log-switch")!;
  assert.equal(sw.checked, false);
  sw.checked = true;
  sw.dispatchEvent(new dom.window.Event("change", { bubbles: true }));
  assert.equal(doc.querySelector<HTMLElement>("#midi-log")!.hidden, false);
  doc.querySelector<HTMLButtonElement>("#midi-log-toggle")!.click();
  assert.equal(doc.querySelector<HTMLElement>("#midi-log")!.hidden, true);
  assert.equal(sw.checked, false);
  // language from here re-draws the app in that language
  const lang = card.querySelector<HTMLSelectElement>("select")!;
  lang.value = "en";
  lang.dispatchEvent(new dom.window.Event("change", { bubbles: true }));
  await tick(5);
  assert.equal(doc.querySelector("main #app-settings h3")!.textContent, "App & screen");
  assert.equal(tabs()[0].querySelector(".lbl")!.textContent, "Sound");
});

test("phone / PC: the connect button stays and opens the device list", () => {
  const btn = doc.querySelector<HTMLButtonElement>("header #connect");
  assert.ok(btn, "connect button in the header");
  btn!.click();
  assert.ok(doc.querySelector("#picker"), "idle: the device list");
  doc.querySelector("#picker")!.remove();
});
