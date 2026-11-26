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
// refusal text.
// Recording (field incident 2026-11-24: ▶ in standby sent Music 1, which does not start a recording; a raw Music 4
// sent by hand then committed a broken song and the song table failed its check):
//   with the core's record control (Identify feat3 0x01, "recordctl")  ● = op cmd 8 (standby with metronome - what
//            Music 3 was), ▶ in standby / count-in = op cmd 11 (start now), ■ / pause while recording or waiting =
//            op cmd 2 / 3 (the core ends the recording once)
//   without it (the stock, an older payload)  ● = Music 3, ■ = Music 2, pause = Music 0. ▶ in standby sends nothing
//            and says to play a key: the stock's Music 1 in state 3 plays the song instead (stock FUN_4016A050)
//            and the stock's remote start, Music 4, is never sent in MusicMode 2 by this app (the stock takes it
//            in any state, also without a standby; the incident's take was opened with it by hand). With the record
//            control the core judges a Kawai Music 4 like cmd 11 - the app sends the op itself.
// Part 2 of a song with no part 1 (the same incident: the stock's commit of such a take ended without a key leaves the
// song's part-1 slot pointing past the song - firmware R3 mends it): ● asks first - record into part 1 instead (op cmd 8
// with part 0, on the stock IntSongPart 0 before Music 3), into part 2 anyway, or cancel. The part ● records into is the
// piano's (op status byte 4: IntSongPart 7F leaves it as it was), on the stock IntSongPart 0 / 1 as last said; part 1
// is the song list's flag bit 0 (track slot 0 present) - an empty song has none.
// A broken song table (op status byte 19 not 0; the song list then reads all zero) leaves every song's state unknown:
// the list says so, and Rec, the automatic empty-song choice, putting a file into the piano, delete and save are
// refused with that reason.
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
// The parts: ui/recmodel.ts (what the piano says, the bar's view - no page), ui/recsend.ts (op 0x1E and the Kawai
// bytes), ui/recsongs.ts (the song rows), ui/recusb.ts (the USB memory as the bar's other source); this file wires them
// to the piano's notices and holds what a tap does.
import { t } from "../i18n.ts";
import { findParam, progress } from "../protocol/codecs.ts";
import {
  NgClient, ngHas, NgRecorder, parseRecorderNotice, REC_PART_NOW, RecorderCmd, RecorderEvent,
  type RecorderSong, type RecorderStatus,
} from "../protocol/ngext.ts";
import { SONGS } from "../protocol/songs.ts";
import { CONFIRM } from "./confirm2.ts";
import type { AppContext } from "./context.ts";
import { h } from "./dom.ts";
import { type FileLock, songCache, songInfoOf, usbPane } from "./files.ts";
import { identifyShared } from "./identify.ts";
import { whileShown } from "./screens/common.ts";
import { songSelectData } from "./screens/music.ts";
import { isRecState, MUSIC_CMD, MUSIC_MODE, playerBar, type PlayerView, STATE_TEXT } from "./screens/songctl.ts";
import { KsoError } from "../protocol/kso.ts";
import { KSO_TEXT, midToKsoFile } from "./midikso.ts";
import { type ListId, pianoModel, type PlayerSource, TABLE_TEXT } from "./recmodel.ts";
import { PIANO_RC, type RecPart, recSender } from "./recsend.ts";
import { songList } from "./recsongs.ts";
import { usbSource } from "./recusb.ts";
import { saveSheet } from "./savesheet.ts";
import { KINDS, kindOf } from "./songkinds.ts";
import { usbAnyRunning, usbPlayback } from "./usbplay.ts";

/** ● would record part 2 of a song that has no part 1 */
const PART2_TEXT = "Song {n} has no part 1, and this take goes into part 2. Record into part 1 instead?";

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
  "Play a key to start recording.", ...Object.values(TABLE_TEXT), "Unknown",
  "Previous song", "Next song", "Go to bar", "More", "Pause", "Stop", "Close",
  PART2_TEXT, "Record into part 1", "Record into part 2",
];

