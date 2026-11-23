// KSO (Kawai recorder song file) <-> Standard MIDI File.
//
// The stock CA79 imports only KSO into an internal recorder song (24 09 <song> 03); a MID file is refused.
// smfToKso() writes a KSO the stock importer and its boot check accept, so an app can upload it (24 05) and
// import it. ksoToSmf() mirrors the stock "save as SMF" (24 0A <song> 02) for the events this module writes.
// Checked against the stock firmware in the emulator: result/stockfw/tools/stock_kso.py (fixture
// tests/fixtures/kso_stock.json).
//
// File = 16-byte file header + the song as it sits in the recorder store, offsets big-endian and +0x10:
//   +0x00 "hKSO" FF " CA99" + 6 bytes version text (only "hKSO" is checked on import)
//   +0x10 name, 32 bytes, space padded (bytes 8.. carry the "- SoundMode" mark of a recorded song)
//   +0x30 part settings offset (0 = play with the piano's current sound)   +0x34 tempo list offset
//   +0x38 track slot count, always 16                                     +0x3C 16 track offsets (0 = none)
//   +0x7C end of track data                                               +0x80 bars of the longest track
//   +0x84 tempo list, tracks, zero padding to a multiple of 4
// Tempo list: [tick (3 bytes, big-endian), bpm] ... FC; bpm 255..400 is FF, bpm-255.
// Track (96 ticks per quarter note), every data byte below 0x80:
//   FA sig         bar start; sig = (denominator code << 5) | (numerator - 1), codes 0..3 = /2 /4 /8 /16
//   F8             next beat (+96 ticks) inside the bar
//   key step gate FD vel rel            note; step = ticks after the current beat, gate = length in ticks
//   key step FE FD gateHi gateLo vel rel   note longer than 127 ticks (gate = hi * 128 + lo)
//   B0 step cc value                    controller; the player acts on 64 (damper), 66 (sostenuto), 67 (soft)
//   FC             end of track (the last bar runs to its full length)
// The piano plays slots 0 and 1 (part 1, part 2) with its piano sound; the loader counts bars by scanning the
// track for FA bytes up to the first FC byte, which is why no data byte may be 0x80 or above.

export const KSO_PPQ = 96;
/** Largest song (file - 16) the store can hold when the other nine songs are empty. */
export const KSO_MAX_SONG = 0xffbb8;
const HEAD = 0x74; // song header
const FILE_HEAD = 0x10;
const SLOTS = 16;
const MAX_GATE = 0x3fff;
const SOUND_MARK = "- SoundMode";

export class KsoError extends Error {
  code: "smf" | "empty" | "size" | "kso";
  constructor(code: KsoError["code"], message: string) {
    super(message);
    this.name = "KsoError";
    this.code = code;
  }
}

export interface SmfToKsoOptions {
  /** Song title: up to 8 ASCII characters are kept in the song name. Default "MIDI". */
  name?: string;
  /** "auto" (default): the first track (or channel) with notes -> part 1, everything else -> part 2. "single": all in part 1. */
  split?: "auto" | "single";
}

export interface KsoResult {
  kso: Uint8Array;
  warnings: string[];
  /** bars of the song */
  bars: number;
  /** notes written to [part 1, part 2] */
  notes: [number, number];
}

interface Note { part: number; tick: number; key: number; vel: number; rel: number; gate: number; seq: number }
interface Ctl { part: number; tick: number; cc: number; val: number; seq: number }
interface Tempo { tick: number; bpm: number }
interface Sig { tick: number; num: number; den: number }

// ------------------------------------------------------------------------------------------------ SMF reader
interface RawEvent { tick: number; track: number; status: number; d1: number; d2: number; meta?: number; data?: Uint8Array; seq: number }

