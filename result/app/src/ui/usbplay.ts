// USB playback: which USB player the piano runs, and the stop that reaches both of them
// (「USB 재생 겹침 · 정지」, 2026-11-13).
//
// The stock has two USB players that run independently: the SMF player (MID) and the audio player (MP3 / WAV).
// Every Music command (32 52 08) in MusicMode 3 goes to the player of the file type the last 24 08 selected
// (0x413CA3F5; FUN_4015DBD4 -> SMF FUN_401737A4 / audio FUN_40123848), and the Music state the piano reports
// back (FUN_4015C27C) is that same player's. So after "MID playing, then an MP3 selected and played" every Reset,
// Pause and MusicMode went to the audio player and the MID played on (field 2026-11-13). What stops a player is
// a file of ITS type selected first: 24 08 <file> sets the type before anything else - while that player runs
// the stock answers Busy and loads nothing, the playing song is untouched - and then Music Reset stops it
// (stock image + ota_main in the emulator: tools/roundtrip_usbplay.py).
//
// The piano's word is followed here, not the parameter store (whose MusicMode is the connect sync's, never our
// 31 52 1D - the reason the row did not show ■ in the field): MusicMode replies 32 52 1D, Music reports 32 52 08
// (for the selected type's player, in MusicMode 3 only - other modes always report 2), and MusicProgress 52 03:
// bar / beat (first byte 0) advance while the SMF player plays, time (first byte 1) while the audio player does.
import { FileAccessError } from "../protocol/fileaccess.ts";
import type { PianoSession } from "../protocol/session.ts";
import { findParam, progress as progressCodec } from "../protocol/codecs.ts";
import type { AppContext } from "./context.ts";
import { MUSIC_MODE } from "./screens/songctl.ts";

/** Music values (32 52 08 / 10 52 08): 0 Pause, 1 Play, 2 Reset (stop and rewind). */
export const MUSIC = { Pause: 0, Play: 1, Reset: 2 } as const;
/** File types as the stock's 24 08 sets them (FUN_4012796C by extension). */
export const USB_TYPE = { KSO: 3, MID: 4, MP3: 5, WAV: 6 } as const;
/** Timings (ms); tests make them short. grace: frames already on the way when the stop went out; watch: how long
 *  the piano must stay quiet after a stop; idle: a player with no sign of life for this long is taken as stopped. */
export const USB_STOP = { grace: 300, watch: 2_500, idle: 6_000 };
const BUSY = 0x4000;

export type UsbPlayer = "smf" | "audio";
export function usbTypeOf(name: string): number | null {
  const ext = name.slice(name.lastIndexOf(".") + 1).toUpperCase();
  return (USB_TYPE as Record<string, number>)[ext] ?? null;
}
export function playerOf(name: string): UsbPlayer | null {
  const t = usbTypeOf(name);
  return t === USB_TYPE.MID ? "smf" : t === USB_TYPE.MP3 || t === USB_TYPE.WAV ? "audio" : null;
}
const playerOfType = (t: number | undefined): UsbPlayer | null =>
  t === USB_TYPE.MID ? "smf" : t === USB_TYPE.MP3 || t === USB_TYPE.WAV ? "audio" : null;

interface PlayerState {
  name: string | null; // the file it plays (null: not known - started elsewhere)
  on: boolean;
  at: number; // the last sign of life
  progress: number | null; // last MusicProgress value of its kind
  progressSeen: boolean; // this player's progress kind has arrived in this session
  pos: number | null; // where it is, for the player bar: bar (SMF, 1-based) or milliseconds (audio)
}
export interface UsbPlayback {
  session: PianoSession | null;
  mode: number | undefined; // the piano's MusicMode as it last said (32 52 1D / 10 52 1D) or we set
  sel: number | undefined; // type of the file last selected by 24 08
  /** the selected file's length, if the piano said one after the select (MaxTime 10 52 18 in MusicMode 3: bar form
   *  [0, hi7, lo7, ..] or time form [1, t4 .. t0]); null = not said - the player bar then shows the position only */
  max: { unit: "bar" | "ms"; total: number } | null;
  smf: PlayerState;
  audio: PlayerState;
  stopping: boolean;
  /** advances of either player since the counter was read (the stop's quiet check) */
  advances: number;
  listeners: Set<() => void>;
  off: (() => void) | null;
  timer: ReturnType<typeof setTimeout> | null;
}

const states = new WeakMap<AppContext, UsbPlayback>();
const fresh = (): PlayerState => ({ name: null, on: false, at: 0, progress: null, progressSeen: false, pos: null });

