// Shared pieces of the tab screens (ui/screens.ts re-exports the screens themselves).
import { selectSound as selectOnPiano } from "../../features/pianosound.ts";
import { findParam } from "../../protocol/codecs.ts";
import type { SoundRef, TabId } from "../../state/settings.ts";
import type { ParamStore } from "../../state/store.ts";
import { type AppContext, createContext } from "../context.ts";

export const PART = { Main1: 0x00, Layer: 0x08, Lower: 0x09, FourHandsRight: 0x0a, FourHandsLeft: 0x0b } as const;
export const PART_LABEL: Record<string, string> = { Main1: "Main", Layer: "Layer", Lower: "Lower", FourHandsRight: "4H Right", FourHandsLeft: "4H Left" };

/** Tab titles (also used by the start-up screen setting). */
export const TAB_TITLES: Record<TabId, string> = {
  sound: "Sound",
  music: "Music",
  metronome: "Metronome",
  technician: "Technician",
  chords: "Chords",
  files: "USB memory",
  bluetooth: "Bluetooth",
  settings: "Settings",
  info: "Information",
};

// contexts created for callers that only pass a store (tests, older callers)
const fallback = new WeakMap<ParamStore, AppContext>();
export function ctxFor(store: ParamStore, ctx?: AppContext): AppContext {
  if (ctx) return ctx;
  let c = fallback.get(store);
  if (!c) fallback.set(store, (c = createContext(store)));
  return c;
}

export function soundParam() {
  return findParam("Sound")!;
}

/** Select a sound on a part; the piano's values are read back afterwards (features/pianosound.ts). */
export function selectSound(ctx: AppContext, part: number, s: SoundRef): Promise<boolean> {
  return selectOnPiano(ctx.store, ctx.library, part, s);
}

/**
 * Call `f` on every store change while `el` is on screen; stops once `el` has been shown and
 * then removed (screens are rebuilt on tab change, so a stale listener must not live on).
 */
export function whileShown(el: HTMLElement, store: ParamStore, f: (key: string) => void): void {
  let seen = false;
  const off = store.subscribe((key) => {
    if (el.isConnected) seen = true;
    else if (seen) {
      off();
      return;
    }
    f(key);
  });
}
