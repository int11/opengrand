// Recorder tab (was the Files tab) shows its lists when it opens, without 「읽기」 / ↻:
//   - USB: one 24 11 per reason (tab open with an old / other-session cache, stick inserted, ↻, after a file op),
//     shared while running, none when the piano says the stick is out; Result 0x21 = "No USB memory", not an error;
//     a failed request keeps the last list.
//   - Internal songs: OpenGrand op 1E 01 (read-only) fills the rows at once and 5E 01 notices keep them current;
//     on a piano without the op nothing is selected on the piano until the user taps 「읽기」.
import { pianoPair, tapTwice, tick } from "./_dom.ts";
import { beforeEach, test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const { bindStore } = await import("../src/ui/controls.ts");
const { createContext } = await import("../src/ui/context.ts");
const { recorderScreen } = await import("../src/ui/recorder.ts");
const { usbListState, USB_LIST_FRESH_MS, NO_MEDIA, USB_EMPTY_RETRY_MS, USB_SETTLE_MS } = await import("../src/ui/usblist.ts");
const FA = await import("../src/protocol/fileaccess.ts");
const ng = await import("../src/protocol/ngext.ts");
const i18n = await import("../src/i18n.ts");

// the empty-list retry waits 1.5 s / 3 s on the piano; 40 / 80 ms here
USB_EMPTY_RETRY_MS.splice(0, USB_EMPTY_RETRY_MS.length, 40, 80);

beforeEach(() => {
  localStorage.clear();
  i18n.setLang("en");
});

const P = (fn: number, body: number[]) => Uint8Array.from([0xf0, 0x40, 0x7f, fn, 0x04, 0x1e, ...body, 0xf7]);

/** Stock-shaped list (02_fn24_static): 24 7F, FileName packets stop-and-wait, 24 1F; or a Result instead. */
function usbPiano(piano: { send(m: Uint8Array): void; onMessage(h: (m: Uint8Array) => void): void }) {
  // emptyFirst: answer that many lists with 24 1F alone (a stick not mounted yet, field 2026-11-03)
  const st = { files: ["A.MID", "B.MP3"], answer: 0 as number, lists: 0, emptyFirst: 0 };
  let queue: Uint8Array[] = [];
  const later = (m: Uint8Array) => setTimeout(() => piano.send(m), 3);
  piano.onMessage((m) => {
    if (m[1] !== 0x40 || m[3] !== 0x24) return;
    const v1 = m[6];
    if (v1 === 0x11) {
      st.lists++;
      later(P(0x24, [0x7f]));
      if (st.answer) {
        const c = st.answer;
        return void later(P(0x7e, [0x03, (c >> 14) & 0x7f, (c >> 7) & 0x7f, c & 0x7f]));
      }
      queue = (st.lists <= st.emptyFirst ? [] : st.files).map((n) => {
        const body = [0x03, 0, ...FA.pack87(FA.nameBytes(n)), 0];
        const f = P(0x24, body);
        f[f.length - 2] = FA.xorChk(f, 1, f.length - 2);
        return f;
      });
      queue.push(P(0x24, [0x1f]));
      later(queue.shift()!);
    } else if (v1 === 0x7f && queue.length) later(queue.shift()!);
  });
  return st;
}

async function setup(attach?: 0 | 1) {
  const p = await pianoPair();
  const store = new ParamStore();
  bindStore(store);
  store.attach(p.session);
  const usb = usbPiano(p.piano);
  let session = p.session;
  const ctx = createContext(store, { session: () => session });
  if (attach !== undefined) p.notify([0x51, 0x2e, 0x7f, attach]);
  await tick(5);
  const open = () => {
    document.body.replaceChildren(recorderScreen(ctx));
    return document.body.firstElementChild as HTMLElement;
  };
  return { ...p, store, ctx, usb, open, swap: (s: typeof session) => (session = s) };
}

const names = (el: HTMLElement) => [...el.querySelectorAll(".file-row")].map((n) => n.getAttribute("data-file"));

test("the tab opens with the USB list (no ↻); opening it again soon does not ask again", async () => {
  const { open, usb } = await setup(1);
  let el = open();
  assert.match(el.querySelector("#usb-result")!.textContent!, /Read the USB memory…/, "reading shown at once");
  await tick(60);
  assert.deepEqual(names(el), ["A.MID", "B.MP3"]);
  assert.equal(el.querySelector("#usb-result")!.textContent, "2 files");
  assert.equal(usb.lists, 1);
  el = open(); // another tab and back
  assert.deepEqual(names(el), ["A.MID", "B.MP3"], "drawn from the cache at once");
  await tick(30);
  assert.equal(usb.lists, 1, "a fresh list is not asked again");
});

test("an old list is asked again when the tab opens; ↻ always asks", async () => {
  const { open, usb, ctx } = await setup(1);
  open();
  await tick(60);
  usbListState(ctx).at -= USB_LIST_FRESH_MS + 1;
  usb.files = ["C.MID"];
  let el = open();
  assert.deepEqual(names(el), ["A.MID", "B.MP3"], "the old list while the new one comes");
  await tick(60);
  assert.deepEqual(names(el), ["C.MID"]);
  assert.equal(usb.lists, 2);
  (el.querySelector('[data-act="refresh"]') as HTMLElement).click();
  await tick(60);
  assert.equal(usb.lists, 3);
  el.remove();
});

test("triggers while a list is running share it: one 24 11", async () => {
  const { open, usb } = await setup(1);
  const el = open(); // auto
  (el.querySelector('[data-act="refresh"]') as HTMLElement).click();
  (el.querySelector('[data-act="refresh"]') as HTMLElement).click();
  await tick(60);
  assert.equal(usb.lists, 1);
  assert.deepEqual(names(el), ["A.MID", "B.MP3"]);
});

test("stick inserted / removed (51 2E change): list once on insert, nothing on remove or on a repeated value", async () => {
  const { open, usb, notify } = await setup(0);
  const el = open();
  await tick(30);
  assert.equal(usb.lists, 0, "the piano said no stick: nothing asked");
  assert.equal(el.querySelector("#usb-status")!.textContent, "No USB memory");
  assert.equal(el.querySelector("#usb-files")!.textContent, "No USB memory");
  notify([0x51, 0x2e, 0x7f, 1]);
  await tick(60);
  assert.equal(usb.lists, 1);
  assert.deepEqual(names(el), ["A.MID", "B.MP3"]);
  notify([0x51, 0x2e, 0x7f, 1]); // the connect sync repeats the value
  await tick(30);
  assert.equal(usb.lists, 1);
  notify([0x51, 0x2e, 0x7f, 0]);
  await tick(10);
  assert.equal(usb.lists, 1);
  assert.deepEqual(names(el), []);
  assert.equal(el.querySelector("#usb-status")!.textContent, "No USB memory");
  el.remove();
});

test("no stick answered as Result 0x21 (stock FUN_4014F018): 'No USB memory', not an error", async () => {
  const { open, usb } = await setup();
  usb.answer = NO_MEDIA;
  const el = open();
  await tick(60);
  assert.equal(usb.lists, 1);
  assert.equal(el.querySelector("#usb-result")!.textContent, "No USB memory");
  assert.equal(el.querySelector("#usb-status")!.textContent, "No USB memory");
  assert.equal(el.querySelector("#usb-files")!.textContent, "No USB memory");
});

test("a failed list keeps the last one (Busy is asked again 4 times, 400 ms apart)", async () => {
  const { open, usb } = await setup(1);
  const el = open();
  await tick(60);
  usb.answer = 0x4000; // Busy (the stock's list running / ota_main's file lease: another controller's job)
  (el.querySelector('[data-act="refresh"]') as HTMLElement).click();
  await tick(1720);
  assert.equal(usb.lists, 6, "the first + Busy + four retries");
  assert.deepEqual(names(el), ["A.MID", "B.MP3"]);
  assert.equal(el.querySelector("#usb-result")!.textContent, "Busy");
});

test("reconnect while the tab is shown: the new session's screen asks again even if the list is fresh", async () => {
  const a = await setup(1);
  a.open();
  await tick(60);
  assert.equal(a.usb.lists, 1);
  const b = await pianoPair();
  const usbB = usbPiano(b.piano);
  usbB.files = ["NEW.MID"];
  a.swap(b.session);
  a.store.attach(b.session);
  const el = a.open(); // main.ts draws the tab again when the session becomes ready
  await tick(60);
  assert.equal(usbB.lists, 1);
  assert.deepEqual(names(el), ["NEW.MID"]);
});

// ---- an empty list right after a plug / connect (field 2026-11-03: 「파일이 없습니다」 with 6 files on the stick) ----

const SIX = ["A.MID", "B.MID", "C.MID", "D.MP3", "E.MP3", "F.MP3"];

test("plugged while the tab is open: an empty first answer is asked again and the 6 files show", async () => {
  const { open, usb, notify } = await setup(0);
  usb.files = SIX;
  usb.emptyFirst = 1;
  const el = open();
  notify([0x51, 0x2e, 0x7f, 1]);
  await tick(30);
  assert.equal(usb.lists, 1);
  assert.match(el.querySelector("#usb-result")!.textContent!, /Read the USB memory…/, "still reading, not 'No files.'");
  await tick(120);
  assert.equal(usb.lists, 2, "asked once more after the first wait");
  assert.deepEqual(names(el), SIX);
  assert.equal(el.querySelector("#usb-result")!.textContent, "6 files");
  el.remove();
});

test("plugged on another tab, then the tab opens: same retry; a stick still empty after two retries is shown empty", async () => {
  const { open, usb, notify } = await setup(0);
  usb.files = SIX;
  usb.emptyFirst = 2;
  notify([0x51, 0x2e, 0x7f, 1]); // no Files screen yet: usblist watches the plug itself
  await tick(5);
  let el = open();
  await tick(250);
  assert.equal(usb.lists, 3, "first + two retries");
  assert.deepEqual(names(el), SIX);
  // a stick that is really empty: three answers, then 'No files.'
  usb.files = [];
  usb.lists = 0;
  usb.emptyFirst = 0;
  notify([0x51, 0x2e, 0x7f, 0]);
  notify([0x51, 0x2e, 0x7f, 1]);
  await tick(250);
  assert.equal(usb.lists, 3);
  assert.equal(el.querySelector("#usb-files")!.textContent, "No files.");
  assert.equal(el.querySelector("#usb-result")!.textContent, "0 files");
  el = open();
  el.remove();
});

test("the first list after connecting gets the same retry; triggers during the retry share it", async () => {
  const { open, usb } = await setup(1);
  usb.files = SIX;
  usb.emptyFirst = 1;
  const el = open();
  await tick(20);
  (el.querySelector('[data-act="refresh"]') as HTMLElement).click(); // while the retry waits
  await tick(150);
  assert.equal(usb.lists, 2, "one retry, the ↻ shared it");
  assert.deepEqual(names(el), SIX);
});

test("an empty cached list is stale when the tab opens; an empty answer long after the plug is not retried", async () => {
  const { open, usb, ctx } = await setup(1);
  usb.files = [];
  let el = open();
  await tick(250);
  assert.equal(usb.lists, 3);
  const st = usbListState(ctx);
  st.plugAt -= USB_SETTLE_MS + 1;
  st.seenAt -= USB_SETTLE_MS + 1;
  (el.querySelector('[data-act="refresh"]') as HTMLElement).click();
  await tick(150);
  assert.equal(usb.lists, 4, "outside the settle window: one request, no retry");
  usb.files = SIX;
  el = open(); // fresh (seconds old) but empty: asked again
  await tick(60);
  assert.equal(usb.lists, 5);
  assert.deepEqual(names(el), SIX);
  el = open(); // fresh and not empty: not asked
  await tick(30);
  assert.equal(usb.lists, 5);
});

test("'no stick' cached while the piano says attached is stale", async () => {
  const { open, usb, ctx } = await setup(1);
  usb.answer = NO_MEDIA;
  open();
  await tick(60);
  assert.equal(usb.lists, 1);
  assert.ok(usbListState(ctx).noMedia);
  usb.answer = 0;
  const el = open();
  await tick(60);
  assert.equal(usb.lists, 2);
  assert.deepEqual(names(el), ["A.MID", "B.MP3"]);
});

// ---- internal songs ----

function listRaw(): number[] {
  const raw = new Array(70).fill(0);
  const put = (i: number, bars: number, flags: number) => {
    raw[7 * i] = 0x10; raw[7 * i + 4] = bars; raw[7 * i + 6] = flags;
  };
  put(0, 3, 1);
  put(1, 5, 3);
  return raw;
}

/** A ready session stand-in: Identify (feat2) and op 1E like recctl_cave.c; Kawai frames recorded. */
function fakeSession(feat2: number | null, deletes = false) {
  const midi: ((m: Uint8Array) => void)[] = [];
  const frames: ((f: { fn: number; body: number[] }) => void)[] = [];
  const sent: number[][] = [];
  const list = listRaw();
  let song = 0;
  const push = (m: Uint8Array) => queueMicrotask(() => midi.forEach((l) => l(m)));
  const reply = (op: number, data: number[]) => push(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...data, 0xf7]));
  return {
    state: "ready",
    deviceId: 0x7f,
    sent,
    push,
    onMidi(fn: (m: Uint8Array) => void) {
      midi.push(fn);
      return () => midi.splice(midi.indexOf(fn), 1);
    },
    onFrame(fn: (f: { fn: number; body: number[] }) => void) {
      frames.push(fn);
      return () => frames.splice(frames.indexOf(fn), 1);
    },
    onState: () => () => {},
    sendParam: async (m: { fn?: number; v1: number; v2: number; data?: number[] }) => {
      sent.push([m.fn ?? 0x10, m.v1, m.v2, ...(m.data ?? [])]);
      if (m.v1 === 0x52 && m.v2 === 0x21 && m.data) {   // the core's delete (recdel.c): the table changes, then 32 51 7E
        const [, hi, lo, part] = m.data;
        const i = (hi! << 7) | lo!;
        list[7 * i + 6] = part === 0x7f ? 0 : list[7 * i + 6]! & ~(1 << part!);
        if (!(list[7 * i + 6]! & 3)) list[7 * i + 4] = 0;
        queueMicrotask(() => frames.forEach((l) => l({ fn: 0x32, body: [0x51, 0x7e, 0x7f, 0x00, 0x00] })));
      }
      return true;
    },
    sendRaw(m: Uint8Array) {
      sent.push([...m]);
      if (m[1] !== 0x7d || feat2 === null) return;
      const op = m[6]!;
      if (op === 0x01) reply(0x01, [1, 2, 19, 0x3b, 0x77, feat2]);
      if (op === 0x1e && m[7] === 1) reply(0x1e, [1, 0, ...ng.pack7(list)]);
      const status = () => {
        const raw = new Array(48).fill(0);
        raw[0] = 1; raw[1] = 2; raw[3] = song; raw[6] = 2;
        raw[31] = deletes ? 1 : 0;   // recctl_cave.c rcc_status: the core deletes songs itself
        reply(0x1e, [0, 0, ...ng.pack7(raw)]);
      };
      if (op === 0x1e && m[7] === 0) status();
      if (op === 0x1e && m[7] === 2) {   // a command (select): rc 0, then the status notice
        if (m[8] === 4) song = (m[9]! << 7) | m[10]!;
        reply(0x1e, [2, 0, m[8]!]);
        status();
      }
    },
  };
}
const info = (el: HTMLElement, n: number) => el.querySelector(`[data-song="${n}"] .song-info`)!.textContent;