/** Per context and session (a reconnect starts over), listening to the piano from the first call on. */
export function usbPlayback(ctx: AppContext): UsbPlayback {
  let st = states.get(ctx);
  const s = ctx.session();
  if (st && st.session === s) return st;
  st?.off?.();
  if (st?.timer) clearTimeout(st.timer);
  const listeners = st?.listeners ?? new Set<() => void>();
  st = { session: s, mode: undefined, sel: undefined, max: null, smf: fresh(), audio: fresh(), stopping: false, advances: 0, listeners, off: null, timer: null };
  states.set(ctx, st);
  const me = st;
  me.off = ctx.store.onMessage((m) => {
    if (ctx.session() !== me.session || m.v1 !== 0x52) return;
    if (m.v2 === 0x1d && (m.fn === 0x10 || m.fn === 0x31 || m.fn === 0x32)) {
      me.mode = m.data[0];
      changed(me);
    } else if (m.v2 === 0x08 && (m.fn === 0x10 || m.fn === 0x32)) onMusic(me, m.data[0]);
    else if (m.v2 === 0x03 && (m.fn === 0x10 || m.fn === 0x32) && m.data.length >= 4) onProgress(me, m.data);
    else if (m.v2 === 0x18 && m.fn === 0x10 && m.data.length >= 3 && me.mode === MUSIC_MODE.UsbMemory) {
      const d = progressCodec.decode(m.data);
      const total = "bar" in d ? d.bar : d.ms;
      me.max = total > 0 ? { unit: "bar" in d ? "bar" : "ms", total } : null;
    }
  });
  return st;
}

function changed(st: UsbPlayback): void {
  for (const l of [...st.listeners]) l();
  // a player kept alive only by its signs of life: look again when it would go idle
  if (st.timer) clearTimeout(st.timer);
  st.timer = null;
  const now = Date.now();
  const due = (["smf", "audio"] as const).filter((p) => st[p].on && st[p].progressSeen).map((p) => st[p].at + USB_STOP.idle).filter((t) => t > now);
  if (due.length) st.timer = setTimeout(() => changed(st), Math.min(...due) - now + 5);
}

/** Music state (FUN_4015C27C): in MusicMode 3 it is the selected type's player; other modes always say 2. */
function onMusic(st: UsbPlayback, v: number | undefined): void {
  if (st.mode !== MUSIC_MODE.UsbMemory) return;
  const p = playerOfType(st.sel);
  if (!p) return;
  if (v === MUSIC.Play) Object.assign(st[p], { on: true, at: Date.now() });
  else if (v === MUSIC.Pause || v === MUSIC.Reset) st[p].on = false;
  else return;
  changed(st);
}

/** MusicProgress: [0, bar hi, bar lo, beat, ..] from the SMF player, [1, t4 .. t0] from the audio player. A value
 *  that grows is a player playing; the same value again (select) or a smaller one (stop -> bar 1) is not. */
function onProgress(st: UsbPlayback, d: number[]): void {
  if (st.mode === MUSIC_MODE.Recorder) return; // the internal recorder's bars
  const p: UsbPlayer | null = d[0] === 0 ? "smf" : d[0] === 1 ? "audio" : null;
  if (!p) return;
  const v = d.slice(1).reduce((a, b) => a * 128 + (b & 0x7f), 0);
  const ps = st[p];
  const grew = ps.progress !== null && v > ps.progress;
  ps.progress = v;
  ps.progressSeen = true;
  const at = progressCodec.decode(d);
  ps.pos = "bar" in at ? at.bar : at.ms;
  if (!grew) return;
  st.advances++;
  ps.on = true;
  ps.at = Date.now();
  changed(st);
}

/** The piano plays (or may play) on this player. Without progress from it, our own word holds until a report. */
export function usbRunning(st: UsbPlayback, p: UsbPlayer, now = Date.now()): boolean {
  const ps = st[p];
  return ps.on && (!ps.progressSeen || now - ps.at < USB_STOP.idle);
}
export function usbAnyRunning(st: UsbPlayback): boolean {
  return usbRunning(st, "smf") || usbRunning(st, "audio");
}
/** What the player bar shows of a running USB playback: the player that runs (the audio one first - it is heard over
 *  an SMF), its file (null: started elsewhere), its position and - only if the piano said one for the file selected
 *  last and it is this player's - the length. null when nothing runs. */
