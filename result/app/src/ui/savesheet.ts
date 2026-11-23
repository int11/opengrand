// 「USB로 저장」 - the one sheet that saves a piano song to the USB memory, with the one format list (owner
// 2026-11-23: one concept, one list everywhere): MID · KSO · MP3 · WAV. It opens from a piano song's ⋯ menu and,
// with 「저장됨: 피아노 곡 N」 on top, when a recording stops (ui/recorder.ts).
//   MID / KSO   the piano writes the song as a file at once (24 0A <song> <type>, SONG_nn.MID / .KSO)
//   MP3 / WAV   Convert to Audio (ui/audiorec.ts): the piano plays the song once into its audio recorder - it takes
//               as long as the song, so the sheet shows the time recorded so far and a Cancel
// The formats offered come from the capability table (ui/songkinds.ts).
import { FILE_TYPE, FileAccessError, resultText } from "../protocol/fileaccess.ts";
import { findParam } from "../protocol/codecs.ts";
import { t, tl } from "../i18n.ts";
import { AUDIOREC_TEXT, convertToAudio, freeName, type ConvertJob } from "./audiorec.ts";
import type { AppContext } from "./context.ts";
import { h } from "./dom.ts";
import { clock } from "./screens/songctl.ts";
import type { SaveFormat } from "./songkinds.ts";
import { usbListState } from "./usblist.ts";

/** Strings shown only in a state a screen test does not reach (for the translation check). */
export const SAVE_SHEET_STRINGS = [
  ...Object.values(AUDIOREC_TEXT), "Saved: piano song {n}", "Also save to USB", "Save to USB", "Song {n}", "Saved to USB: {name}",
  "Making {fmt}… {time}", "Cancelled. Nothing was saved.", "Put a USB memory into the piano to save a file.", "Cancel", "Close",
  "MID and KSO are written at once. MP3 and WAV: the piano plays the song once to make the sound file, so it takes as long as the song.",
  "Stopping…", "Save as SMF", "Save as KSO", "{op}…", "{op} failed: {e}",
];

export interface SaveSheet {
  el: HTMLDialogElement;
  /** song 0-based; recorded: a recording just ended in that song (「저장됨: 피아노 곡 N」 · 「USB에도 저장」) */
  open(song: number, recorded?: boolean): void;
  close(): void;
}

export interface SaveSheetDeps {
  /** one file operation at a time (shared with the lists) */
  lock: { busy: boolean };
  formats: readonly SaveFormat[];
  /** stop what plays before the piano plays the song into a file */
  quiet(): Promise<void>;
  /** the USB list is read again after a save */
  refresh(): Promise<void>;
  /** a line outside the sheet that also gets the result (it stays after the sheet closes) */
  out?: HTMLElement;
}

export function saveSheet(ctx: AppContext, id: string, deps: SaveSheetDeps): SaveSheet {
  let song = 0;
  let job: ConvertJob | null = null;
  let tick: ReturnType<typeof setInterval> | undefined;
  const title = h("h3", {});
  const sub = h("p", { class: "sub", id: `${id}-sub` });
  const status = h("p", { class: "muted", id: `${id}-status`, role: "status" });
  const say = (s: string) => {
    status.textContent = s;
    if (deps.out) deps.out.textContent = s;
  };
  const noUsb = () => usbListState(ctx).noMedia || ctx.store.get(findParam("USBMemory"), 0x7f)?.[0] === 0;
  const chips = deps.formats.map((f) => h("button", { type: "button", class: "chip", "data-format": f, onclick: () => void save(f) }, f));
  const cancel = h("button", { type: "button", "data-act": "save-cancel", hidden: true, onclick: () => {
    if (!job) return;
    status.textContent = t("Stopping…");
    job.cancel();
  } }, t("Cancel"));
  const closeBtn = h("button", { type: "button", "data-act": "save-close", onclick: () => close() }, t("Close"));
  const el = h("dialog", { class: "sheet save-sheet", id },
    title, sub,
    h("div", { class: "chips" }, ...chips),
    h("p", { class: "muted save-hint" }, t("MID and KSO are written at once. MP3 and WAV: the piano plays the song once to make the sound file, so it takes as long as the song.")),
    status, cancel, closeBtn) as HTMLDialogElement;
  const sync = () => {
    const off = noUsb() || !!job || deps.lock.busy;
    chips.forEach((c) => ((c as HTMLButtonElement).disabled = off));
    cancel.hidden = !job;
    closeBtn.disabled = !!job; // a convert runs: Cancel first
  };
  const close = () => {
    if (job) return;
    if (typeof el.close === "function" && el.open) el.close();
    else el.removeAttribute("open");
  };
  // Esc / the backdrop: not while the piano records
  el.addEventListener("cancel", (e) => {
    if (job) e.preventDefault();
  });
  el.addEventListener("click", (e) => {
    if (e.target === el) close();
  });

  const fail = (what: string, e: unknown) => {
    const msg = e instanceof FileAccessError ? tl(resultText(e.code)) : t((e as Error).message);
    say(t("{op} failed: {e}", { op: what, e: msg }));
  };
  const save = async (fmt: SaveFormat) => {
    if (job || deps.lock.busy) return;
    const fa = ctx.files();
    if (!fa) return say(t("Connect to the piano first."));
    const n = song;
    const base = `SONG_${String(n + 1).padStart(2, "0")}`;
    const audio = fmt === "MP3" || fmt === "WAV";
    if (audio) {
      // before the lock: stopping a USB playback is a file operation of its own
      job = { done: Promise.resolve("cancelled"), cancel() {} };
      sync();
      await deps.quiet();
      job = null;
    }
    deps.lock.busy = true;
    try {
      if (fmt === "MID" || fmt === "KSO") {
        const what = fmt === "MID" ? t("Save as SMF") : t("Save as KSO");
        const name = `${base}.${fmt}`;
        say(t("{op}…", { op: what }));
        sync();
        try {
          await fa.saveSong(n, name, FILE_TYPE[fmt]);
          say(t("Saved to USB: {name}", { name }));
        } catch (e) {
          return fail(what, e);
        }
      } else {
        const name = freeName(ctx.usbFiles, base, fmt);
        const from = Date.now();
        let ms: number | null = null;
        const show = () => (status.textContent = t("Making {fmt}… {time}", { fmt, time: clock(ms ?? Date.now() - from) }));
        job = convertToAudio(ctx, { song: n, format: fmt, name, onProgress: (v) => { ms = v; show(); } });
        show();
        sync();
        tick = setInterval(show, 500);
        try {
          const r = await job.done;
          say(r === "saved" ? t("Saved to USB: {name}", { name }) : t("Cancelled. Nothing was saved."));
          if (r !== "saved") return;
        } catch (e) {
          return fail(fmt, e);
        } finally {
          clearInterval(tick);
          job = null;
        }
      }
    } finally {
      deps.lock.busy = false;
      sync();
    }
    await deps.refresh();
    sync();
  };

  return {
    el, close,
    open(n, recorded = false) {
      if (job) return;
      song = n;
      title.textContent = recorded ? t("Saved: piano song {n}", { n: n + 1 }) : t("Song {n}", { n: n + 1 });
      sub.textContent = recorded ? t("Also save to USB") : t("Save to USB");
      status.textContent = noUsb() ? t("Put a USB memory into the piano to save a file.") : "";
      sync();
      if (el.open) return;
      if (typeof el.showModal === "function") el.showModal();
      else el.setAttribute("open", "");
    },
  };
}
