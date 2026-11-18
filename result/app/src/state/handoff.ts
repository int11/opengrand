// BLE-MIDI handoff: the piano's
// BLE-MIDI module takes ONE connection, so an app that is not in use must let it go - otherwise a phone cannot find
// the piano while a PC tab sits in the background (field 2026-11-01).  Page closed -> DISCONNECT + GATT disconnect at
// once; page hidden for HANDOFF_HIDDEN_MS (a background tab, a phone browser in the background) -> the same; back in
// front -> reconnect to the same piano.  The Android shell does the same with its own LinkLifecycle grace.
// The desktop app keeps the link while minimized (user decision 2026-11-02): only closing the app or
// 「연결 끊기」 lets the piano go there (keepWhenHidden).

export const HANDOFF_HIDDEN_MS = 60000; // a file chooser / a quick app switch keeps the link

export interface HandoffTarget {
  suspend(): void;
  resume(): void;
}

export interface HandoffEnv {
  doc: { hidden: boolean; addEventListener(type: "visibilitychange", fn: () => void): void };
  win: { addEventListener(type: "pagehide", fn: () => void): void };
  onShell?(fn: (state: "background" | "foreground") => void): unknown;
  hiddenMs?: number;
  keepWhenHidden?: boolean;
  setTimer?(fn: () => void, ms: number): unknown;
  clearTimer?(h: unknown): void;
}

export function installHandoff(conn: HandoffTarget, env: HandoffEnv): void {
  const ms = env.hiddenMs ?? HANDOFF_HIDDEN_MS;
  const set = env.setTimer ?? ((fn: () => void, m: number) => setTimeout(fn, m));
  const clear = env.clearTimer ?? ((h: unknown) => clearTimeout(h as ReturnType<typeof setTimeout>));
  let timer: unknown;
  env.win.addEventListener("pagehide", () => conn.suspend());
  env.doc.addEventListener("visibilitychange", () => {
    if (env.keepWhenHidden) return;
    clear(timer);
    timer = undefined;
    if (env.doc.hidden) timer = set(() => conn.suspend(), ms);
    else conn.resume();
  });
  env.onShell?.((st) => (st === "background" ? conn.suspend() : conn.resume()));
}
