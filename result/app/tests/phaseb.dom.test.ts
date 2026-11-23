// Phase B UI against a scripted fake piano: user touch curve editor (both
// receive forms of §9.1), per-note editors (§9.2), USB file manager (§9.5),
// Concert Magic result display (§9.4), USB song picker (§9.3).
import { byText, change, field, pianoPair, tapTwice, tick } from "./_dom.ts";
import { beforeEach, test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const { bindStore } = await import("../src/ui/controls.ts");
const { createContext } = await import("../src/ui/context.ts");
const S = await import("../src/ui/screens.ts");
const { touchCurveEditor, perNoteEditor } = await import("../src/ui/vtedit.ts");
const { recorderScreen } = await import("../src/ui/recorder.ts");
const { PER_NOTE, interpolate } = await import("../src/features/vtedit.ts");
const FA = await import("../src/protocol/fileaccess.ts");
const { encodeFrame, decodeFrame } = await import("../src/protocol/sysex.ts");
const i18n = await import("../src/i18n.ts");

beforeEach(() => {
  localStorage.clear();
  i18n.setLang("en");
});

const P = (fn: number, body: number[]) => encodeFrame({ dev: 0, fn, group: 4, machine: 0x1e, body });

async function setup() {
  const p = await pianoPair();
  const store = new ParamStore();
  bindStore(store);
  store.attach(p.session);
  const ctx = createContext(store, { session: () => p.session });
  return { ...p, store, ctx };
}

/** Curve with one point at 64 -> 100, as the piano would hold it. */
function curve() {
  const exist = Array(128).fill(false);
  exist[64] = true;
  const vel = Array.from({ length: 128 }, (_, i) => i);
  vel[64] = 100;
  return { exist, vel: interpolate(exist, vel) };
}

for (const form of ["bulk 5A 0D", "chunked 5A 04/05/06"] as const) {
  test(`touch curve editor reads the piano's curve (${form}) and sends edits`, async () => {
    const { store, sent, piano } = await setup();
    const c = curve();
    // the fake piano answers 02 5A 7F 7F with name + curve + 03 5A 7F 7F
    piano.onMessage((m) => {
      const f = decodeFrame(m)!;
      sent.push(Array.from(m, (b) => b.toString(16).padStart(2, "0").toUpperCase()).join(" "));
      if (f.fn === 0x10) piano.send(P(0x07, f.body.slice(0, 3)));
      if (f.fn === 0x02 && f.body[0] === 0x5a) {
        piano.send(P(0x10, [0x5a, 0x0c, 0x7f, 0x7f, ..."Soft".split("").flatMap((ch) => [ch.charCodeAt(0), 0, 0])]));
        if (form === "bulk 5A 0D") {
          piano.send(P(0x10, [0x5a, 0x0d, 0x7f, 0x7f, ...c.exist.flatMap((e, i) => [e ? 1 : 0, c.vel[i]])]));
        } else {
          piano.send(P(0x10, [0x5a, 0x04, 0x7f, 0x7f, 1, 0, 0]));
          for (let off = 0; off < 128; off += 32)
            piano.send(P(0x10, [0x5a, 0x05, 0x7f, 0x7f, off, ...Array.from({ length: 32 }, (_, i) => [c.exist[off + i] ? 1 : 0, c.vel[off + i]]).flat()]));
          piano.send(P(0x10, [0x5a, 0x06, 0x7f]));
        }
        piano.send(P(0x03, [0x5a, 0x7f, 0x7f]));
      }
    });
    const { el, dispose } = touchCurveEditor(store);
    document.body.append(el);
    await tick(40);
    assert.ok(sent.includes("F0 40 7F 10 08 02 50 00 00 40 F7"), "TouchCurve = User");
    assert.ok(sent.includes("F0 40 7F 02 08 02 5A 7F 7F F7"), "curve requested");
    assert.equal((el.querySelector("input[type=text]") as HTMLInputElement).value, "Soft");
    assert.deepEqual([...el.querySelectorAll(".chips .chip")].map((b) => b.textContent), ["64→100"]);
    assert.equal(el.querySelectorAll("circle").length, 1);
    assert.match(el.querySelector("#curve-status")!.textContent!, /User/);

    // move the point 64 -> 40 with output 90
    (el.querySelector(".chips .chip") as HTMLElement).click();
    change(field(el, "Input velocity"), "40");
    change(field(el, "Output velocity"), "90");
    byText(el, "button", "Set point").click();
    await tick(20);
    assert.ok(sent.includes("F0 40 7F 10 08 02 5A 07 7F 40 28 01 5A F7"), "5A 07 [old 64, new 40, 1, 90]");
    byText(el, "button", "Remove point").click();
    byText(el, "button", "Clear all").click();
    byText(el, "button", "Learn from playing").click();
    byText(el, "button", "Save").click();
    await tick(30);
    for (const s of ["F0 40 7F 10 08 02 5A 07 7F 28 28 00 28 F7", "F0 40 7F 10 08 02 5A 08 7F F7", "F0 40 7F 32 08 02 5A 0B 7F 7F 01 F7", "F0 40 7F 32 08 02 5A 0A 7F F7"]) assert.ok(sent.includes(s), s);
    change(el.querySelector("input[type=text]") as HTMLInputElement, "Mine");
    byText(el, "button", "Rename").click();
    await tick(30);
    assert.ok(sent.includes("F0 40 7F 10 08 02 5A 02 7F 7F 00 4D 00 00 69 00 00 6E 00 00 65 00 00 F7"), "name data");
    byText(el, "button", "Discard").click();
    await tick(20);
    assert.ok(sent.includes("F0 40 7F 31 08 02 5A 09 7F F7"), "edit exit");
    dispose();
    el.remove();
  });
}

test("per-note editors: request, octave edits, temperament, save/discard", async () => {
  const { store, sent, notify } = await setup();
  const voicing = PER_NOTE.find((k) => k.id === "voicing")!;
  const { el, dispose } = perNoteEditor(store, voicing);
  document.body.append(el);
  await tick(20);
  assert.ok(sent.includes("F0 40 7F 10 08 02 50 01 00 40 F7"), "Voicing = User");
  assert.ok(sent.includes("F0 40 7F 02 08 02 50 17 7F F7"), "table requested");
  // piano sends the table: note 60 = +3
  const table = Array(128).fill(64);
  table[60] = 67;
  notify([0x50, 0x17, 0x7f, 0x7f, 0x7f, ...table]);
  await tick(5);
  const c4 = [...el.querySelectorAll(".pn-rows .row")].find((r) => r.querySelector("span")!.textContent === "C4")!;
  assert.equal(c4.querySelector("output")!.textContent, "+3");
  const d4 = [...el.querySelectorAll(".pn-rows .row")].find((r) => r.querySelector("span")!.textContent === "D4")!.querySelector("input")!;
  change(d4, "-2");
  await tick(20);
  const oct5 = Array(12).fill(64);
  oct5[0] = 67;
  oct5[2] = 62;
  assert.ok(sent.includes(`F0 40 7F 10 08 02 50 17 7F 7F 05 ${oct5.map((v) => v.toString(16).toUpperCase().padStart(2, "0")).join(" ")} F7`), "octave 5 sent");
  // octave selector covers A0..C8
  const oct = field<HTMLSelectElement>(el, "Octave");
  assert.equal(oct.options.length, 9);
  change(oct, "9");
  assert.deepEqual([...el.querySelectorAll(".pn-rows .row span")].map((s) => s.textContent), ["C8"]);
  byText(el, "button", "Save").click();
  byText(el, "button", "Discard").click();
  await tick(30);
  assert.ok(sent.includes("F0 40 7F 32 08 02 50 17 7F 00 F7"), "save");
  assert.ok(sent.includes("F0 40 7F 31 08 02 50 17 7F 00 F7"), "exit");
  dispose();
  el.remove();

  const temp = perNoteEditor(store, PER_NOTE.find((k) => k.id === "temperament")!);
  document.body.append(temp.el);
  await tick(20);
  assert.equal(temp.el.querySelectorAll(".pn-rows .row").length, 12);
  change([...temp.el.querySelectorAll(".pn-rows input")][4] as HTMLInputElement, "-14");
  byText(temp.el, "button", "Save").click();
  await tick(30);
  assert.ok(sent.includes("F0 40 7F 10 08 02 50 0D 00 40 00 F7"), "Temperament = User (key C)");
  assert.ok(sent.some((s) => s.startsWith("F0 40 7F 10 08 02 50 0E 7F 7F 7F 40 40 40 40 32")), "12 values, E = -14");
  assert.ok(sent.includes("F0 40 7F 32 08 02 50 0E 7F F7"), "temperament save without data");
  temp.dispose();
  temp.el.remove();
});

test("technician screen opens the user editors", async () => {
  const { store, ctx } = await setup();
  const el = S.vtScreen(store, ctx);
  document.body.append(el);
  for (const id of ["touch", "voicing", "keyVolume", "tuning", "temperament"]) {
    (el.querySelector(`[data-editor="${id}"]`) as HTMLElement).click();
    assert.equal(el.querySelectorAll(".editor-host .editor").length, 1, id);
  }
  assert.equal(el.querySelectorAll("section:nth-of-type(2) .row").length, 22, "22 VT rows unchanged");
  el.remove();
});

/** Fake piano side of §9.5, shaped like the stock (stock_fn24.py): requests acked with 24 7F, list with
 *  stop-and-wait including 24 1F, delete / rename by name with the stock success 32 51 7E 7F 00 00. */
function usbPiano(piano: { send(m: Uint8Array): void; onMessage(h: (m: Uint8Array) => void): void }, files: string[], sent: string[]) {
  const pkt = (v1: number, counter: number, raw: number[]) => {
    const body = [v1, counter, ...FA.pack87(raw), 0];
    const m = P(0x24, body);
    m[m.length - 2] = FA.xorChk(m, 1, m.length - 2);
    return m;
  };
  const result = (code: number) => piano.send(P(0x7e, [0x03, (code >> 14) & 0x7f, (code >> 7) & 0x7f, code & 0x7f]));
  let queue: Uint8Array[] = [];
  let op = 0;
  let names: string[] = [];
  let nameBuf: number[] = [];
  const stockOk = () => piano.send(P(0x32, [0x51, 0x7e, 0x7f, 0, 0]));
  piano.onMessage((m) => {
    sent.push(Array.from(m, (b) => b.toString(16).padStart(2, "0").toUpperCase()).join(" "));
    const f = decodeFrame(m)!;
    if (f.fn === 0x10) piano.send(P(0x07, f.body.slice(0, 3)));
    if (f.fn === 0x24 && f.body[0] === 0x11) {
      piano.send(P(0x24, [0x7f]));
      queue = files.flatMap((n) => {
        const b = FA.nameBytes(n);
        const out: Uint8Array[] = [];
        for (let i = 0, c = 0; i < b.length; i += 21, c++) out.push(pkt(0x03, c, b.slice(i, i + 21)));
        return out;
      });
      queue.push(P(0x24, [0x1f]));
      piano.send(queue.shift()!);
    } else if (f.fn === 0x24 && f.body[0] === 0x7f) {
      if (queue.length) piano.send(queue.shift()!);
      else sent.push("acked: end of list");
    } else if (f.fn === 0x24 && (f.body[0] === 0x06 || f.body[0] === 0x07 || f.body[0] === 0x0a)) {
      piano.send(P(0x24, [0x7f]));
      op = f.body[0];
      names = [];
      nameBuf = [];
    } else if (f.fn === 0x24 && f.body[0] === 0x03) {
      piano.send(P(0x24, [0x7f]));
      nameBuf.push(...(FA.readPacket(m) ?? []));
      if (nameBuf.at(-1) !== 0) return;
      names.push(String.fromCharCode(...nameBuf.slice(1, -1)));
      nameBuf = [];
      if (op === 0x06) {
        files.splice(files.indexOf(names[0]), 1);
        stockOk();
      } else if (op === 0x07 && names.length === 2) {
        files[files.indexOf(names[0])] = names[1];
        stockOk();
      } else if (op === 0x0a) {
        result(0x21); // no media
      }
    } else if (f.fn === 0x32 && f.body[0] === 0x51 && f.body[1] === 0x3c) {
      files.length = 0;
      result(0);
    } else if (f.fn === 0x32 && f.body[0] === 0x52 && f.body[1] === 0x20) {
      result(0x21); // no media
    }
  });
}

test("USB file manager lists files, deletes / renames by name, formats, reports results", async () => {
  const { store, ctx, piano, sent, notify } = await setup();
  const files = ["A.MID", "LONG_NAME_FOR_TWO_PACKETS.MID", "C.MID"];
  usbPiano(piano, files, sent);
  notify([0x51, 0x2e, 0x7f, 1]);
  await tick(5);
  const el = recorderScreen(ctx);
  document.body.append(el);
  await tick(40);
  assert.equal(el.querySelector("#usb-status")!.textContent, "USB memory attached");
  const names = () => [...el.querySelectorAll(".file-row")].map((n) => n.getAttribute("data-file"));
  assert.deepEqual(names(), ["A.MID", "LONG_NAME_FOR_TWO_PACKETS.MID", "C.MID"]);

  (el.querySelector('[data-file="C.MID"] [data-act="rename"]') as HTMLElement).click(); // prompt() returns the default -> no change
  (globalThis as unknown as { prompt: () => string }).prompt = () => "Z.MID";
  (el.querySelector('[data-file="C.MID"] [data-act="rename"]') as HTMLElement).click();
  await tick(40);
  assert.ok(sent.includes("acked: end of list"), "24 1F acknowledged (the stock unbinds otherwise)");
  assert.ok(sent.includes("F0 40 7F 24 08 02 07 F7"), "rename by name (24 07), which the stock answers");
  assert.ok(!sent.some((m) => m.startsWith("F0 40 7F 32 08 02 51 3A")), "not by id: the stock gives 51 3A no reply");
  assert.deepEqual(names(), ["A.MID", "LONG_NAME_FOR_TWO_PACKETS.MID", "Z.MID"]);

  tapTwice(el.querySelector('[data-file="A.MID"] [data-act="delete"]'));
  await tick(40);
  assert.ok(sent.includes("F0 40 7F 24 08 02 06 F7"), "delete by name (24 06)");
  assert.deepEqual(names(), ["LONG_NAME_FOR_TWO_PACKETS.MID", "Z.MID"]);

  (el.querySelector('[data-song="1"] [data-act="song-pick"]') as HTMLElement).click();
  await tick(30);
  (el.querySelector('[data-song="1"] [data-act="song-save"]') as HTMLElement).click();
  (el.querySelector('#save-sheet [data-format="MID"]') as HTMLElement).click(); // the one format list (ui/savesheet.ts)
  await tick(30);
  assert.ok(sent.includes("F0 40 7F 24 08 02 0A 00 02 F7"), "save internal song 0 as MID by name (24 0A 00 02), which the stock executes");
  assert.ok(!sent.some((m) => m.startsWith("F0 40 7F 32 08 02 52 20")), "not 32 52 20: a no-op on the stock");
  assert.equal(el.querySelector("#songs-status")!.textContent, "Save as SMF failed: No media");

  tapTwice(el.querySelector('[data-act="format"]'));
  await tick(40);
  assert.ok(sent.includes("F0 40 7F 32 08 02 51 3C 7F 00 F7"), "format");
  assert.equal(el.querySelector("#usb-files")!.textContent, "No files.");
  notify([0x51, 0x2e, 0x7f, 0]);
  await tick(5);
  assert.equal(el.querySelector("#usb-status")!.textContent, "No USB memory");
  void store;
  el.remove();
});

test("music tab: the song picker holds the mode's songs only (USB files play from the Recorder tab) and Concert Magic settings + result display", async () => {
  const { store, ctx, sent, notify } = await setup();
  ctx.usbFiles = ["X.MID", "Y.MID"];
  const el = S.musicScreen(store, ctx);
  document.body.append(el);
  await tick(10);
  assert.ok(sent.includes("F0 40 7F 02 08 02 52 0D 7F F7"), "CM settings requested");
  byText(el, "button", "Concert Magic").click();
  const cm = [...el.querySelectorAll("section")].find((s) => s.querySelector("h3")!.textContent === "Concert Magic")!;
  const pick = field<HTMLSelectElement>(cm, "Concert Magic song");
  assert.deepEqual([...pick.options].map((o) => o.textContent), Array.from({ length: 10 }, (_, i) => `Song ${i + 1}`), "no USB entries");
  change(pick, "1");
  await tick(20);
  assert.ok(sent.includes("F0 40 7F 10 08 02 52 00 7F 00 00 01 F7"), "SongSelect song 2 of the mode (numType 0)");

  notify([0x52, 0x0d, 0x7f, 0, 1, 1]);
  await tick(5);
  assert.equal(field<HTMLSelectElement>(cm, "Concert Magic mode").value, "0");
  assert.equal(field(cm, "Smooth tempo").checked, true);
  change(field<HTMLSelectElement>(cm, "Concert Magic mode"), "1");
  await tick(20);
  assert.ok(sent.includes("F0 40 7F 10 08 02 52 0D 7F 01 01 01 F7"), "settings sent");

  notify([0x52, 0x0b, 0x7f, 2, 0, 12]);
  await tick(5);
  assert.equal(cm.querySelector("#cm-result")!.textContent, "Fast · 12 notes played");
  notify([0x52, 0x0b, 0x7f, 5, 1, 0]);
  await tick(5);
  assert.equal(cm.querySelector("#cm-result")!.getAttribute("data-code"), "5");
  assert.match(cm.querySelector("#cm-result")!.textContent!, /128/);
  el.remove();
});
