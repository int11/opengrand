// Recorder tab (owner 2026-11-22): the home of recordings and files - the tab that was 「USB 메모리」, with the
// recorder that was a mode of the Music tab. Since 2026-11-23 it is laid out as a media player (owner: 「보통 곡 재생은
// 이런 바를 쓴다」):
//   the player bar    (screens/songctl.ts playerBar, docked under the header while the lists scroll) - what is loaded,
//                     the position on a progress line, play / pause, previous / next, stop, Rec, part and options.
//                     It shows the chosen piano song, or - while a USB file plays or after one was chosen in the USB
//                     list - that file with its own position (field 2026-11-23: 「USB 재생 중」 stood over 「곡 1 ·
//                     199마디」).
//   the lists         [Piano songs | USB memory]: the piano's ten recorder songs as rows (number, bars or empty; a tap
//                     chooses the song = loads it into the bar, the playing one is marked, ⋯ opens 「USB로 저장」
//                     and delete), or the USB memory's file manager (ui/files.ts usbPane)
// Three rules (owner 2026-11-23: 「기능이 섞여서 복잡하다」):
//   1. a tap plays, whatever and wherever it is. Piano songs and USB files are rows of one style with a badge (피아노 /
//      MID / MP3 / WAV / KSO). A KSO - which the piano's USB player does not play - is put into an empty piano song
//      (24 09) and played there; only when no song is empty is the song to replace asked about.
//   2. ● Rec records into the piano: into an empty song chosen by this app (asked only when none is empty). There is
//      no target or format to choose first. When the recording stops, one sheet says 「저장됨: 피아노 곡 N」 and
//      offers 「USB에도 저장」.
//   3. 「USB로 저장」 is one sheet (ui/savesheet.ts) with one format list, MID · KSO · MP3 · WAV, in every piano
//      song's ⋯ menu and after a recording.
// What a kind of item supports (parts, bar slider, pause, save formats, into the piano) is read from the capability
// table ui/songkinds.ts.
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
import { BUSY_RETRY } from "../protocol/fileaccess.ts";
import { findParam, progress } from "../protocol/codecs.ts";
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
import { KsoError } from "../protocol/kso.ts";
import { KSO_TEXT, midToKsoFile } from "./midikso.ts";
import { saveSheet } from "./savesheet.ts";
import { KINDS, kindOf } from "./songkinds.ts";
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

/** Strings shown only in a state a screen test does not reach (for the translation check). */
export const RECORDER_STRINGS = [
  ...Object.values(PIANO_RC), ...STATE_TEXT, "Bar {bar}", "{a} / {n} bars", "The piano did not confirm the command.", "No answer from the piano.",
  "The piano rejected this value.", "{n} bars", "Empty", "Part 1", "Part 2", "USB playing", "Choose a song first.",
  "Stop the recording before choosing another song.", "Song {n} already has a recording. Recording again replaces it.",
  "Song {n} may already have a recording. Recording replaces it.", "Record over it", "Cancel",
  "Delete part 1", "Delete part 2", "Delete", "Delete song {n} on the piano?", "Delete part {p} of song {n} on the piano?", "Save to USB",
  "All 10 piano songs are used. Choose the song to record over below, then press Rec.", "Replace song {n}",
  "No piano song is empty.", "The piano's songs have not been read yet (Scan reads them).",
  "Playing {name} puts it into song {n} and replaces what is there.", "{name} goes into song {n} and replaces what is there.",
  "{name} is now piano song {n}.", "Playing {name} from piano song {n}.", "Stop the recording first.", "Import", "Piano",
  ...Object.values(KSO_TEXT), "Convert to a piano song", "{n} details a piano song cannot hold were left out or rounded.",
  "Needs OpenGrand song delete: the piano's own delete does not remove the song.", "Play a key or press Play to start recording.",
  "Previous song", "Next song", "Go to bar", "More", "Pause", "Stop", "Close",
];

type ListId = "internal" | "usb";
/** The list last shown and what the bar last held, per context (the tab is rebuilt on every visit and language change). */
const shownList = new WeakMap<AppContext, ListId>();
const shownSource = new WeakMap<AppContext, ListId>();
/** The piano song a USB file was put into (a KSO tap), per context and session: the next tap plays that song again
 *  instead of filling another one. */
