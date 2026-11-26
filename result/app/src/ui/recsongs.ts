// Recorder tab (ui/recorder.ts): the piano's ten songs as rows - number and what the song holds, a tap chooses (and
// plays), ⋯ opens 「USB로 저장」 and delete - with the list's status line, the Scan button and the table warning.
import { t } from "../i18n.ts";
import { InternalSongs, SONGS } from "../protocol/songs.ts";
import { confirmTap } from "./confirm2.ts";
import type { AppContext } from "./context.ts";
import { h } from "./dom.ts";
import type { FileLock } from "./files.ts";
import type { PianoModel } from "./recmodel.ts";
import { iconButton, playingMark } from "./screens/songctl.ts";
import { KINDS } from "./songkinds.ts";
import { usbAnyRunning, type UsbPlayback } from "./usbplay.ts";

/** What the song rows ask of the tab. */
export interface SongListDeps {
  /** a tap on a song row */
  tap(n: number): void;
  /** 「USB로 저장」 for song n */
  save(n: number): void;
  /** true = refused (the reason is under the bar) */
  refuseTable(): boolean;
  /** OpenGrand: read the song table (op 1E 01); false = no answer */
  readNg(): Promise<boolean>;
  /** what the stock scan puts back on the piano afterwards */
  restoreOf(): { song: number; part: number | undefined };
  /** the song whose tap is being carried out (ui/transport.ts target): its row shows a spinner; -1 none */
  loading?(): number;
}

export interface SongList {
  pane: HTMLElement;
  /** the list's status line */
  status: HTMLElement;
  /** 「n of 10 songs recorded」, or why the song table is not known */
  recordedText(): string;
  /** draw the rows that changed */
  render(): void;
}

