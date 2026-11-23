// Recorder tab (owner 2026-11-22): the home of recordings and files - the tab that was 「USB 메모리」, with the
// recorder that was a mode of the Music tab. Since 2026-11-23 it is laid out as a media player (owner: 「보통 곡 재생은
// 이런 바를 쓴다」):
//   the player bar    (screens/songctl.ts playerBar, docked under the header while the lists scroll) - what is loaded,
//                     the position on a progress line, play / pause, previous / next, stop, Rec, part and options.
//                     It shows the chosen piano song, or - while a USB file plays or after one was chosen in the USB
//                     list - that file with its own position (field 2026-11-23: 「USB 재생 중」 stood over 「곡 1 ·
//                     199마디」).
//   the lists         [Piano songs | USB memory]: the piano's ten recorder songs as rows (number, bars or empty; a tap
//                     chooses the song = loads it into the bar, the playing one is marked, ⋯ opens export to USB and
//                     delete), or the USB memory's file manager (ui/files.ts usbPane)
// A recording target is one entry of TARGETS; only the piano's own recorder exists today, so the target buttons are
// not drawn (a second entry - USB audio, WAV / MP3 - makes them appear).
//
// The bytes are the stock ones: MusicMode 31 52 1D (2 = recorder, sent before Rec / Play / a song choice when the
// piano is in another mode), SongSelect / IntSongPart 10 52 00 / 01, Music 32 52 08 (3 = record standby), and on a
// piano with the playback control (Identify feat2 0x10) op 0x1E select / play / pause / stop / bar seek with its
// refusal text. The op neither starts nor ends a recording: while one runs or waits, Stop / Pause / Play go as Music
// bytes.
// What the bar can and cannot do:
//   position   piano songs: the bar the piano reports (op 1E status byte 29, on the stock MusicProgress 32 52 03) of
//              the song's bars (op status MaxTime / the song table / the stock's MaxTime after a select)
//   jump       only op 0x1E cmd 7 (bar seek, MusicMode 2) - the line is a slider there and nowhere else: this app has
//              never sent the stock's MusicProgress set, and USB files have no jump at all. Rewind / fast forward
//              (Music 8 / 7) are in the options, named as what they are
//   ⏮ ⏭       the previous / next song number (SongSelect / op select), or the previous / next USB file
//   USB        ▶ plays the chosen file and ■ stops it (ui/usbplay.ts; there is no USB pause in this app); the time is
//              the piano's MusicProgress, the length only if the piano says one (MaxTime in MusicMode 3)
// Rows play on a tap (owner 2026-11-23): a USB file, or a piano song that holds a recording (an empty one is only
// chosen - it is where Rec records).
// The internal songs come from OpenGrand's read-only song table (op 1E 01) and its 5E 01 notices; on a piano without
// that op they need 「읽기」: the stock way selects each song on the piano (protocol/songs.ts).
// Delete is offered only where the piano's core deletes (5E 00 byte 31 bit 0, firmware recdel.c): the stock's own
// InternalSongDelete re-writes the song from the last take - nothing deleted, and another song overwritten.
import { t } from "../i18n.ts";
import { findParam, progress } from "../protocol/codecs.ts";
import { FILE_TYPE } from "../protocol/fileaccess.ts";
import {
  NgClient, ngHas, NgRecorder, parseRecorderNotice, RecorderCmd, RecorderEvent, RecorderRc, type RecorderSong,
  type RecorderStatus,
} from "../protocol/ngext.ts";
import { InternalSongs, SONGS, type SongInfo } from "../protocol/songs.ts";
import { CONFIRM, confirmTap } from "./confirm2.ts";
import type { AppContext } from "./context.ts";
import { h } from "./dom.ts";
import { type FileLock, songCache, songInfoOf, usbPane } from "./files.ts";
import { identifyShared } from "./identify.ts";
import { whileShown } from "./screens/common.ts";
import { songSelectData } from "./screens/music.ts";
import { iconButton, isRecState, MUSIC_CMD, MUSIC_MODE, playerBar, playingMark, type PlayerView, STATE_TEXT } from "./screens/songctl.ts";
import { playerOf, usbAnyRunning, usbNow, usbPlayback } from "./usbplay.ts";

