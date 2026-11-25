// Online self-update UI (features/selfupdate.ts): the Info screen block (state, on/off, 「지금 확인」, install / restart)
// and the small banner under the header once a new version is ready. Both draw only the hub's state.
import { type SelfUpdateHub, type SelfUpdateState } from "../features/selfupdate.ts";
import { t } from "../i18n.ts";
import { h } from "./dom.ts";

const ERR: Record<string, string> = {
  offline: "No internet connection. The app checks again later.",
  ratelimit: "GitHub is busy (request limit). The app checks again later.",
  norelease: "No release has been published yet.",
  sha256: "Update refused: the download does not match its checksum.",
  size: "Update refused: the download does not match its checksum.",
  apk: "Update refused: the download is not this app.",
};
const MANUAL: Record<string, string> = {
  portable: "The portable build cannot replace itself. Download the new version from the release page.",
  dev: "Development run: updates are not installed.",
  readonly: "This app's folder cannot be written. Download the new version from the release page.",
};
const BTN: Record<string, string> = { install: "Install", restart: "Restart to update", manual: "Open the release page" };

/** Every string this file can show (translation test). */
export const SELF_UPDATE_STRINGS: string[] = [
  ...Object.values(ERR), ...Object.values(MANUAL), ...Object.values(BTN),
  "Automatic updates", "Check for new versions automatically", "Check now", "Not checked yet.", "Checking for updates…",
  "Up to date ({v}).", "Downloading {v}… {p}%", "Version {v} is ready to install.", "Version {v} is ready. Restart the app to update.",
  "Version {v} is available.", "Opening the installer…", "Update check failed: {e}", "Updated to {v}.",
  "The last update could not be applied ({e}). The previous version is still installed.", "OpenGrand {v} is ready.", "Later",
  "Last checked: {t}", "Downloads new versions of this app from GitHub; you confirm the install.",
];

export function selfUpdateText(s: SelfUpdateState | null): string {
  if (!s) return t("Not checked yet.");
  const v = s.available ?? "?";
  switch (s.phase) {
    case "idle": return t("Not checked yet.");
    case "checking": return t("Checking for updates…");
    case "uptodate": return t("Up to date ({v}).", { v: s.current ?? "?" });
    case "downloading": return t("Downloading {v}… {p}%", { v, p: s.progress ?? 0 });
    case "installing": return t("Opening the installer…");
    case "ready":
      if (s.action === "manual") return [t("Version {v} is available.", { v }), s.reason && MANUAL[s.reason] ? t(MANUAL[s.reason]) : ""].filter(Boolean).join(" ");
      return s.action === "restart" ? t("Version {v} is ready. Restart the app to update.", { v }) : t("Version {v} is ready to install.", { v });
    case "error": return ERR[s.error ?? ""] ? t(ERR[s.error!]) : t("Update check failed: {e}", { e: s.message || s.error || "?" });
  }
}

function lastResultText(s: SelfUpdateState | null): string {
  const r = s?.lastResult;
  if (!r) return "";
  return r.state === "ok" ? t("Updated to {v}.", { v: s?.current ?? "?" }) : t("The last update could not be applied ({e}). The previous version is still installed.", { e: r.error || "?" });
}

function applyButton(hub: SelfUpdateHub, s: SelfUpdateState | null, id: string): HTMLButtonElement | null {
  if (s?.phase !== "ready" || !s.action) return null;
  const b = h("button", { class: "primary", id }, t(BTN[s.action])) as HTMLButtonElement;
  b.onclick = () => {
    if (s.action === "manual") hub.updater.openPage();
    else {
      b.disabled = true;
      void hub.apply().then((ok) => (b.disabled = ok && s.action === "restart"));
    }
  };
  return b;
}

/** Info screen block. */
export function selfUpdateBox(hub: SelfUpdateHub): HTMLElement {
  const status = h("p", { class: "muted", id: "self-update-status" });
  const result = h("p", { class: "muted", id: "self-update-result" });
  const when = h("p", { class: "muted", id: "self-update-when" });
  const actions = h("div", { class: "inline" });
  const auto = h("input", { type: "checkbox", id: "self-update-auto", checked: hub.prefs().enabled }) as HTMLInputElement;
  auto.onchange = () => hub.setEnabled(auto.checked);
  const checkBtn = h("button", { id: "self-update-check" }, t("Check now")) as HTMLButtonElement;
  checkBtn.onclick = () => void hub.checkNow();
  const el = h("div", { id: "self-update" },
    h("p", { class: "muted" }, t("Downloads new versions of this app from GitHub; you confirm the install.")),
    h("label", { class: "row" }, auto, h("span", {}, t("Check for new versions automatically"))),
    status, result, when, actions);
  const draw = (s: SelfUpdateState | null) => {
    if (drawn && !el.isConnected) return void off(); // the Info screen was replaced: stop drawing this copy
    drawn = true;
    status.textContent = selfUpdateText(s);
    const r = lastResultText(s);
    result.textContent = r;
    result.hidden = !r;
    when.textContent = s?.checkedAt ? t("Last checked: {t}", { t: new Date(s.checkedAt).toLocaleString() }) : "";
    when.hidden = !s?.checkedAt;
    checkBtn.disabled = s?.phase === "checking" || s?.phase === "downloading";
    actions.replaceChildren(checkBtn, ...[applyButton(hub, s, "self-update-apply")].filter((x): x is HTMLButtonElement => !!x));
  };
  let drawn = false;
  const off = hub.onChange(draw);
  draw(hub.state);
  return el;
}

/** Banner under the header while a new version is ready (closed with 「나중에」 until the next version). */
export function selfUpdateBanner(hub: SelfUpdateHub): HTMLElement {
  const msg = h("span", {});
  const later = h("button", { id: "self-update-later" }, t("Later")) as HTMLButtonElement;
  const slot = h("span", { class: "inline" });
  const el = h("div", { class: "banner", id: "self-update-banner", role: "status", hidden: true }, msg, slot);
  let dismissed = "";
  later.onclick = () => {
    dismissed = hub.state?.available ?? "";
    el.hidden = true;
  };
  const draw = (s: SelfUpdateState | null) => {
    const show = s?.phase === "ready" && !!s.available && s.available !== dismissed;
    el.hidden = !show;
    if (!show) return;
    msg.textContent = t("OpenGrand {v} is ready.", { v: s!.available! });
    later.textContent = t("Later");
    slot.replaceChildren(...[applyButton(hub, s, "self-update-banner-apply"), later].filter((x): x is HTMLButtonElement => !!x));
  };
  hub.onChange(draw);
  draw(hub.state);
  return el;
}