export function songList(ctx: AppContext, lock: FileLock, m: PianoModel, pb: UsbPlayback, d: SongListDeps): SongList {
  const songs = m.songs;
  const recordedText = () => m.tableReason() ?? t("{n} of 10 songs recorded", { n: songs.filter((i) => i?.hasData).length });
  const songsStatus = h("p", { class: "muted", id: "songs-status", role: "status" }, songs.some((i) => i) ? recordedText() : t("Not scanned yet."));
  const songsHint = h("p", { class: "muted", id: "songs-hint", hidden: true }, t("The piano's recorder memory (10 songs). Scanning selects each song on the piano; song names are not sent by the piano and cannot be changed."));
  const list = h("div", { class: "list", id: "songs-list" });
  // the song table failed its check (or is not checked yet): one line over the rows, which then say 「Unknown」
  const tableWarn = h("p", { class: "songs-warn", id: "songs-table-warn", role: "alert", hidden: true });
  const songRun = async (what: string, run: (c: InternalSongs) => Promise<unknown>): Promise<boolean> => {
    const s = ctx.session();
    if (!s) {
      songsStatus.textContent = t("Connect to the piano first.");
      return false;
    }
    lock.busy = true;
    songsStatus.textContent = t("{op}…", { op: what });
    try {
      const r = await run(new InternalSongs(s));
      if (r === false) throw new Error(t("no answer"));
      songsStatus.textContent = t("{op}: done", { op: what });
      return true;
    } catch (e) {
      songsStatus.textContent = t("{op} failed: {e}", { op: what, e: (e as Error).message });
      return false;
    } finally {
      lock.busy = false;
    }
  };
  const rescanOne = (n: number) => songRun(t("Read song {n}", { n: n + 1 }), async (c) => {
    if (m.rec && (await d.readNg())) return;
    if (m.tableReason()) return;
    songs[n] = await c.info(n);
    render();
  });
  const delSong = (n: number, part: 0 | 1 | 0x7f, btn: EventTarget | null) => {
    if (!m.canDelete || d.refuseTable()) return;
    const msg = part === 0x7f ? t("Delete song {n} on the piano?", { n: n + 1 }) : t("Delete part {p} of song {n} on the piano?", { p: part + 1, n: n + 1 });
    confirmTap(btn, msg, () => void songRun(t("Delete"), (c) => c.delete(n, part)).then((ok) => ok && rescanOne(n)));
  };
  // the song whose other buttons (export, delete) are shown: one at a time
  let openSong = -1;
  // rows are replaced only when what they show changes: a notice while a finger is on a row must not replace it
  const rowKeys: string[] = [];
  const rowEls: HTMLElement[] = [];
  const playsNow = (n: number) => n === m.sel && m.state === 1 && !usbAnyRunning(pb);
  const loadingNow = () => (d.loading ? d.loading() : -1);
  const songRow = (n: number): HTMLElement => {
    const i = songs[n];
    const on = n === m.sel;
    const empty = i !== null && !i.hasData;
    const both = !!i?.parts && i.parts[0] && i.parts[1];
    const isOpen = openSong === n && !empty;
    const canDelete = m.canDelete;
    const acts: (HTMLElement | null)[] = empty ? [] : [
      // one way out, one format list (ui/savesheet.ts): MID, KSO (the piano's own format - a lossless backup that
      // loads back with 24 09), MP3, WAV
      KINDS.piano.saveTo.length ? h("button", { "data-act": "song-save", onclick: () => d.save(n) }, t("Save to USB")) : null,
      canDelete && both ? h("button", { class: "danger", "data-act": "song-delete-p1", onclick: (e: Event) => delSong(n, 0, e.currentTarget) }, t("Delete part 1")) : null,
      canDelete && both ? h("button", { class: "danger", "data-act": "song-delete-p2", onclick: (e: Event) => delSong(n, 1, e.currentTarget) }, t("Delete part 2")) : null,
      canDelete && i?.hasData ? h("button", { class: "danger", "data-act": "song-delete", onclick: (e: Event) => delSong(n, 0x7f, e.currentTarget) }, t("Delete")) : null,
    ];
    return h(
      "div",
      { class: "song-row", "data-song": String(n + 1), "data-state": i === null ? "unknown" : i.hasData ? "data" : "empty", "data-on": on ? "1" : undefined, "data-open": isOpen ? "1" : undefined, "data-playing": playsNow(n) ? "1" : undefined, "data-loading": loadingNow() === n ? "1" : undefined },
      h("div", { class: "file-main" },
        // owner 2026-11-24: the list's own heading says these are the piano's songs (no 「피아노」 badge), and the number
        // is the song's name (no 「곡 N」 next to the same number) - the row reads number + what it holds
        h("button", { class: "song-pick", "data-act": "song-pick", "aria-pressed": String(on), "aria-label": `${t("Song {n}", { n: n + 1 })} · ${m.songText(i)}`, onclick: () => d.tap(n) },
          h("span", { class: "song-num" }, n + 1),
          h("span", { class: "song-txt" }, h("span", { class: "song-info" }, m.songText(i))),
          playsNow(n) ? playingMark() : null),
        empty ? null : iconButton("more", t("More"), { class: "row-more", "data-act": "song-more", "aria-expanded": String(isOpen), onclick: () => { openSong = isOpen ? -1 : n; render(); } })),
      acts.length ? h("div", { class: "row-acts", hidden: !isOpen }, ...acts) : null,
      // the stock's own delete does not remove the song: say why there is no Delete
      isOpen && i?.hasData && !canDelete ? h("p", { class: "muted row-note" }, t("Needs OpenGrand song delete: the piano's own delete does not remove the song.")) : null,
    );
  };
  function render() {
    for (let n = 0; n < SONGS; n++) {
      const i = songs[n];
      const key = [n === m.sel, playsNow(n), openSong === n, m.canDelete, i ? `${i.hasData}|${i.bars}|${i.parts}` : m.rec && m.tableReason() ? "x" : "?"].join("/");
      if (rowKeys[n] === key && rowEls[n]) continue;
      const row = songRow(n);
      if (rowEls[n]?.parentElement === list) rowEls[n].replaceWith(row);
      else list.append(row);
      rowEls[n] = row;
      rowKeys[n] = key;
    }
    // the loading mark changes in place (not in the key: the tapped row is not replaced under the finger)
    const at = loadingNow();
    rowEls.forEach((r, n) => {
      if ((at === n) !== r.hasAttribute("data-loading")) {
        if (at === n) r.setAttribute("data-loading", "1");
        else r.removeAttribute("data-loading");
      }
    });
    songsHint.hidden = !!m.rec || songs.every((i) => i);
    const why = m.tableReason();
    tableWarn.hidden = !why;
    if (why && tableWarn.textContent !== why) tableWarn.textContent = why;
  }
  const scanSongs = async () => {
    // OpenGrand: the song table; the stock way below only when the op does not answer
    if (m.rec) {
      songsStatus.textContent = t("{op}…", { op: t("Scan") });
      if (await d.readNg()) return;
      if (d.refuseTable()) return; // the stock scan selects every song: not on a table that failed its check
    }
    const ok = await songRun(t("Scan"), async (c) => {
      songs.fill(null);
      render();
      await c.scan(d.restoreOf(), (i) => {
        songs[i.n] = i;
        render();
      });
    });
    if (ok) songsStatus.textContent = recordedText();
  };
  const pane = h("div", { class: "pane", id: "songs-pane", "data-title": t("Piano songs") },
    h("div", { class: "pane-head" },
      songsStatus,
      h("div", { class: "pane-btns" }, h("button", { "data-act": "songs-scan", onclick: () => void scanSongs() }, `↻ ${t("Scan")}`))),
    songsHint,
    tableWarn,
    list);
  return { pane, status: songsStatus, recordedText, render };
}
