// Recorder tab (ui/recorder.ts): what goes to the piano - op 0x1E commands (with the piano's refusal under the bar) and
// the Kawai bytes: Music 32 52 08, MusicMode 31 52 1D, IntSongPart 10 52 01.
import { t } from "../i18n.ts";
import { BUSY_RETRY } from "../protocol/fileaccess.ts";
import { findParam } from "../protocol/codecs.ts";
import { REC_OVERWRITE, REC_PART_NOW, RecorderCmd, RecorderRc } from "../protocol/ngext.ts";
import type { ParamStore } from "../state/store.ts";
import type { PianoModel } from "./recmodel.ts";
import { MUSIC_CMD, MUSIC_MODE } from "./screens/songctl.ts";
import type { UsbPlayback } from "./usbplay.ts";

/** Refusals of the piano's playback control (OpenGrand 0x7D op 0x1E rc - REMOTE_PROTOCOL.md section 10.9). The piano
 * judges every command the same way for the panel and this app; the panel shows the piano's own word. */
export const PIANO_RC: Record<number, string> = {
  [RecorderRc.Busy]: "The piano is busy, try again in a moment.",
  [RecorderRc.Recording]: "The piano is recording: playback waits until it stops.",
  [RecorderRc.Pending]: "Wait for the previous command.",
  [RecorderRc.NoData]: "Nothing recorded in this song.",
  [RecorderRc.OtherPlayer]: "Another player mode is active on the piano.",
  [RecorderRc.NotBound]: "The piano is controlled by another device.",
  [RecorderRc.Wrong]: "The piano cannot do that in this state.",
  [RecorderRc.Exists]: "This part already has a recording.",
};

/** the part a recording goes to: the piano's now, or part 1 */
export type RecPart = 0 | typeof REC_PART_NOW;

export interface RecSender {
  /** one Kawai Music value (32 52 08) */
  kawai(v: number): void;
  /** MusicMode 2 (recorder) when the piano is - or may be - in another mode. */
  ensureMode(): Promise<void>;
  /** One op 0x1E command; the piano's refusal (rc) is said under the transport. true = taken */
  op(opCmd: number, arg?: number): Promise<boolean>;
  /** Rec: record standby (op cmd 8 with the record control, else Music 3). overwrite: the user agreed to record over
   *  what the song may hold (cmd 8 refuses a recorded part without it); part: the piano's part now, or part 1 */
  standby(overwrite: boolean, part?: RecPart): void;
  /** IntSongPart 0: the part Music 3 records into (the stock, a core without the record control) */
  partOne(): Promise<unknown>;
}

/** say: the line under the bar */
export function recSender(store: ParamStore, pb: UsbPlayback, m: PianoModel, say: (text: string) => void): RecSender {
  const music = findParam("Music", 0x32)!;
  const partP = findParam("IntSongPart")!;
  const modeP = findParam("MusicMode", 0x31)!;
  const kawai = (v: number) => void store.command(music, 0x7f, [v]);
  // Busy (rc 3): the core's song-table write window is open (an import, a save, the end of a recording - REMOTE_PROTOCOL
  // R3) and closes when the NOR write ends: asked again a few times (field 2026-11-24: Play right after a KSO import)
  const op = async (opCmd: number, arg = 0): Promise<boolean> => {
    let rc = await m.rec!.command(opCmd, arg);
    for (let i = 0; rc === RecorderRc.Busy && i < BUSY_RETRY.tries; i++) {
      await new Promise((r) => setTimeout(r, BUSY_RETRY.ms));
      rc = await m.rec!.command(opCmd, arg);
    }
    say(rc === null ? t("No answer from the piano.")
      : rc <= RecorderRc.Nop ? "" : t(PIANO_RC[rc] ?? "The piano rejected this value."));
    return rc !== null && rc <= RecorderRc.Nop;
  };
  return {
    kawai,
    ensureMode: async () => {
      if (pb.mode === MUSIC_MODE.Recorder || (pb.mode === undefined && m.ngMode === MUSIC_MODE.Recorder)) return;
      pb.mode = MUSIC_MODE.Recorder;
      m.ngMode = MUSIC_MODE.Recorder;
      await store.command(modeP, 0x7f, [MUSIC_MODE.Recorder]);
    },
    op,
    standby: (overwrite, part = REC_PART_NOW) => (m.recCtl ? void op(RecorderCmd.RecStandby, part | (overwrite ? REC_OVERWRITE : 0)) : kawai(MUSIC_CMD.Rec)),
    partOne: () => store.set(partP, 0x7f, [0]),
  };
}
