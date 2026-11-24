// Playback control of the piano's internal songs by the core (0x7D op 0x1E, REMOTE_PROTOCOL.md section 10.9;
// firmware result/firmware/ports/cave/recctl_cave.c, core src/recorder/recctl.c - Identify feat2 0x10).
// The piano judges every play / stop / pause / song / part / tempo / seek the same way whoever sends it (the panel,
// this app, Kawai bytes or this op) and runs it through the stock song player. Recording (standby / count-in / start,
// cmd 8-11) only where Identify feat3 0x01 (R2, "recordctl") is set; without it the piano answers rc 2. No saving here.
import { u16, u32 } from "./bytes.ts";
import type { NgClient } from "./client.ts";
import { NgOp, parseNgReply, unpack7 } from "./wire.ts";

export const RecorderCmd = {
  Play: 1, Stop: 2, Pause: 3, Select: 4, Part: 5, Tempo: 6, Seek: 7,
  // R2 (feat3 0x01): arg = part 0 / 1 / 0x7F (the part the piano records now) | REC_OVERWRITE
  RecStandby: 8, RecStandbyQuiet: 9, CountIn: 10,
  /** start recording now - only from standby / count-in (rc 10 otherwise). A recording ends with Stop or Pause. */
  RecStart: 11,
} as const;
/** cmd 8-10 argument bit: the part may already hold a recording (without it: rc 11) */
export const REC_OVERWRITE = 0x100;
/** cmd 8-10 argument: the part the piano records now (IntSongPart) */
export const REC_PART_NOW = 0x7f;
export type RecorderCmdName = keyof typeof RecorderCmd;

/** 5E 02 rc (recctl.h RC_*, + 9 not bound) */
export const RecorderRc = {
  Ok: 0, Nop: 1, BadArg: 2, Busy: 3, Recording: 4, Pending: 5, NoData: 6, OtherPlayer: 7, State: 8, NotBound: 9,
  /** not in this state (start without standby, stop during the count-in) - feat3 0x01 */
  Wrong: 10,
  /** the part already holds a recording and REC_OVERWRITE was not set - feat3 0x01 */
  Exists: 11,
} as const;
/** 5E 03 events */
export const RecorderEvent = { Done: 1, Timeout: 2, Replaced: 3 } as const;

/** the stock recorder state 0x4191EF79 (recview.h) */
export const RECORDER_STATES = ["paused", "playing", "stopped", "rec standby", "recording", "rec standby", "count-in"] as const;

export interface RecorderStatus {
  /** raw stock state: 0 paused / just stopped, 1 playing, 2 stopped, 3-6 recording family */
  state: number;
  playing: boolean;
  recording: boolean;
  /** selected song 0-9 */
  song: number;
  /** byte 4: the part a recording goes to (stock 0x4191EF73: 0 part 1, 1 part 2) - IntSongPart 0 / 1 set it, 7F (both)
   *  leaves it as it was, so it can be part 2 while both parts play */
  recPart: number;
  /** play-part selector: 0 both, 1 part 1, 2 part 2 */
  playSel: number;
  musicMode: number;
  /** busy bits: 1 NOR job, 2 worker, 4 file operation, 8 recording */
  busy: number;
  tempo: number;
  tempoOrig: number;
  maxBars: number;
  /** byte 19: 0 = the song table passes the boot check, 0xFF = not read yet, else recview.h RV_STORE_* (the table is
   *  broken - the song list then reads all zero). Anything but 0 = no song's state is known. */
  store: number;
  /** byte 28: where the table check stopped (meaningful when store is not 0) */
  storeAt: number;
  /** song progress bar, 1-based */
  bar: number;
  /** command waiting for its effect (RecorderCmd value, 0 none) */
  pending: number;
  /** byte 31 bit 0: the piano's core deletes songs itself (InternalSongDelete, firmware recdel.c). Without it the stock
   * delete runs, which re-writes the song from the last take instead of deleting it - the app does not offer Delete then. */
  deletes: boolean;
}

/** The length of an empty song in RecorderData (the ROM's empty-song frame, 0x74 bytes). */
export const EMPTY_SONG_LEN = 0x74;

