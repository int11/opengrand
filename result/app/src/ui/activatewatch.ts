// The app's watch over the panel loader's "start the new UI" (Control 1) — for loaders that do not watch it themselves
// (USB bundle 0.2.9 and older; the loader in panelui/uiloader.js since 2026-11-15 sets `activateWatch` and this stands
// down). Field 2026-11-15 (last lines): a package stored with
// "start right away" while the shell's Android Settings activity was on top — the page never reloaded and kept the
// loader's full-screen 「새 UI 시작 중 · starting the new UI…」 for 43 min, the UI under it unusable.
//
// This file ships in the remote UI package (app JS), so it reaches the panel over BLE with the next package; the
// loader fix needs the next USB bundle. Both do the same: no reload within timeoutMs → take the notice down (the old UI
// is usable), ask the loader to start the new UI again when the page is shown again (visibilitychange), at most
// `retries` times; after that the next boot starts the newest version anyway.

/** The part of window.CA79UI (panelui/uiloader.js) used here. */
export interface UiLoaderApi {
  on(f: (kind: string, info: Record<string, unknown>) => void): void;
  control(sub: number): number;
  activateWatch?: number;
}

const CONTROL_ACTIVATE = 1;

export function watchActivate(ui: UiLoaderApi | null | undefined, opts: { doc?: Document; timeoutMs?: number; retries?: number } = {}): boolean {
  if (!ui || typeof ui.on !== "function" || typeof ui.control !== "function" || ui.activateWatch) return false;
  const doc = opts.doc ?? document;
  const timeoutMs = opts.timeoutMs ?? 10_000;
  const retries = opts.retries ?? 3;
  let timer: ReturnType<typeof setTimeout> | undefined;
  let pending = false;
  let stalls = 0;
  ui.on((kind) => {
    if (kind !== "activate") return;
    clearTimeout(timer);
    pending = false;
    // the loader adds its notice right after this event and navigates 300 ms later; still here after timeoutMs = stuck
    timer = setTimeout(() => {
      doc.querySelectorAll("#ca79ui-notice").forEach((el) => el.remove());
      stalls++;
      if (stalls <= retries) pending = true;
    }, timeoutMs);
  });
  doc.addEventListener("visibilitychange", () => {
    if (doc.visibilityState === "hidden" || !pending) return;
    pending = false;
    ui.control(CONTROL_ACTIVATE); // the loader's own Control 1: notice + reload (its "activate" event re-arms the watch)
  });
  return true;
}