/** Refusals of the piano's playback control (OpenGrand 0x7D op 0x1E rc - REMOTE_PROTOCOL.md section 10.9). The piano
 * judges every command the same way for the panel and this app; the panel shows the piano's own word. */
const PIANO_RC: Record<number, string> = {
  [RecorderRc.Busy]: "The piano is busy, try again in a moment.",
  [RecorderRc.Recording]: "The piano is recording: playback waits until it stops.",
  [RecorderRc.Pending]: "Wait for the previous command.",
  [RecorderRc.NoData]: "Nothing recorded in this song.",
  [RecorderRc.OtherPlayer]: "Another player mode is active on the piano.",
  [RecorderRc.NotBound]: "The piano is controlled by another device.",
};

/** What a recording goes to. One today; the target buttons appear with a second entry (USB audio - WAV / MP3 - needs
 *  the piano's audio recorder, which this app does not drive yet). */
const TARGETS: { id: string; label: string }[] = [{ id: "internal", label: "Piano songs" }];

/** Strings shown only in a state a screen test does not reach (for the translation check). */
export const RECORDER_STRINGS = [
  ...Object.values(PIANO_RC), ...STATE_TEXT, "Bar {bar}", "{a} / {n} bars", "The piano did not confirm the command.", "No answer from the piano.",
  "The piano rejected this value.", "{n} bars", "Empty", "Part 1", "Part 2", "USB playing", "Choose a song first.",
  "Stop the recording before choosing another song.", "Song {n} already has a recording. Recording again replaces it.",
  "Song {n} may already have a recording. Recording replaces it.", "Record over it", "Cancel",
  "Song {n} already has a recording. Importing replaces it.", "Delete part 1", "Delete part 2", "Delete",
  "Delete song {n} on the piano?", "Delete part {p} of song {n} on the piano?", "To USB (SMF)", "To USB (KSO)",
  "Needs OpenGrand song delete: the piano's own delete does not remove the song.", "Play a key or press Play to start recording.",
  "Previous song", "Next song", "Go to bar", "More", "Pause", "Stop", "Close",
];

type ListId = "internal" | "usb";
/** The list last shown and what the bar last held, per context (the tab is rebuilt on every visit and language change). */
const shownList = new WeakMap<AppContext, ListId>();
const shownSource = new WeakMap<AppContext, ListId>();

