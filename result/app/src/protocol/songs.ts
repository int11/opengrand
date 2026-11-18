// Internal recorder songs 1-10 (RecorderData) as far as a remote controller can see them on the stock
// V2.19 (emulator: result/stockfw/tools/stock_songs.py).
// The stock has no song directory request. What it does answer:
//   - SongSelect 10 52 00 7F [0, hi, lo]: Result 7E 03 00 01 6C (0xEC "no data") when the song is empty,
//     then MaxTime 10 52 18 7F [0, bars hi7, bars lo7, 0, 0, 0] = bars of the longest part;
//   - IntSongPart 10 52 01 7F <0|1|7F>: 0xEC when the selected song has no track for that part;
//   - InternalSongDelete 32 52 21 7F [04, hi, lo, part 0/1/7F]: 32 51 7E 7F 00 00 (stock) or
//     ResetCompleteInfo 32 52 22 (OpenGrand). d0 must be 04 or 7F on the stock; 04 is used because older
//     OpenGrand firmware read d0 = 7F as "all songs". The STOCK delete does not delete: it re-writes song n from the last
//     take's track table - send it only where the core deletes
//     (OpenGrand op 1E 00 status byte 31 bit 0, RecorderStatus.deletes; ui/files.ts).
// The song name (32 bytes in RecorderData) is never sent, and nothing renames a song.
// A scan changes the piano's selected song and play part; scan() puts both back when it ends.
import { DisplacedError, type PianoSession } from "./session.ts";
import { Fn, type KawaiFrame } from "./sysex.ts";

export const SONGS = 10;
const DISPLACED = Symbol("displaced");

export interface SongInfo {
  /** 0-based song number */
  n: number;
  /** false: the stock answered 0xEC (no data) */
  hasData: boolean;
  /** bars of the longest part (MaxTime); null when no MaxTime arrived */
  bars: number | null;
  /** [part 1, part 2] recorded; null when not probed (empty song or no answer) */
  parts: [boolean, boolean] | null;
}

/** 0-based song n -> SongSelect data (numType 0 = internal song). */
export const songSelectBytes = (n: number) => [0, (n >> 7) & 0x7f, n & 0x7f];
/** InternalSongDelete data: part 0 / 1 = that part only, 0x7F = whole song. */
export const songDeleteBytes = (n: number, part: 0 | 1 | 0x7f) => [0x04, (n >> 7) & 0x7f, n & 0x7f, part];

const isNoData = (f: KawaiFrame) => f.fn === Fn.System && f.body[0] === 0x03 && f.body[1] === 0x00 && f.body[2] === 0x01 && f.body[3] === 0x6c;
const isMaxTime = (f: KawaiFrame) => f.fn === Fn.Param && f.body[0] === 0x52 && f.body[1] === 0x18 && f.body.length >= 6;
const isDeleteDone = (f: KawaiFrame) =>
  (f.fn === Fn.Remote && f.body[0] === 0x51 && f.body[1] === 0x7e) || (f.fn === Fn.Remote && f.body[0] === 0x52 && f.body[1] === 0x22) || (f.fn === Fn.System && f.body[0] === 0x03);

export class InternalSongs {
  private s: PianoSession;
  private timeoutMs: number;

  constructor(s: PianoSession, timeoutMs = 1500) {
    this.s = s;
    this.timeoutMs = timeoutMs;
  }

  /** Send one fn 0x10 and collect: was 0xEC answered, and the MaxTime bars (null on timeout). */
  private async ask(v2: number, data: number[]): Promise<{ noData: boolean; bars: number | null }> {
    // displaced (7E 01 00): the stock ignores an unbound port, so the rest of a scan would only wait
    if (this.s.displaced) throw new DisplacedError();
    let noData = false;
    let off = () => {};
    const bars = new Promise<number | null>((resolve) => {
      const timer = setTimeout(() => resolve(null), this.timeoutMs);
      off = this.s.onFrame((f) => {
        if (isNoData(f)) noData = true;
        else if (isMaxTime(f)) {
          clearTimeout(timer);
          resolve((f.body[4] << 7) | f.body[5]);
        }
      });
    });
    try {
      const sent = await this.s.sendParam({ v1: 0x52, v2, part: 0x7f, data });
      if (!sent && this.s.displaced) throw new DisplacedError();
      const stop = this.untilDisplaced();
      const got = await Promise.race([bars, stop.promise]).finally(stop.off);
      if (got === DISPLACED) throw new DisplacedError();
      return { noData, bars: got };
    } finally {
      off();
    }
  }