test("OpenGrand (feat2 0x10): the internal songs are there when the tab opens, read-only, notices update them", async () => {
  const s = fakeSession(0x1f);
  const ctx = createContext(new ParamStore(), { session: () => s as never });
  const el = recorderScreen(ctx);
  document.body.replaceChildren(el);
  for (let i = 0; i < 20; i++) await tick(2);
  assert.equal(info(el, 1), "3 bars");
  assert.equal(info(el, 2), "5 bars · Part 1 + Part 2");
  assert.equal(info(el, 3), "Empty");
  assert.equal(el.querySelector("#songs-status")!.textContent, "2 of 10 songs recorded");
  assert.ok(!s.sent.some((m) => m[0] === 0x10), "no SongSelect / IntSongPart / mode: nothing changes on the piano");
  assert.ok(s.sent.every((m) => m[0] !== 0xf0 || m[1] !== 0x7d || m[6] === 0x01 || (m[6] === 0x1e && m[7] <= 1)),
    "only Identify and 1E 00 / 01 - power_frame_quiet reads");
  // the piano recorded song 3 (5E 01 notice)
  const raw = listRaw();
  raw[7 * 2 + 4] = 7; raw[7 * 2 + 6] = 2;
  s.push(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, 0x5e, 0x01, 0x00, ...ng.pack7(raw), 0xf7]));
  for (let i = 0; i < 5; i++) await tick(2);
  assert.equal(info(el, 3), "7 bars · Part 2");
  // another tab and back: drawn from the cache at once
  const el2 = recorderScreen(ctx);
  document.body.replaceChildren(el2);
  assert.equal(info(el2, 3), "7 bars · Part 2");
});

