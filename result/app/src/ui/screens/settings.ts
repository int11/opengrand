// Settings tab: app & screen, tuning, speaker / headphone, power (the stock auto power-off + the OpenGrand power policy),
// MIDI, system, startup settings, user data. Layout:
import { LANGS, t } from "../../i18n.ts";
import { findParam, offset64, tuning } from "../../protocol/codecs.ts";
import { AUTO_OFF_MINUTES, SCALES, TAB_IDS, type TabId } from "../../state/settings.ts";
import type { ParamStore } from "../../state/store.ts";
import type { AppContext } from "../context.ts";
import { control } from "../controls.ts";
import { toneRows } from "../tone.ts";
import { confirmTap, disarm } from "../confirm2.ts";
import { h, section } from "../dom.ts";
import { powerPolicySection } from "../power.ts";
import { startupSection } from "../startup.ts";
import { ctxFor, TAB_TITLES } from "./common.ts";

function appSelect<V extends string | number>(label: string, options: [V, string][], value: V, onChange: (v: V) => void): HTMLElement {
  const sel = h("select", {}, ...options.map(([v, l]) => h("option", { value: String(v) }, l))) as HTMLSelectElement;
  sel.value = String(value);
  sel.addEventListener("change", () => onChange(options.find(([v]) => String(v) === sel.value)![0]));
  return h("label", { class: "row" }, h("span", {}, label), sel);
}

/** A panel-local setting seen from a phone/PC: no control, no value that could pass for the panel's. */
function panelOnlyRow(label: string, id: string): HTMLElement {
  return h("div", { class: "row panel-only", "data-panel-only": id }, h("span", {}, label), h("output", {}, t("Set on the panel · value unknown")));
}

/**
 * 「앱 · 화면」: this app's own settings and tools, first on the Settings tab — language,
 * startup screen, the MIDI log, and inside the original panel shell the UI chooser and the Kawai UI (they were
 * in the header's ⋯ / the narrow "More" sheet before 2026-11-02).
 */
function appSection(c: AppContext): HTMLElement {
  const s = c.settings;
  const app = c.app;
  const log = h("input", { type: "checkbox", id: "midi-log-switch" }) as HTMLInputElement;
  log.checked = !!app?.midiLogOpen();
  log.addEventListener("change", () => app?.setMidiLog(log.checked));
  return h(
    "section",
    { class: "card", id: "app-settings" },
    h("h3", {}, t("App & screen")),
    appSelect(t("Language"), LANGS.map((l) => [l.id, l.name] as [string, string]), s.lang, (v) => c.updateSettings({ lang: v as typeof s.lang })),
    appSelect(t("Startup screen"), TAB_IDS.map((id) => [id, t(TAB_TITLES[id])] as [string, string]), s.startTab, (v) => c.updateSettings({ startTab: v as TabId })),
    ...(app ? [h("label", { class: "row", id: "midi-log-row" }, h("span", {}, t("MIDI log")), log)] : []),
    ...(app?.shell ? [h("div", { class: "list", id: "panel-ui-links" },
      h("button", { class: "item", id: "ui-chooser", onclick: () => app.go("../index.html?stay=1") }, t("UI chooser")),
      h("button", { class: "item", id: "kawai-ui", onclick: () => app.go("../ui/html/index.html") }, t("Kawai UI")),
      // owner 2026-11-15: Android Settings straight from our UI (was only the Kawai UI's hidden 12-tap menu / chooser)
      ...(app.androidSettings ? [h("button", { class: "item", id: "android-settings", onclick: () => app.androidSettings?.() }, t("Android settings"))] : [])),
      // the S3 install probe tools (open the stick / an address, 「Android 탐색」) are field-test tools: the Info tab's
      // 「개발자용」 card (ui/paneltools.ts, owner 2026-11-16)
    ] : []),
  );
}

/** Full factory reset (36 [04 7F 7F 7F]): every setting back to factory now and the 10 recorder songs deleted. Two
 * steps: tick "I understand" (which enables the button), then tap it twice (in-page confirm, ui/confirm2.ts). */