const putInto = new WeakMap<AppContext, { session: unknown; at: Map<string, number> }>();

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
    intoPiano: (name, andPlay) => void intoPiano(name, andPlay),
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
  /** The files the USB player plays itself - what ⏮ ⏭ step through (a KSO is played through a piano song). */
  const direct = () => ctx.usbFiles.filter((f) => { const k = kindOf(f); return !k || KINDS[k].howPlayed === "direct"; });
  /** The USB file the bar is about: the one that plays, else the one chosen in the list. */
  const usbItem = () => (usbAnyRunning(pb) ? usbNow(pb)?.name ?? null : usb.selected());
  const view = (): PlayerView => {
    if (usbView()) {
      const on = usbAnyRunning(pb);
      const now = on ? usbNow(pb) : null;
      const name = usbItem();
      const files = direct();
      const k = name === null ? -1 : files.indexOf(name);
      const kind = name === null ? null : kindOf(name);
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
        part: kind ? KINDS[kind].parts : false,
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
      seekable: KINDS.piano.seek && !!rec && !recording && sel >= 0 && state !== null,
      main: state === 1 || state === 4 || state === 6 ? (KINDS.piano.pause ? "pause" : "stop") : "play",
      prev: !recording && sel > 0, next: !recording && sel < SONGS - 1,
      part: KINDS.piano.parts,
    };
  };
  // a recording that ran in a song (-1: none): when it stops, the sheet says where it is and offers the USB memory
  let recSong = -1;
  const renderNow = () => {
    hint.hidden = state !== 3 && state !== 5;
    player.set(view());
    if (state === 4) recSong = sel;
    else if (recSong >= 0 && !isRecState(state)) {
      const n = recSong;
      recSong = -1;
      if (state === 2 && typeof document !== "undefined" && el.isConnected) saved.open(n, true);
    }
  };

  /** MusicMode 2 (recorder) when the piano is - or may be - in another mode. */
  const ensureMode = async () => {
    if (pb.mode === MUSIC_MODE.Recorder || (pb.mode === undefined && ngMode === MUSIC_MODE.Recorder)) return;
    pb.mode = MUSIC_MODE.Recorder;
    ngMode = MUSIC_MODE.Recorder;
    await store.command(modeP, 0x7f, [MUSIC_MODE.Recorder]);
  };
  /** One op 0x1E command; the piano's refusal (rc) is shown under the transport. */
  // Busy (rc 3): the core's song-table write window is open (an import, a save, the end of a recording - REMOTE_PROTOCOL
  // R3) and closes when the NOR write ends: asked again a few times (field 2026-11-24: Play right after a KSO import)
  const op = async (opCmd: number, arg = 0): Promise<boolean> => {
    let rc = await rec!.command(opCmd, arg);
    for (let i = 0; rc === RecorderRc.Busy && i < BUSY_RETRY.tries; i++) {
      await new Promise((r) => setTimeout(r, BUSY_RETRY.ms));
      rc = await rec!.command(opCmd, arg);
    }
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
      const files = direct();
      const to = files[(name === null ? (d > 0 ? -1 : files.length) : files.indexOf(name)) + d];
      if (to !== undefined) usb.pick(to, false); // plays only if a USB file plays now
      return;
    }
    const n = sel < 0 ? 0 : sel + d;
    if (n >= 0 && n < SONGS) void pick(n);
  };
  /** A question under the bar with one red button (record over a song, replace a song with a file). */
  const askOver = (text: string, label: string, act: string, run: () => void) => {
    over.replaceChildren(
      h("p", {}, text),
      h("div", { class: "tp-row" },
        h("button", { class: "danger", "data-act": act, onclick: () => { hideOver(); run(); } }, label),
        h("button", { "data-act": "rec-cancel", onclick: hideOver }, t("Cancel"))));
    over.hidden = false;
    clearTimeout(overTimer);
    overTimer = setTimeout(hideOver, CONFIRM.ms);
  };
  /** The song a new recording or a file goes to without asking: the chosen one if it is empty, else the first empty
   *  one; -1 = none is known to be empty. */
  const emptySong = () => (sel >= 0 && songs[sel] && !songs[sel]!.hasData ? sel : songs.findIndex((i) => i !== null && !i.hasData));
  /** The piano's word that a song choice (op 0x1E select) is finished - or 1.5 s. */
  const selectWaiters = new Set<() => void>();
  const selectDone = () => new Promise<void>((res) => {
    const w = () => {
      clearTimeout(timer);
      selectWaiters.delete(w);
      res();
    };
    const timer = setTimeout(w, 1500);
    selectWaiters.add(w);
  });
  // Rec: record standby (Music 3) - in an empty song this app chooses (rule 2); only when none is empty, or the song
  // table is not known (stock, not scanned), the chosen song is asked about first.
  const startRec = async () => {
    hideOver();
    if (usbAnyRunning(pb)) await usb.stop();
    await ensureMode();
    kawai(MUSIC_CMD.Rec);
  };
  const recTap = () => {
    note.textContent = "";
    if (isRecState(state)) return kawai(MUSIC_CMD.Rec);
    // a recording goes to the piano song: the bar shows it from here on
    setSrc("internal");
    renderNow();
    const i = sel < 0 ? null : songs[sel];
    const part = store.get(partP, 0x7f)?.[0];
    // one part alone was chosen in the options and the chosen song has room in it: the second hand of that song
    if (i?.hasData && i.parts && (part === 0 || part === 1) && !i.parts[part]) return void startRec();
    const to = emptySong();
    if (to >= 0) return void (async () => {
      if (to !== sel) {
        // OpenGrand: the piano confirms one command at a time - Rec goes out when the song choice is done (panelsim
        // 2026-11-23: a Music 3 right behind the op select was dropped)
        const done = rec ? selectDone() : null;
        if (!(await pick(to))) return;
        await done;
      }
      if (sel === to) await startRec();
    })();
    if (sel < 0) {
      note.textContent = songs.some((s) => s) ? t("All 10 piano songs are used. Choose the song to record over below, then press Rec.") : t("Choose a song first.");
      return;
    }
    const ask = i ? "Song {n} already has a recording. Recording again replaces it." : "Song {n} may already have a recording. Recording replaces it.";
    askOver(t(ask, { n: sel + 1 }), `● ${t("Record over it")}`, "rec-confirm", () => void startRec());
  };

  const player = playerBar(store, "rec-panel", {
    play: () => void play(), pause, stop, prev: () => step(-1), next: () => step(1), seek: (to) => void seek(to), rec: recTap,
    partPill: false, // the part is in the options sheet only (owner 2026-11-23)
  });
  player.foot.append(over, hint, note);
  const panel = player.el;
  // 「USB로 저장」 (rule 3) - also what opens when a recording stops (rule 2)
  const saved = saveSheet(ctx, "save-sheet", {
    lock, formats: KINDS.piano.saveTo,
    quiet: async () => {
      if (usbAnyRunning(pb)) await usb.stop();
      if (state === 0 || state === 1) {
        if (rec) await op(RecorderCmd.Stop);
        else kawai(MUSIC_CMD.Stop);
      }
    },
    refresh: () => usb.reload(),
    get out() { return songsStatus; },
  });
  panel.append(saved.el);

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
  // the stock tells a selected song's length with MaxTime (10 52 18) right after the SongSelect: this app's own choice
  // is noted here so that answer fills the row (a scan's answers are not this)
  let expectMax = -1;
  let expectTimer: ReturnType<typeof setTimeout> | undefined;
  // a tap on a row (or ⏮ ⏭): that song is the piano's selected song and what the bar holds
  const pick = async (n: number, again = false) => {
    if (n === sel && !again) {
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
    renderSongs();
    renderNow();
    await ensureMode();
    if (rec) {
      if ((await op(RecorderCmd.Select, n)) || sel !== n) return true;
      sel = before; // refused: the piano's song did not change
      renderSongs();
      renderNow();
    } else {
      expectMax = n;
      clearTimeout(expectTimer);
      expectTimer = setTimeout(() => (expectMax = -1), 2000);
      void store.set(songP, 0x7f, songSelectData(n));
    }
    return sel === n;
  };
  // A file the piano takes into a recorder song (a KSO; rule 1): into an empty piano song - asked only when none is
  // empty - with 24 09 <song> 03, and for a row's tap played there: the bar then shows that piano song.
  const intoPiano = async (name: string, andPlay: boolean) => {
    hideOver();
    note.textContent = "";
    if (isRecState(state)) {
      note.textContent = t("Stop the recording first.");
      return;
    }
    const leaf = name.slice(name.lastIndexOf("\\") + 1);
    const put = () => {
      let c = putInto.get(ctx);
      if (!c || c.session !== ctx.session()) putInto.set(ctx, (c = { session: ctx.session(), at: new Map() }));
      return c.at;
    };
    const go = async (n: number) => {
      if (usbAnyRunning(pb)) await usb.stop();
      if (state === 0 || state === 1) {
        if (rec) await op(RecorderCmd.Stop);
        else kawai(MUSIC_CMD.Stop);
      }
      // a MID becomes a KSO file next to it first (ui/midikso.ts); what the converter had to leave out is counted
      let file = name;
      let left: string[] = [];
      if (kindOf(name) === "MID") {
        const made: { v: { name: string; warnings: string[] } | null } = { v: null };
        const ok = await usb.run(t("Convert to a piano song"), async () => {
          try {
            made.v = await midToKsoFile(ctx.files()!, name, ctx.usbFiles);
          } catch (e) {
            throw e instanceof KsoError ? new Error(t(KSO_TEXT[e.code])) : e;
          }
        }, true);
        if (!ok || !made.v) return;
        file = made.v.name;
        left = made.v.warnings;
      }
      const lossy = left.length ? ` ${t("{n} details a piano song cannot hold were left out or rounded.", { n: left.length })}` : "";
      note.title = left.join("; ");
      if (!(await usb.run(t("Import"), () => ctx.files()!.load(file, n), false))) return;
      put().set(name, n);
      // the song now holds the file: the song table says so on OpenGrand; on the stock it is noted here (its length
      // comes with the select)
      if (!(rec && (await readNg()))) {
        songs[n] = { n, hasData: true, bars: null, parts: null };
        renderSongs();
      }
      if (!andPlay) {
        note.textContent = t("{name} is now piano song {n}.", { name: leaf, n: n + 1 }) + lossy;
        return;
      }
      // chosen again even if it was: the piano loads what the file put there
      if (!(await pick(n, true))) return;
      await play(true);
      note.textContent = t("Playing {name} from piano song {n}.", { name: leaf, n: n + 1 }) + lossy;
    };
    // tapped before in this session and the song still holds something: that song again, no second copy
    const was = put().get(name);
    if (andPlay && was !== undefined && songs[was]?.hasData) {
      if (usbAnyRunning(pb)) await usb.stop();
      if (!(await pick(was, true))) return;
      await play(true);
      note.textContent = t("Playing {name} from piano song {n}.", { name: leaf, n: was + 1 });
      return;
    }
    const to = emptySong();
    if (to >= 0) return go(to);
    const n = sel >= 0 ? sel : SONGS - 1;
    // the stock before a scan: the songs are not known - say that, not that none is empty
    const why = songs.some((i) => i) ? t("No piano song is empty.") : t("The piano's songs have not been read yet (Scan reads them).");
    const ask = andPlay ? "Playing {name} puts it into song {n} and replaces what is there." : "{name} goes into song {n} and replaces what is there.";
    askOver(`${why} ${t(ask, { name: leaf, n: n + 1 })}`, t("Replace song {n}", { n: n + 1 }), "into-confirm", () => void go(n));
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
      // one way out, one format list (ui/savesheet.ts): MID, KSO (the piano's own format - a lossless backup that
      // loads back with 24 09), MP3, WAV
      KINDS.piano.saveTo.length ? h("button", { "data-act": "song-save", onclick: () => saved.open(n) }, t("Save to USB")) : null,
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
          // the same badge a USB file's row has (rule 1)
          h("span", { class: "file-ext song-kind" }, t(KINDS.piano.badge)),
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
    // field 2026-11-23: right after a USB stop the bar said 「재생 중 · 곡 2 · 13/13마디」 for a second or two - the USB
    // player's last words taken as the piano song's. While MusicMode is 3 or a USB stop runs they are not ours.
    if (pb.stopping || pb.recording) return;
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
      ngMode = st.musicMode;
      canDelete = st.deletes;
      // in MusicMode 3, and while a USB stop runs, the state and the position are a USB player's (or the audio
      // recorder's), not the piano song's (field 2026-11-23, see the Kawai path above): the song is at rest
      const usbs = st.musicMode === MUSIC_MODE.UsbMemory || pb.stopping || pb.recording;
      if (usbs) {
        if (!isRecState(state) || st.musicMode === MUSIC_MODE.UsbMemory) state = 2;
        bar = 0;
      } else {
        if (st.state <= 6) state = st.state;
        bar = st.bar;
        maxBars = st.maxBars;
      }
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
      if (n.event && n.event.cmd === RecorderCmd.Select) for (const w of [...selectWaiters]) w();
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