function readSmf(smf: Uint8Array): { format: number; ppq: number; events: RawEvent[] } {
  const u32 = (p: number) => ((smf[p] << 24) | (smf[p + 1] << 16) | (smf[p + 2] << 8) | smf[p + 3]) >>> 0;
  const tag = (p: number) => String.fromCharCode(smf[p], smf[p + 1], smf[p + 2], smf[p + 3]);
  if (smf.length < 14 || tag(0) !== "MThd" || u32(4) < 6) throw new KsoError("smf", "not a Standard MIDI File");
  const format = (smf[8] << 8) | smf[9];
  const division = (smf[12] << 8) | smf[13];
  if (format > 1) throw new KsoError("smf", `SMF format ${format} is not supported`);
  if (division & 0x8000 || division === 0) throw new KsoError("smf", "SMPTE time division is not supported");
  const events: RawEvent[] = [];
  let p = 8 + u32(4);
  let track = 0;
  let seq = 0;
  while (p + 8 <= smf.length) {
    const len = u32(p + 4);
    const end = Math.min(smf.length, p + 8 + len);
    if (tag(p) === "MTrk") {
      let q = p + 8;
      let tick = 0;
      let running = 0;
      const vlq = () => {
        let v = 0;
        for (let i = 0; i < 4 && q < end; i++) {
          const c = smf[q++];
          v = (v << 7) | (c & 0x7f);
          if (c < 0x80) break;
        }
        return v;
      };
      while (q < end) {
        tick += vlq();
        if (q >= end) break;
        let st = smf[q];
        if (st === 0xff) {
          const meta = smf[q + 1];
          q += 2;
          const n = vlq();
          events.push({ tick, track, status: 0xff, d1: 0, d2: 0, meta, data: smf.subarray(q, Math.min(end, q + n)), seq: seq++ });
          q += n;
          if (meta === 0x2f) break;
        } else if (st === 0xf0 || st === 0xf7) {
          q += 1;
          q += vlq();
        } else {
          if (st & 0x80) {
            running = st;
            q += 1;
          } else st = running;
          if (st < 0x80) throw new KsoError("smf", "broken SMF track (data byte without status)");
          const two = (st & 0xf0) !== 0xc0 && (st & 0xf0) !== 0xd0;
          events.push({ tick, track, status: st, d1: smf[q] & 0x7f, d2: two ? smf[q + 1] & 0x7f : 0, seq: seq++ });
          q += two ? 2 : 1;
        }
      }
      track += 1;
    }
    p = end;
  }
  if (track === 0) throw new KsoError("smf", "SMF has no track");
  return { format, ppq: division, events };
}

// ------------------------------------------------------------------------------------------------ SMF -> KSO
const SIG_DEN = [2, 4, 8, 16];
const BEAT_TICKS = [192, 96, 48, 24];
const sigByte = (num: number, den: number) => (SIG_DEN.indexOf(den) << 5) | (num - 1);
const barTicks = (sig: number) => BEAT_TICKS[sig >> 5] * ((sig & 0x1f) + 1);

