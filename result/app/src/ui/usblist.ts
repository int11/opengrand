// USB memory file list cache of the Recorder tab's USB list (ui/files.ts); it lives per context, so the tab draws it at once.
// The list comes from fn 0x24 24 11 (stock FUN_4014F018: no media -> Result 0x21, a list already running -> Busy).
// It is a stop-and-wait exchange of several frames, and on OpenGrand ota_main every fn 0x24 frame is APO activity
// (power.h power_frame_quiet), so the app asks only when it has a reason: the tab opens with an old list (or one
// from another session), the piano reports a stick inserted, a file operation finished, or the user taps ↻.
// One request at a time per session; a failed request keeps the last good list.
// Right after a stick is plugged in (or right after connecting) the piano can answer with an empty list (only 24 1F):
// the stock list task sends 24 1F after any walk that found nothing or failed with code 0x31, and the media flag
// it checks first is set by the attach callback (record 02_usb_autoload, field 2026-11-03). So an empty list within
// USB_SETTLE_MS of a plug / connect is asked again after 1.5 s, 3 s and 5 s, and an empty cached list is stale. The same
// for "No media" (0x21) that soon after a plug notice: the stick is reported before it is mounted (field 2026-11-27:
// re-plugged, the status said attached and the list stayed 「없음」 until ↻).
import { FileAccessError, resultText } from "../protocol/fileaccess.ts";
import { findParam } from "../protocol/codecs.ts";
import type { PianoSession } from "../protocol/session.ts";
import { tl } from "../i18n.ts";
import type { AppContext } from "./context.ts";

/** A list younger than this is shown as is when the tab opens again (same session). */
export const USB_LIST_FRESH_MS = 60_000;
/** Result code "No media" (stock FUN_4014DC0C(0x21)). */
export const NO_MEDIA = 0x21;
/** Result code "Busy" (stock FUN_4014BEF0: a list was still running; ota_main's file lease: another controller's file job,
 * or one that ended less than 0.6 s ago). */
const BUSY = 0x4000;
const BUSY_RETRY_MS = 400;
/** Busy answers asked again before giving up (the panel's own list on the same plug notice takes ~1 s + 0.6 s quiet). */
const BUSY_RETRIES = 4;
/** An empty list this soon after a plug notice / the first request of a session is not trusted. */
export const USB_SETTLE_MS = 5_000;
/** Waits before asking again after such an empty list (field 11-03: a replug listed the files within 1 s). */
export const USB_EMPTY_RETRY_MS = [1_500, 3_000, 5_000];

export interface UsbListState {
  /** ms of the last answer (list or no media); 0 = never */
  at: number;
  /** the session that answered */
  session: PianoSession | null;
  /** the piano said there is no stick (Result 0x21 or USBMemory = 0) */
  noMedia: boolean;
  /** text of the last failed request (the list shown is the last good one), null after a good one */
  error: string | null;
  inflight: Promise<boolean> | null;
  /** how many 24 11 requests this context sent (tests, diagnostics) */
  requests: number;
  /** ms of the last USBMemory change to 1 (stick inserted, or the first value after connecting); 0 = none */
  plugAt: number;
  /** the session of the last request and when it was first asked for (≈ connect) */
  seenSession: PianoSession | null;
  seenAt: number;
}

const states = new WeakMap<AppContext, UsbListState>();

const attachedOf = (ctx: AppContext) => ctx.store.get(findParam("USBMemory")!, 0x7f)?.[0];

/** The state for this context. The first call also starts watching USBMemory for plugs (main.ts calls it at start). */
export function usbListState(ctx: AppContext): UsbListState {
  let s = states.get(ctx);
  if (!s) {
    const st: UsbListState = { at: 0, session: null, noMedia: false, error: null, inflight: null, requests: 0, plugAt: 0, seenSession: null, seenAt: 0 };
    states.set(ctx, (s = st));
    let last = attachedOf(ctx);
    ctx.store.subscribe((k) => {
      if (k !== "USBMemory@127") return;
      const a = attachedOf(ctx);
      if (a === 1 && last !== 1) st.plugAt = Date.now();
      last = a;
    });
  }
  return s;
}

