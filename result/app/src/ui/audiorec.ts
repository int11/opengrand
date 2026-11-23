// The piano's USB audio recorder, driven for one job: 「Convert to Audio」 - the piano plays a recorder song once
// into a WAV / MP3 file on the USB memory (the MP3 · WAV formats of 「USB로 저장」, ui/savesheet.ts). No DOM here.
//
// The bytes are the stock's (V2.19; the apply function's handler of 10 52 23 at 0x401608D8, the audio recorder's
// engine handler FUN_40123E34, the fn 0x24 worker FUN_4014F364):
//   1. MusicMode 2, SongSelect 10 52 00 <song>, IntSongPart 10 52 01 7F (both parts) - the song the piano will play.
//      The compound frame below takes a song only in MusicMode 2, so it is chosen first.
//   2. 10 52 23 7F 03 <fmt 0 WAV | 1 MP3> 02 7F 7F 7F 03 - MusicMode 3 (USB memory), AudioRecFormat, RecMode 2
//      (convert), Music 3 (record standby) in one frame, as the stock panel sends it. The piano answers 52 1D 03 and
//      52 08 03. Without a USB memory the mode does not change (52 1D says the old one) and Result 0x21 comes.
//   3. 10 52 08 7F 04 - record. The piano opens \kawairectempfile.tmp, starts its audio recorder and then the song
//      (FUN_40166E74 on the recorder's start confirmation); it says 52 08 04 and, once a second, the recorded time
//      as MusicProgress 52 03 [1, ms].
//   4. the song's end stops the recorder by itself (FUN_4016D9B0, RecMode 2 only): 52 08 02. A cancel is Music 2.
//   5. the take is still \kawairectempfile.tmp: 24 07 renames it to the name wanted (the piano has no save of its
//      own; a name that exists would be replaced silently - the caller picks a free one), or 24 06 deletes it.
//   6. RecMode 0 again (left at 2, the end of every piano song would post a stop to the audio recorder) and
//      MusicMode 2, where the Recorder tab lives.
// Music and MusicMode are never sent while a file operation is pending (the stock drops them then).
import { findParam, progress } from "../protocol/codecs.ts";
import { resultCode } from "../protocol/fileaccess.ts";
import type { ParamMessage } from "../protocol/session.ts";
import type { AppContext } from "./context.ts";
import { MUSIC_MODE } from "./screens/songctl.ts";
import { usbPlayback } from "./usbplay.ts";

/** The audio recorder's take until it is renamed (the stock panel's name for it; root of the USB memory). */
export const AUDIO_TEMP = "kawairectempfile.tmp";
export type AudioFormat = "WAV" | "MP3";
/** Timings (ms); tests make them short. step: the piano's word after one command; max: the longest convert. */
export const AUDIOREC = { stepMs: 4_000, maxMs: 3 * 60 * 60_000 };

export type AudioRecWhy = "offline" | "no-usb" | "no-standby" | "no-start" | "no-end";
export class AudioRecError extends Error {
  why: AudioRecWhy;
  constructor(why: AudioRecWhy) {
    super(AUDIOREC_TEXT[why]);
    this.why = why;
  }
}
/** English source strings (translated by the caller). */
export const AUDIOREC_TEXT: Record<AudioRecWhy, string> = {
  offline: "Connect to the piano first.",
  "no-usb": "No USB memory",
  "no-standby": "The piano did not get ready to record.",
  "no-start": "The piano did not start recording.",
  "no-end": "The piano did not say that the song ended.",
};

export interface ConvertJob {
  /** saved: the file is on the USB memory under `name`; cancelled: nothing was kept */
  done: Promise<"saved" | "cancelled">;
  cancel(): void;
}

