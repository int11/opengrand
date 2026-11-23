// What each kind of item in the Recorder tab really supports - ONE table (owner 2026-11-23: what the UI shows must
// follow this, not scattered ifs). The player bar (ui/recorder.ts view()), the ⋯ menus (ui/recorder.ts song rows,
// ui/files.ts file rows) and the 「USB로 저장」 sheet (ui/savesheet.ts) read only this.
export type ItemKind = "piano" | "MID" | "MP3" | "WAV" | "KSO";
export type SaveFormat = "MID" | "KSO" | "MP3" | "WAV";

export interface KindCaps {
  /** the row's badge (translated where it is a word) */
  badge: string;
  /** the part choice (options sheet) means something for it */
  parts: boolean;
  /** the progress line can be a bar slider - on a piano with the OpenGrand playback control (op 0x1E cmd 7) */
  seek: boolean;
  /** the main button pauses while it plays (else it stops) */
  pause: boolean;
  /** formats of 「USB로 저장」 (empty: no such button) */
  saveTo: readonly SaveFormat[];
  /** 「피아노에 넣기」 is shown */
  intoPiano: boolean;
  /** direct: the piano's USB player plays the file; via piano song: it is loaded into a recorder song and played there */
  howPlayed: "direct" | "via piano song";
}

/** MID / KSO = the piano writes the song as a file (24 0A); MP3 / WAV = the stock's Convert to Audio (RecMode 2: the
 *  piano plays the song once into its USB audio recorder - ui/audiorec.ts). */
const SAVE_ALL: readonly SaveFormat[] = ["MID", "KSO", "MP3", "WAV"];

export const KINDS: Record<ItemKind, KindCaps> = {
  piano: { badge: "Piano", parts: true, seek: true, pause: true, saveTo: SAVE_ALL, intoPiano: false, howPlayed: "direct" },
  // field 2026-11-23 (PARTTEST.MID, channels 1-4, IntSongPart 7F / 00 / 01 each ACKed): all four notes sounded every
  // time - the USB SMF player ignores the part. intoPiano: through the app's MID -> KSO converter (ui/midikso.ts,
  // protocol/kso.ts) - the piano itself takes only KSO into a recorder song (24 09 answers 0xF0 for a MID).
  MID: { badge: "MID", parts: false, seek: false, pause: false, saveTo: [], intoPiano: true, howPlayed: "direct" },
  MP3: { badge: "MP3", parts: false, seek: false, pause: false, saveTo: [], intoPiano: false, howPlayed: "direct" },
  WAV: { badge: "WAV", parts: false, seek: false, pause: false, saveTo: [], intoPiano: false, howPlayed: "direct" },
  // the stock's USB player does not play a KSO: a tap loads it into a piano song (24 09) and plays that - from then
  // on it is a piano song
  KSO: { badge: "KSO", parts: true, seek: true, pause: true, saveTo: [], intoPiano: true, howPlayed: "via piano song" },
};

/** The kind of a USB file by its extension; null = not something the piano plays. */
export function kindOf(name: string): Exclude<ItemKind, "piano"> | null {
  const ext = name.slice(name.lastIndexOf(".") + 1).toUpperCase();
  return ext === "MID" || ext === "MP3" || ext === "WAV" || ext === "KSO" ? ext : null;
}