/** Convert a Standard MIDI File (format 0 or 1) to a KSO file for 24 09 <song> 03. Throws KsoError. */
export function smfToKso(smf: Uint8Array, opts: SmfToKsoOptions = {}): KsoResult {
  const { format, ppq, events } = readSmf(smf);
  const warnings: string[] = [];
  const scale = (t: number) => Math.round((t * KSO_PPQ) / ppq);
  if (KSO_PPQ % ppq !== 0 && ppq % KSO_PPQ !== 0) warnings.push(`timing is rounded to ${KSO_PPQ} ticks per quarter note (file: ${ppq})`);

  // which source group (track in format 1, else channel) goes to which part
  const isOn = (e: RawEvent) => (e.status & 0xf0) === 0x90 && e.d2 > 0;
  const drums = (e: RawEvent) => (e.status & 0x0f) === 9;
  const noteTracks = new Set(events.filter((e) => isOn(e) && !drums(e)).map((e) => e.track));
  const byTrack = format === 1 && noteTracks.size > 1;
  const group = (e: RawEvent) => (byTrack ? e.track : e.status & 0x0f);
  const groups: number[] = [];
  for (const e of events) if (isOn(e) && !drums(e) && !groups.includes(group(e))) groups.push(group(e));
  if (groups.length === 0) throw new KsoError("empty", "the MIDI file has no notes");
  const split = (opts.split ?? "auto") === "auto" && groups.length > 1;
  const partOf = (e: RawEvent) => (split && groups.includes(group(e)) && group(e) !== groups[0] ? 1 : 0);
  if (groups.length > 2) warnings.push(`${groups.length} ${byTrack ? "tracks" : "channels"} with notes: the first is part 1, the others are merged into part 2`);

  const notes: Note[] = [];
  const ctls: Ctl[] = [];
  const tempos: Tempo[] = [];
  const sigs: Sig[] = [];
  const open = new Map<number, Note>(); // (part, key) -> sounding note
  let endTick = 0;
  let dropped = { drums: 0, ctl: 0, other: 0, sig: 0, tempo: 0, long: 0 };
  const close = (id: number, tick: number, rel: number) => {
    const n = open.get(id);
    if (!n) return;
    open.delete(id);
    n.gate = Math.max(1, tick - n.tick);
    n.rel = rel;
  };
  const sorted = [...events].sort((a, b) => a.tick - b.tick || a.seq - b.seq);
  for (const e of sorted) {
    const tick = scale(e.tick);
    const kind = e.status & 0xf0;
    if (e.status === 0xff) {
      if (e.meta === 0x51 && e.data!.length >= 3) {
        const us = (e.data![0] << 16) | (e.data![1] << 8) | e.data![2];
        let bpm = us ? Math.round(60_000_000 / us) : 120;
        if (bpm < 10 || bpm > 400) {
          dropped.tempo += 1;
          bpm = Math.min(400, Math.max(10, bpm));
        }
        tempos.push({ tick, bpm });
      } else if (e.meta === 0x58 && e.data!.length >= 2) {
        const num = e.data![0];
        const den = 1 << e.data![1];
        if (num >= 1 && num <= 32 && SIG_DEN.includes(den)) sigs.push({ tick, num, den });
        else dropped.sig += 1;
      } else if (e.meta === 0x2f) endTick = Math.max(endTick, tick);
      continue;
    }
    if (kind === 0x90 || kind === 0x80) {
      if (drums(e)) {
        if (isOn(e)) dropped.drums += 1;
        continue;
      }
      const part = partOf(e);
      const id = part * 128 + e.d1;
      if (isOn(e)) {
        close(id, tick, 0x40); // the same key again: the first note ends here
        const n: Note = { part, tick, key: e.d1, vel: e.d2, rel: 0x40, gate: 0, seq: e.seq };
        notes.push(n);
        open.set(id, n);
      } else close(id, tick, kind === 0x80 && e.d2 ? e.d2 : 0x40);
      endTick = Math.max(endTick, tick);
    } else if (kind === 0xb0) {
      if (drums(e)) continue;
      if (e.d1 === 64 || e.d1 === 66 || e.d1 === 67) {
        ctls.push({ part: partOf(e), tick, cc: e.d1, val: e.d2, seq: e.seq });
        endTick = Math.max(endTick, tick);
      } else dropped.ctl += 1;
    } else dropped.other += 1;
  }
  for (const id of [...open.keys()]) close(id, Math.max(endTick, open.get(id)!.tick + 1), 0x40);
  for (const n of notes) {
    if (n.gate > MAX_GATE) {
      n.gate = MAX_GATE;
      dropped.long += 1;
    }
    endTick = Math.max(endTick, n.tick + n.gate);
  }
  if (dropped.drums) warnings.push(`${dropped.drums} drum notes (channel 10) were left out`);
  if (dropped.ctl) warnings.push(`${dropped.ctl} controller events other than the three pedals were left out`);
  if (dropped.other) warnings.push(`${dropped.other} program change / pitch bend / aftertouch events were left out`);
  if (dropped.sig) warnings.push("an unsupported time signature was ignored (4/4 or the previous one is used)");
  if (dropped.tempo) warnings.push("a tempo outside 10-400 bpm was limited");
  if (dropped.long) warnings.push(`${dropped.long} notes longer than ${MAX_GATE} ticks were shortened`);

  // bars: the time signature in force at each bar start, up to the end of the last event
  const bars: { start: number; sig: number }[] = [];
  {
    let sig = sigByte(4, 4);
    let si = 0;
    let start = 0;
    do {
      while (si < sigs.length && sigs[si].tick <= start) sig = sigByte(sigs[si].num, sigs[si++].den);
      bars.push({ start, sig });
      start += barTicks(sig);
    } while (start < endTick);
    if (si < sigs.length && sigs.slice(si).some((s) => s.tick < start)) warnings.push("a time signature change inside a bar starts at the next bar");
  }

  // tempo list: one entry per change, the first at tick 0
  const list: Tempo[] = [{ tick: 0, bpm: 120 }];
  for (const t of tempos) {
    const last = list[list.length - 1];
    if (t.tick <= last.tick) last.bpm = t.bpm;
    else if (t.bpm !== last.bpm) list.push({ tick: t.tick, bpm: t.bpm });
  }
  const tempoBytes: number[] = [];
  for (const t of list) {
    if (t.tick >= 0xfc0000) throw new KsoError("size", "the song is too long for a recorder song");
    tempoBytes.push((t.tick >> 16) & 0xff, (t.tick >> 8) & 0xff, t.tick & 0xff);
    if (t.bpm >= 0xff) tempoBytes.push(0xff, t.bpm - 0xff);
    else tempoBytes.push(t.bpm);
  }
  tempoBytes.push(0xfc);

  const encodeTrack = (part: number): number[] | null => {
    const ev: (Note | Ctl)[] = [...notes.filter((n) => n.part === part), ...ctls.filter((c) => c.part === part)];
    if (!ev.some((e) => "key" in e)) return null;
    ev.sort((a, b) => a.tick - b.tick || a.seq - b.seq);
    const out: number[] = [];
    let i = 0;
    for (let b = 0; b < bars.length; b++) {
      const { start, sig } = bars[b];
      const end = start + barTicks(sig);
      out.push(0xfa, sig);
      let base = start;
      const beat = () => {
        out.push(0xf8);
        base += KSO_PPQ;
      };
      for (; i < ev.length && (ev[i].tick < end || b === bars.length - 1); i++) {
        const e = ev[i];
        const tick = Math.min(e.tick, end - 1);
        while (tick - base >= KSO_PPQ) beat();
        if ("key" in e) {
          if (e.gate > 0x7f) out.push(e.key, tick - base, 0xfe, 0xfd, e.gate >> 7, e.gate & 0x7f, e.vel, e.rel);
          else out.push(e.key, tick - base, e.gate, 0xfd, e.vel, e.rel);
        } else out.push(0xb0, tick - base, e.cc, e.val);
      }
      while (base + KSO_PPQ < end) beat();
    }
    out.push(0xfc);
    return out;
  };
  const tracks = [encodeTrack(0), encodeTrack(1)];

  let len = HEAD + tempoBytes.length + (tracks[0]?.length ?? 0) + (tracks[1]?.length ?? 0);
  len += -len & 3;
  if (len > KSO_MAX_SONG) {
    throw new KsoError("size", `the song needs ${len} bytes; the piano's recorder holds ${KSO_MAX_SONG} bytes for all ten songs`);
  }
  const kso = new Uint8Array(FILE_HEAD + len);
  const be = (off: number, v: number) => new DataView(kso.buffer).setUint32(off, v);
  const ascii = (off: number, s: string) => {
    for (let k = 0; k < s.length; k++) kso[off + k] = s.charCodeAt(k);
  };
  ascii(0, "hKSO");
  kso[4] = 0xff;
  ascii(5, " CA99");
  ascii(10, "V2.19");
  const title = (opts.name ?? "MIDI").replace(/[^\x20-\x7e]/g, "?").slice(0, 8).padEnd(8);
  ascii(FILE_HEAD, (title + SOUND_MARK).padEnd(32));
  let at = HEAD; // song-relative
  be(FILE_HEAD + 0x24, at + FILE_HEAD);
  kso.set(tempoBytes, FILE_HEAD + at);
  at += tempoBytes.length;
  be(FILE_HEAD + 0x28, SLOTS);
  tracks.forEach((t, slot) => {
    if (!t) return;
    be(FILE_HEAD + 0x2c + 4 * slot, at + FILE_HEAD);
    kso.set(t, FILE_HEAD + at);
    at += t.length;
  });
  be(FILE_HEAD + 0x6c, at + FILE_HEAD);
  be(FILE_HEAD + 0x70, bars.length);
  const count: [number, number] = [0, 0];
  for (const n of notes) count[n.part] += 1;
  const problem = checkKso(kso);
  if (problem) throw new KsoError("kso", `internal error: ${problem}`);
  return { kso, warnings, bars: bars.length, notes: count };
}

