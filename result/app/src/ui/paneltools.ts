// Panel-shell field-test tools (S3 install probe; closed no-go): open the USB stick / an address through the shell's openURL, and 「Android 탐색」. Shown inside the
// Info tab's 「개발자용」 card as `#app-dev` (owner 2026-11-16 — it was in
// Settings › 앱 · 화면 › 개발자용). The ids stay for op 0x37 / the screen report (`#usb-open-result` · `#peek-out`, main.ts).
import { t } from "../i18n.ts";
import type { AppContext } from "./context.ts";
import { h } from "./dom.ts";

/** The `#app-dev` block, or null outside the panel shell (no openUsb). */
export function panelToolsBox(app: AppContext["app"]): HTMLElement | null {
  if (!app?.shell || !app.openUsb) return null;
  return h("div", { id: "app-dev" }, h("h4", {}, t("Panel USB tools")), usbOpenRow(app));
}

function usbOpenRow(app: NonNullable<AppContext["app"]>): HTMLElement {
  const out = h("output", { id: "usb-open-result", class: "muted" });
  const run = (rel: string) => async () => { out.textContent = "…"; out.textContent = await app.openUsb!(rel); };
  // free-form tries (owner 2026-11-15): one package, many URIs — {usb} = the stick's volume id
  const URIS = [
    "content://com.android.externalstorage.documents/root/{usb}",
    "content://com.android.externalstorage.documents/document/{usb}%3A",
    "content://com.android.externalstorage.documents/document/{usb}%3Aca79ng-probe.apk",
    "content://media/external/file",
    "file:///storage/{usb}/ca79ng-probe.apk",
    "file:///mnt/media_rw/{usb}/ca79ng-probe.apk",
  ];
  const sel = h("select", { id: "uri-pick" }, ...URIS.map((u) => h("option", { value: u }, u))) as HTMLSelectElement;
  const txt = h("input", { id: "uri-text", type: "text", value: URIS[0], spellcheck: false }) as HTMLInputElement;
  sel.addEventListener("change", () => (txt.value = sel.value));
  const go = h("button", { id: "uri-open", onclick: async () => { out.textContent = "…"; out.textContent = app.openUri ? await app.openUri(txt.value.trim()) : "—"; } }, t("Open this address"));
  return h("div", { class: "list", id: "usb-open" },
    h("button", { class: "item", id: "usb-open-folder", onclick: run("") }, t("Open the USB stick (file manager)")),
    h("button", { class: "item", id: "usb-open-probe", onclick: run("ca79ng-probe.apk") }, t("Open ca79ng-probe.apk on the USB stick")),
    ...(app.openUri ? [sel, txt, go] : []),
    out,
    ...(app.readFile ? [androidPeek(app)] : []));
}

/** 「Android 탐색」 (owner 2026-11-15): read files on the panel's Android through the Kawai shell (text only, no
 *  listing, no root) to find the package installer / Explorer and what they accept. Only paths whose first line is not
 *  empty (the shell trips on an empty first line — see kwmcore.readAnyFile). */
function androidPeek(app: NonNullable<AppContext["app"]>): HTMLElement {
  const PATHS = [
    "/proc/version", "/proc/cpuinfo", "/proc/mounts",
    "/system/priv-app/PackageInstaller/PackageInstaller.apk", "/system/app/PackageInstaller/PackageInstaller.apk",
    "/system/priv-app/DocumentsUI/DocumentsUI.apk", "/system/app/DocumentsUI/DocumentsUI.apk",
    "/system/app/RkExplorer/RkExplorer.apk", "/system/app/Explorer/Explorer.apk", "/system/priv-app/RkExplorer/RkExplorer.apk",
    "/system/etc/permissions/platform.xml", "/data/system/packages.list", "/data/system/packages.xml",
  ];
  const sel = h("select", { id: "peek-pick" }, ...PATHS.map((u) => h("option", { value: u }, u))) as HTMLSelectElement;
  const txt = h("input", { id: "peek-path", type: "text", value: PATHS[0], spellcheck: false }) as HTMLInputElement;
  sel.addEventListener("change", () => (txt.value = sel.value));
  const res = h("pre", { id: "peek-out", class: "muted", style: "white-space:pre-wrap;max-height:40vh;overflow:auto" });
  let last = { path: "", text: "" };
  const read = h("button", { id: "peek-read", onclick: async () => {
    res.textContent = "…";
    const path = txt.value.trim();
    const v = await app.readFile!(path);
    last = { path, text: v ?? "" };
    res.textContent = v === null ? t("Missing or not readable: {p}", { p: path }) : t("{n} characters", { n: v.length }) + "\n" + v.slice(0, 3000);
  } }, t("Read"));
  const save = h("button", { id: "peek-save", onclick: async () => {
    if (!last.path || !app.dumpToUsb) return;
    res.textContent = (await app.dumpToUsb(last.path, last.text)) + "\n" + res.textContent;
  } }, t("Save to the USB stick"));
  return h("div", { class: "list", id: "android-peek" }, h("h4", {}, t("Android explorer")), sel, txt, h("div", { class: "row" }, read, save), res);
}
