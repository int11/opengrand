// Panel display services (features 71 LCD brightness, 72 display scale,
// 73 auto display off). Brightness goes to the Android shell's setBacklight;
// the scale is a CSS zoom of the whole UI; auto display off dims the backlight
// after N minutes without touch and wakes on the next touch (that touch is
// swallowed so it cannot press a button the user could not see).
import type { AppSettings } from "../state/settings.ts";

export interface DisplayDeps {
  setBacklight(level: number): void;
  root: HTMLElement; // element carrying --ui-scale (document.documentElement)
  events: EventTarget; // where user activity is observed (document)
  /** false on a phone/PC: brightness and auto display off are the panel's, only the scale applies */
  panel?: boolean;
  setTimer?: (fn: () => void, ms: number) => unknown;
  clearTimer?: (h: unknown) => void;
}

/** Backlight level used while the display is "off" (0 = dark). */
export const DIM_LEVEL = 0;

export function brightnessToBacklight(level: number): number {
  return Math.round((Math.max(1, Math.min(10, level)) / 10) * 255);
}

const ACTIVITY = ["pointerdown", "touchstart", "mousedown", "keydown", "wheel"];

export class DisplayController {
  private readonly d: Required<Omit<DisplayDeps, "panel">>;
  private readonly panel: boolean;
  private settings: AppSettings | null = null;
  private timer: unknown = null;
  dimmed = false;
  private swallowClick = false;

  constructor(deps: DisplayDeps) {
    this.d = {
      setTimer: (fn, ms) => setTimeout(fn, ms),
      clearTimer: (h) => clearTimeout(h as ReturnType<typeof setTimeout>),
      ...deps,
    };
    this.panel = deps.panel ?? true;
    for (const type of ACTIVITY) this.d.events.addEventListener(type, (e) => this.activity(e), true);
    // the click that follows a waking touch must not reach the UI either
    this.d.events.addEventListener("click", (e) => {
      if (!this.swallowClick) return;
      this.swallowClick = false;
      e.preventDefault();
      e.stopPropagation();
    }, true);
  }

  /** Apply (or re-apply after a change) brightness, scale and the idle timer. */
  apply(s: AppSettings): void {
    this.settings = s;
    this.d.root.style.setProperty("--ui-scale", String(s.scale / 100));
    this.d.root.setAttribute("data-scale", String(s.scale));
    if (!this.panel) return;
    if (!this.dimmed) this.d.setBacklight(brightnessToBacklight(s.brightness));
    this.arm();
  }

  private arm(): void {
    if (this.timer !== null) this.d.clearTimer(this.timer);
    this.timer = null;
    const min = this.panel ? (this.settings?.autoOffMin ?? 0) : 0;
    if (min > 0 && !this.dimmed) this.timer = this.d.setTimer(() => this.dim(), min * 60_000);
  }

  dim(): void {
    this.timer = null;
    if (this.dimmed) return;
    this.dimmed = true;
    this.d.root.classList.add("display-off");
    this.d.setBacklight(DIM_LEVEL);
  }

  wake(): void {
    if (!this.dimmed) return;
    this.dimmed = false;
    this.d.root.classList.remove("display-off");
    if (this.settings) this.d.setBacklight(brightnessToBacklight(this.settings.brightness));
    this.arm();
  }

  private activity(e: Event): void {
    if (this.dimmed) {
      this.wake();
      this.swallowClick = e.type !== "keydown";
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    this.arm();
  }
}
