// Firmware update screen on the built-in panel: the piano was switched on with E+A+B+C and its USB updater
// (BootProgram) answered our CONNECT with Mode DevelopperMode/HostUpdate. That updater answers only the first
// CONNECT: the Kawai MAIN UI would never be answered now.
// main.ts shows this screen ("do not switch off") and hands over at once to the Kawai update page
// (appUI/developper/dpupdatemode) — the page the original UI opens on that reply; it does not need the Mode reply
// (it falls back to HostUpdate after 6 s) and follows the progress frames. This screen is the fallback when the
// page cannot be opened, and draws the same progress frames meanwhile.
import { t } from "../i18n.ts";
import type { UpdaterState } from "../protocol/updatemode.ts";
import { h } from "./dom.ts";

/** The stock Kawai update page, from our ng/index.html. */
export const KAWAI_UPDATE_PAGE = "../developper/dpupdatemode/html/index.html";

export interface UpdateScreen {
  el: HTMLElement;
  render(s: UpdaterState): void;
}

const hex = (n: number) => "0x" + n.toString(16).toUpperCase().padStart(6, "0");

export function updateScreen(opt: { kawai: () => void; dismiss: () => void }): UpdateScreen {
  const title = h("h2", {});
  const warn = h("p", { class: "upd-warn", role: "alert" });
  const bar = h("div", { class: "upd-bar" }, h("div", { class: "upd-fill" }));
  const pct = h("p", { class: "upd-pct" });
  const files = h("ul", { class: "upd-files" });
  const note = h("p", { class: "muted" });
  const kawai = h("button", { onclick: () => opt.kawai() }) as HTMLButtonElement;
  const dismiss = h("button", { onclick: () => opt.dismiss() }) as HTMLButtonElement;
  const el = h("div", { id: "update-mode", class: "update-mode", hidden: true, role: "dialog", "aria-modal": "true" },
    title, warn, bar, pct, files, note, h("div", { class: "upd-btns" }, kawai, dismiss));
  const render = (s: UpdaterState) => {
    el.hidden = !s.active;
    if (!s.active) return;
    el.dataset.phase = s.phase;
    title.textContent = t("Firmware update (USB)");
    const busy = s.phase === "writing";
    warn.textContent = busy
      ? t("A firmware update may be in progress — do not switch the piano off.")
      : s.phase === "done"
        ? t("Update finished (Boot End). Hold the power switch for 5 s to switch off, take out the USB memory, then switch on again.")
        : s.phase === "nousb"
          ? t("No USB memory found. Switch the piano off, put the USB memory in and switch on with E+A+B+C again.")
          : s.phase === "nofile"
            ? t("No usable update file on the USB memory (exactly one CAx9_P*.SYS). Nothing was written. Switch the piano off.")
            : t("A file failed to write (Write error). Do not switch off before the end; then run the USB update again.");
    warn.className = busy ? "upd-warn" : s.phase === "done" ? "upd-warn ok" : "upd-warn bad";
    const p = s.progress;
    bar.hidden = p === null;
    (bar.firstChild as HTMLElement).style.width = `${Math.round((p ?? 0) * 100)}%`;
    pct.textContent = p === null ? (busy ? t("Writing…") : "") : `${Math.round(p * 100)} %`;
    files.textContent = "";
    for (const f of s.files) {
      const r = f.result === "ok" ? t("OK") : f.result === "error" ? t("Write error") : t("Writing…");
      files.append(h("li", {}, `${f.name || "?"} · ${f.offset === null ? "—" : hex(f.offset)} · ${r}`));
    }
    note.textContent = s.via === "progress"
      ? t("Recognised from the update's progress messages (the piano's first answer went to an earlier page).")
      : t("The piano answered as its USB updater (E+A+B+C at power on). The OpenGrand UI starts again at the next normal power on.");
    kawai.textContent = t("Kawai update screen");
    dismiss.textContent = t("Not an update — back to the OpenGrand UI");
    dismiss.hidden = busy && s.files.length > 0;
  };
  return { el, render };
}