/** true when opening the tab should ask the piano again: never asked, another session, older than the limit, an
 *  empty list (it may have been read before the stick was ready), or "no stick" while the piano says attached. */
export function usbListStale(ctx: AppContext, now = Date.now()): boolean {
  const st = usbListState(ctx);
  if (st.at === 0 || st.session !== ctx.session() || now - st.at > USB_LIST_FRESH_MS) return true;
  if (st.noMedia) return attachedOf(ctx) === 1;
  return ctx.usbFiles.length === 0;
}

/** The piano reported the stick removed (USBMemory = 0): empty list, no request. */
export function usbListGone(ctx: AppContext): void {
  const st = usbListState(ctx);
  ctx.usbFiles = [];
  st.noMedia = true;
  st.error = null;
  st.at = Date.now();
  st.session = ctx.session();
}

/**
 * Ask the piano for the list (24 11). A request already running for this session is shared, not repeated.
 * Resolves true when the list (or "no media") came back, false on an error or without a connection.
 * trustEmpty: an empty answer is final even right after a plug (after a file operation the stick is surely mounted).
 */
export function loadUsbList(ctx: AppContext, trustEmpty = false): Promise<boolean> {
  const st = usbListState(ctx);
  const s = ctx.session();
  const fa = ctx.files();
  if (!s || !fa) return Promise.resolve(false);
  if (st.inflight && st.session === s) return st.inflight;
  st.session = s;
  if (st.seenSession !== s) {
    st.seenSession = s;
    st.seenAt = Date.now();
  }
  const once = async (): Promise<string[]> => {
    for (let k = 0; ; k++) {
      st.requests++;
      try {
        return await fa.list();
      } catch (e) {
        if (!(e instanceof FileAccessError) || e.code !== BUSY || k >= BUSY_RETRIES) throw e;
        await new Promise((r) => setTimeout(r, BUSY_RETRY_MS));
      }
    }
  };
  // an empty list soon after a plug / connect: ask again (same in-flight promise, so triggers meanwhile share it)
  const ask = async (): Promise<string[]> => {
    const since = Math.max(st.plugAt, st.seenAt);
    // "No media" right after a plug notice (the piano says attached): not mounted yet - asked again like an empty list
    const early = () => !trustEmpty && Date.now() - since <= USB_SETTLE_MS;
    const plugged = () => st.plugAt >= st.seenAt && attachedOf(ctx) === 1;
    const tryOnce = async (last: boolean): Promise<string[] | null> => {
      try {
        return await once();
      } catch (e) {
        if (!last && e instanceof FileAccessError && e.code === NO_MEDIA && plugged()) return null;
        throw e;
      }
    };
    const firstEarly = early();
    let files = await tryOnce(!firstEarly);
    if (!firstEarly) return files!;
    for (let i = 0; i < USB_EMPTY_RETRY_MS.length; i++) {
      if (files && files.length) break;
      await new Promise((r) => setTimeout(r, USB_EMPTY_RETRY_MS[i]));
      if (ctx.session() !== s) break;
      files = await tryOnce(i === USB_EMPTY_RETRY_MS.length - 1);
    }
    return files ?? [];
  };
  const p = ask().then(
    (files) => {
      ctx.usbFiles = files;
      st.noMedia = false;
      st.error = null;
      st.at = Date.now();
      return true;
    },
    (e: unknown) => {
      if (e instanceof FileAccessError && e.code === NO_MEDIA) {
        ctx.usbFiles = [];
        st.noMedia = true;
        st.error = null;
        st.at = Date.now();
        return true;
      }
      st.error = e instanceof FileAccessError ? tl(resultText(e.code)) : (e as Error).message;
      return false;
    },
  ).finally(() => {
    if (st.inflight === p) st.inflight = null;
  });
  st.inflight = p;
  return p;
}