function fullResetBox(remote: (name: string, data: number[]) => () => void): HTMLElement {
  const btn = h("button", { class: "danger", "data-action": "full-reset", disabled: true }, t("Full factory reset")) as HTMLButtonElement;
  const ack = h("input", { type: "checkbox", name: "fullResetAck" }) as HTMLInputElement;
  ack.addEventListener("change", () => {
    btn.disabled = !ack.checked;
    if (!ack.checked) disarm(btn);
  });
  btn.addEventListener("click", () => {
    if (!ack.checked) return;
    confirmTap(btn, t("Full factory reset: every setting goes back to factory now and ALL 10 recorder songs on the piano are deleted. This cannot be undone. Continue?"), () => {
      if (!ack.checked) return;
      ack.checked = false;
      btn.disabled = true;
      remote("StartUpSettingResetStart", [0x04, 0x7f, 0x7f, 0x7f])();
    });
  });
  return h("div", { class: "danger-zone", id: "full-reset" },
    h("label", { class: "row" }, h("span", {}, t("I understand: all settings are reset and the 10 recorder songs are deleted")), ack),
    h("div", { class: "transport" }, btn));
}

export function settingsScreen(store: ParamStore, ctx?: AppContext): HTMLElement {
  const c = ctxFor(store, ctx);
  const s = c.settings;
  const remote = (name: string, data: number[]) => () => void store.command(findParam(name, 0x32)!, 0x7f, data);

  // LCD brightness 1..10 -> shell backlight
  const bright = h("input", { type: "range", min: 1, max: 10, step: 1, value: s.brightness }) as HTMLInputElement;
  const brightOut = h("output", {}, String(s.brightness));
  bright.addEventListener("input", () => {
    brightOut.textContent = bright.value;
    c.updateSettings({ brightness: Number(bright.value) });
  });

  const lib = c.library;
  const recStatus = h("p", { class: "muted", id: "rec-reset" });
  store.onMessage((m) => {
    // ResetCompleteInfo 32 52 22 7F [0, hi, lo]; [0, 7F, 0] = all songs
    if (m.fn === 0x32 && m.v1 === 0x52 && m.v2 === 0x22) recStatus.textContent = m.data[1] === 0x7f ? t("All recorder songs deleted.") : t("Song {n} deleted.", { n: ((m.data[1] << 7) | m.data[2]) + 1 });
  });
  const reset = (msg: string, fn: () => void) => (e: Event) => confirmTap(e.currentTarget, msg, fn);

  return h(
    "div",
    { class: "screen" },
    appSection(c),
    // global, not per sound: here since the Piano tab went into the Sound tab
    h(
      "section",
      { class: "card", id: "tuning-transpose" },
      h("h3", {}, t("Tuning & Transpose")),
      control(store, "Tuning", { label: t("Tuning"), min: 427, max: 453, toRaw: tuning.encode, fromRaw: tuning.decode, format: (v) => `${v.toFixed(1)} Hz` }),
      control(store, "TransposeValue", { label: t("Transpose"), part: 0x0f, min: -12, max: 12, toRaw: offset64.encode, fromRaw: offset64.decode }),
    ),
    section(t("Speaker / Headphone"), control(store, "ToneControl", { label: t("Tone control") }), ...toneRows(store), control(store, "SpeakerVolume", { label: t("Speaker volume") }), control(store, "SHSMode", { label: t("Spatial headphone sound") }), control(store, "PhonesType", { label: t("Headphone type") }), control(store, "PhonesVolume", { label: t("Headphone volume") }), control(store, "LineInLevel", { label: t("Line in level") })),
    // 「전원」: the stock auto power-off and, on OpenGrand, standby / off / wake and the power policy (ui/power.ts). The
    // piano's Bluetooth settings are on the Bluetooth tab only.
    h("section", { class: "card", id: "power-settings" },
      h("h3", {}, t("Power")),
      control(store, "AutoPowerOff", { label: t("Auto power off") }),
      // OpenGrand power policy (0x7D op 0x18 / 0x19): hidden unless the piano reports the feature
      powerPolicySection(c)),
    section(
      t("MIDI"),
      control(store, "MIDIChannel", { label: t("Channel"), min: 1, max: 16, toRaw: (v) => [v - 1], fromRaw: (d) => (d[0] ?? 0) + 1 }),
      control(store, "LocalControl", { label: t("Local control") }),
      control(store, "TransmitProgramChange", { label: t("Transmit program change") }),
      control(store, "MultiTimbralMode", { label: t("Multi-timbral mode") }),
    ),
    h(
      "section",
      { class: "card", id: "system-settings" },
      h("h3", {}, t("System")),
      // LCD brightness and Auto Display Off belong to the built-in panel (its app keeps them in
      // user/uicache.json); the piano firmware has
      // no such parameter and the panel never sends them, so a phone/PC cannot read them.
      ...(c.panel
        ? [
            h("label", { class: "row slider" }, h("span", {}, t("LCD brightness")), bright, brightOut),
            appSelect(t("Display scale"), SCALES.map((v) => [v, `${v}%`] as [number, string]), s.scale, (v) => c.updateSettings({ scale: v })),
            appSelect(t("Auto display off"), AUTO_OFF_MINUTES.map((m) => [m, m === 0 ? t("Off") : m < 1 ? t("{n} sec", { n: m * 60 }) : t("{n} min", { n: m })] as [number, string]), s.autoOffMin, (v) => c.updateSettings({ autoOffMin: v })),
          ]
        : [
            appSelect(t("Display scale"), SCALES.map((v) => [v, `${v}%`] as [number, string]), s.scale, (v) => c.updateSettings({ scale: v })),
            panelOnlyRow(t("LCD brightness"), "lcd-brightness"),
            panelOnlyRow(t("Auto display off"), "auto-display-off"),
          ]),
      control(store, "LockMode", { label: t("Lock mode"), part: 0x7f }),
      // 36 [04 7F 7F 7F] = full factory reset now, the 10 recorder songs deleted (the Kawai panel's Factory Reset)
      fullResetBox(remote),
    ),
    // one 「시작 설정」 card (owner 2026-11-15): the OpenGrand part (0x7D op 0x1C, hidden unless
    // the piano reports the feature) and under it the stock startup actions. The stock looks these up by the first data
    // byte: 34 [07 00 7F 7F] = save the settings as the stock
    // startup settings (UserMemory, restored by the stock at power-on), 36 [07 7F 7F 7F] = only the saved startup
    // settings back to factory (from the next power-on). The old 00 00 00 00 matched no command: the piano did nothing.
    startupSection(c, h("div", { id: "startup-stock" },
      h("p", { class: "sub" }, t("Piano's own startup settings")),
      h("div", { class: "transport" },
        h("button", { "data-action": "startup-save", onclick: remote("StartUpSettingSave", [0x07, 0x00, 0x7f, 0x7f]) }, t("Save startup settings")),
        h("button", { "data-action": "startup-factory", onclick: (e: Event) =>
          confirmTap(e.currentTarget, t("Reset the saved startup settings to factory defaults? Takes effect at the next power-on; recorder songs are kept."), remote("StartUpSettingResetStart", [0x07, 0x7f, 0x7f, 0x7f])) }, t("Startup settings to factory"))))),
    h(
      "section",
      { class: "card", id: "user-data" },
      h("h3", {}, t("User data")),
      h("p", { class: "muted" }, t("Stored on this device: {f} favorites, {r} recent, {u} user sounds.", { f: lib.favorites.length, r: lib.recent.length, u: lib.user.length })),
      h("div", { class: "transport" },
        h("button", { class: "danger", "data-reset": "favorites", onclick: reset(t("Clear all favorites?"), () => { lib.clearFavorites(); c.rerender(); }) }, t("Clear favorites")),
        h("button", { class: "danger", "data-reset": "recent", onclick: reset(t("Clear recently played sounds?"), () => { lib.clearRecent(); c.rerender(); }) }, t("Clear recent")),
        h("button", { class: "danger", "data-reset": "user", onclick: reset(t("Delete all user sounds?"), () => { lib.clearUser(); c.rerender(); }) }, t("Clear user sounds")),
      ),
      // §9.6: InternalSongDelete 7F 7F 7F 7F deletes every recorder song on the piano
      h("div", { class: "transport" },
        h("button", { class: "danger", "data-reset": "recorder", onclick: reset(t("Delete all recorder songs on the piano?"), () => {
          recStatus.textContent = t("Deleting…");
          void store.send({ fn: 0x32, v1: 0x52, v2: 0x21, data: [0x7f, 0x7f, 0x7f, 0x7f] });
        }) }, t("Delete all recorder songs")),
      ),
      recStatus,
    ),
  );
}