  /** Resolves DISPLACED as soon as the session is displaced (never otherwise). */
  private untilDisplaced(): { promise: Promise<typeof DISPLACED>; off: () => void } {
    let off = () => {};
    const promise = new Promise<typeof DISPLACED>((resolve) => {
      off = this.s.onState((st) => {
        if (st === "displaced") resolve(DISPLACED);
      });
    });
    return { promise, off };
  }

  /** MusicMode 2 = internal recorder (fn 0x31, not ACKed). */
  async recorderMode(): Promise<void> {
    await this.s.sendParam({ fn: Fn.Mode, v1: 0x52, v2: 0x1d, part: 0x7f, data: [0x02] });
  }

  /** Select song n and read what the stock says about it (parts: probe IntSongPart 1, 0, then 7F). */
  async info(n: number, probeParts = true): Promise<SongInfo> {
    const sel = await this.ask(0x00, songSelectBytes(n));
    const hasData = !sel.noData && sel.bars !== 0;
    const out: SongInfo = { n, hasData, bars: sel.bars, parts: null };
    if (!hasData || !probeParts) return out;
    const p2 = await this.ask(0x01, [0x01]);
    const p1 = await this.ask(0x01, [0x00]);
    await this.ask(0x01, [0x7f]);
    if (p1.bars !== null || p2.bars !== null || p1.noData || p2.noData) out.parts = [!p1.noData, !p2.noData];
    return out;
  }

  /**
   * All 10 songs. Ends with the piano's song and play part as they were (`restore`: SongSelect data
   * [0, hi, lo] and IntSongPart value from the parameter mirror; defaults song 1 / both parts).
   */
  async scan(restore: { song?: number; part?: number } = {}, onSong?: (i: SongInfo) => void): Promise<SongInfo[]> {
    // SongSelect while the recorder plays restarts playback with the new song (FUN_401687E4 state 1)
    await this.stop();
    await this.recorderMode();
    await this.ask(0x01, [0x7f]);
    const out: SongInfo[] = [];
    for (let n = 0; n < SONGS; n++) {
      const i = await this.info(n);
      out.push(i);
      onSong?.(i);
    }
    await this.ask(0x00, songSelectBytes(restore.song ?? 0));
    const part = restore.part ?? 0x7f;
    // IntSongPart 0/1 also picks the recording part on the stock (FUN_4016A1E4); the probes left it at 0
    if (part !== 0x7f) await this.ask(0x01, [part]);
    return out;
  }

  /** Play song n in the internal recorder: MusicMode 2, SongSelect, Music Play. */
  async play(n: number): Promise<void> {
    await this.recorderMode();
    await this.ask(0x00, songSelectBytes(n));
    await this.s.sendParam({ fn: Fn.Remote, v1: 0x52, v2: 0x08, part: 0x7f, data: [0x01] });
  }

  /** Music Reset (stop and rewind). */
  async stop(): Promise<void> {
    await this.s.sendParam({ fn: Fn.Remote, v1: 0x52, v2: 0x08, part: 0x7f, data: [0x02] });
  }

  /**
   * Delete song n (part 0x7F) or one part. Resolves true on 32 51 7E 7F 00 00 / 32 52 22, false on a
   * Result error or timeout. The stock then rewrites RecorderData in NOR (one copy: do not power off).
   */
  async delete(n: number, part: 0 | 1 | 0x7f): Promise<boolean> {
    let off = () => {};
    const done = new Promise<boolean>((resolve) => {
      const timer = setTimeout(() => resolve(false), this.timeoutMs * 2);
      off = this.s.onFrame((f) => {
        if (!isDeleteDone(f)) return;
        clearTimeout(timer);
        const ok = f.fn === Fn.Remote ? f.body[0] === 0x52 || (f.body[3] === 0 && f.body[4] === 0) : false;
        resolve(ok);
      });
    });
    try {
      await this.s.sendParam({ fn: Fn.Remote, v1: 0x52, v2: 0x21, part: 0x7f, data: songDeleteBytes(n, part) });
      return await done;
    } finally {
      off();
    }
  }
}