test("without the op (stock / old payload): no automatic scan - the piano's song is not touched until 「읽기」", async () => {
  const s = fakeSession(null); // Identify unanswered
  const ctx = createContext(new ParamStore(), { session: () => s as never });
  const el = recorderScreen(ctx);
  document.body.replaceChildren(el);
  await tick(800); // Identify timeout (700 ms)
  assert.equal(info(el, 1), "not scanned");
  assert.equal(el.querySelector("#songs-status")!.textContent, "Not scanned yet.");
  assert.ok(!s.sent.some((m) => m[0] === 0x10), "no SongSelect");
});

// field 2026-11-15: the stock InternalSongDelete re-writes the song from the
// last take - Delete is offered only when the piano's core deletes (5E 00 byte 31 bit 0)
const del = (el: HTMLElement, n: number, act: string) => el.querySelector(`[data-song="${n}"] [data-act="${act}"]`) as HTMLButtonElement;

// choose the song (a tap on its row), then open its other buttons (⋯)
const pickSong = async (el: HTMLElement, n: number) => {
  (el.querySelector(`[data-song="${n}"] [data-act="song-pick"]`) as HTMLElement).click();
  for (let i = 0; i < 10; i++) await tick(2);
  (el.querySelector(`[data-song="${n}"] [data-act="song-more"]`) as HTMLElement).click();
};