export function recorderScreen(ctx: AppContext): HTMLElement {
  const store = ctx.store;
  const music = findParam("Music", 0x32)!;
  const partP = findParam("IntSongPart")!;
  const songP = findParam("SongSelect")!;
  const modeP = findParam("MusicMode", 0x31)!;
  const kawai = (v: number) => void store.command(music, 0x7f, [v]);
  const lock: FileLock = { busy: false };
  const cache = songCache(ctx);
  const songs = cache.songs;
  // the piano's MusicMode as it last said or this app set (ui/usbplay.ts), and the USB players
  const pb = usbPlayback(ctx);

  // ---- model: what the piano says ----
  // state: Music report 32 52 08 / op 1E status (STATE_TEXT index); null until the piano says something
  let state: number | null = null;
  // where the piano song is (bar, 1-based; 0 = not said) and - op status only - the bars of what is selected to play
  let bar = 0;
  let maxBars = 0;
  let ngMode: number | undefined;
  // selected recorder song (0-based; -1 = not known)
  const stockSel = () => {
    const d = store.get(songP, 0x7f);
    return d && d[0] === 0 && d.length >= 3 ? (d[1] << 7) | d[2] : -1;
  };
  let sel = stockSel();
  // OpenGrand playback control (op 0x1E): null on the stock and on a payload without it
  let rec: NgRecorder | null = null;
  let canDelete = false;
  // what the bar holds when no USB file plays: the piano song, or the file chosen in the USB list
  let src: ListId = shownSource.get(ctx) ?? "internal";
  const setSrc = (v: ListId) => {
    src = v;
    shownSource.set(ctx, v);
  };
  /** The bar is the USB player's: a USB file plays (never while a recording runs or waits), or one was chosen last
   *  and the piano song is at rest - a song that plays or is paused (started on the panel, the piano's buttons) is
   *  what the bar must show and its ■ must stop. */
  const usbView = () => (usbAnyRunning(pb) ? !isRecState(state) : src === "usb" && (state === null || state === 2));

  const usb = usbPane(ctx, lock, {
    songHasData: (n) => !!songs[n]?.hasData,
    onPick: () => {
      setSrc("usb");
      note.textContent = "";
      renderNow();
    },
  });

  // ---- the player bar ----
  const hint = h("p", { class: "muted", id: "rec-hint", hidden: true }, t("Play a key or press Play to start recording."));
  const note = h("p", { class: "muted", id: "rec-note", role: "status" });
  const over = h("div", { class: "confirm-ask", id: "rec-overwrite", role: "alert", hidden: true });
  let overTimer: ReturnType<typeof setTimeout> | undefined;
  const hideOver = () => {
    clearTimeout(overTimer);
    over.hidden = true;
  };
  // a song in part 1 alone - the usual recording - says nothing of parts (owner 2026-11-23: 「파트」 meant nothing)
  const partsText = (i: SongInfo) => (i.parts && i.parts[1] ? [i.parts[0] ? t("Part 1") : "", t("Part 2")].filter(Boolean).join(" + ") : "");
  const songText = (i: SongInfo | null): string => {
    if (!i) return t("not scanned");
    if (!i.hasData) return t("Empty");
    return [i.bars !== null ? t("{n} bars", { n: i.bars }) : "", partsText(i)].filter(Boolean).join(" · ");
  };
  /** The USB file the bar is about: the one that plays, else the one chosen in the list. */
  const usbItem = () => (usbAnyRunning(pb) ? usbNow(pb)?.name ?? null : usb.selected());
  const view = (): PlayerView => {
    if (usbView()) {
      const on = usbAnyRunning(pb);
      const now = on ? usbNow(pb) : null;
      const name = usbItem();
      const files = ctx.usbFiles;
      const k = name === null ? -1 : files.indexOf(name);
      return {
        state: on ? 1 : 2, stateText: on ? t("USB playing") : undefined,
        // the whole name with its type; "USB" yields first when the name is long
        title: name !== null ? name.slice(name.lastIndexOf("\\") + 1) : on ? t("USB memory") : t("Choose a song below."),
        sub: name !== null ? "USB" : "",
        // a file that does not play has no position; its kind says which unit will come
        unit: now ? now.unit : name !== null && playerOf(name) === "smf" ? "bar" : "ms",
        pos: now ? now.pos : null, total: now ? now.total : null,
        seekable: false, main: on ? "stop" : "play",
        prev: k > 0, next: files.length > 0 && k < files.length - 1,
        part: false, // a USB file has no parts
      };
    }
    const i = sel < 0 ? null : songs[sel];
    const recording = isRecState(state);
    const total = recording ? null : (rec && maxBars) || i?.bars || null;
    const moving = state === 0 || state === 1;
    return {
      state,
      title: sel < 0 ? t("Choose a song below.") : t("Song {n}", { n: sel + 1 }),
      sub: sel < 0 ? "" : songText(i),
      unit: "bar",
      // stopped = at the first bar
      pos: recording ? bar || null : moving && bar ? (total ? Math.min(bar, total) : bar) : total ? 1 : null,
      total,
      seekable: !!rec && !recording && sel >= 0 && state !== null,
      main: state === 1 || state === 4 || state === 6 ? "pause" : "play",
      prev: !recording && sel > 0, next: !recording && sel < SONGS - 1,
    };
  };
  const renderNow = () => {
    hint.hidden = state !== 3 && state !== 5;
    player.set(view());
  };

  /** MusicMode 2 (recorder) when the piano is - or may be - in another mode. */
  const ensureMode = async () => {
    if (pb.mode === MUSIC_MODE.Recorder || (pb.mode === undefined && ngMode === MUSIC_MODE.Recorder)) return;
    pb.mode = MUSIC_MODE.Recorder;
    ngMode = MUSIC_MODE.Recorder;
    await store.command(modeP, 0x7f, [MUSIC_MODE.Recorder]);
  };
  /** One op 0x1E command; the piano's refusal (rc) is shown under the transport. */
  const op = async (opCmd: number, arg = 0): Promise<boolean> => {
    const rc = await rec!.command(opCmd, arg);
    note.textContent = rc === null ? t("No answer from the piano.")
      : rc <= RecorderRc.Nop ? "" : t(PIANO_RC[rc] ?? "The piano rejected this value.");
    return rc !== null && rc <= RecorderRc.Nop;
  };
  // Play / pause / stop: op 0x1E where the piano has it (it answers why not); while a recording runs or waits, and on
  // the stock, the Kawai Music bytes (the op does not record). A USB playback is stopped first.
  // piano: the piano song whatever the bar showed (a song row's tap - also while a USB stop is still being confirmed)
  const play = async (piano = false) => {
    hideOver();
    note.textContent = "";
    if (isRecState(state)) return kawai(MUSIC_CMD.Play); // starts the recording that waits
    if (!piano && usbView()) {
      // the USB file chosen in the list (MusicMode 3, 24 08, Play - ui/usbplay.ts)
      const name = usb.selected();
      if (name === null) note.textContent = t("Choose a song first.");
      else await usb.play(name);
      return;
    }
    if (usbAnyRunning(pb)) await usb.stop();
    await ensureMode();
    if (rec) await op(RecorderCmd.Play);
    else kawai(MUSIC_CMD.Play);
  };
  const pause = () => {
    hideOver();
    if (rec && !isRecState(state)) void op(RecorderCmd.Pause);
    else kawai(MUSIC_CMD.Pause);
  };
  const stop = () => {
    hideOver();
    // the USB players: whatever runs, and - with a USB file in the bar - also what this app does not know of
    if (usbView()) return void usb.stop();
    if (rec && !isRecState(state)) void op(RecorderCmd.Stop);
    else kawai(MUSIC_CMD.Stop);
  };
  // the progress line's dot let go on a bar: op 0x1E cmd 7 (the only jump - see the head of this file)
  const seek = async (to: number) => {
    if (!rec || isRecState(state)) return;
    note.textContent = "";
    const was = bar;
    bar = to; // shown until the piano's next position notice
    renderNow();
    await ensureMode();
    if (await op(RecorderCmd.Seek, to)) return;
    bar = was; // refused: the dot goes back, the reason is under the buttons
    renderNow();
  };
  // ⏮ ⏭: the neighbouring song number, or the neighbouring USB file
  const step = (d: -1 | 1) => {
    if (usbView()) {
      const name = usbItem() ?? usb.selected();
      const files = ctx.usbFiles;
      const to = files[(name === null ? (d > 0 ? -1 : files.length) : files.indexOf(name)) + d];
      if (to !== undefined) usb.pick(to, false); // plays only if a USB file plays now
      return;
    }
    const n = sel < 0 ? 0 : sel + d;
    if (n >= 0 && n < SONGS) void pick(n);
  };
  // Rec: record standby (Music 3). A song that has - or may have - data in the part to record is asked about first.
  const startRec = async () => {
    hideOver();
    if (usbAnyRunning(pb)) await usb.stop();
    await ensureMode();
    kawai(MUSIC_CMD.Rec);
  };
  const recTap = () => {
    note.textContent = "";
    if (isRecState(state)) return kawai(MUSIC_CMD.Rec);
    if (sel < 0) {
      note.textContent = t("Choose a song first.");
      return;
    }
    // a recording goes to the piano song: the bar shows it from here on
    setSrc("internal");
    renderNow();
    const i = songs[sel];
    const part = store.get(partP, 0x7f)?.[0];
    const replaces = !i || (i.hasData && (i.parts && (part === 0 || part === 1) ? i.parts[part] : true));
    if (!replaces) return void startRec();
    const ask = i ? "Song {n} already has a recording. Recording again replaces it." : "Song {n} may already have a recording. Recording replaces it.";
    over.replaceChildren(
      h("p", {}, t(ask, { n: sel + 1 })),
      h("div", { class: "tp-row" },
        h("button", { class: "danger", "data-act": "rec-confirm", onclick: () => void startRec() }, `● ${t("Record over it")}`),
        h("button", { "data-act": "rec-cancel", onclick: hideOver }, t("Cancel"))));
    over.hidden = false;
    clearTimeout(overTimer);
    overTimer = setTimeout(hideOver, CONFIRM.ms);
  };

  const player = playerBar(store, "rec-panel", {
    play: () => void play(), pause, stop, prev: () => step(-1), next: () => step(1), seek: (to) => void seek(to), rec: recTap,
  });
  const targetBar = h("div", { class: "segmented", id: "rec-target", hidden: TARGETS.length < 2 },
    ...TARGETS.map((g, k) => h("button", { "data-target": g.id, class: k === 0 ? "on" : undefined }, t(g.label))));
  player.foot.append(targetBar, over, hint, note);
  const panel = player.el;

  let attached = false;
  const timer = setInterval(() => {
    // stop polling once the screen has been replaced
    if (panel.isConnected) attached = true;
    else if (attached) return clearInterval(timer);
    // the bar docks under the app's header, whatever its height is (one or two lines, the panel's larger buttons)
    const head = typeof document !== "undefined" ? document.querySelector("header") : null;
    const top = `${head ? head.offsetHeight : 0}px`;
    if (head && el.style.getPropertyValue("--dock") !== top) el.style.setProperty("--dock", top);
    renderNow();
  }, 250);

  // ---- the piano's ten songs ----
  const recordedText = () => t("{n} of 10 songs recorded", { n: songs.filter((i) => i?.hasData).length });
  const songsStatus = h("p", { class: "muted", id: "songs-status", role: "status" }, songs.some((i) => i) ? recordedText() : t("Not scanned yet."));
  const songsHint = h("p", { class: "muted", id: "songs-hint", hidden: true }, t("The piano's recorder memory (10 songs). Scanning selects each song on the piano; song names are not sent by the piano and cannot be changed."));
  const songList = h("div", { class: "list", id: "songs-list" });
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
  const fillFromNg = (list: RecorderSong[]) => {
    list.slice(0, SONGS).forEach((g, n) => (songs[n] = songInfoOf(g, n)));
    songsStatus.textContent = recordedText();
    renderSongs();
  };
  const readNg = async (): Promise<boolean> => {
    const list = await rec?.songs();
    if (!list) return false;
    fillFromNg(list);
    return true;
  };
  const restoreOf = () => ({ song: Math.max(0, stockSel()), part: store.get(partP, 0x7f)?.[0] });
  const rescanOne = (n: number) => songRun(t("Read song {n}", { n: n + 1 }), async (c) => {
    if (rec && (await readNg())) return;
    songs[n] = await c.info(n);
    renderSongs();
  });
  const delSong = (n: number, part: 0 | 1 | 0x7f, btn: EventTarget | null) => {
    if (!canDelete) return;
    const msg = part === 0x7f ? t("Delete song {n} on the piano?", { n: n + 1 }) : t("Delete part {p} of song {n} on the piano?", { p: part + 1, n: n + 1 });
    confirmTap(btn, msg, () => void songRun(t("Delete"), (c) => c.delete(n, part)).then((ok) => ok && rescanOne(n)));
  };
  const saveSong = (n: number, ext: "MID" | "KSO") =>
    void usb.run(ext === "MID" ? t("Save as SMF") : t("Save as KSO"), () => ctx.files()!.saveSong(n, `SONG_${String(n + 1).padStart(2, "0")}.${ext}`, FILE_TYPE[ext]), true, songsStatus);
  // the stock tells a selected song's length with MaxTime (10 52 18) right after the SongSelect: this app's own choice
  // is noted here so that answer fills the row (a scan's answers are not this)
  let expectMax = -1;
  let expectTimer: ReturnType<typeof setTimeout> | undefined;
  // a tap on a row (or ⏮ ⏭): that song is the piano's selected song and what the bar holds
  const pick = async (n: number) => {
    if (n === sel) {
      if (src !== "internal") {
        setSrc("internal");
        renderNow();
      }
      return;
    }
    if (isRecState(state)) {
      note.textContent = t("Stop the recording before choosing another song.");
      return;
    }
    hideOver();
    note.textContent = "";
    const before = sel;
    sel = n;
    bar = 0;
    setSrc("internal");
    usb.setImportTarget(n);
    renderSongs();
    renderNow();
    await ensureMode();
    if (rec) {
      if ((await op(RecorderCmd.Select, n)) || sel !== n) return;
      sel = before; // refused: the piano's song did not change
      renderSongs();
      renderNow();
    } else {
      expectMax = n;
      clearTimeout(expectTimer);
      expectTimer = setTimeout(() => (expectMax = -1), 2000);
      void store.set(songP, 0x7f, songSelectData(n));
    }
  };
  // a tap on a song row: choose it and - when it holds a recording - play it, as a media player does (owner 2026-11-23:
  // a tap that only chose read as 「재생이 안 된다」). An empty song is only chosen: it is where Rec records. The
  // playing song's row does nothing; during a recording the choice is refused (pick says why).
  const tapSong = async (n: number) => {
    if (n === sel && (state === 1 || isRecState(state)) && !usbAnyRunning(pb)) return;
    const before = sel;
    await pick(n);
    if (sel !== n || isRecState(state)) return; // refused
    if (!songs[n]?.hasData) return;
    if (n === before && state === 1 && !usbAnyRunning(pb)) return;
    await play(true);
  };
  // the song whose other buttons (export, delete) are shown: one at a time
  let openSong = -1;
  // rows are replaced only when what they show changes: a notice while a finger is on a row must not replace it
  const rowKeys: string[] = [];
  const rowEls: HTMLElement[] = [];
  const playsNow = (n: number) => n === sel && state === 1 && !usbAnyRunning(pb);
  const songRow = (n: number): HTMLElement => {
    const i = songs[n];
    const on = n === sel;
    const empty = i !== null && !i.hasData;
    const both = !!i?.parts && i.parts[0] && i.parts[1];
    const isOpen = openSong === n && !empty;
    const acts: (HTMLElement | null)[] = empty ? [] : [
      h("button", { "data-act": "song-save", onclick: () => saveSong(n, "MID") }, t("To USB (SMF)")),
      // KSO = the piano's own song format, a lossless backup that loads back (24 09); owner 2026-11-15 (R2 backup)
      h("button", { "data-act": "song-save-kso", onclick: () => saveSong(n, "KSO") }, t("To USB (KSO)")),
      canDelete && both ? h("button", { class: "danger", "data-act": "song-delete-p1", onclick: (e: Event) => delSong(n, 0, e.currentTarget) }, t("Delete part 1")) : null,
      canDelete && both ? h("button", { class: "danger", "data-act": "song-delete-p2", onclick: (e: Event) => delSong(n, 1, e.currentTarget) }, t("Delete part 2")) : null,
      canDelete && i?.hasData ? h("button", { class: "danger", "data-act": "song-delete", onclick: (e: Event) => delSong(n, 0x7f, e.currentTarget) }, t("Delete")) : null,
    ];
    return h(
      "div",
      { class: "song-row", "data-song": String(n + 1), "data-state": i === null ? "unknown" : i.hasData ? "data" : "empty", "data-on": on ? "1" : undefined, "data-open": isOpen ? "1" : undefined, "data-playing": playsNow(n) ? "1" : undefined },
      h("div", { class: "file-main" },
        h("button", { class: "song-pick", "data-act": "song-pick", "aria-pressed": String(on), onclick: () => void tapSong(n) },
          h("span", { class: "song-num" }, n + 1),
          h("span", { class: "song-txt" }, h("span", { class: "song-name" }, t("Song {n}", { n: n + 1 })), h("span", { class: "song-info" }, songText(i))),
          playsNow(n) ? playingMark() : null),
        empty ? null : iconButton("more", t("More"), { class: "row-more", "data-act": "song-more", "aria-expanded": String(isOpen), onclick: () => { openSong = isOpen ? -1 : n; renderSongs(); } })),
      acts.length ? h("div", { class: "row-acts", hidden: !isOpen }, ...acts) : null,
      // the stock's own delete does not remove the song: say why there is no Delete
      isOpen && i?.hasData && !canDelete ? h("p", { class: "muted row-note" }, t("Needs OpenGrand song delete: the piano's own delete does not remove the song.")) : null,
    );
  };
  const renderSongs = () => {
    for (let n = 0; n < SONGS; n++) {
      const i = songs[n];
      const key = [n === sel, playsNow(n), openSong === n, canDelete, i ? `${i.hasData}|${i.bars}|${i.parts}` : "?"].join("/");
      if (rowKeys[n] === key && rowEls[n]) continue;
      const row = songRow(n);
      if (rowEls[n]?.parentElement === songList) rowEls[n].replaceWith(row);
      else songList.append(row);
      rowEls[n] = row;
      rowKeys[n] = key;
    }
    songsHint.hidden = !!rec || songs.every((i) => i);
  };
  const scanSongs = async () => {
    // OpenGrand: the song table; the stock way below only when the op does not answer
    if (rec) {
      songsStatus.textContent = t("{op}…", { op: t("Scan") });
      if (await readNg()) return;
    }
    const ok = await songRun(t("Scan"), async (c) => {
      songs.fill(null);
      renderSongs();
      await c.scan(restoreOf(), (i) => {
        songs[i.n] = i;
        renderSongs();
      });
    });
    if (ok) songsStatus.textContent = recordedText();
  };
  const songsPane = h("div", { class: "pane", id: "songs-pane", "data-title": t("Piano songs") },
    h("div", { class: "pane-head" },
      songsStatus,
      h("div", { class: "pane-btns" }, h("button", { "data-act": "songs-scan", onclick: () => void scanSongs() }, `↻ ${t("Scan")}`))),
    songsHint,
    songList);

  // ---- [Piano songs | USB memory] ----
  let shown: ListId = shownList.get(ctx) ?? "internal";
  const listBar = h("div", { class: "segmented", id: "rec-lists" },
    ...([["internal", "Piano songs"], ["usb", "USB memory"]] as const).map(([id, label]) =>
      h("button", { "data-list": id, onclick: () => { shown = id; shownList.set(ctx, id); showList(); } }, t(label))));
  const showList = () => {
    listBar.querySelectorAll("button").forEach((b) => b.classList.toggle("on", b.getAttribute("data-list") === shown));
    songsPane.hidden = shown !== "internal";
    usb.el.hidden = shown !== "usb";
  };
  usb.el.setAttribute("data-title", t("USB memory")); // the list's heading where both lists show (styles.css)
  const el = h("div", { class: "screen recorder" }, panel,
    h("section", { class: "card", id: "rec-files" }, listBar, songsPane, usb.el));

  // ---- the piano's word: the parameter mirror, the Music state report, the song length after a select ----
  whileShown(el, store, (k) => {
    if (k === "IntSongPart@127") player.syncPart();
    else if (k === "SongSelect@127" && !rec) {
      const n = stockSel();
      if (n < 0 || n === sel) return;
      sel = n;
      bar = 0;
      renderSongs();
      renderNow();
    }
  });
  const recorderMode = () => pb.mode === undefined || pb.mode === MUSIC_MODE.Recorder;
  let seen = false;
  const offMsg = store.onMessage((m) => {
    if (el.isConnected) seen = true;
    else if (seen) return void offMsg(); // the screen was replaced
    if (m.v1 !== 0x52) return;
    if (m.v2 === 0x08 && (m.fn === 0x10 || m.fn === 0x32) && m.data[0] !== undefined && m.data[0] <= 6 && !rec && recorderMode()) {
      // the Music report is the recorder's only in the recorder mode (MusicMode 3: a USB player's - ui/usbplay.ts)
      state = m.data[0];
      if (state === 2) bar = 0;
      renderSongs();
      renderNow();
    } else if (m.v2 === 0x03 && (m.fn === 0x10 || m.fn === 0x32) && m.data.length >= 4 && !rec && pb.mode === MUSIC_MODE.Recorder) {
      // MusicProgress in the recorder mode, bar form: where the piano song is (the op's status says it on OpenGrand)
      const at = progress.decode(m.data);
      if ("bar" in at) bar = at.bar;
    } else if (m.v2 === 0x18 && m.fn === 0x10 && m.data.length >= 3 && expectMax >= 0 && expectMax === sel && !lock.busy && store.get(partP, 0x7f)?.[0] === 0x7f) {
      // MaxTime after this app's SongSelect with both parts on = bars of that song (0: empty) - protocol/songs.ts
      const bars = (m.data[1] << 7) | m.data[2];
      const old = songs[sel];
      songs[sel] = { n: sel, hasData: bars !== 0, bars, parts: old && old.hasData && bars !== 0 ? old.parts : null };
      expectMax = -1;
      renderSongs();
      renderNow();
    }
  });
  // the USB players' word (ui/usbplay.ts): the bar is the USB file's while one runs
  let seenPlay = false;
  const onUsbPlay = () => {
    if (!el.isConnected) {
      if (seenPlay) pb.listeners.delete(onUsbPlay);
      return;
    }
    seenPlay = true;
    renderSongs();
    renderNow();
  };
  pb.listeners.add(onUsbPlay);

  // ---- OpenGrand playback control: the song table, the selected song and the state come as notices ----
  const s = ctx.session();
  if (s && s.state === "ready") { // main.ts draws the tab again when the session becomes ready
    const onStatus = (st: RecorderStatus) => {
      if (st.state <= 6) state = st.state;
      bar = st.bar;
      maxBars = st.maxBars;
      ngMode = st.musicMode;
      canDelete = st.deletes;
      if (st.song < SONGS && st.song !== sel) {
        hideOver();
        sel = st.song;
      }
    };
    let on = false;
    const off = s.onMidi((m) => {
      if (el.isConnected) on = true;
      else if (on) return void off(); // the screen was replaced
      const n = parseRecorderNotice(m);
      if (!n || !rec) return;
      if (n.status) onStatus(n.status);
      if (n.songs) fillFromNg(n.songs);
      if (n.event && n.event.event === RecorderEvent.Timeout) note.textContent = t("The piano did not confirm the command.");
      renderSongs();
      renderNow();
    });
    void (async () => {
      const id = await identifyShared(s);
      if (!ngHas(id, "recorder") || ctx.session() !== s) return void off(); // stock or a payload without the feature
      rec = new NgRecorder(new NgClient(s));
      if (!songs.some((i) => i)) songsStatus.textContent = t("{op}…", { op: t("Scan") });
      // a 1E request also subscribes this controller to 5E 01 (song table changed) while it stays bound
      if (!(await readNg()) && !songs.some((i) => i)) songsStatus.textContent = t("Not scanned yet.");
      const st = await rec.status();
      if (st && ctx.session() === s) onStatus(st);
      renderSongs();
      renderNow();
    })();
  }

  renderSongs();
  renderNow();
  showList();
  return el;
}