export interface RecorderSong {
  /** bytes in the RecorderData table */
  length: number;
  bars: number;
  part1: boolean;
  part2: boolean;
  /** only an empty frame: length 0x74 and no track */
  empty: boolean;
  /** the entry says nothing (length 0: the core sends 70 zero bytes when the table check fails, field 2026-11-24) -
   *  neither empty nor recorded */
  unknown: boolean;
}

/** Is the song table good (op status byte 19 = 0)? Anything else - 0xFF not read yet or an RV_STORE_* code - means
 *  no entry may be taken as empty (a recording or an import there could overwrite a song). */
export function recorderTableOk(st: RecorderStatus | null | undefined): boolean {
  return !!st && st.store === 0;
}

export function decodeRecorderStatus(raw: Uint8Array): RecorderStatus | null {
  if (raw.length < 33 || raw[0] !== 1) return null;
  const state = raw[1]!;
  return {
    state, playing: state === 1, recording: state >= 3 && state <= 6, song: raw[3]!, recPart: raw[4]!, playSel: raw[5]!, musicMode: raw[6]!,
    busy: raw[7]!, tempo: u16(raw, 8), tempoOrig: u16(raw, 10), maxBars: u16(raw, 16), store: raw[19]!, storeAt: raw[28]!, bar: u16(raw, 29),
    pending: raw[32]!, deletes: !!(raw[31]! & 1),
  };
}

export function decodeSongList(raw: Uint8Array): RecorderSong[] | null {
  if (raw.length < 70) return null;
  return Array.from({ length: 10 }, (_, i) => {
    const o = 7 * i, f = raw[o + 6]!;
    const s = { length: u32(raw, o), bars: u16(raw, o + 4), part1: !!(f & 1), part2: !!(f & 2) };
    // flags 1 / 2 / 4: part 1, part 2, an extra track - any of them is a recording
    const unknown = s.length === 0;
    return { ...s, unknown, empty: !unknown && s.length === EMPTY_SONG_LEN && !(f & 7) };
  });
}

export interface RecorderNotice {
  status?: RecorderStatus;
  songs?: RecorderSong[];
  /** 5E 03: the command this app sent reached its state (Done), did not (Timeout) or a Stop / Pause took its place */
  event?: { cmd: number; event: number };
}

/** Op 0x1E over an NgClient. Status / list on any port; commands from a bound controller (the core's table). */
export class NgRecorder {
  private readonly ng: NgClient;
  constructor(ng: NgClient) {
    this.ng = ng;
  }
  /** also subscribes this controller to the piano's 5E 00 / 5E 01 notices */
  async status(timeoutMs = 700): Promise<RecorderStatus | null> {
    const d = await this.ng.call(NgOp.Recorder, [0x00], timeoutMs, (x) => x[0] === 0x00);
    return d && d.length >= 2 ? decodeRecorderStatus(unpack7(d.slice(2))) : null;
  }
  async songs(timeoutMs = 700): Promise<RecorderSong[] | null> {
    const d = await this.ng.call(NgOp.Recorder, [0x01], timeoutMs, (x) => x[0] === 0x01);
    return d && d.length >= 2 ? decodeSongList(unpack7(d.slice(2))) : null;
  }
  /** -> the immediate rc (RecorderRc), null = no answer. The outcome comes as a 5E 03 notice. */
  async command(cmd: number, arg = 0, timeoutMs = 700): Promise<number | null> {
    const d = await this.ng.call(NgOp.Recorder, [0x02, cmd, (arg >> 7) & 0x7f, arg & 0x7f], timeoutMs,
      (x) => x[0] === 0x02 && x[2] === cmd);
    return d && d.length >= 3 ? d[1]! : null;
  }
}

/** A 5E frame from the piano (reply or unsolicited notice) -> what it says; null for anything else. */
export function parseRecorderNotice(msg: Uint8Array): RecorderNotice | null {
  const r = parseNgReply(msg);
  if (!r || r.op !== NgOp.Recorder || r.data.length < 2) return null;
  const sub = r.data[0]!;
  if (sub === 0x00) {
    const status = decodeRecorderStatus(unpack7(r.data.slice(2)));
    return status ? { status } : null;
  }
  if (sub === 0x01) {
    const songs = decodeSongList(unpack7(r.data.slice(2)));
    return songs ? { songs } : null;
  }
  if (sub === 0x03 && r.data.length >= 3) return { event: { event: r.data[1]!, cmd: r.data[2]! } };
  return null;
}
