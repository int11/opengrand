// Shared jsdom setup for UI tests (not a test file itself: the runner only
// picks up *.test.ts). Import it before any src/ui module.
import { after } from "node:test";
import { JSDOM } from "jsdom";

export const dom = new JSDOM("<!doctype html><html><body><div id=app></div></body></html>", { url: "http://localhost/" });
const g = globalThis as unknown as Record<string, unknown>;
g.window = dom.window;
g.document = dom.window.document;
g.HTMLElement = dom.window.HTMLElement;
g.Node = dom.window.Node;
// CustomEvent/Event stay Node natives: app-level EventTargets (ctx.midi) are Node ones;
// DOM events are created with dom.window.Event or element.click().
g.localStorage = dom.window.localStorage;
g.requestAnimationFrame = (f: () => void) => setTimeout(f, 0) as unknown as number;
g.cancelAnimationFrame = (h: number) => clearTimeout(h);
// no native dialogs: destructive actions use the in-page two-step confirm (src/ui/confirm2.ts, field 2026-11-15 — a
// native Electron confirm stayed on the user's screen after a test tool accepted the page's one over CDP)
g.confirm = () => { throw new Error("native confirm() used — use confirmTap (src/ui/confirm2.ts)"); };
g.prompt = (_m: string, d?: string) => d ?? "";
const realSetInterval = globalThis.setInterval;
g.setInterval = ((f: () => void, ms: number) => realSetInterval(f, ms).unref()) as unknown;
after(() => dom.window.close());
// the in-page confirm ignores a second tap within gapMs (one gesture); tests tap twice at once
const { CONFIRM } = await import("../src/ui/confirm2.ts");
CONFIRM.gapMs = 0;

/** A destructive button: tap, then tap again to confirm (src/ui/confirm2.ts). */
export function tapTwice(el: Element | null): void {
  (el as HTMLElement).click();
  (el as HTMLElement).click();
}

export const tick = (ms = 20) => new Promise((r) => setTimeout(r, ms));

export function byText<T extends Element = HTMLElement>(root: ParentNode, sel: string, text: string | RegExp): T {
  const el = [...root.querySelectorAll(sel)].find((e) => (typeof text === "string" ? e.textContent?.trim() === text : text.test(e.textContent ?? "")));
  if (!el) throw new Error(`no ${sel} with text ${text}`);
  return el as T;
}

/** Row (label.row / div.row) whose first span reads `label`; returns its input/select. */
export function field<T extends Element = HTMLInputElement>(root: ParentNode, label: string): T {
  const row = [...root.querySelectorAll(".row")].find((r) => r.querySelector("span")?.textContent === label);
  if (!row) throw new Error(`no row ${label}`);
  return row.querySelector("input, select, output") as T;
}

export function change(el: HTMLInputElement | HTMLSelectElement, value: string | boolean, type = "change"): void {
  if (typeof value === "boolean") (el as HTMLInputElement).checked = value;
  else el.value = value;
  el.dispatchEvent(new dom.window.Event(type, { bubbles: true }));
}

/** Loopback "piano" that ACKs every fn 0x10 and records what the app sent; `endGets` also ends every get `02 v1 v2 part` with `03 v1 v2 7F`. */
export async function pianoPair(opt: { endGets?: boolean } = {}) {
  const { PianoSession } = await import("../src/protocol/session.ts");
  const { LoopbackLink } = await import("../src/transport/link.ts");
  const { toHex, encodeFrame, decodeFrame } = await import("../src/protocol/sysex.ts");
  const [app, piano] = LoopbackLink.pair();
  const sent: string[] = [];
  piano.onMessage((m) => {
    sent.push(toHex(m));
    const f = decodeFrame(m);
    if (f && f.fn === 0x10) piano.send(encodeFrame({ dev: 0, fn: 0x07, group: 4, machine: 0x1e, body: f.body.slice(0, 3) }));
    if (f && f.fn === 0x02 && opt.endGets) piano.send(encodeFrame({ dev: 0, fn: 0x03, group: 4, machine: 0x1e, body: [f.body[0], f.body[1], 0x7f] }));
  });
  const session = new PianoSession(app, { ackTimeoutMs: 50 });
  const notify = (body: number[]) => piano.send(encodeFrame({ dev: 0, fn: 0x10, group: 4, machine: 0x1e, body }));
  return { session, sent, piano, notify };
}