// ------------------------------------------------------------------------------------------------ KSO reader
export interface KsoSong {
  name: string;
  bars: number;
  tempos: Tempo[];
  /** 16 slots; null = no track. Each is the track bytes from its first FA to its FC. */
  tracks: (Uint8Array | null)[];
}

/** Length of the event at p, or 0 when the byte is not an event this module knows. */
function eventLength(t: Uint8Array, p: number): number {
  const b = t[p];
  if (b < 0x80) return 4 + (t[p + 2] === 0xfe ? 2 : 0) + (t[p + 3] === 0xfd ? 2 : 0);
  const k = b < 0xf0 ? b & 0xf0 : b;
  if (k === 0xa0 || k === 0xb0 || k === 0xe0 || k === 0xf5) return 4;
  if (k === 0xc0 || k === 0xd0) return 3;
  if (k === 0xf1) return 5;
  if (k >= 0xf2 && k <= 0xf4) return 8;
  if (k === 0xf8 || k === 0xfc) return 1;
  if (k === 0xfa) return 2;
  if (k === 0xf0) {
    const end = t.indexOf(0xf7, p + 2);
    return end < 0 ? 0 : end + 1 - p;
  }
  return 0;
}

/**
 * What the stock does not check on import but its boot check and loader rely on. Returns the first problem,
 * or null when the file is safe to import. A file that fails here can make the piano format all ten songs
 * at its next power-on, so check every KSO that was not written by the piano itself before 24 09.
 */
