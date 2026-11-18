// How often a light status poll runs (the header chips: op 0x1B 03 co-bound, op 0x1A Bluetooth audio). Every poll is a
// frame the piano receives; an app left open and idle should send as few as it can (field 2026-11-02: the desktop app
// kept the piano on all night; the firmware no longer counts polls
// as activity, this is the app side of it). A hidden page polls not at all (the callers' live()); a window that is
// shown but not focused (the desktop app behind other windows, a second monitor) polls BACKGROUND_FACTOR times slower.
export const BACKGROUND_FACTOR = 6;

type Doc = { hidden?: boolean; hasFocus?: () => boolean };

/** The next delay for a poll whose foreground period is `periodMs`. */
export function pollDelay(periodMs: number, doc: Doc | undefined = typeof document === "undefined" ? undefined : document): number {
  if (!doc || typeof doc.hasFocus !== "function") return periodMs;
  return doc.hasFocus() ? periodMs : periodMs * BACKGROUND_FACTOR;
}