/** The list last shown and what the bar last held, per context (the tab is rebuilt on every visit and language change). */
const shownList = new WeakMap<AppContext, ListId>();
const shownSource = new WeakMap<AppContext, ListId>();
/** The piano song a USB file was put into (a KSO tap), per context and session: the next tap plays that song again
 *  instead of filling another one. */
const putInto = new WeakMap<AppContext, { session: unknown; at: Map<string, number> }>();

export function recorderScreen(ctx: AppContext): HTMLElement {
  const store = ctx.store;
  const partP = findParam("IntSongPart")!;
  const songP = findParam("SongSelect")!;
  const lock: FileLock = { busy: false };
  const cache = songCache(ctx);
  const songs = cache.songs;
  // the piano's MusicMode as it last said or this app set (ui/usbplay.ts), and the USB players
  const pb = usbPlayback(ctx);

  // ---- model: what the piano says (ui/recmodel.ts) ----
  const stockSel = () => {
    const d = store.get(songP, 0x7f);
    return d && d[0] === 0 && d.length >= 3 ? (d[1] << 7) | d[2] : -1;
  };
  const m = pianoModel(songs, () => store.get(partP, 0x7f)?.[0], stockSel());
  // what the bar holds when no USB file plays: the piano song, or the file chosen in the USB list
  let src: ListId = shownSource.get(ctx) ?? "internal";
  const setSrc = (v: ListId) => {
    src = v;
    shownSource.set(ctx, v);
  };

  const usb = usbPane(ctx, lock, {
    intoPiano: (name, andPlay) => void intoPiano(name, andPlay),
    // a USB file never plays over a piano song: the song is stopped first (field 2026-11-27 on the panel: a piano song
    // played on under the USB file - MusicMode 3 does not stop the recorder's playback). The stock panel never has
    // both: leaving its player sends Music Reset (stock UI app.js exitPlayer).
    beforePlay: async () => {
      if (isRecState(m.state)) {
        note.textContent = t("Stop the recording first.");
        return false;
      }
      if (m.state === 0 || m.state === 1) {
        if (m.rec) await op(RecorderCmd.Stop);
        else {
          await ensureMode();
          await store.command(findParam("Music", 0x32)!, 0x7f, [MUSIC_CMD.Stop]);
        }
        m.state = 2;
        m.bar = 0;
      }
      return true;
    },
    onPick: () => {
      setSrc("usb");
      note.textContent = "";
      renderNow();
    },
  });
  const usbSrc = usbSource(ctx, pb, usb, () => m.state, () => src);
  const usbView = () => usbSrc.active();

  // ---- the player bar ----
  const hintText = () => t(m.recCtl ? "Play a key or press Play to start recording." : "Play a key to start recording.");
  const hint = h("p", { class: "muted", id: "rec-hint", hidden: true }, hintText());
  const note = h("p", { class: "muted", id: "rec-note", role: "status" });
  const over = h("div", { class: "confirm-ask", id: "rec-overwrite", role: "alert", hidden: true });
  let overTimer: ReturnType<typeof setTimeout> | undefined;
  const hideOver = () => {
    clearTimeout(overTimer);
    over.hidden = true;
  };
  const send = recSender(store, pb, m, (text) => (note.textContent = text));
  const { kawai, ensureMode, op, standby } = send;
  // the bar's two sources: the piano song (⏮ ⏭ = the neighbouring song number) and the USB file
  const piano: PlayerSource = {
    view: () => m.view(),
    step: (d) => {
      const n = m.sel < 0 ? 0 : m.sel + d;
      if (n >= 0 && n < SONGS) void pick(n);
    },
  };
  const source = (): PlayerSource => (usbView() ? usbSrc : piano);
  const view = (): PlayerView => source().view();
  // a recording that ran in a song (-1: none): when it stops, the sheet says where it is and offers the USB memory - on
  // the device whose user pressed Rec only (field 2026-11-27: recorded from the desktop, the sheet also opened on the
  // panel, which every controller's song notices reach)
  let recSong = -1;
  let recByMe = false;
  let standbyAt = 0; // when this device asked for the standby (a standby that never became a recording stops counting after 10 s)
  const renderNow = () => {
    hint.hidden = m.state !== 3 && m.state !== 5;
    const ht = hintText();
    if (hint.textContent !== ht) hint.textContent = ht;
    player.set(view());
    if (m.state === 4) recSong = m.sel;
    else if (recSong >= 0 && !isRecState(m.state)) {
      const n = recSong;
      const mine = recByMe;
      recSong = -1;
      recByMe = false;
      if (mine && m.state === 2 && typeof document !== "undefined" && el.isConnected) openSave(n, true);
    } else if (recByMe && !isRecState(m.state) && recSong < 0 && standbyAt && Date.now() - standbyAt > 10_000) recByMe = false; // a standby that never became a recording
  };
  /** true = refused (the reason is under the bar) */
  const refuseTable = (): boolean => {
    const why = m.tableReason();
    if (why) {
      hideOver();
      note.textContent = why;
      // ask again (a lost answer, a check not run yet, a recovery): a good status lifts the refusal
      void m.rec?.status().then((st) => st && onStatusRef?.(st));
    }
    return !!why;
  };
  /** 「USB로 저장」 - not while the song table is broken (what would be read is not known) */
  const openSave = (n: number, recorded = false) => {
    if (refuseTable()) return;
    saved.open(n, recorded);
  };

  // Play / pause / stop: op 0x1E where the piano has it (it answers why not); while a recording runs or waits the op only
  // with the record control (feat3 0x01), else - and on the stock - the Kawai Music bytes. A USB playback is stopped
  // first.
  // piano: the piano song whatever the bar showed (a song row's tap - also while a USB stop is still being confirmed)
  const play = async (piano = false) => {
    hideOver();
    note.textContent = "";
    if (isRecState(m.state)) {
      if (m.state === 4) return; // records already
      // standby / count-in: the core starts the recording (cmd 11). Never Music 4 (see the head of this file); Music 1
      // would play the song on the stock - there the first key starts the recording
      recByMe = true;
      standbyAt = Date.now();
      if (m.recCtl) await op(RecorderCmd.RecStart);
      else note.textContent = t("Play a key to start recording.");
      return;
    }
    if (!piano && usbView()) {
      // the USB file chosen in the list (MusicMode 3, 24 08, Play - ui/usbplay.ts)
      const name = usb.selected();
      if (name === null) note.textContent = t("Choose a song first.");
      else await usb.play(name);
      return;
    }
    if (usbAnyRunning(pb)) await usb.stop();
    await ensureMode();
    if (m.rec) await op(RecorderCmd.Play);
    else kawai(MUSIC_CMD.Play);
  };
  // the op also while a recording runs or waits where the core controls recording (cmd 3 ends it, like the panel)
  const opNow = () => !!m.rec && (!isRecState(m.state) || m.recCtl);
  const pause = () => {
    hideOver();
    if (opNow()) void op(RecorderCmd.Pause);
    else kawai(MUSIC_CMD.Pause);
  };
  const stop = () => {
    hideOver();
    // the USB players: whatever runs, and - with a USB file in the bar - also what this app does not know of
    if (usbView()) return void usb.stop();
    if (opNow()) void op(RecorderCmd.Stop);
    else kawai(MUSIC_CMD.Stop);
  };
  // the progress line's dot let go on a bar: op 0x1E cmd 7 (the only jump - see the head of this file)
  const seek = async (to: number) => {
    if (!m.rec || isRecState(m.state)) return;
    note.textContent = "";
    const was = m.bar;
    m.bar = to; // shown until the piano's next position notice
    renderNow();
    await ensureMode();
    if (await op(RecorderCmd.Seek, to)) return;
    m.bar = was; // refused: the dot goes back, the reason is under the buttons
    renderNow();
  };
  // ⏮ ⏭: the neighbouring song number, or the neighbouring USB file
  const step = (d: -1 | 1) => source().step(d);
  /** A question under the bar: its buttons, then Cancel. */
  const ask = (text: string, btns: { label: string; act: string; cls?: string; run: () => void }[]) => {
    over.replaceChildren(
      h("p", {}, text),
      h("div", { class: "tp-row" },
        ...btns.map((b) => h("button", { class: b.cls, "data-act": b.act, onclick: () => { hideOver(); b.run(); } }, b.label)),
        h("button", { "data-act": "rec-cancel", onclick: hideOver }, t("Cancel"))));
    over.hidden = false;
    clearTimeout(overTimer);
    overTimer = setTimeout(hideOver, CONFIRM.ms);
  };
  /** A question under the bar with one red button (record over a song, replace a song with a file). */
  const askOver = (text: string, label: string, act: string, run: () => void) => ask(text, [{ label, act, cls: "danger", run }]);
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
  // Rec: record standby (op cmd 8 with the record control, else Music 3) - in an empty song this app chooses (rule 2);
  // only when none is empty, or the song table is not known (stock, not scanned), the chosen song is asked about first.
  // overwrite: the user agreed to record over what the song may hold (cmd 8 refuses a recorded part without it)
  // part: the part the piano records now (REC_PART_NOW), or part 1 chosen in the part question
  const startRec = async (overwrite = false, part: RecPart = REC_PART_NOW) => {
    hideOver();
    if (refuseTable()) return;
    if (usbAnyRunning(pb)) await usb.stop();
    await ensureMode();
    if (refuseTable()) return;
    // the stock (and a core without the record control): IntSongPart 0 sets the part Music 3 records into
    if (part === 0 && !m.recCtl) await send.partOne();
    recByMe = true;
    standbyAt = Date.now();
    standby(overwrite, part);
  };
  /** ● into song n: part 2 of a song with no part 1 is asked about first (see the head of this file), before anything
   *  is sent - go gets the part to record into. */
  const partGuard = (n: number, go: (part: RecPart) => void) => {
    if (m.recPartNow() !== 1 || !m.noPart1(n)) return go(REC_PART_NOW);
    ask(t(PART2_TEXT, { n: n + 1 }), [
      { label: `● ${t("Record into part 1")}`, act: "rec-part1", cls: "primary", run: () => go(0) },
      { label: t("Record into part 2"), act: "rec-part2", run: () => go(REC_PART_NOW) },
    ]);
  };
  const recTap = () => {
    note.textContent = "";
    if (isRecState(m.state)) return standby(false);
    if (refuseTable()) return;
    // a recording goes to the piano song: the bar shows it from here on
    setSrc("internal");
    renderNow();
    const sel = m.sel;
    const i = sel < 0 ? null : songs[sel];
    const part = store.get(partP, 0x7f)?.[0];
    // one part alone was chosen in the options and the chosen song has room in it: the second hand of that song
    if (i?.hasData && i.parts && (part === 0 || part === 1) && !i.parts[part]) return partGuard(sel, (p) => void startRec(false, p));
    const to = m.emptySong();
    if (to >= 0) return partGuard(to, (p) => void (async () => {
      if (to !== m.sel) {
        // OpenGrand: the piano confirms one command at a time - Rec goes out when the song choice is done (panelsim
        // 2026-11-23: a Music 3 right behind the op select was dropped)
        const done = m.rec ? selectDone() : null;
        if (!(await pick(to))) return;
        await done;
      }
      if (m.sel === to) await startRec(false, p);
    })());
    if (sel < 0) {
      note.textContent = songs.some((s) => s) ? t("All 10 piano songs are used. Choose the song to record over below, then press Rec.") : t("Choose a song first.");
      return;
    }
    const q = i ? "Song {n} already has a recording. Recording again replaces it." : "Song {n} may already have a recording. Recording replaces it.";
    const n = sel;
    askOver(t(q, { n: n + 1 }), `● ${t("Record over it")}`, "rec-confirm", () => partGuard(n, (p) => void startRec(true, p)));
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
      if (m.state === 0 || m.state === 1) {
        if (m.rec) await op(RecorderCmd.Stop);
        else kawai(MUSIC_CMD.Stop);
      }
    },
    refresh: () => usb.reload(),
    get out() { return list.status; },
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

  // ---- the piano's ten songs (ui/recsongs.ts) ----
  const list = songList(ctx, lock, m, pb, {
    tap: (n) => void tapSong(n),
    save: (n) => openSave(n),
    refuseTable,
    readNg: () => readNg(),
    restoreOf: () => ({ song: Math.max(0, stockSel()), part: store.get(partP, 0x7f)?.[0] }),
  });
  const renderSongs = list.render;
  const fillFromNg = (rows: RecorderSong[]) => {
    // a zero-length entry is not known (songInfoOf -> null); with a bad table nothing is
    const bad = !!m.tableReason();
    listStale = bad;
    rows.slice(0, SONGS).forEach((g, n) => (songs[n] = bad ? null : songInfoOf(g, n)));
    list.status.textContent = list.recordedText();
    renderSongs();
    // entries that say nothing: ask the piano how its table is (the 5E 00 says why)
    if (!bad && rows.some((g) => g.unknown)) void m.rec?.status().then((st) => st && onStatusRef?.(st));
  };
  let onStatusRef: ((st: RecorderStatus) => void) | null = null;
  // the list was taken while the table was not known to be good (rows blank): read it again once it is
  let listStale = false;
  const readNg = async (): Promise<boolean> => {
    const rows = await m.rec?.songs();
    if (!rows) return false;
    fillFromNg(rows);
    return true;
  };
  // the stock tells a selected song's length with MaxTime (10 52 18) right after the SongSelect: this app's own choice
  // is noted here so that answer fills the row (a scan's answers are not this)
  let expectMax = -1;
  let expectTimer: ReturnType<typeof setTimeout> | undefined;
  // a tap on a row (or ⏮ ⏭): that song is the piano's selected song and what the bar holds
  const pick = async (n: number, again = false) => {
    if (n === m.sel && !again) {
      if (src !== "internal") {
        setSrc("internal");
        renderNow();
      }
      return;
    }
    if (isRecState(m.state)) {
      note.textContent = t("Stop the recording before choosing another song.");
      return;
    }
    hideOver();
    note.textContent = "";
    const before = m.sel;
    m.sel = n;
    m.bar = 0;
    setSrc("internal");
    renderSongs();
    renderNow();
    await ensureMode();
    if (m.rec) {
      if ((await op(RecorderCmd.Select, n)) || m.sel !== n) return true;
      m.sel = before; // refused: the piano's song did not change
      renderSongs();
      renderNow();
    } else {
      expectMax = n;
      clearTimeout(expectTimer);
      expectTimer = setTimeout(() => (expectMax = -1), 2000);
      void store.set(songP, 0x7f, songSelectData(n));
    }
    return m.sel === n;
  };
  // A file the piano takes into a recorder song (a KSO; rule 1): into an empty piano song - asked only when none is
  // empty - with 24 09 <song> 03, and for a row's tap played there: the bar then shows that piano song.
  const intoPiano = async (name: string, andPlay: boolean) => {
    hideOver();
    note.textContent = "";
    if (isRecState(m.state)) {
      note.textContent = t("Stop the recording first.");
      return;
    }
    if (refuseTable()) return;
    const leaf = name.slice(name.lastIndexOf("\\") + 1);
    const put = () => {
      let c = putInto.get(ctx);
      if (!c || c.session !== ctx.session()) putInto.set(ctx, (c = { session: ctx.session(), at: new Map() }));
      return c.at;
    };
    const go = async (n: number) => {
      if (refuseTable()) return; // asked again: the table may have failed while the question was shown
      if (usbAnyRunning(pb)) await usb.stop();
      if (m.state === 0 || m.state === 1) {
        if (m.rec) await op(RecorderCmd.Stop);
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
      if (refuseTable()) return;
      if (!(await usb.run(t("Import"), () => ctx.files()!.load(file, n), false))) return;
      put().set(name, n);
      // the song now holds the file: the song table says so on OpenGrand; on the stock it is noted here (its length
      // comes with the select)
      if (!(m.rec && (await readNg()))) {
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
    const to = m.emptySong();
    if (to >= 0) return go(to);
    const n = m.sel >= 0 ? m.sel : SONGS - 1;
    // the stock before a scan: the songs are not known - say that, not that none is empty
    const why = songs.some((i) => i) ? t("No piano song is empty.") : t("The piano's songs have not been read yet (Scan reads them).");
    const q = andPlay ? "Playing {name} puts it into song {n} and replaces what is there." : "{name} goes into song {n} and replaces what is there.";
    askOver(`${why} ${t(q, { name: leaf, n: n + 1 })}`, t("Replace song {n}", { n: n + 1 }), "into-confirm", () => void go(n));
  };
  // a tap on a song row: choose it and - when it holds a recording - play it, as a media player does (owner 2026-11-23:
  // a tap that only chose read as 「재생이 안 된다」). An empty song is only chosen: it is where Rec records. The
  // playing song's row does nothing; during a recording the choice is refused (pick says why).
  const tapSong = async (n: number) => {
    if (n === m.sel && (m.state === 1 || isRecState(m.state)) && !usbAnyRunning(pb)) return;
    const before = m.sel;
    await pick(n);
    if (m.sel !== n || isRecState(m.state)) return; // refused
    if (!songs[n]?.hasData) return;
    if (n === before && m.state === 1 && !usbAnyRunning(pb)) return;
    await play(true);
  };

  // ---- [Piano songs | USB memory] ----
  let shown: ListId = shownList.get(ctx) ?? "internal";
  const listBar = h("div", { class: "segmented", id: "rec-lists" },
    ...([["internal", "Piano songs"], ["usb", "USB memory"]] as const).map(([id, label]) =>
      h("button", { "data-list": id, onclick: () => { shown = id; shownList.set(ctx, id); showList(); } }, t(label))));
  const showList = () => {
    listBar.querySelectorAll("button").forEach((b) => b.classList.toggle("on", b.getAttribute("data-list") === shown));
    list.pane.hidden = shown !== "internal";
    usb.el.hidden = shown !== "usb";
  };
  usb.el.setAttribute("data-title", t("USB memory")); // the list's heading where both lists show (styles.css)
  const el = h("div", { class: "screen recorder" }, panel,
    h("section", { class: "card", id: "rec-files" }, listBar, list.pane, usb.el));

  // ---- the piano's word: the parameter mirror, the Music state report, the song length after a select ----
  whileShown(el, store, (k) => {
    if (k === "IntSongPart@127") player.syncPart();
    else if (k === "SongSelect@127" && !m.rec) {
      const n = stockSel();
      if (n < 0 || n === m.sel) return;
      m.sel = n;
      m.bar = 0;
      renderSongs();
      renderNow();
    }
  });
  const recorderMode = () => pb.mode === undefined || pb.mode === MUSIC_MODE.Recorder;
  let seen = false;
  const offMsg = store.onMessage((msg) => {
    if (el.isConnected) seen = true;
    else if (seen) return void offMsg(); // the screen was replaced
    if (msg.v1 !== 0x52) return;
    // field 2026-11-23: right after a USB stop the bar said 「재생 중 · 곡 2 · 13/13마디」 for a second or two - the USB
    // player's last words taken as the piano song's. While MusicMode is 3 or a USB stop runs they are not ours.
    if (pb.stopping || pb.recording) return;
    if (msg.v2 === 0x08 && (msg.fn === 0x10 || msg.fn === 0x32) && msg.data[0] !== undefined && msg.data[0] <= 6 && !m.rec && recorderMode()) {
      // the Music report is the recorder's only in the recorder mode (MusicMode 3: a USB player's - ui/usbplay.ts)
      m.state = msg.data[0];
      if (m.state === 2) m.bar = 0;
      renderSongs();
      renderNow();
    } else if (msg.v2 === 0x03 && (msg.fn === 0x10 || msg.fn === 0x32) && msg.data.length >= 4 && !m.rec && pb.mode === MUSIC_MODE.Recorder) {
      // MusicProgress in the recorder mode, bar form: where the piano song is (the op's status says it on OpenGrand)
      const at = progress.decode(msg.data);
      if ("bar" in at) m.bar = at.bar;
    } else if (msg.v2 === 0x18 && msg.fn === 0x10 && msg.data.length >= 3 && expectMax >= 0 && expectMax === m.sel && !lock.busy && store.get(partP, 0x7f)?.[0] === 0x7f) {
      // MaxTime after this app's SongSelect with both parts on = bars of that song (0: empty) - protocol/songs.ts
      const bars = (msg.data[1] << 7) | msg.data[2];
      const old = songs[m.sel];
      songs[m.sel] = { n: m.sel, hasData: bars !== 0, bars, parts: old && old.hasData && bars !== 0 ? old.parts : null };
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
      m.ngMode = st.musicMode;
      // the op status says the piano's MusicMode: the USB players' tracker (ui/usbplay.ts) takes it too - field
      // 2026-11-24: right after the app started the mode was not known, a piano song played by op 0x1E sends no
      // MusicMode, its bars were taken for the SMF player and the bar said 「USB 재생 중」 (■ only tried USB stops)
      if (st.musicMode !== undefined && pb.mode !== st.musicMode) pb.mode = st.musicMode;
      m.canDelete = st.deletes;
      m.tempoOrig = st.tempoOrig;
      m.recPart = st.recPart;
      // the song table check: anything but 0 = no song's state is known (the list reads zero then); back to 0 = read
      // the list again
      m.tableStore = st.store;
      if (st.store !== 0) {
        songs.fill(null);
        listStale = true;
      } else if (listStale) {
        listStale = false;
        void readNg();
      }
      list.status.textContent = songs.some((i) => i) || m.tableReason() ? list.recordedText() : list.status.textContent;
      // in MusicMode 3, and while a USB stop runs, the state and the position are a USB player's (or the audio
      // recorder's), not the piano song's (field 2026-11-23, see the Kawai path above): the song is at rest
      const usbs = st.musicMode === MUSIC_MODE.UsbMemory || pb.stopping || pb.recording;
      if (usbs) {
        if (!isRecState(m.state) || st.musicMode === MUSIC_MODE.UsbMemory) m.state = 2;
        m.bar = 0;
      } else {
        if (st.state <= 6) m.state = st.state;
        m.bar = st.bar;
        m.maxBars = st.maxBars;
      }
      if (st.song < SONGS && st.song !== m.sel) {
        hideOver();
        m.sel = st.song;
      }
    };
    onStatusRef = (st) => {
      onStatus(st);
      renderSongs();
      renderNow();
    };
    let on = false;
    const off = s.onMidi((msg) => {
      if (el.isConnected) on = true;
      else if (on) return void off(); // the screen was replaced
      const n = parseRecorderNotice(msg);
      if (!n || !m.rec) return;
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
      const rec = new NgRecorder(new NgClient(s));
      m.rec = rec;
      m.recCtl = ngHas(id, "recordctl");
      if (!songs.some((i) => i)) list.status.textContent = t("{op}…", { op: t("Scan") });
      // the status first: its table check (byte 19) says whether the list can be trusted at all. A 1E request also
      // subscribes this controller to 5E 00 / 01 while it stays bound
      const st = await rec.status();
      if (st && ctx.session() === s) onStatus(st);
      if (!(await readNg()) && !songs.some((i) => i) && !m.tableReason()) list.status.textContent = t("Not scanned yet.");
      renderSongs();
      renderNow();
    })();
  }

  renderSongs();
  renderNow();
  showList();
  return el;
}
