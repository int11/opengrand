// USB memory file manager - the 「USB 메모리」 list of the Recorder tab (ui/recorder.ts; features 43, 45, 68 on the
// app side; REMOTE_PROTOCOL.md §9.5): list / play / import / upload / delete / rename / format.
// Delete and rename go by name (24 06 / 24 07), which the stock firmware and OpenGrand both
// answer; the by-id remote parameters 32 51 3B / 3A get no reply from the stock
// On the stock the list holds paths
// relative to the root ("DIR\SONG.MID"), so a rename keeps the file in its directory.
// Play and import follow what the stock V2.19 accepts: play = MusicMode 3 (USB memory) + 24 08 select + Music Play (mode 6 is the
// built-in Piano Music library and ignores Play); import = 24 09 <song> <type> (stock: KSO only). File-list ids are
// never used here.
// The list is shown as soon as the tab opens, from the shared cache (ui/usblist.ts, asked again only when old, empty,
// from another session, or after a stick was inserted; an empty answer right after a plug / connect is asked again
// after 1.5 s and 3 s).
// Play / stop (「USB 재생 정지」 and 「USB 재생 겹침 · 정지」,
// ui/usbplay.ts): the stock's SMF and audio players run independently and Music Reset reaches only the player of
// the type the last 24 08 selected, so a stop selects a file of each type and resets it; ▶ never starts a USB file
// while another USB playback runs (it stops that first). The playing row is marked from the app's own ▶ and the
// piano's word (32 52 1D, 32 52 08, MusicProgress 52 03); the pane's ■ always stops both players.
// Rows (owner 2026-11-23, the player bar): a tap on a row chooses the file - it is what the Recorder tab's player bar
// shows and its ▶ plays (nothing is sent by the choice itself; while a USB file plays, the tapped one plays instead);
// ⋯ opens that file's other buttons (import, rename, delete) under it. The rarely used things - where an import goes,
// sending a file from this device, format - are in the folded 「USB 관리」.
// The play / stop line says only what went wrong: that a file plays is the player bar's to show (field 2026-11-23:
// 「재생: 완료」 stayed under the list while the file was still playing).
import { FileAccessError, resultText } from "../protocol/fileaccess.ts";
import type { PianoSession } from "../protocol/session.ts";
import { SONGS, type SongInfo } from "../protocol/songs.ts";
import type { RecorderSong } from "../protocol/ngext.ts";
import { findParam } from "../protocol/codecs.ts";
import { t, tl } from "../i18n.ts";
import type { AppContext } from "./context.ts";
import { confirmTap } from "./confirm2.ts";
import { h } from "./dom.ts";
import { loadUsbList, usbListGone, usbListStale, usbListState } from "./usblist.ts";
import { iconButton, playingMark } from "./screens/songctl.ts";
import { usbAnyRunning, usbPlay, usbPlayback, usbPlayingName, usbStopAll } from "./usbplay.ts";

/** Last internal song table this app saw, per context: drawn at once when the tab opens again (same session). */
interface SongCache {
  session: PianoSession | null;
  songs: (SongInfo | null)[];
}
const songCaches = new WeakMap<AppContext, SongCache>();
export function songCache(ctx: AppContext): SongCache {
  let c = songCaches.get(ctx);
  const s = ctx.session();
  if (!c || c.session !== s) songCaches.set(ctx, (c = { session: s, songs: Array.from({ length: SONGS }, () => null) }));
  return c;
}

export { MUSIC } from "./usbplay.ts";

/** op 1E 01 entry -> the row model the stock scan fills (n 0-based). */
export function songInfoOf(g: RecorderSong, n: number): SongInfo {
  return { n, hasData: !g.empty, bars: g.bars, parts: g.empty ? null : [g.part1, g.part2] };
}

/** One operation at a time over the file pane and the song list (they share the piano's file channel). */
export interface FileLock { busy: boolean }

export interface UsbPane {
  el: HTMLElement;
  /** Run a file operation and show "…" / "done" / "failed" in `out` (default: the pane's own line); refresh: read the
   *  list again afterwards. */
  run(what: string, op: () => Promise<unknown>, refresh?: boolean, out?: HTMLElement): Promise<boolean>;
  /** Stop both USB players (ui/usbplay.ts usbStopAll). */
  stop(): Promise<void>;
  /** The internal song an import goes to (0-based). */
  setImportTarget(n: number): void;
  /** The file chosen in the list (what the player bar's ▶ plays); null = none. */
  selected(): string | null;
  /** Choose a file as a tap on its row does. */
  pick(name: string): void;
  /** Play a file (ui/usbplay.ts usbPlay: whatever USB playback runs is stopped first). */
  play(name: string): Promise<void>;
}

