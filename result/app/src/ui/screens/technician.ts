// Virtual Technician tab: preset, the VT parameters of the Main sound, sound preview, user edit.
// Like the stock VT editor the rows follow the Main sound (pianosound.ts vtVisible); for a sound
// outside the Piano category the stock shows no VT preset and calls the same rows "Other sound
// settings" (manual p.34 (8), p.54 CHECK).
import { t, tl } from "../../i18n.ts";
import { applySettings, applyVtPreset, currentSound, isPiano, settingsOf, trackVt, VT_PARAMS, vtVisible } from "../../features/pianosound.ts";
import { TEMPERAMENT_KEYS, temperamentTypes } from "../../features/soundfx.ts";
import { findParam } from "../../protocol/codecs.ts";
import { VT_PRESETS } from "../../protocol/params.gen.ts";
import type { ParamStore } from "../../state/store.ts";
import type { AppContext } from "../context.ts";
import { control } from "../controls.ts";
import { h, section } from "../dom.ts";
import { leftDocument, onDispose } from "../scope.ts";
import { userEditSection } from "../vtedit.ts";
import { ctxFor, whileShown } from "./common.ts";

const VT_LABEL: Record<string, string> = { KeyAttackNoise: "Hammer Noise", DCAReleaseTime: "Release Time", TopboardSimulation: "Topboard", FallbackNoise: "Fall-back Noise" };

/** One line per Virtual Technician term (our wording; the behaviour each parameter models: manual p.55-64). */
export const VT_HELP: Record<string, string> = {
  TouchCurve: "How hard you must play for a loud note; Off plays every note at one volume.",
  Voicing: "Hammer character: mellow to bright, or changing with how hard you play.",
  ResonanceDepth: "Overall amount of resonance in the SK-EX Rendering sound.",
  DamperResonance: "Other strings ringing along while the sustain pedal is down.",
  DamperNoise: "The dampers touching and leaving the strings when you use the pedal.",
  StringResonance: "Held notes ringing along with related notes you play.",
  UndampedStringResonance: "The top strings, which have no dampers, ringing along with lower notes.",
  CabinetResonance: "Resonance of the piano's case and frame.",
  KeyOffEffect: "Faint sound of the damper stopping a note as you let go of a key.",
  FallbackNoise: "Sound of the key action returning after you release a key.",
  KeyAttackNoise: "Knock of hammer and key at the start of each note.",
  HammerDelay: "A short lag between a soft key press and the note.",
  TopboardSimulation: "Lid position: open for a fuller sound, closed for a darker one.",
  DecayTime: "How long a held note keeps sounding.",
  DCAReleaseTime: "How long a note fades after you let go of the key.",
  MinimumTouch: "The softest key press that still makes a sound.",
  StretchTuning: "Bass slightly flat and treble slightly sharp, as tuners do on acoustic pianos.",
  Temperament: "Tuning system. All but Equal favour the key chosen below.",
  KeyVolume: "Quietens part of the keyboard: treble, bass, both ends or the middle.",
  HalfPedalAdjust: "How far the sustain pedal goes down before it starts to work; 8-10 are for MIDI use.",
  SoftPedalDepth: "How much the soft (left) pedal quietens the sound.",
  DamperHold: "Whether organ, strings and similar sounds keep sounding while the sustain pedal is down.",
};

/** Temperament `50 0D` [type, key] as two selects (manual p.62-63: type and key are separate settings); each
 * re-sends the other byte as the piano has it, and sends nothing while the value is unknown. */
function temperamentRows(store: ParamStore): HTMLElement {
  const p = findParam("Temperament")!;
  const types = temperamentTypes();
  const typeSel = h("select", {}, ...types.map(([raw, name]) => h("option", { value: String(raw) }, tl(name)))) as HTMLSelectElement;
  const keySel = h("select", {}, ...TEMPERAMENT_KEYS.map((k, i) => h("option", { value: String(i) }, k))) as HTMLSelectElement;
  const typeRow = h("label", { class: "row" }, h("span", {}, tl("Temperament")), typeSel);
  const keyRow = h("label", { class: "row" }, h("span", {}, t("Temperament key")), keySel);
  const keyNote = h("p", { class: "muted row-note" }, t("The key has no effect with Equal temperament."));
  const sync = () => {
    const d = store.get(p, 0x00);
    for (const r of [typeRow, keyRow]) r.classList.toggle("unknown", !d);
    if (!d) {
      typeSel.selectedIndex = -1;
      keySel.selectedIndex = -1;
      return;
    }
    typeSel.value = String(d[0]);
    keySel.value = String(d[1] ?? 0);
    keyNote.hidden = d[0] !== 0;
  };
  const send = (type: number, key: number) => void store.set(p, 0x00, [type, key]);
  typeSel.addEventListener("change", () => {
    const d = store.get(p, 0x00);
    if (d) send(Number(typeSel.value), d[1] ?? 0);
  });
  keySel.addEventListener("change", () => {
    const d = store.get(p, 0x00);
    if (d) send(d[0], Number(keySel.value));
  });
  const box = h("div", { "data-param": "Temperament" }, typeRow, keyRow, keyNote);
  whileShown(box, store, sync);
  store.want(p, 0x00);
  sync();
  return box;
}

/** VT parameter -> SoundPreviewStart/Stop value (labels from the original table). */
export const PREVIEW_INDEX: Record<string, number> = {
  TouchCurve: 0, Voicing: 1, DamperResonance: 2, DamperNoise: 3, StringResonance: 4, KeyOffEffect: 5,
  KeyAttackNoise: 6, HammerDelay: 7, TopboardSimulation: 8, DecayTime: 9, MinimumTouch: 10, StretchTuning: 11,
  Temperament: 13, KeyVolume: 15, HalfPedalAdjust: 17, SoftPedalDepth: 18, UndampedStringResonance: 21, CabinetResonance: 22,
};

