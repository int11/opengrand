// Recorder tab (ui/recorder.ts): what the piano says about its recorder, without the page - the state, the position,
// the selected song, the song table check and the record part - and what the player bar shows for the piano song
// (view()). The USB side shows through the same PlayerSource (ui/recusb.ts).
import { t } from "../i18n.ts";
import type { NgRecorder } from "../protocol/ngext.ts";
import { SONGS, type SongInfo } from "../protocol/songs.ts";
import { isRecState, type PlayerView } from "./screens/songctl.ts";
import { KINDS } from "./songkinds.ts";

export type ListId = "internal" | "usb";

/** What the player bar shows and where ⏮ ⏭ go: the piano song or a USB file. */
export interface PlayerSource {
  view(): PlayerView;
  step(d: -1 | 1): void;
}

/** Why nothing may be written to the piano's songs (op status byte 19): not checked yet / the check failed. */
export const TABLE_TEXT = {
  unread: "The piano has not checked its song list yet.",
  bad: "The piano's song list is not valid. Check it before switching the piano off.",
} as const;

export interface PianoModel {
  /** the piano's ten songs (ui/files.ts songCache - the same array the cache keeps) */
  readonly songs: (SongInfo | null)[];
  // state: Music report 32 52 08 / op 1E status (STATE_TEXT index); null until the piano says something
  state: number | null;
  // where the piano song is (bar, 1-based; 0 = not said) and - op status only - the bars of what is selected to play
  bar: number;
  maxBars: number;
  // the song's own tempo (op status bytes 10-11; 0 = not known): the options sheet's song tempo reads a share of it
  tempoOrig: number;
  ngMode: number | undefined;
  // selected recorder song (0-based; -1 = not known)
  sel: number;
  // OpenGrand playback control (op 0x1E): null on the stock and on a payload without it
  rec: NgRecorder | null;
  // the core's record control (feat3 0x01): standby / start / end as op 0x1E cmd 8 / 11 / 2
  recCtl: boolean;
  // op status byte 19, the song table check: 0 OK, 0xFF not read yet, else broken; null = no status yet
  tableStore: number | null;
  // op status byte 4: the part a recording goes to (0 part 1, 1 part 2); null = no status yet
  recPart: number | null;
  canDelete: boolean;
  /** Why the piano's songs must not be written now (null: they may): OpenGrand reports its song table check in the op
   *  status (byte 19) - until it says 0 no song is known to be empty. The stock does not say (null). */
  tableReason(): string | null;
  /** what a song holds, as its row and the bar say it */
  songText(i: SongInfo | null): string;
  /** The song a new recording or a file goes to without asking: the chosen one if it is empty, else the first empty
   *  one; -1 = none is known to be empty. */
  emptySong(): number;
  /** The part ● records into: the piano's (op status byte 4 - ● sends 「the part now」 and IntSongPart 7F leaves it as
   *  it was), else IntSongPart 0 / 1 as last said; null = not known. */
  recPartNow(): number | null;
  /** Song n is known to hold no part 1: empty, or its tracks without part 1 (song list flag bit 0). */
  noPart1(n: number): boolean;
  /** the bar for the piano song */
  view(): PlayerView;
}

/** partNow: IntSongPart as the parameter mirror holds it; sel: the piano's selected song as known at the start */
export function pianoModel(songs: (SongInfo | null)[], partNow: () => number | undefined, sel: number): PianoModel {
  // a song in part 1 alone - the usual recording - says nothing of parts (owner 2026-11-23: 「파트」 meant nothing)
  const partsText = (i: SongInfo) => (i.parts && i.parts[1] ? [i.parts[0] ? t("Part 1") : "", t("Part 2")].filter(Boolean).join(" + ") : "");
  const m: PianoModel = {
    songs, state: null, bar: 0, maxBars: 0, tempoOrig: 0, ngMode: undefined, sel, rec: null, recCtl: false, tableStore: null,
    recPart: null, canDelete: false,
    tableReason() {
      if (!m.rec || m.tableStore === 0) return null;
      return t(m.tableStore === null || m.tableStore === 0xff ? TABLE_TEXT.unread : TABLE_TEXT.bad);
    },
    songText(i) {
      if (!i) return m.rec && m.tableReason() ? t("Unknown") : t("not scanned");
      if (!i.hasData) return t("Empty");
      return [i.bars !== null ? t("{n} bars", { n: i.bars }) : "", partsText(i)].filter(Boolean).join(" · ");
    },
    emptySong: () => (m.tableReason() ? -1
      : m.sel >= 0 && songs[m.sel] && !songs[m.sel]!.hasData ? m.sel : songs.findIndex((i) => i !== null && !i.hasData)),
    recPartNow() {
      if (m.rec && m.recPart !== null) return m.recPart;
      const v = partNow();
      return v === 0 || v === 1 ? v : null;
    },
    noPart1(n) {
      const i = songs[n];
      return !!i && (!i.hasData || (!!i.parts && !i.parts[0]));
    },
    view() {
      const { sel, state, bar, rec } = m;
      const i = sel < 0 ? null : songs[sel];
      const recording = isRecState(state);
      const total = recording ? null : (rec && m.maxBars) || i?.bars || null;
      const moving = state === 0 || state === 1;
      return {
        state,
        title: sel < 0 ? t("Choose a song below.") : t("Song {n}", { n: sel + 1 }),
        sub: sel < 0 ? "" : m.songText(i),
        unit: "bar",
        // stopped = at the first bar
        pos: recording ? bar || null : moving && bar ? (total ? Math.min(bar, total) : bar) : total ? 1 : null,
        total,
        seekable: KINDS.piano.seek && !!rec && !recording && sel >= 0 && state !== null,
        main: state === 1 || state === 4 || state === 6 ? (KINDS.piano.pause ? "pause" : "stop") : "play",
        prev: !recording && sel > 0, next: !recording && sel < SONGS - 1,
        part: KINDS.piano.parts,
        tempoBase: rec && sel >= 0 ? m.tempoOrig : null,
      };
    },
  };
  return m;
}