test("songs delete: a payload whose core does not delete (byte 31 = 0) offers no Delete and sends no 52 21", async () => {
  const s = fakeSession(0x1f, false);
  const ctx = createContext(new ParamStore(), { session: () => s as never });
  const el = recorderScreen(ctx);
  document.body.replaceChildren(el);
  for (let i = 0; i < 20; i++) await tick(2);
  assert.equal(info(el, 2), "5 bars · Part 1 + Part 2");
  await pickSong(el, 2);
  assert.equal(el.querySelector('[data-song="2"]')!.getAttribute("data-on"), "1");
  assert.ok(del(el, 2, "song-save"), "the song's export buttons are there");
  for (const act of ["song-delete", "song-delete-p1", "song-delete-p2"]) assert.equal(del(el, 2, act), null, `no ${act}`);
  assert.match(el.querySelector('[data-song="2"] .row-note')!.textContent!, /OpenGrand song delete/);
  await tick(20);
  assert.ok(!s.sent.some((m) => m[1] === 0x52 && m[2] === 0x21), "no InternalSongDelete");
});

test("songs delete: the core deletes (byte 31 bit 0): part 2, then the song; the table is read again at once", async () => {
  const s = fakeSession(0x1f, true);
  const ctx = createContext(new ParamStore(), { session: () => s as never });
  const el = recorderScreen(ctx);
  document.body.replaceChildren(el);
  for (let i = 0; i < 20; i++) await tick(2);
  assert.equal(el.querySelectorAll('.song-row .row-acts:not([hidden])').length, 0, "Delete is behind a row's ⋯: no row shows its buttons until then");
  await pickSong(el, 2);
  assert.ok(s.sent.some((m) => m[6] === 0x1e && m[7] === 2 && m[8] === 4 && m[10] === 1), "choosing the row selects song 2 on the piano (op 1E 02 04)");
  assert.ok(del(el, 2, "song-delete-p1") && del(el, 2, "song-delete-p2") && del(el, 2, "song-delete"), "both parts recorded: per-part delete too");
  assert.equal(el.querySelector('[data-song="2"] .row-note'), null);
  del(el, 2, "song-delete-p2").click();
  for (let i = 0; i < 5; i++) await tick(2);
  assert.ok(!s.sent.some((m) => m[1] === 0x52 && m[2] === 0x21), "first tap only asks");
  assert.match(el.querySelector("[data-confirm=ask]")!.textContent!, /Delete part 2 of song 2 on the piano\?/);
  del(el, 2, "song-delete-p2").click();
  for (let i = 0; i < 10; i++) await tick(2);
  assert.ok(s.sent.some((m) => m.join(" ") === [0x32, 0x52, 0x21, 0x04, 0x00, 0x01, 0x01].join(" ")), "04 00 01 01");
  assert.equal(info(el, 2), "5 bars");
  tapTwice(del(el, 2, "song-delete"));
  for (let i = 0; i < 10; i++) await tick(2);
  assert.ok(s.sent.some((m) => m.join(" ") === [0x32, 0x52, 0x21, 0x04, 0x00, 0x01, 0x7f].join(" ")), "04 00 01 7F");
  assert.equal(info(el, 2), "Empty");
});
