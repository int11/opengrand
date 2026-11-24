// Panel screen reporter (panelui/panelscreen.js, ops 0x35 / 0x36):
// the report's content and size bound on a panel page, the hook order under the UI loader (also the field loader
// that swallows ops 0x35-0x3F), the chunked transfer through the piano's relay model (tests/_puirelay.ts — the
// cave's rules) with losses and a link that cannot carry long replies, the 「패널 화면 보기」 card and the
// window.__panelReport / __panelSnapshot hooks. The DOM → canvas painter itself runs in Chromium (npm run panelsim).
import { dom as appDom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";
import { randomBytes } from "node:crypto";
import { JSDOM, VirtualConsole } from "jsdom";

import { chooserHtml, buildApp, probeFiles } from "../scripts/bundlecore.ts";
import { keyFromPem, loaderSource, newKeyPem, screenSource, uiConfig } from "../scripts/panelpkg.ts";
import { actRequest, captureRequest, decodeCapture, estimateScreenTransfer, PanelScreenClient, PS_KIND, readRequest, reportLines } from "../src/protocol/panelscreen.ts";
import { parsePuiReply, puiRequest, PanelUiOp, type PuiPort } from "../src/protocol/panelui.ts";
import { installPanelScreenGlobals, panelScreenBox, type PanelScreenGlobals } from "../src/ui/panelscreenbox.ts";
import { MemStorage, PanelRelayModel } from "./_puirelay.ts";

const KEY = keyFromPem(newKeyPem());
const CFG = uiConfig(KEY, { version: "0.2.0", seq: 1000 });
const SRC = screenSource();

type ScreenApi = {
  version: number;
  install(k: unknown): unknown;
  provide(fn: () => Record<string, unknown>): void;
  report(max?: number): Record<string, unknown> & { t: string[]; err: string[]; js: string[]; cut?: number };
  render: (o: { w: number; kb: number }, cb: (rc: number, r: unknown) => void) => void;
};
type PanelWin = Window & { CA79Screen: ScreenApi; CA79UI: { hookKwm(k: unknown): void; setPage(p: number): void }; KWM: Record<string, unknown>; eval(s: string): unknown };

const APP_HTML = `<!doctype html><html><head><title>OpenGrand</title></head><body><div id="app">
<header><strong class="brand">OpenGrand</strong><span class="status">피아노 연결됨</span></header>
<div class="banner" role="alert">다른 기기가 조작 중입니다</div>
<nav><div class="tabs"><button data-tab="sound" class="on">음색</button><button data-tab="settings">설정</button></div></nav>
<main><section class="card"><h3>화면</h3>
<label class="row"><span>밝기</span><input type="range" min="1" max="10" value="8"></label>
<label class="row"><span>화면 자동 꺼짐</span><select><option>안 함</option><option selected>1분</option></select></label>
<label class="row"><span>시작 탭</span><input type="checkbox" checked></label>
<p hidden>SECRET hidden line</p>
<details><summary>개발자</summary><p>HIDDEN closed details body</p></details>
<button disabled>보내기</button>
</section></main></div></body></html>`;

/** A panel page: the UI loader (hooked first, as ng/index.html does), then the screen reporter, with a fake shell. */
function panelPage(html = APP_HTML, o: { loader?: string | null; install?: boolean; url?: string } = {}) {
  const sent: number[][] = [];
  const shell: string[] = [];
  const app: number[][] = [];
  const vc = new VirtualConsole();
  const d = new JSDOM(html, { url: o.url ?? "file:///data/data/x/files/appUI/ng/index.html?embedded=1", runScripts: "outside-only", virtualConsole: vc });
  const w = d.window as unknown as PanelWin;
  Object.defineProperty(w, "localStorage", { value: new MemStorage(), configurable: true });
  (w as unknown as Record<string, unknown>).kwmcore = {
    postMessage(u: string) {
      if (u.startsWith("kwmcore://sendMidiMessage/")) sent.push(JSON.parse(u.slice(u.indexOf("?") + 1)).msg);
      else shell.push(u);
    },
  };
  w.KWM = { onMidiMessage: () => {} };
  if (o.loader !== null) {
    w.eval(o.loader ?? loaderSource(CFG));
    w.CA79UI.hookKwm(w.KWM);
  }
  w.eval(SRC);
  if (o.install !== false) w.CA79Screen.install(w.KWM);
  // the app assigns its handler later (installKwm: Object.assign → the setters)
  Object.assign(w.KWM, { onMidiMessage: (m: number[]) => void app.push(m) });
  const deliver = (m: number[]) => (w.KWM.onMidiMessage as (a: number[], id: string, t: string) => void)(m, "USB", "0");
  return { w, sent, shell, app, deliver, close: () => d.window.close() };
}

const bytesOf = (s: string) => Buffer.byteLength(s, "utf8");

test("report: page, tab, backlight, visible labels and values, error banner, JS errors; hidden things left out", () => {
  const p = panelPage();
  try {
    p.w.localStorage.setItem("ca79ng.settings", JSON.stringify({ brightness: 6, autoOffMin: 1 }));
    p.w.CA79Screen.provide(() => ({ pg: "app", ver: "0.2.3", tab: "sound", con: "ready" }));
    p.w.dispatchEvent(new (p.w as unknown as { ErrorEvent: typeof ErrorEvent }).ErrorEvent("error", { message: "TypeError: x is not a function", filename: "file:///a/app.js", lineno: 3, colno: 9 }));
    const r = p.w.CA79Screen.report();
    assert.equal(r.pg, "app");
    assert.equal(r.tab, "sound");
    assert.equal(r.con, "ready");
    assert.equal(r.bl, 60, "backlight from the app's brightness setting");
    assert.equal(r.off, 1, "auto display off minutes");
    assert.equal(r.dim, false);
    assert.match(String(r.ld), /^run none · bundled 0\.2\.0/);
    assert.match(String(r.vp), /^\d+x\d+@1/);
    assert.deepEqual([...r.err], ["다른 기기가 조작 중입니다"]);
    assert.equal(r.js.length, 1);
    assert.match(r.js[0]!, /TypeError: x is not a function @app\.js:3:9 \(\d+s ago\)/);
    const all = r.t.join("\n");
    for (const s of ["OpenGrand", "[음색]*", "[설정]", "# 화면", "밝기", "=8 (1..10)", "=1분 ▾", "[x]", "# 개발자", "[보내기]~"]) assert.ok(all.includes(s), `${s} in\n${all}`);
    assert.ok(!/SECRET|HIDDEN/.test(all), "hidden elements and a closed details body are not on the screen");
    assert.ok(bytesOf(JSON.stringify(r)) <= 1024);
    // the app's lines for people
    assert.match(reportLines(r as never).join("\n"), /app 0\.2\.3 · tab sound · ready\nbacklight 60% · auto off 1 min/);
  } finally {
    p.close();
  }
});

test("report: at most the asked bytes of UTF-8 JSON — the screen's lines go first (bottom up), the header and banners stay", () => {
  const many = Array.from({ length: 300 }, (_, i) => `<p>한글 줄 ${i} — 피아노 설정 값과 설명이 길게 이어지는 문장입니다</p>`).join("");
  const p = panelPage(APP_HTML.replace("</main>", `${many}</main>`));
  try {
    for (const max of [1024, 512, 2048]) {
      const r = p.w.CA79Screen.report(max);
      const n = bytesOf(JSON.stringify(r));
      assert.ok(n <= max, `${n} <= ${max}`);
      assert.ok(n > max - 200, `the budget is used (${n} of ${max})`);
      assert.ok((r.cut ?? 0) > 0, "lines were cut and counted");
      assert.equal(r.t[0], "OpenGrand", "the top of the screen is kept");
      assert.ok(r.t.some((l) => l.includes("[음색]*")), "the top lines survive");
      assert.deepEqual([...r.err], ["다른 기기가 조작 중입니다"]);
      assert.ok(r.ua && r.ld && r.vp);
    }
  } finally {
    p.close();
  }
});

test("hook order: screen ops answered in front of the loader; loader ops and the app's MIDI still reach their handlers", () => {
  const p = panelPage();
  try {
    p.deliver([...captureRequest(PS_KIND.Report, 5)]);
    assert.equal(p.sent.length, 1);
    const c = decodeCapture(parsePuiReply(Uint8Array.from(p.sent[0]!))!.data)!;
    assert.equal(c.rc, 0);
    assert.equal(c.tag, 5);
    assert.equal(c.fmt, 1);
    assert.ok(c.length > 100 && c.length <= 1024);
    p.deliver([...puiRequest(PanelUiOp.Status)]);
    assert.equal(p.sent.length, 2, "the loader answered Status");
    assert.equal(parsePuiReply(Uint8Array.from(p.sent[1]!))!.op, PanelUiOp.Status);
    p.deliver([0xf0, 0x40, 0x7f, 0x10, 0x04, 0x02, 0x10, 0x00, 0x00, 0xf7]);
    assert.equal(p.app.length, 1, "other MIDI reaches the app");
    // a newer copy (a remote package over an older bundle): swaps the handler, no second wrapper
    p.w.eval(screenSource({ minify: false }).replace(/var VERSION = 1;/, "var VERSION = 2;"));
    assert.equal(p.w.CA79Screen.version, 3);
    p.w.CA79Screen.install(p.w.KWM);
    p.deliver([...captureRequest(PS_KIND.Report, 6)]);
    assert.equal(p.sent.length, 3, "one reply, not two");
  } finally {
    p.close();
  }
});

test("field loader (0.2.0 swallows ops 0x35-0x3F): no reply until the remote package's copy installs after it", () => {
  const field = loaderSource(CFG).replace(/var S = root\.CA79Screen[\s\S]*?return r \|\| \[\];/, "return [];");
  assert.notEqual(field, loaderSource(CFG), "made the field variant");
  const without = panelPage(APP_HTML, { loader: field, install: false });
  try {
    without.deliver([...captureRequest(PS_KIND.Report, 7)]);
    assert.equal(without.sent.length, 0, "swallowed by the loader");
    assert.equal(without.app.length, 0);
    without.w.CA79Screen.install(without.w.KWM); // what the app / probe of a remote package does at start
    without.deliver([...captureRequest(PS_KIND.Report, 8)]);
    assert.equal(without.sent.length, 1);
  } finally {
    without.close();
  }
  // the new loader asks the reporter itself: answered whatever the install order
  const late = panelPage(APP_HTML, { install: false });
  try {
    late.deliver([...captureRequest(PS_KIND.Report, 9)]);
    assert.equal(late.sent.length, 1);
  } finally {
    late.close();
  }
});

/** the relay model (cave rules) between a phone port and a panel page */
function rig(p: ReturnType<typeof panelPage>, fast = true) {
  const relay = new PanelRelayModel({ ui: null as never, sent: p.sent, deliver: p.deliver });
  if (fast) relay.panelBytesPerMs = 31.25;
  return relay;
}
/** a phone port that loses some panel replies (decided once per frame, whatever listens) */
function lossy(port: PuiPort, drop: (m: Uint8Array, n: number) => boolean): PuiPort & { dropped: number } {
  const seen = new Map<Uint8Array, boolean>();
  let n = 0;
  const out = {
    dropped: 0,
    sendRaw: (m: Uint8Array) => port.sendRaw(m),
    onMidi: (fn: (m: Uint8Array) => void) => port.onMidi((m) => {
      if (!seen.has(m)) {
        const d = drop(m, ++n);
        seen.set(m, d);
        if (d) out.dropped++;
      }
      if (!seen.get(m)) fn(m);
    }),
  };
  return out;
}
const fakeJpeg = (n: number) => Uint8Array.from([0xff, 0xd8, ...randomBytes(n - 4), 0xff, 0xd9]);

test("relay: report and snapshot arrive whole through the piano's relay (window 2, frames ≤ 244 B, busy polling)", async () => {
  const p = panelPage();
  const pic = fakeJpeg(24 * 1024);
  let renders = 0;
  p.w.CA79Screen.render = (o, cb) => {
    renders++;
    assert.equal(JSON.stringify(o), JSON.stringify({ w: 240, kb: 24 })); // o is from the page realm
    setTimeout(() => cb(0, { bytes: pic, fmt: 2, w: 240, h: 400, ms: 123 }), 600); // still drawing for a few polls
  };
  const relay = rig(p);
  try {
    const cli = new PanelScreenClient(relay.phone());
    const rep = await cli.report();
    assert.equal(rep.ok, true, rep.error);
    assert.equal(rep.report?.pg, "ng");
    assert.ok(rep.report?.t?.includes("# 화면"));
    const shot = await cli.snapshot();
    assert.equal(shot.ok, true, shot.error);
    assert.equal(renders, 1, "polls while drawing ask the same capture, drawn once");
    assert.deepEqual(shot.bytes, pic);
    assert.equal(shot.info?.width, 240);
    assert.equal(shot.info?.ms, 123);
    assert.equal(shot.mime, "image/jpeg");
    assert.ok(shot.dataUrl?.startsWith("data:image/jpeg;base64,/9"));
    assert.equal(shot.frames, Math.ceil(pic.length / 200));
    assert.equal(shot.retries, 0);
    const s = relay.stats;
    assert.equal(s.dropLen + s.dropDir + s.dropNoSrc + s.dropFull, 0);
    assert.ok(relay.port2.every((f) => f.length <= 244), `panel replies ≤ 244 B (max ${Math.max(...relay.port2.map((f) => f.length))})`);
    assert.ok(relay.port3.every((f) => f.length <= 16), "requests are small");
    assert.ok(s.peak3 < 64, "window 2 keeps two small requests on the panel ring");
  } finally {
    relay.stop();
    p.close();
  }
});

test("relay: lost replies are fetched again (go-back-N); a link that loses long frames gets smaller replies", async () => {
  const p = panelPage();
  const pic = fakeJpeg(9000);
  p.w.CA79Screen.render = (_o, cb) => cb(0, { bytes: pic, fmt: 2, w: 240, h: 400, ms: 5 });
  const relay = rig(p);
  try {
    // every 7th panel reply lost
    const port = lossy(relay.phone(), (_m, n) => n % 7 === 0);
    const r = await new PanelScreenClient(port).snapshot({ ackTimeoutMs: 150 });
    assert.equal(r.ok, true, r.error);
    assert.deepEqual(r.bytes, pic);
    assert.ok(port.dropped > 3 && r.retries > 0, `dropped ${port.dropped}, retries ${r.retries}`);
    // the link drops every frame over 200 B: two timeouts → 100 B replies (130 B frames) → done
    const short = lossy(relay.phone(), (m) => m.length > 200);
    const r2 = await new PanelScreenClient(short).snapshot({ ackTimeoutMs: 150 });
    assert.equal(r2.ok, true, r2.error);
    assert.deepEqual(r2.bytes, pic);
    assert.equal(r2.chunk, 100);
  } finally {
    relay.stop();
    p.close();
  }
});

test("relay: no reporter on the panel page (Kawai UI, old bundle) — 'no reply'; a page without a canvas — rc 13", async () => {
  const field = loaderSource(CFG).replace(/var S = root\.CA79Screen[\s\S]*?return r \|\| \[\];/, "return [];");
  const p = panelPage(APP_HTML, { loader: field, install: false });
  const relay = rig(p);
  try {
    const r = await new PanelScreenClient(relay.phone()).report();
    assert.equal(r.ok, false);
    assert.match(r.error!, /^no reply/);
  } finally {
    relay.stop();
    p.close();
  }
  const q = panelPage(); // jsdom: no layout, no canvas → the real renderer says "unsupported"
  const relay2 = rig(q);
  try {
    const r = await new PanelScreenClient(relay2.phone()).snapshot();
    assert.equal(r.ok, false);
    assert.equal(r.info?.rc, 13);
    // a stale Read (the page reloaded since the capture: a tag it does not hold) answers rc 8. The tag must differ
    // from the snapshot's: the client starts its tags at random (1..100), and a fixed 77 met it about once in 100
    // runs — the panel then threw on a capture without bytes and sent nothing (q.sent.at(-1) undefined).
    const stale = (r.info!.tag % 127) + 1;
    const before = q.sent.length;
    q.deliver([...readRequest(stale, 0)]);
    assert.equal(q.sent.length, before + 1, "the Read is answered at once");
    assert.equal(decodeCapture(parsePuiReply(Uint8Array.from(q.sent.at(-1)!))!.data)?.rc, 8);
  } finally {
    relay2.stop();
    q.close();
  }
});

test("app card 「패널 화면 보기」 and window.__panelReport / __panelSnapshot", async () => {
  const p = panelPage();
  const pic = fakeJpeg(3000);
  p.w.CA79Screen.render = (_o, cb) => cb(0, { bytes: pic, fmt: 2, w: 240, h: 400, ms: 40 });
  p.w.CA79Screen.provide(() => ({ pg: "app", tab: "settings" }));
  const relay = rig(p);
  try {
    const box = panelScreenBox(() => relay.phone());
    appDom.window.document.body.append(box);
    (box.querySelector("#ps-shot") as HTMLButtonElement).click();
    for (let i = 0; i < 100 && (box.querySelector("#ps-image") as HTMLImageElement).hidden; i++) await tick(20);
    const img = box.querySelector("#ps-image") as HTMLImageElement;
    assert.equal(img.hidden, false);
    assert.ok(img.src.startsWith("data:image/jpeg;base64,"));
    assert.equal((box.querySelector("#ps-save") as HTMLAnchorElement).hidden, false);
    assert.match(box.querySelector("#ps-msg")!.textContent!, /240×400, 3000 bytes/);
    (box.querySelector("#ps-read") as HTMLButtonElement).click();
    for (let i = 0; i < 100 && (box.querySelector("#ps-report") as HTMLElement).hidden; i++) await tick(20);
    assert.match(box.querySelector("#ps-report")!.textContent!, /^app · tab settings/);
    box.remove();

    const g: PanelScreenGlobals = {};
    installPanelScreenGlobals(() => relay.phone(), g);
    const url = await g.__panelSnapshot!();
    assert.equal(url, `data:image/jpeg;base64,${Buffer.from(pic).toString("base64")}`);
    assert.equal((g.__panelLast as { bytes: number }).bytes, 3000);
    const rep = await g.__panelReport!();
    assert.equal((rep as { pg: string }).pg, "app");
    installPanelScreenGlobals(() => null, g);
    assert.deepEqual(await g.__panelReport!(), { error: "not connected" });
    assert.equal(await g.__panelSnapshot!(), null);
  } finally {
    relay.stop();
    p.close();
  }
});

test("bundle: chooser, ng/index.html, the app js and the probe page carry the reporter; the chooser answers as 'chooser'", async () => {
  const app = await buildApp("0.0.0-test", CFG);
  const ng = app.find((f) => f.path === "ng/index.html")!.data.toString();
  const hook = ng.indexOf("CA79UI.hookKwm(window.KWM)");
  assert.ok(hook > 0 && ng.indexOf("CA79Screen.install(window.KWM)", hook) > hook, "installed after the loader's hook");
  const js = app.find((f) => f.path.endsWith(".js"))!.data.toString();
  assert.ok(js.startsWith(SRC), "the app js starts with the reporter (remote packages bring it)");
  assert.ok(probeFiles("0.2.3").js.startsWith(SRC));
  const chooser = chooserHtml({ version: "0.2.3", built: "t", kawai: "1.3.10" }, CFG).toString();
  const sent: number[][] = [];
  const d = new JSDOM(chooser, {
    url: "file:///data/data/x/files/appUI/index.html?stay=1&reason=timeout",
    runScripts: "dangerously",
    beforeParse(w) {
      (w as unknown as Record<string, unknown>).kwmcore = { postMessage: (u: string) => { if (u.startsWith("kwmcore://sendMidiMessage/")) sent.push(JSON.parse(u.slice(u.indexOf("?") + 1)).msg); } };
    },
  });
  try {
    const K = (d.window as unknown as { KWM: { onMidiMessage(m: number[], id: string): void } }).KWM;
    K.onMidiMessage([...captureRequest(PS_KIND.Report, 11)], "USB");
    const c = decodeCapture(parsePuiReply(Uint8Array.from(sent[0]!))!.data)!;
    let raw: number[] = [];
    for (let off = 0; off < c.length; off += 200) {
      K.onMidiMessage([...readRequest(11, off)], "USB");
      const r = parsePuiReply(Uint8Array.from(sent.at(-1)!))!;
      const { unpack7 } = await import("../src/protocol/ngext.ts");
      raw = raw.concat([...unpack7(r.data).slice(6)]);
    }
    const rep = JSON.parse(Buffer.from(raw).toString("utf8"));
    assert.equal(rep.pg, "chooser");
    assert.equal(rep.ver, "0.2.3");
    assert.match(rep.why, /^timeout/, "why the chooser is up (the failed UI's reason)");
  } finally {
    d.window.close();
  }
});

test("transfer time at the field relay rate: report ≈ 1 s, a 24 KiB picture ≈ 17 s", () => {
  const rep = estimateScreenTransfer(1024);
  const pic = estimateScreenTransfer(24 * 1024);
  assert.equal(pic.frames, 123);
  assert.ok(rep.seconds < 1.2, String(rep.seconds));
  assert.ok(pic.seconds > 14 && pic.seconds < 20, String(pic.seconds));
});

test("act (op 0x37): click / set an element by id on the panel page; bad id → rc 1, missing → rc 4; through the loader", async () => {
  const html = APP_HTML.replace("<button disabled>보내기</button>", '<button id="go">누름</button><input id="path" value="a"><select id="pick"><option value="x">x</option><option value="y">y</option></select>');
  const p = panelPage(html);
  try {
    let clicks = 0, changed = "";
    p.w.document.getElementById("go")!.addEventListener("click", () => void clicks++);
    p.w.document.getElementById("pick")!.addEventListener("change", (e) => void (changed = (e.target as HTMLSelectElement).value));
    const rc = () => p.sent[p.sent.length - 1]!.slice(7, 10);
    p.deliver([...actRequest(5, 1, "go")]);
    assert.deepEqual(rc(), [0, 5, 1]);
    await tick(5);
    assert.equal(clicks, 1);
    p.deliver([...actRequest(6, 2, "path", "/proc/mounts")]);
    await tick(5);
    assert.equal((p.w.document.getElementById("path") as HTMLInputElement).value, "/proc/mounts");
    p.deliver([...actRequest(7, 2, "pick", "y")]);
    await tick(5);
    assert.equal(changed, "y");
    p.deliver([...actRequest(8, 1, "nope")]);
    assert.deepEqual(rc(), [4, 8, 1]);
    p.deliver([...actRequest(9, 1, "a b")]);
    assert.deepEqual(rc(), [1, 9, 1]);
    // v3: by CSS selector — list rows have no ids
    p.deliver([...actRequest(11, 3, 'button[id="go"]')]);
    assert.deepEqual(rc(), [0, 11, 3]);
    await tick(5);
    assert.equal(clicks, 2);
    p.deliver([...actRequest(12, 4, "select#pick", "x")]);
    await tick(5);
    assert.equal(changed, "x");
    p.deliver([...actRequest(13, 3, ".nothing-here")]);
    assert.deepEqual(rc(), [4, 13, 3]);
    p.deliver([...actRequest(14, 3, "[[bad")]);
    assert.deepEqual(rc(), [1, 14, 3]);
    assert.equal(p.app.length, 0, "act frames never reach the app's MIDI handler");
    // a WebView in the background (another activity on top) runs no timers — field 2026-11-15: rc 0 but no click.
    // The click must not wait for a timer.
    const realSetTimeout = p.w.setTimeout;
    (p.w as unknown as { setTimeout: unknown }).setTimeout = () => 0;
    try {
      p.deliver([...actRequest(10, 1, "go")]);
      assert.deepEqual(rc(), [0, 10, 1]);
      await Promise.resolve();
      assert.equal(clicks, 3, "clicked without a timer");
    } finally {
      (p.w as unknown as { setTimeout: unknown }).setTimeout = realSetTimeout;
    }
  } finally {
    p.close();
  }
});