export interface UsbPaneOptions {
  songHasData?: (n: number) => boolean;
  /** a file was chosen in the list */
  onPick?: (name: string) => void;
}

/** The last path part and its extension: "DIR\SONG.MID" -> ["SONG", "MID"]. */
export function fileParts(name: string): { base: string; ext: string } {
  const leaf = name.slice(name.lastIndexOf("\\") + 1);
  const dot = leaf.lastIndexOf(".");
  return dot > 0 ? { base: leaf.slice(0, dot), ext: leaf.slice(dot + 1).toUpperCase() } : { base: leaf, ext: "" };
}

/** The file last chosen, per context (the tab is rebuilt on every visit and language change). */
const chosenFile = new WeakMap<AppContext, string>();

export function usbPane(ctx: AppContext, lock: FileLock, opts: UsbPaneOptions = {}): UsbPane {
  const songHasData = opts.songHasData ?? (() => false);
  const store = ctx.store;
  const status = h("p", { class: "muted", id: "usb-status" });
  const result = h("p", { class: "muted pane-result", id: "usb-result", role: "status" }, "");
  const list = h("div", { class: "list", id: "usb-files" });
  const usb = usbListState(ctx);
  const attached = () => store.get(findParam("USBMemory"), 0x7f)?.[0];
  const showStatus = () => {
    const a = attached();
    status.textContent = a === 1 ? t("USB memory attached") : a === 0 || usb.noMedia ? t("No USB memory") : t("USB memory state unknown");
  };

  const run = async (what: string, op: () => Promise<unknown>, refresh = true, out: HTMLElement = result): Promise<boolean> => {
    const fa = ctx.files();
    if (!fa) {
      out.textContent = t("Connect to the piano first.");
      return false;
    }
    lock.busy = true;
    out.textContent = t("{op}…", { op: what });
    try {
      await op();
      out.textContent = t("{op}: done", { op: what });
      if (refresh) await load(false, true); // the op worked, so the stick is mounted: an empty list is final
      return true;
    } catch (e) {
      const msg = e instanceof FileAccessError ? tl(resultText(e.code)) : (e as Error).message;
      out.textContent = t("{op} failed: {e}", { op: what, e: msg });
      return false;
    } finally {
      lock.busy = false;
    }
  };

  const songSel = h("select", { id: "usb-import-song" }, ...Array.from({ length: SONGS }, (_, i) => h("option", { value: String(i) }, t("Song {n}", { n: i + 1 })))) as HTMLSelectElement;
  const target = () => Number(songSel.value);
  // the file whose other buttons are shown (one at a time), and the file chosen for the player bar
  let open: string | null = null;
  let chosen: string | null = chosenFile.get(ctx) ?? null;
  const importFile = (name: string, btn: EventTarget | null) => {
    const n = target();
    const go = () => void run(t("Import"), () => ctx.files()!.load(name, n), false);
    if (songHasData(n)) confirmTap(btn, t("Song {n} already has a recording. Importing replaces it.", { n: n + 1 }), go);
    else go();
  };
  const render = () => {
    const files = ctx.usbFiles;
    shownKey = playKey();
    if (!files.length) {
      list.replaceChildren(h("p", { class: "muted" }, usb.noMedia ? t("No USB memory") : t("No files.")));
      return;
    }
    if (open !== null && !files.includes(open)) open = null;
    if (chosen !== null && !files.includes(chosen)) chosen = null;
    list.replaceChildren(
      ...files.map((name) => {
        const isOpen = open === name;
        const on = chosen === name;
        const plays = usbPlayingName(playing, name);
        const { ext } = fileParts(name);
        return h(
          "div",
          { class: "file-row", "data-file": name, "data-on": on ? "1" : undefined, "data-open": isOpen ? "1" : undefined, "data-playing": plays ? "1" : undefined },
          h("div", { class: "file-main" },
            h("button", { class: "file-pick", "data-act": "file-pick", "aria-pressed": String(on), title: name, onclick: () => pick(name) },
              h("span", { class: "file-ext" }, plays ? playingMark() : ext || "–"),
              // the type is the badge; the name keeps its directory ("DIR\SONG")
              h("span", { class: "file-name" }, ext ? name.slice(0, name.length - ext.length - 1) : name)),
            iconButton("more", t("More"), { class: "row-more", "data-act": "file-more", "aria-expanded": String(isOpen), onclick: () => { open = isOpen ? null : name; render(); } })),
          h("div", { class: "row-acts", hidden: !isOpen },
            h("button", { "data-act": "import", onclick: (e: Event) => importFile(name, e.currentTarget) }, `${t("Import")} → ${t("Song {n}", { n: target() + 1 })}`),
            h("button", { "data-act": "rename", onclick: () => void rename(name) }, t("Rename")),
            h("button", { class: "danger", "data-act": "delete", onclick: (e: Event) => del(name, e.currentTarget) }, t("Delete"))),
        );
      }),
    );
  };
  songSel.addEventListener("change", render);

  // report: show "reading…" and then the count (tab open, ↻, stick inserted); false after a file operation,
  // whose own "done" line stays. A request already running is shared (loadUsbList).
  const load = async (report = true, trustEmpty = false) => {
    if (!ctx.files()) return;
    if (report) result.textContent = t("{op}…", { op: t("Read the USB memory") });
    const ok = await loadUsbList(ctx, trustEmpty);
    if (!ok) result.textContent = usb.error ?? t("Connect to the piano first.");
    else if (usb.noMedia) result.textContent = t("No USB memory");
    else if (report) result.textContent = t("{n} files", { n: ctx.usbFiles.length });
    showStatus();
    render();
  };
  /** Tab open / reconnect: the cached list at once; ask the piano only when the cache is old or from another
   *  session, the stick is not known to be out, and the page is visible (else when it becomes visible). */
  const autoLoad = () => {
    if (!ctx.files()) return;
    if (attached() === 0) {
      usbListGone(ctx);
      showStatus();
      render();
      return;
    }
    if (!usbListStale(ctx)) return;
    if (typeof document !== "undefined" && document.visibilityState === "hidden") {
      const onVis = () => {
        if (document.visibilityState === "hidden") return;
        document.removeEventListener("visibilitychange", onVis);
        if (list.isConnected) autoLoad();
      };
      document.addEventListener("visibilitychange", onVis);
      return;
    }
    void load();
  };

  // play: USB memory mode (MusicMode 3), select the file into the USB player, then Play (ui/usbplay.ts) - after
  // stopping whatever USB playback runs; refused (with the panel way to stop) when it does not stop.
  const playing = usbPlayback(ctx);
  // the rows' playing marks as last drawn (onPlay below redraws only when one changes)
  const playKey = () => ctx.usbFiles.map((n) => (usbPlayingName(playing, n) ? "■" : "▶") + n).join("\n");
  let shownKey = "";
  const stillText = () => t("The piano still plays from the USB memory. Stop it on the piano: on the panel's USB memory screen choose the playing file and press Stop; if it still plays, switch the piano off and on.");
  const play = async (name: string) => {
    if (lock.busy || playing.stopping) return;
    let refused: string | null = null;
    const ok = await run(t("Play"), async () => {
      const r = await usbPlay(ctx, name, () => (result.textContent = t("Stopping the USB playback that runs first…")));
      if (!r.ok && r.why === "not-stopped") refused = t("{name} was not started: the USB playback that runs did not stop.", { name }) + " " + stillText();
      else if (!r.ok && r.why === "offline") throw new Error(t("Connect to the piano first."));
    }, false);
    // started: the player bar and the row's mark say so - no "done" line that would outlast the playback
    if (refused) result.textContent = refused;
    else if (ok) result.textContent = "";
    render();
  };
  // a tap on a row: the file the player bar shows; while a USB file plays, the tapped one plays instead
  const pick = (name: string) => {
    chosen = name;
    chosenFile.set(ctx, name);
    render();
    opts.onPick?.(name);
    if (usbAnyRunning(playing) && !usbPlayingName(playing, name)) void play(name);
  };
  // stop: both USB players (the selected type's, the audio one, the SMF one), then the piano must stay quiet
  const stop = async () => {
    if (lock.busy || playing.stopping) return;
    if (!ctx.files()) {
      result.textContent = t("Connect to the piano first.");
      return;
    }
    lock.busy = true;
    result.textContent = t("{op}…", { op: t("Stop") });
    try {
      const r = await usbStopAll(ctx);
      result.textContent = r === "stopped" ? "" : r === "still" ? stillText() : t("Connect to the piano first.");
    } finally {
      lock.busy = false;
    }
    render();
  };

  const rename = (name: string) => {
    const to = typeof prompt === "function" ? prompt(t("New name for {name}", { name }), name) : null;
    if (!to || to === name) return;
    const dir = name.includes("\\") ? name.slice(0, name.lastIndexOf("\\") + 1) : "";
    void run(t("Rename"), () => ctx.files()!.rename(name, to.includes("\\") ? to : dir + to));
  };
  const del = (name: string, btn: EventTarget | null) => {
    confirmTap(btn, t("Delete {name}?", { name }), () => void run(t("Delete"), () => ctx.files()!.delete(name)));
  };

  const upload = h("input", { type: "file", accept: ".mid,.MID,.smf,audio/midi" }) as HTMLInputElement;
  const readUpload = async (): Promise<{ name: string; data: Uint8Array } | null> => {
    const f = upload.files?.[0];
    if (!f) {
      result.textContent = t("Choose a MIDI file first.");
      return null;
    }
    return { name: f.name.replace(/[\\/]/g, "_"), data: new Uint8Array(await f.arrayBuffer()) };
  };

  // The piano's word on the USB players (ui/usbplay.ts) redraws the rows while this tab is shown - only when a row's
  // ▶/■ changes: the bar notices (52 03) come about twice a second while a song plays, and redrawing on each one
  // replaced the ■ under the finger (field 2026-11-15, F13a: the stop button kept detaching, a tap could be lost).
  const onPlay = () => {
    if (!list.isConnected) {
      if (seenPlay) playing.listeners.delete(onPlay);
      return;
    }
    seenPlay = true;
    if (playKey() === shownKey) return;
    render();
  };
  let seenPlay = false;
  playing.listeners.add(onPlay);
  // The piano's USBMemory notice (51 2E): a stick inserted -> ask (as the stock panel does, kawaipiano.js
  // onUsbStatus -> FileListRequest), removed -> empty. Only a change counts: the connect sync repeats the
  // value, and a reconnect is handled by the new screen (another session -> stale cache).
  let lastAttach = attached();
  let seenUsb = false;
  const offUsb = store.subscribe((k) => {
    if (k !== "USBMemory@127") return;
    if (status.isConnected) seenUsb = true;
    else {
      if (seenUsb) offUsb();
      return;
    }
    const a = attached();
    showStatus();
    if (a === lastAttach) return;
    lastAttach = a;
    if (a === 1 && ctx.session()?.state !== "syncing") void load();
    if (a === 0) {
      usbListGone(ctx);
      showStatus();
      render();
    }
  });
  showStatus();
  render();
  autoLoad();

  const el = h(
    "div",
    { class: "pane", id: "usb-pane" },
    h("div", { class: "pane-head" },
      status,
      h("div", { class: "pane-btns" },
        h("button", { onclick: () => void load(), "data-act": "refresh" }, `↻ ${t("Refresh")}`), // always asks
        // always there: playback the app did not start (panel, piano buttons) or whose notice never came
        h("button", { "data-act": "usb-stop", onclick: () => void stop() }, `■ ${t("Stop")}`))),
    result,
    list,
    h("details", { class: "fold", id: "usb-manage" },
      h("summary", {}, h("span", { class: "fold-title" }, t("Manage the USB memory"), h("span", { class: "fold-note" }, t("Import target · send a file · format")))),
      h("div", { class: "fold-body" },
        // saving an internal song to USB is in the song's own row (the other list); this picks where 「가져오기」 goes
        h("label", { class: "row" }, h("span", {}, t("Import into")), songSel),
        h("p", { class: "sub" }, t("Upload from this device")),
        upload,
        h("div", { class: "transport" },
          h("button", { "data-act": "upload", onclick: async () => { const u = await readUpload(); if (u) void run(t("Upload"), () => ctx.files()!.write(u.name, u.data)); } }, t("Copy to USB memory")),
          h("button", { "data-act": "play-data", onclick: async () => { const u = await readUpload(); if (u) void run(t("Play"), () => ctx.files()!.playData(u.data), false); } }, t("Play without saving"))),
        h("p", { class: "sub" }, t("Format")),
        h("div", { class: "transport" },
          h("button", { class: "danger", "data-act": "format", onclick: (e: Event) => {
            confirmTap(e.currentTarget, t("Format the USB memory? All files will be erased."), () => void run(t("Format"), () => ctx.files()!.format()));
          } }, t("Format the USB memory"))))),
  );
  return {
    el, run, stop, pick, play,
    selected: () => (chosen !== null && ctx.usbFiles.includes(chosen) ? chosen : null),
    setImportTarget(n) {
      if (n < 0 || n >= SONGS || target() === n) return;
      songSel.value = String(n);
      render();
    },
  };
}
