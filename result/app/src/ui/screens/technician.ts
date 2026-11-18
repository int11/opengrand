// Virtual Technician tab: preset, every VT parameter, sound preview, user edit.
import { t, tl } from "../../i18n.ts";
import { applySettings, applyVtPreset, settingsOf, trackVt, VT_PARAMS } from "../../features/pianosound.ts";
import { findParam } from "../../protocol/codecs.ts";
import { VT_PRESETS } from "../../protocol/params.gen.ts";
import type { ParamStore } from "../../state/store.ts";
import type { AppContext } from "../context.ts";
import { control } from "../controls.ts";
import { h, section } from "../dom.ts";
import { userEditSection } from "../vtedit.ts";
import { ctxFor, whileShown } from "./common.ts";

const VT_LABEL: Record<string, string> = { KeyAttackNoise: "Hammer Noise", DCAReleaseTime: "Release Time", TopboardSimulation: "Topboard", FallbackNoise: "Fall-back Noise" };

/** VT parameter -> SoundPreviewStart/Stop value (labels from the original table). */
export const PREVIEW_INDEX: Record<string, number> = {
  TouchCurve: 0, Voicing: 1, DamperResonance: 2, DamperNoise: 3, StringResonance: 4, KeyOffEffect: 5,
  KeyAttackNoise: 6, HammerDelay: 7, TopboardSimulation: 8, DecayTime: 9, MinimumTouch: 10, StretchTuning: 11,
  Temperament: 13, KeyVolume: 15, HalfPedalAdjust: 17, SoftPedalDepth: 18, UndampedStringResonance: 21, CabinetResonance: 22,
};

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
  const target = h("select", {}, ...previewable.map((n) => h("option", { value: n }, tl(VT_LABEL[n] ?? humanizeVt(n))))) as HTMLSelectElement;
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
  store.onMessage((m) => {
    if (m.fn === 0x10 && m.v1 === 0x50 && m.v2 === 0x1e && m.data[0] === playing) {
      playing = null;
      startBtn.classList.remove("on");
    }
  });
  const stopBtn = h("button", { onclick: stop }, `■ ${t("Stop")}`);

  const advanced = section(t("Advanced"), ...VT_PARAMS.map((n) => {
    const row = control(store, n, { label: tl(VT_LABEL[n] ?? humanizeVt(n)), part: n === "DamperHold" ? 0x0f : 0x00 });
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

  const root = h(
    "div",
    { class: "screen" },
    section(t("Preset"), h("label", { class: "row" }, h("span", {}, t("Virtual Technician preset")), presets)),
    advanced,
    section(
      t("Sound preview"),
      h("label", { class: "row" }, h("span", {}, t("Parameter")), target),
      h("div", { class: "transport" }, startBtn, stopBtn),
      control(store, "SoundPreviewVolume", { label: t("Preview volume"), part: 0x7f }),
    ),
    userEditSection(store),
  );
  whileShown(root, store, syncPreset);
  syncPreset();
  return root;
}

function humanizeVt(n: string): string {
  return n.replace(/([a-z])([A-Z])/g, "$1 $2");
}
