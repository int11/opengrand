// Settings > Startup settings (OpenGrand, Identify feat2 0x08 - 0x7D op 0x1C, REMOTE_PROTOCOL.md section 10.8): what
// the piano does at power-on. The piano keeps the mode and the snapshots itself (owner decision 2026-11-01): this
// card only reads and writes the piano's values. The OpenGrand part is hidden on a stock piano and on payloads without the
// feature; the stock startup actions under it (screens/settings.ts) show on every piano.
import { t } from "../i18n.ts";
import { NgClient, ngHas, NgStartup, StartupResult, type StartupMode, type StartupReply, type StartupStatus } from "../protocol/ngext.ts";
import type { AppContext } from "./context.ts";
import { confirmTap } from "./confirm2.ts";
import { h } from "./dom.ts";
import { identifyShared } from "./identify.ts";

const MODE_LABEL: Record<StartupMode, string> = {
  stock: "Piano default (stock)",
  current: "Saved settings",
  poweroff: "Settings at the last power-off",
};

/**
 * The one 「시작 설정」 card (owner 2026-11-15): the OpenGrand part
 * (`#startup-ng`, hidden on a stock piano / a payload without the feature) above `stock` - the stock 「시작 설정 저장」 /
 * 「시작 설정을 공장값으로」 actions, shown on every piano.
 */
export function startupSection(ctx: AppContext, stock?: HTMLElement): HTMLElement {
  const ng = h("div", { id: "startup-ng", hidden: true });
  const el = h("section", { class: "card", id: "startup-settings" }, h("h3", {}, t("Startup settings")), ng, stock ?? null);
  if (!stock) el.hidden = true;   // nothing to show until the OpenGrand part answers
  const s = ctx.session();
  if (!s || s.state !== "ready") return el;
  const su = new NgStartup(new NgClient(s));
  const note = h("p", { class: "muted", id: "startup-status" });

  const rcText = (r: StartupReply | null): string =>
    !r ? t("No answer from the piano.")
    : r.rc === StartupResult.Ok ? t("Saved on the piano.")
    : r.rc === StartupResult.NotBound ? t("The piano is controlled by another device.")
    : r.rc === StartupResult.Busy ? t("The piano is busy, try again in a moment.")
    : r.rc === StartupResult.SaveFailed ? t("The piano could not save it.")
    : t("The piano rejected this value.");

  const render = (st: StartupStatus) => {
    const sel = h("select", { name: "startupMode" },
      ...(["stock", "current", "poweroff"] as StartupMode[]).map((m) => h("option", { value: m, selected: m === st.mode }, t(MODE_LABEL[m])))) as HTMLSelectElement;
    sel.addEventListener("change", async () => {
      note.textContent = t("Saving…");
      const r = await su.setMode(sel.value as StartupMode);
      if (r?.status) render(r.status);
      note.textContent = rcText(r);
    });
    const info = st.mode === "stock"
      ? t("At power-on the piano starts with its own saved sound settings (factory unless saved with “Save startup settings” below); everything else goes back to factory.")
      : st.mode === "current"
        ? (st.hasCurrent ? t("At power-on the piano restores the settings saved with \"Save current settings\".") : t("Nothing saved yet: press \"Save current settings\"."))
        : t("At power-on the piano restores the settings it had when it was switched off (saved 3 s after every change).");
    const last = st.restoredAtBoot && st.applied
      ? t("This power-on: {n} settings restored.", { n: st.applied })
      : "";
    // a mode / snapshot of a firmware before record 14 §7 covers the sound settings only until it is chosen / saved again
    const older = st.allSettings == null ? ""
      : st.mode === "stock" ? (st.allSettings ? "" : t("Chosen with an older firmware: Bluetooth and auto power off settings stay as they are until you choose the mode again."))
      : st.mode === "current" ? (st.hasCurrent && !st.currentHasAll ? t("Saved with an older firmware: Bluetooth and auto power off settings stay as they are until you save again.") : "")
      : st.hasPowerOff && !st.powerOffHasAll ? t("Saved with an older firmware: Bluetooth and auto power off settings follow from the next change.") : "";
    ng.replaceChildren(
      h("label", { class: "row" }, h("span", {}, t("At power-on")), sel),
      h("p", { class: "muted", id: "startup-scope" }, t("Covers every piano setting — sound, Bluetooth devices, auto reconnect, auto power off and lock — except Bluetooth pairing and the Bluetooth module's own settings.")),
      h("p", { class: "muted" }, info, last ? " " + last : ""),
      ...(older ? [h("p", { class: "muted", id: "startup-older" }, older)] : []),
      h("div", { class: "transport" },
        h("button", { "data-action": "startup-save-current", onclick: async () => {
          note.textContent = t("Saving…");
          const r = await su.saveCurrent();
          if (r?.status) render(r.status);
          note.textContent = rcText(r);
        } }, t("Save current settings")),
        h("button", { "data-action": "startup-clear", onclick: (e: Event) => confirmTap(e.currentTarget, t("Forget the saved startup settings? From the next power-on the piano starts with its own sound settings and everything else at factory."), async () => {
          note.textContent = t("Saving…");
          const r = await su.clear();
          if (r?.status) render(r.status);
          note.textContent = rcText(r);
        }) }, t("Forget saved startup settings"))),
      note,
    );
    ng.hidden = false;
    el.hidden = false;
  };

  void (async () => {
    const id = await identifyShared(s);
    if (!ngHas(id, "startup")) return; // stock or a payload without the feature
    const st = await su.status();
    if (st) render(st);
  })();
  return el;
}

/** Strings this card shows (for the translation check: the card needs a feat2 0x08 piano). */
export const STARTUP_STRINGS = [
  ...Object.values(MODE_LABEL), "Startup settings", "At power-on", "Save current settings", "Forget saved startup settings",
  "Forget the saved startup settings? From the next power-on the piano starts with its own sound settings and everything else at factory.", "The piano is busy, try again in a moment.",
  "The piano could not save it.", "At power-on the piano starts with its own saved sound settings (factory unless saved with “Save startup settings” below); everything else goes back to factory.",
  "Covers every piano setting — sound, Bluetooth devices, auto reconnect, auto power off and lock — except Bluetooth pairing and the Bluetooth module's own settings.",
  "Chosen with an older firmware: Bluetooth and auto power off settings stay as they are until you choose the mode again.",
  "Saved with an older firmware: Bluetooth and auto power off settings stay as they are until you save again.",
  "Saved with an older firmware: Bluetooth and auto power off settings follow from the next change.",
  "At power-on the piano restores the settings saved with \"Save current settings\".", "Nothing saved yet: press \"Save current settings\".",
  "At power-on the piano restores the settings it had when it was switched off (saved 3 s after every change).",
  "This power-on: {n} settings restored.",
];