export const vtLabel = (n: string): string => tl(VT_LABEL[n] ?? humanizeVt(n));

/** One control per VT parameter (Temperament as type + key) with its one-line note; `refresh()` hides the
 * parameters the Main sound has not (returns what shows). */
export function vtRows(store: ParamStore): { rows: Map<string, HTMLElement>; refresh(): Set<string> } {
  const rows = new Map(VT_PARAMS.map((n) => {
    const row = n === "Temperament" ? temperamentRows(store) : control(store, n, { label: vtLabel(n), part: n === "DamperHold" ? 0x0f : 0x00 });
    return [n, n in VT_HELP ? h("div", { class: "vt-param" }, row, h("p", { class: "muted row-note" }, t(VT_HELP[n]))) : row];
  }));
  const refresh = () => {
    const vis = vtVisible(store);
    for (const [n, row] of rows) row.hidden = !vis.has(n);
    return vis;
  };
  return { rows, refresh };
}

/** True when the Main sound is known and is not a piano: the stock "Other sound settings". */
export function otherSoundSettings(store: ParamStore): boolean {
  const s = currentSound(store);
  return !!s && !isPiano(s);
}

export function vtScreen(store: ParamStore, ctx?: AppContext): HTMLElement {
  const lib = ctxFor(store, ctx).library;
  // the preset is derived from the piano's values like the stock app: an edit selects "User"
  const presets = h(
    "select",
    {},
    h("option", { value: "", disabled: true }, "–"),
    ...VT_PRESETS.map((p, i) => h("option", { value: String(i) }, tl(String(p.name)))),
    h("option", { value: "user" }, t("User")),
  ) as HTMLSelectElement;
  presets.addEventListener("change", () => {
    if (presets.value === "user") {
      const u = lib.userVt;
      if (u) void applySettings(store, settingsOf(u));
    } else if (presets.value !== "") void applyVtPreset(store, Number(presets.value));
  });
  const syncPreset = () => {
    const st = trackVt(store, lib);
    presets.value = st.kind === "preset" ? String(st.index) : st.kind === "user" ? "user" : "";
    (presets.querySelector('option[value="user"]') as HTMLOptionElement).disabled = st.kind !== "user" && !lib.userVt;
  };

  // Sound preview (feature 81): the piano plays a demonstration phrase for the
  // parameter being edited. The target follows the last control the user touched.
  const previewable = VT_PARAMS.filter((n) => n in PREVIEW_INDEX);
  const target = h("select", {}, ...previewable.map((n) => h("option", { value: n }, vtLabel(n)))) as HTMLSelectElement;
  let playing: number | null = null;
  const startP = findParam("SoundPreviewStart")!;
  const stopP = findParam("SoundPreviewStop")!;
  const stop = () => {
    if (playing === null) return;
    void store.set(stopP, 0x7f, [playing]);
    playing = null;
    startBtn.classList.remove("on");
  };
  const start = () => {
    stop();
    playing = PREVIEW_INDEX[target.value];
    void store.set(startP, 0x7f, [playing]);
    startBtn.classList.add("on");
  };
  const startBtn = h("button", { class: "primary", onclick: start }, `▶ ${t("Preview")}`);
  // §9.10: the piano reports SoundPreviewStop when the phrase ends by itself
  // goes with the screen (ui/scope.ts); built without a scope, at the first message after the tab was left
  const gone = leftDocument(() => startBtn);
  const offPreview = store.onMessage((m) => {
    if (gone()) return void offPreview();
    if (m.fn === 0x10 && m.v1 === 0x50 && m.v2 === 0x1e && m.data[0] === playing) {
      playing = null;
      startBtn.classList.remove("on");
    }
  });
  onDispose(offPreview);
  const stopBtn = h("button", { onclick: stop }, `■ ${t("Stop")}`);

  const vt = vtRows(store);
  const advanced = section(t("Advanced"), ...[...vt.rows].map(([n, row]) => {
    if (n in PREVIEW_INDEX) {
      const follow = () => {
        if (target.value !== n) {
          stop();
          target.value = n;
        }
      };
      row.addEventListener("input", follow);
      row.addEventListener("change", follow);
      // a tap on the row (not pointerdown: a scroll starting on the row must not stop the preview)
      row.addEventListener("click", follow);
    }
    return row;
  }));

  const presetSection = section(t("Preset"), h("label", { class: "row" }, h("span", {}, t("Virtual Technician preset")), presets));
  const title = advanced.querySelector("h3")!;
  const syncSound = () => {
    const vis = vt.refresh();
    const other = otherSoundSettings(store);
    presetSection.hidden = other;
    title.textContent = other ? t("Other sound settings") : t("Advanced");
    // the preview offers only the rows on screen; a hidden target stops and moves to the first shown
    for (const o of target.options) o.hidden = o.disabled = !vis.has(o.value);
    if (!vis.has(target.value)) {
      stop();
      const first = [...target.options].find((o) => !o.disabled);
      if (first) target.value = first.value;
    }
  };

  const root = h(
    "div",
    { class: "screen" },
    presetSection,
    advanced,
    section(
      t("Sound preview"),
      h("label", { class: "row" }, h("span", {}, t("Parameter")), target),
      h("div", { class: "transport" }, startBtn, stopBtn),
      control(store, "SoundPreviewVolume", { label: t("Preview volume"), part: 0x7f }),
    ),
    userEditSection(store),
  );
  whileShown(root, store, () => {
    syncPreset();
    syncSound();
  });
  syncPreset();
  syncSound();
  return root;
}

function humanizeVt(n: string): string {
  return n.replace(/([a-z])([A-Z])/g, "$1 $2");
}
