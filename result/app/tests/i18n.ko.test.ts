// Korean labels from the protocol tables (tl): parameter names, enum values and categories are shown in
// Korean; only proper names stay English (sound names, rhythm names, effect types, language names).
import "./_dom.ts";
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const S = await import("../src/ui/screens.ts");
const { createContext } = await import("../src/ui/context.ts");
const { SOUNDS, RHYTHMS, PARAMS } = await import("../src/protocol/params.gen.ts");
const i18n = await import("../src/i18n.ts");

afterEach(() => i18n.setLang("en"));

const HANGUL = /[가-힣]/;
const PROPER = new Set<string>([
  ...SOUNDS.map((s) => s.name),
  ...(PARAMS.find((p) => p.name === "EffectType")?.values ?? []).map((v) => v.label),
  ...i18n.LANGS.map((l) => l.name),
  "MIDI", "Bluetooth", "Bluetooth MIDI",
]);
const RHYTHM_NAMES = new Set(RHYTHMS.map((r) => r.name));

/** Labels, options and chips that still read English and are not a proper name. */
function english(el: Element): string[] {
  const out = new Set<string>();
  const texts = [...el.querySelectorAll("option, .row > span:first-child, button.chip, h3")].map((e) => e.textContent!.trim());
  for (const s of texts) {
    if (!/[A-Za-z]{2,}/.test(s) || HANGUL.test(s) || PROPER.has(s)) continue;
    out.add(s);
  }
  return [...out];
}

test("ko: sound, technician, metronome, music and settings show no English table labels", () => {
  const store = new ParamStore();
  const ctx = createContext(store, { panel: true, rerender: () => {} });
  i18n.setLang("ko");
  const screens = {
    sound: S.soundScreen(store, ctx),
    technician: S.vtScreen(store, ctx),
    metronome: S.metronomeScreen(store, new EventTarget()),
    music: S.musicScreen(store, () => "", ctx),
    settings: S.settingsScreen(store, ctx),
  };
  for (const [name, el] of Object.entries(screens)) assert.deepEqual(english(el), [], name);
  // rhythm options: Korean category, English rhythm name
  const opts = [...screens.metronome.querySelectorAll("select option")].map((o) => o.textContent!).filter((t) => t.includes(" · "));
  assert.equal(opts.length, RHYTHMS.length);
  for (const o of opts) {
    const [cat, nm] = o.split(" · ");
    assert.ok(HANGUL.test(cat), `rhythm category ${cat}`);
    assert.ok(RHYTHM_NAMES.has(nm), `rhythm name ${nm}`);
  }
  // sound names stay as Kawai writes them
  assert.ok([...screens.sound.querySelectorAll(".sound-item .item")].some((b) => b.textContent === "SK-EX | Concert"));
});

test("ko: temperament labels translate the name and keep the key", () => {
  i18n.setLang("ko");
  assert.equal(i18n.tl("Equal/C#"), "평균율/C#");
  assert.equal(i18n.tl("Werckmeister/A"), "베르크마이스터/A");
  assert.equal(i18n.tl("Damper Resonance"), "댐퍼 공명");
  // a proper name with a slash is left alone
  assert.equal(i18n.tl("Chorus/Classic Chorus"), "Chorus/Classic Chorus");
  i18n.setLang("en");
  assert.equal(i18n.tl("Equal/C#"), "Equal/C#");
});