export function checkKso(kso: Uint8Array): string | null {
  if (kso.length < FILE_HEAD + HEAD) return "file too short";
  if (kso.length > 0xfffff) return "file too large";
  if (String.fromCharCode(...kso.subarray(0, 4)) !== "hKSO") return "not a KSO file";
  const len = kso.length - FILE_HEAD;
  if (len & 3) return "song length is not a multiple of 4";
  if (len > KSO_MAX_SONG) return "song larger than the recorder store";
  const dv = new DataView(kso.buffer, kso.byteOffset, kso.byteLength);
  const word = (songOff: number) => dv.getUint32(FILE_HEAD + songOff);
  if (word(0x28) !== SLOTS) return "track slot count is not 16";
  for (const off of [0x20, 0x24]) {
    const v = word(off);
    if (v && (v < FILE_HEAD + HEAD || v >= kso.length)) return "settings offset out of range";
  }
  if (word(0x24) && kso.indexOf(0xfc, word(0x24)) < 0) return "tempo list has no end";
  let any = false;
  for (let slot = 0; slot < SLOTS; slot++) {
    const v = word(0x2c + 4 * slot);
    if (!v) continue;
    any = true;
    if (v < FILE_HEAD + HEAD || v >= kso.length) return `track ${slot + 1} offset out of range`;
    if (kso[v] !== 0xfa) return `track ${slot + 1} does not start with a bar`;
    // walk the events: the loader's raw scan must see the same bars and the same end
    let p = v;
    let bars = 0;
    for (;;) {
      if (p >= kso.length) return `track ${slot + 1} has no end`;
      const b = kso[p];
      const n = eventLength(kso, p);
      if (!n || p + n > kso.length) return `track ${slot + 1} has an unknown event`;
      if (b === 0xfc) break;
      if (b === 0xfa) bars += 1;
      else if (b === 0xf0 && slot > 1) return `track ${slot + 1} has a parameter event`;
      for (let k = 1; k < n; k++) {
        const c = kso[p + k];
        if ((c === 0xfa && b !== 0xfa) || c === 0xfc) return `track ${slot + 1} has a data byte the bar counter would misread`;
      }
      p += n;
    }
    if (bars === 0) return `track ${slot + 1} has no bar`;
  }
  const end = word(0x6c);
  if (any && (end < FILE_HEAD + HEAD || end > kso.length)) return "end offset out of range";
  return null;
}