export function usbNow(st: UsbPlayback): { player: UsbPlayer; name: string | null; unit: "bar" | "ms"; pos: number | null; total: number | null } | null {
  const p = (["audio", "smf"] as const).find((q) => usbRunning(st, q));
  if (!p) return null;
  const unit = p === "smf" ? "bar" : "ms";
  return { player: p, name: st[p].name, unit, pos: st[p].pos, total: st.max && st.max.unit === unit && playerOfType(st.sel) === p ? st.max.total : null };
}
/** The file a row shows as playing. */
export function usbPlayingName(st: UsbPlayback, name: string): boolean {
  return (["smf", "audio"] as const).some((p) => usbRunning(st, p) && st[p].name === name);
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

function sender(ctx: AppContext, st: UsbPlayback) {
  const store = ctx.store;
  return {
    mode3: () => {
      st.mode = MUSIC_MODE.UsbMemory;
      return store.command(findParam("MusicMode", 0x31)!, 0x7f, [MUSIC_MODE.UsbMemory]);
    },
    music: (v: number) => store.command(findParam("Music", 0x32)!, 0x7f, [v]),
    /** 24 08: Busy means that player runs - the stock set the type first and loaded nothing (the stop's case) */
    select: async (name: string, busyOk: boolean) => {
      st.max = null; // the length the piano said belonged to the file selected before
      try {
        await ctx.files()!.select(name);
      } catch (e) {
        if (!(busyOk && e instanceof FileAccessError && e.code === BUSY)) throw e;
      }
      st.sel = usbTypeOf(name) ?? undefined;
    },
  };
}

export type StopResult = "stopped" | "still" | "offline";

/**
 * Stop every USB playback: the selected type's player, then both players - the one that plays last - each by
 * MusicMode 3 + 24 08 <a file of its type> + Music Reset (MusicMode 3 again before each Reset: a mode change the
 * stock deferred is applied by the next Music command, which it then answers Busy). Then the piano must stay quiet
 * (no progress advancing) for USB_STOP.watch; if it does not, the whole sequence once more; "still" if it plays on.
 */
export async function usbStopAll(ctx: AppContext): Promise<StopResult> {
  const st = usbPlayback(ctx);
  if (!ctx.files()) return "offline";
  const tx = sender(ctx, st);
  const files = ctx.usbFiles;
  const pick = (p: UsbPlayer) => st[p].name ?? files.find((f) => playerOf(f) === p) ?? null;
  st.stopping = true;
  changed(st);
  try {
    for (let attempt = 0; attempt < 2; attempt++) {
      await tx.mode3();
      await tx.music(MUSIC.Reset);
      // the player that plays goes LAST: a Reset reaches the player of the type selected last, and the stock applies
      // it after the next select has already switched (field 2026-11-15, F13b: MP3 playing, order audio → smf, the
      // MID select came ~60 ms after the audio Reset and the MP3 played on). Unknown: audio last, then the other
      // order on the retry.
      const audioLast = usbRunning(st, "audio") || !usbRunning(st, "smf");
      const order: UsbPlayer[] = (attempt === 0) === audioLast ? ["smf", "audio"] : ["audio", "smf"];
      for (const p of order) {
        const f = pick(p);
        if (!f) continue;
        await tx.mode3();
        try {
          await tx.select(f, true);
        } catch {
          // another failure (file gone, timeout): the Reset still goes out; the quiet check decides
        }
        await tx.music(MUSIC.Reset);
      }
      for (const p of ["smf", "audio"] as const) st[p].on = false;
      changed(st);
      await sleep(USB_STOP.grace);
      const before = st.advances;
      await sleep(USB_STOP.watch);
      if (st.advances === before) return "stopped";
    }
    return "still";
  } finally {
    st.stopping = false;
    changed(st);
  }
}

export type PlayResult = { ok: true } | { ok: false; why: "stopping" | "not-stopped" | "offline" };

/**
 * Play one USB file - never on top of another USB playback: whatever the piano plays (by our word or its own
 * progress) is stopped first (usbStopAll), and a select answered Busy (that player runs although nothing said so)
 * also stops everything first. Throws what the file client throws (shown by the caller).
 */
export async function usbPlay(ctx: AppContext, name: string, onStopping?: () => void): Promise<PlayResult> {
  const st = usbPlayback(ctx);
  if (!ctx.files()) return { ok: false, why: "offline" };
  if (st.stopping) return { ok: false, why: "stopping" };
  const tx = sender(ctx, st);
  if (usbAnyRunning(st)) {
    onStopping?.();
    const r = await usbStopAll(ctx);
    if (r !== "stopped") return { ok: false, why: r === "offline" ? "offline" : "not-stopped" };
  }
  await tx.mode3();
  try {
    await tx.select(name, false);
  } catch (e) {
    if (!(e instanceof FileAccessError && e.code === BUSY)) throw e;
    onStopping?.();
    const r = await usbStopAll(ctx);
    if (r !== "stopped") return { ok: false, why: r === "offline" ? "offline" : "not-stopped" };
    await tx.mode3();
    await tx.select(name, false);
  }
  await tx.music(MUSIC.Play);
  const p = playerOf(name);
  if (p) Object.assign(st[p], { name, on: true, at: Date.now(), pos: null });
  changed(st);
  return { ok: true };
}
