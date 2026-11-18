// Sound tab (the former Piano tab folded in, user 2026-11-09):
// the sound list (Piano category first and open by default, then the other categories, favorites, recent, user
// sounds) with the chosen part's sound shown as selected, ☆ and "Save this sound"; then everything choosing a
// sound changes right below it — SK-EX Rendering (character, resonance, ambience) for Rendering, the Virtual
// Technician preset for piano sounds, Effect, Reverb (not for Rendering); keyboard mode and parts last.
// Tuning and Transpose are global, not per sound: they are on the Settings tab.
// Stock behaviour and evidence:
import { t, tl } from "../../i18n.ts";
import {
  applySettings,
  applyUserSound,
  applyVtPreset,
  captureUserSound,
  currentSound,
  currentUserSound,
  isPiano,
  isRendering,
  MAIN,
  presetSettings,
  settingsOf,
  trackVt,
} from "../../features/pianosound.ts";
import { offset64 } from "../../protocol/codecs.ts";
import { SOUNDS, VT_PRESETS } from "../../protocol/params.gen.ts";
import type { SoundRef } from "../../state/settings.ts";
import type { ParamStore } from "../../state/store.ts";
import type { AppContext } from "../context.ts";
import { control } from "../controls.ts";
import { h, section } from "../dom.ts";
import { ctxFor, PART, PART_LABEL, selectSound, soundParam, whileShown } from "./common.ts";

const USER_VT = "user";