/** Split a KSO file into its parts. Throws KsoError when checkKso() finds a problem. */
export function parseKso(kso: Uint8Array): KsoSong {
  const problem = checkKso(kso);
  if (problem) throw new KsoError("kso", problem);
  const dv = new DataView(kso.buffer, kso.byteOffset, kso.byteLength);
  const word = (songOff: number) => dv.getUint32(FILE_HEAD + songOff);
  const tempos: Tempo[] = [];
  for (let p = word(0x24); p && kso[p] !== 0xfc; ) {
    const tick = (kso[p] << 16) | (kso[p + 1] << 8) | kso[p + 2];
    const wide = kso[p + 3] === 0xff;
    tempos.push({ tick, bpm: wide ? kso[p + 4] + 0xff : kso[p + 3] });
    p += wide ? 5 : 4;
  }
  const tracks: (Uint8Array | null)[] = [];
  for (let slot = 0; slot < SLOTS; slot++) {
    const v = word(0x2c + 4 * slot);
    if (!v) {
      tracks.push(null);
      continue;
    }
    let p = v;
    while (kso[p] !== 0xfc) p += eventLength(kso, p);
    tracks.push(kso.subarray(v, p + 1));
  }
  return { name: String.fromCharCode(...kso.subarray(FILE_HEAD, FILE_HEAD + 32)), bars: word(0x70), tempos, tracks };
}

// ------------------------------------------------------------------------------------------------ KSO -> SMF
export interface KsoToSmfOptions {
  /** true (default): one empty bar in front, as the piano's own "save as SMF" writes it. */
  leadBar?: boolean;
}

/** MIDI channel the stock SMF writer uses for part 1 / part 2 (single-sound parts). */
const PART_CHANNEL = [0, 2];

/**
 * Format-0 SMF (96 ticks per quarter) of parts 1 and 2: notes, pedals, tempo changes, the first time signature.
 * For a song without parameter events and with single-sound parts this is byte for byte what the piano's
 * "save as SMF" writes.
 */