/** Make `name` (WAV / MP3) on the USB memory from piano song `song` (0-based, must hold a recording). */
export function convertToAudio(ctx: AppContext, o: { song: number; format: AudioFormat; name: string; onProgress?: (ms: number) => void }): ConvertJob {
  let cancelled = false;
  let onCancel = () => {};
  const cancelSignal = new Promise<null>((r) => (onCancel = () => r(null)));
  const run = async (): Promise<"saved" | "cancelled"> => {
    const store = ctx.store;
    const fa = ctx.files();
    const s = ctx.session();
    if (!fa || !s) throw new AudioRecError("offline");
    const modeP = findParam("MusicMode", 0x31)!;
    const music = findParam("Music", 0x32)!;
    const pb = usbPlayback(ctx);
    const waiters = new Set<(m: ParamMessage) => void>();
    const timers = new Set<ReturnType<typeof setTimeout>>();
    let recording = false;
    let armed = false; // the piano is in record standby or records: a stop is owed
    let noMedia = false;
    const off = store.onMessage((m) => {
      if (m.v1 !== 0x52) return;
      if (recording && m.v2 === 0x03 && m.data[0] === 1 && m.data.length >= 4) {
        const at = progress.decode(m.data);
        if ("ms" in at) o.onProgress?.(at.ms);
      }
      for (const w of [...waiters]) w(m);
    });
    const offResult = s.onFrame((f) => {
      if (resultCode(f) === 0x21) noMedia = true;
    });
    /** The piano's next 52 <v2> whose data passes; null after `ms`. Registered before the command goes out. */
    const wait = (v2: number, ok: (d: number[]) => boolean, ms: number) => new Promise<number[] | null>((res) => {
      const w = (m: ParamMessage) => {
        if (m.v2 !== v2 || !ok(m.data)) return;
        clearTimeout(timer);
        timers.delete(timer);
        waiters.delete(w);
        res(m.data);
      };
      const timer = setTimeout(() => {
        timers.delete(timer);
        waiters.delete(w);
        res(null);
      }, ms);
      timers.add(timer);
      waiters.add(w);
    });
    const stopAndDrop = async () => {
      const stopped = wait(0x08, (d) => d[0] === 2 || d[0] === 0, AUDIOREC.stepMs);
      await store.command(music, 0x7f, [2]);
      await stopped;
      armed = false;
      try {
        await fa.delete(AUDIO_TEMP);
      } catch {
        // nothing recorded yet, or the stick is gone: there is no take to drop
      }
    };
    pb.recording = true;
    try {
      // 1. the song, in the recorder mode
      pb.mode = MUSIC_MODE.Recorder;
      await store.command(modeP, 0x7f, [MUSIC_MODE.Recorder]);
      await store.set(findParam("SongSelect")!, 0x7f, [0, (o.song >> 7) & 0x7f, o.song & 0x7f]);
      await store.set(findParam("IntSongPart")!, 0x7f, [0x7f]);
      if (cancelled) return "cancelled";
      // 2. USB memory mode + format + convert + record standby, one frame
      const mode = wait(0x1d, () => true, AUDIOREC.stepMs);
      const standby = wait(0x08, (d) => d[0] === 3, AUDIOREC.stepMs);
      await store.send({ v1: 0x52, v2: 0x23, part: 0x7f, data: [MUSIC_MODE.UsbMemory, o.format === "MP3" ? 1 : 0, 2, 0x7f, 0x7f, 0x7f, 3] });
      const said = await mode;
      if (noMedia || (said && said[0] !== MUSIC_MODE.UsbMemory)) throw new AudioRecError("no-usb");
      if (!(await standby)) throw new AudioRecError(noMedia ? "no-usb" : "no-standby");
      armed = true;
      if (cancelled) return await stopAndDrop().then(() => "cancelled" as const);
      // 3. record: the piano starts the song itself
      const started = wait(0x08, (d) => d[0] === 4, AUDIOREC.stepMs);
      const ended = wait(0x08, (d) => d[0] === 2 || d[0] === 0, AUDIOREC.maxMs);
      await store.command(music, 0x7f, [4]);
      if (!(await started)) throw new AudioRecError(noMedia ? "no-usb" : "no-start");
      recording = true;
      // 4. the song's end stops the recorder - or the user cancels
      const end = await Promise.race([ended, cancelSignal]);
      recording = false;
      if (cancelled) return await stopAndDrop().then(() => "cancelled" as const);
      if (!end) throw new AudioRecError("no-end");
      armed = false;
      // 5. the take gets its name
      await fa.rename(AUDIO_TEMP, o.name);
      return "saved";
    } catch (e) {
      if (armed) await stopAndDrop();
      throw e;
    } finally {
      off();
      offResult();
      for (const timer of timers) clearTimeout(timer); // a wait nobody needs any more (the song's end after a cancel)
      // 6. back to a plain recorder
      await store.send({ v1: 0x52, v2: 0x1c, part: 0x7f, data: [0] });
      pb.mode = MUSIC_MODE.Recorder;
      await store.command(modeP, 0x7f, [MUSIC_MODE.Recorder]);
      pb.sel = undefined; // the piano loaded its take into the audio player
      pb.recording = false;
    }
  };
  return {
    done: run(),
    cancel() {
      cancelled = true;
      onCancel();
    },
  };
}

/** A name for a new file that no file on the USB memory has: BASE.EXT, then BASE_2.EXT, BASE_3.EXT ... */
export function freeName(files: readonly string[], base: string, ext: string): string {
  const have = new Set(files.map((f) => f.toUpperCase()));
  let name = `${base}.${ext}`;
  for (let k = 2; have.has(name.toUpperCase()); k++) name = `${base}_${k}.${ext}`;
  return name;
}