export function soundScreen(store: ParamStore, ctx?: AppContext): HTMLElement {
  const c = ctxFor(store, ctx);
  const lib = c.library;
  let target: number = PART.Main1;
  let shown = "";
  // Piano first (SOUNDS keeps the panel's category order, Piano leads)
  const cats = [...new Set(SOUNDS.map((s) => s.category))].sort((a, b) => Number(b === "Piano") - Number(a === "Piano"));
  const listEl = h("div", { class: "list" });
  const now = h("p", { class: "muted current-sound" });
  const saved = h("p", { class: "muted", id: "sound-saved" });

  const soundItem = (s: SoundRef) => {
    const star = h("button", { class: "star", title: t("Favorite"), "aria-pressed": String(lib.isFavorite(s)) }, lib.isFavorite(s) ? "★" : "☆");
    star.addEventListener("click", () => {
      lib.toggleFavorite(s);
      showCat(shown);
      refresh();
    });
    const item = h("button", { class: "item", "data-sound": `${s.msb}/${s.lsb}/${s.pc}`, "aria-pressed": "false", onclick: () => void selectSound(c, target, s) }, s.name);
    return h("div", { class: "sound-item" }, item, star);
  };

  // the sound of the chosen part shows as selected and follows the piano (panel changes too)
  const markSelected = () => {
    const d = store.get(soundParam(), target);
    const id = d ? `${d[0]}/${d[1]}/${d[2]}` : "";
    listEl.querySelectorAll<HTMLElement>(".item[data-sound]").forEach((b) => {
      const on = b.getAttribute("data-sound") === id;
      b.classList.toggle("on", on);
      b.setAttribute("aria-pressed", String(on));
    });
    const u = currentUserSound(store, lib);
    listEl.querySelectorAll<HTMLElement>(".item[data-user]").forEach((b) => b.classList.toggle("on", b.getAttribute("data-user") === u?.name));
  };

  const showCat = (cat: string) => {
    shown = cat;
    catBar.querySelectorAll("button").forEach((b) => b.classList.toggle("on", b.getAttribute("data-cat") === cat));
    if (cat === "@fav" || cat === "@recent") {
      const items = cat === "@fav" ? lib.favorites : lib.recent;
      listEl.replaceChildren(...(items.length ? items.map(soundItem) : [h("p", { class: "muted" }, cat === "@fav" ? t("No favorites yet. Tap ☆ next to a sound.") : t("No sounds played yet."))]));
    } else if (cat === "@user") {
      listEl.replaceChildren(...userItems());
    } else {
      listEl.replaceChildren(...SOUNDS.filter((s) => s.category === cat).map(soundItem));
    }
    markSelected();
  };

  // user sounds: named registrations of Main/Layer/Lower sounds + keyboard mode, effects, VT
  const userItems = (): HTMLElement[] => {
    const items: HTMLElement[] = lib.user.map((u) =>
      h(
        "div",
        { class: "sound-item" },
        h("button", { class: "item", "data-user": u.name, onclick: () => void applyUserSound(store, u) }, u.name),
        h("button", { class: "star", title: t("Delete"), onclick: () => { lib.removeUser(u.name); showCat("@user"); } }, "✕"),
      ),
    );
    if (!items.length) items.push(h("p", { class: "muted" }, t("No user sounds saved.")));
    items.push(h("button", { class: "primary", onclick: () => saveUser() }, t("Save current as user sound")));
    return items;
  };
  // stock Piano tab (8) Menu → Save this sound: the registration goes to the User category, which then opens
  const saveUser = () => {
    const def = t("User {n}", { n: lib.user.length + 1 });
    const name = typeof prompt === "function" ? prompt(t("Name for this user sound"), def) : def;
    if (!name) return;
    lib.saveUser(captureUserSound(store, name));
    saved.textContent = t("Saved as user sound “{name}”.", { name });
    showCat("@user");
  };

  const catBar = h(
    "div",
    { class: "chips" },
    ...cats.map((cat) => h("button", { class: "chip", "data-cat": cat, onclick: () => showCat(cat) }, tl(cat))),
    h("button", { class: "chip", "data-cat": "@fav", onclick: () => showCat("@fav") }, `★ ${t("Favorites")}`),
    h("button", { class: "chip", "data-cat": "@recent", onclick: () => showCat("@recent") }, t("Recent")),
    h("button", { class: "chip", "data-cat": "@user", onclick: () => showCat("@user") }, t("User")),
  );
  const partBar = h(
    "div",
    { class: "segmented" },
    ...Object.entries(PART).map(([name, v]) =>
      h("button", {
        class: v === target ? "on" : undefined,
        onclick: (e: Event) => {
          target = v;
          partBar.querySelectorAll("button").forEach((b) => b.classList.remove("on"));
          (e.currentTarget as HTMLElement).classList.add("on");
          refresh();
        },
      }, t(PART_LABEL[name])),
    ),
  );

  // favorite of the chosen part's sound (stock Piano tab (7) Favorite) + save this sound
  const star = h("button", { class: "star", title: t("Favorite"), "aria-pressed": "false", "data-action": "favorite-current" }, "☆");
  star.addEventListener("click", () => {
    const s = currentSound(store, target);
    if (s) lib.toggleFavorite(s);
    showCat(shown); // the list's ☆ follow
    refresh();
  });
  const save = h("button", { "data-action": "save-sound", onclick: () => saveUser() }, t("Save this sound"));

  // ---- what choosing a sound changes (Main1, stock `sendSound`) ----
  // Virtual Technician preset: Default … Historical, then User
  const vtSelect = h(
    "select",
    {},
    h("option", { value: "", disabled: true }, "–"),
    ...VT_PRESETS.map((p, i) => h("option", { value: String(i) }, tl(String(p.name)))),
    h("option", { value: USER_VT }, t("User")),
  ) as HTMLSelectElement;
  vtSelect.addEventListener("change", () => {
    const v = vtSelect.value;
    if (v === USER_VT) {
      const u = lib.userVt;
      if (u) void applySettings(store, settingsOf(u));
    } else if (v !== "") void applyVtPreset(store, Number(v));
  });
  const vtNote = h("p", { class: "muted" });
  const vtSection = section(t("Virtual Technician"), h("label", { class: "row" }, h("span", {}, t("Virtual Technician preset")), vtSelect), vtNote);
  const renderingSection = section(
    t("SK-EX Rendering"),
    control(store, "RenderingCharacter", { label: t("Character") }),
    control(store, "ResonanceDepth", { label: t("Resonance Depth") }),
    control(store, "AmbienceType", { label: t("Ambience") }),
    control(store, "AmbienceDepth", { label: t("Ambience Depth") }),
  );
  const effectSection = section(t("Effect"), control(store, "Effect", { label: t("Effect") }), control(store, "EffectType", { label: t("Type") }), control(store, "EffectSetting1", { label: t("Setting 1") }), control(store, "EffectSetting2", { label: t("Setting 2") }));
  const reverbSection = section(t("Reverb"), control(store, "Reverb", { label: t("Reverb") }), control(store, "ReverbType", { label: t("Type") }), control(store, "ReverbDepth", { label: t("Depth") }), control(store, "ReverbTime", { label: t("Time") }));
  for (const s of [vtSection, renderingSection, effectSection, reverbSection]) s.classList.add("sound-dep");

  function refresh(): void {
    markSelected();
    const d = store.get(soundParam(), target);
    const cur = currentSound(store, target);
    now.textContent = !d ? t("Reading the current sound…") : t("Selected: {name}", { name: cur?.name ?? `${d[0]}/${d[1]}/${d[2]}` });
    const fav = !!cur && lib.isFavorite(cur);
    star.textContent = fav ? "★" : "☆";
    star.setAttribute("aria-pressed", String(fav));
    (star as HTMLButtonElement).disabled = !cur;
    // the sections follow the Main sound (a sound brings its values on Main1 only). The manual: VT presets
    // are not shown for SK-EX Rendering; Ambience only for it, Reverb not for it
    const main = currentSound(store, MAIN);
    const rendering = isRendering(main);
    renderingSection.hidden = !!main && !rendering;
    reverbSection.hidden = rendering;
    vtSection.hidden = rendering || (!!main && !isPiano(main));
    const st = trackVt(store, lib);
    vtSelect.value = st.kind === "preset" ? String(st.index) : st.kind === "user" ? USER_VT : "";
    (vtSelect.querySelector(`option[value="${USER_VT}"]`) as HTMLOptionElement).disabled = st.kind !== "user" && !lib.userVt;
    vtNote.textContent = st.kind === "user" ? t("Edited values: the preset shows User.") : st.kind === "unknown" ? t("Reading the Virtual Technician values…") : "";
  }
  // every value the preset match compares: read back what the dump / group gets left out
  for (const s of presetSettings(VT_PRESETS[0])) store.want(s.p, s.part);
  whileShown(listEl, store, () => refresh());
  showCat("Piano");
  refresh();

  return h(
    "div",
    { class: "screen" },
    section(t("Sound select"), partBar, now, h("div", { class: "transport" }, star, save), saved, catBar, listEl),
    renderingSection,
    vtSection,
    effectSection,
    reverbSection,
    section(t("Keyboard mode"), control(store, "KeyboardMode", { label: t("Mode"), part: 0x0f }), control(store, "SplitPoint", { part: PART.Lower, label: t("Split point") }), control(store, "Balance", { part: PART.Layer, label: t("Balance") }), control(store, "LowerPedal", { part: 0x0f, label: t("Lower pedal") })),
    section(
      t("Part"),
      control(store, "OctaveShift", { part: PART.Layer, label: t("Layer octave"), min: -3, max: 3, toRaw: offset64.encode, fromRaw: offset64.decode }),
      control(store, "OctaveShift", { part: PART.Lower, label: t("Lower octave"), min: -3, max: 3, toRaw: offset64.encode, fromRaw: offset64.decode }),
      control(store, "Dynamics", { part: PART.Layer, label: t("Layer dynamics") }),
    ),
  );
}