export function ksoToSmf(kso: Uint8Array, opts: KsoToSmfOptions = {}): Uint8Array {
  const song = parseKso(kso);
  const out: number[] = [];
  const vlq = (v: number) => {
    const b = [v & 0x7f];
    for (v >>= 7; v; v >>= 7) b.unshift((v & 0x7f) | 0x80);
    out.push(...b);
  };
  const tempo = (delta: number, bpm: number) => {
    const us = Math.floor(60_000_000 / bpm);
    vlq(delta);
    out.push(0xff, 0x51, 0x03, (us >> 16) & 0xff, (us >> 8) & 0xff, us & 0xff);
  };
  interface State { t: Uint8Array; p: number; bar: number; base: number; len: number; time: number; done: boolean }
  const states: (State | null)[] = song.tracks.map((t) => {
    if (!t) return null;
    const s: State = { t, p: 2, bar: 0, base: 0, len: barTicks(t[1]), time: 0, done: false };
    return s;
  });
  const eventTime = (s: State) => {
    const b = s.t[s.p];
    s.time = b === 0xfa || b === 0xfc ? s.bar + s.len : b === 0xf8 ? s.base + KSO_PPQ : s.base + s.t[s.p + 1];
  };
  states.forEach((s) => s && eventTime(s));
  const firstSig = states.find((s) => s)?.t[1] ?? sigByte(4, 4);
  let ti = 0;
  // the stock writes a tick-0 tempo here and once more in the loop below (it does not step its list)
  if (song.tempos.length && song.tempos[0].tick === 0) tempo(0, song.tempos[0].bpm);
  vlq(0);
  out.push(0xff, 0x58, 0x04, (firstSig & 0x1f) + 1, (firstSig >> 5) + 1, 0x18, 0x08);

  const sounding: ({ gate: number; ch: number; key: number } | null)[] = new Array(128).fill(null);
  let delta = (opts.leadBar ?? true) && states[0] ? states[0].len : 0;
  for (let tick = 1; ; tick++) {
    for (let k = 0; k < sounding.length; k++) {
      const n = sounding[k];
      if (n && --n.gate <= 0) {
        sounding[k] = null;
        vlq(delta);
        out.push(0x80 | n.ch, n.key, 0x40);
        delta = 0;
      }
    }
    for (; ti < song.tempos.length && song.tempos[ti].tick < tick; ti++) {
      tempo(delta, song.tempos[ti].bpm);
      delta = 0;
    }
    states.forEach((s, slot) => {
      while (s && !s.done && s.time < tick) {
        const { t, p } = s;
        const b = t[p];
        const n = eventLength(t, p);
        if (b < 0x80) {
          const wide = t[p + 2] === 0xfe;
          const gate = wide ? t[p + 4] * 128 + t[p + 5] : t[p + 2];
          const vel = t[p + 3] === 0xfd ? t[p + (wide ? 6 : 4)] : t[p + 3];
          const free = sounding.indexOf(null);
          if (slot < 2 && free >= 0) {
            sounding[free] = { gate: Math.max(1, gate - (tick - s.time - 1)), ch: PART_CHANNEL[slot], key: b };
            vlq(delta);
            out.push(0x90 | PART_CHANNEL[slot], b, vel);
            delta = 0;
          }
        } else if ((b & 0xf0) === 0xb0 && slot < 2) {
          vlq(delta);
          out.push(0xb0 | PART_CHANNEL[slot], t[p + 2], t[p + 3]);
          delta = 0;
        } else if (b === 0xf8) s.base += KSO_PPQ;
        else if (b === 0xfa) {
          s.bar += s.len;
          s.base = s.bar;
          s.len = barTicks(t[p + 1]);
        } else if (b === 0xfc) {
          s.done = true;
          break;
        }
        s.p = p + n;
        eventTime(s);
      }
    });
    if (!states.some((s) => s && !s.done)) break;
    delta += 1;
  }
  vlq(delta);
  out.push(0xff, 0x2f, 0x00);
  const n = out.length;
  return Uint8Array.from([
    0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0, KSO_PPQ,
    0x4d, 0x54, 0x72, 0x6b, (n >>> 24) & 0xff, (n >>> 16) & 0xff, (n >>> 8) & 0xff, n & 0xff, ...out,
  ]);
}
