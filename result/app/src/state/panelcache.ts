// Panel-local display settings shared with the original Kawai panel UI.
//
// LCD brightness and Auto Display Off are not piano parameters: the original panel app keeps them
// in `user/uicache.json` through the KWMCore shell (`KWM.saveJson` / `loadJson`, app.js
// `saveSettings` / `loadSettings`: `lcdContrast` 5..100, `displayOff` index into
// [Never, 30 s, 1, 2, 3, 4, 5 min]) The piano
// never sees them, so a phone or PC cannot read them; inside the original shell (our UI bundle,
// transport/kwmcore.ts) we can, and we write our changes back so both UIs show the same value.
import { callPromise } from "../transport/kwmcore.ts";
import { AUTO_OFF_MINUTES, type AppSettings } from "./settings.ts";

export const UICACHE = "user/uicache.json";

export interface PanelCache {
  lcdContrast?: number;
  displayOff?: number;
  [k: string]: unknown;
}

/** uicache fields -> our settings (only what is present and valid). */
export function fromPanelCache(o: PanelCache): Partial<AppSettings> {
  const out: Partial<AppSettings> = {};
  const lc = Number(o.lcdContrast);
  // the original clamps to 5..100 and treats 0 / out of range as 100 (app.js 466-469, 7219)
  if (Number.isFinite(lc)) out.brightness = lc <= 0 || lc > 100 ? 10 : Math.max(1, Math.min(10, Math.round(lc / 10)));
  const d = Number(o.displayOff);
  if (Number.isInteger(d) && d >= 0 && d < AUTO_OFF_MINUTES.length) out.autoOffMin = AUTO_OFF_MINUTES[d];
  return out;
}

/** our settings -> uicache fields. */
export function toPanelCache(s: Pick<AppSettings, "brightness" | "autoOffMin">): { lcdContrast: number; displayOff: number } {
  return { lcdContrast: Math.max(10, Math.min(100, s.brightness * 10)), displayOff: Math.max(0, AUTO_OFF_MINUTES.indexOf(s.autoOffMin)) };
}

function asObject(v: unknown): PanelCache | null {
  if (typeof v === "string") {
    try {
      v = JSON.parse(v);
    } catch {
      return null;
    }
  }
  return v && typeof v === "object" && !Array.isArray(v) ? (v as PanelCache) : null;
}

export async function loadPanelCache(timeoutMs = 3000): Promise<PanelCache | null> {
  return asObject(await callPromise<unknown>("loadJson", { name: UICACHE }, timeoutMs).catch(() => null));
}

interface Ctx {
  settings: AppSettings;
  updateSettings(patch: Partial<AppSettings>): void;
  rerender(): void;
}

/**
 * Inside the original shell: take brightness / Auto Display Off from the panel's uicache.json and
 * write our changes back into it (other fields kept). Nothing is written when the file could not be
 * read — the original app fills a missing file with its own defaults, a partial one would break it.
 */
export async function attachPanelCache(ctx: Ctx, timeoutMs = 3000): Promise<boolean> {
  const cache = await loadPanelCache(timeoutMs);
  if (!cache) return false;
  let current: PanelCache = cache;
  const base = ctx.updateSettings.bind(ctx);
  ctx.updateSettings = (patch) => {
    base(patch);
    if (patch.brightness === undefined && patch.autoOffMin === undefined) return;
    const next = { ...current, ...toPanelCache(ctx.settings) };
    if (next.lcdContrast === current.lcdContrast && next.displayOff === current.displayOff) return;
    current = next;
    void callPromise("saveJson", { name: UICACHE, json: next }, timeoutMs).catch(() => {});
  };
  const patch = fromPanelCache(cache);
  const differs = (Object.keys(patch) as (keyof AppSettings)[]).some((k) => patch[k] !== ctx.settings[k]);
  if (differs) {
    base(patch);
    ctx.rerender();
  }
  return true;
}
