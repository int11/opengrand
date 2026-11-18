// Touch scrolling must never press a control (field 2026-11-03: dragging the panel / a phone screen up or
// down pressed buttons and changed sliders, switches and selects).
//
// One document-level guard, on every surface; it acts only on touch input (mouse and keyboard are unchanged):
//  - tap rule: a touch counts as a tap only while the finger stayed within SLOP px and no scroll took over
//    (the browser's own scroll cancels the touch: pointercancel). Otherwise the click that may still follow is
//    cancelled in the capture phase (buttons, switches, label rows, selects never see it), and so is a tap
//    that only stops a running fling. Controls already act on `click`/`change`, never on pointerdown.
//  - sliders: in touch mode (html.touch-ui) the native range does not take touches (`pointer-events: none`,
//    styles.css) — a native range jumps to the finger on touchstart and swallows vertical drags (Chrome 71).
//    Its row (`.slider`, `touch-action: pan-y`) scrolls vertically; a clear horizontal move (≥ LOCK px and
//    more sideways than up/down) locks the gesture to the slider, which then moves by the finger's travel
//    from that point (no jump), firing `input` while dragging and `change` at the end.
// The panel WebView is Chrome 71: pointer events (55+) and touch-action (36+) are there; without
// PointerEvent the guard falls back to touch events.

export const SLOP = 10; // px a tap may wander
export const LOCK = 8; // px of sideways travel that takes a slider
const CLICK_WINDOW_MS = 800; // a click this long after the touch ended still belongs to it
const FLING_MS = 150; // a touch this soon after a scroll from a drag stops the fling: not a tap

export interface TapGuardOptions {
  /** start in touch mode (the built-in panel; otherwise a coarse primary pointer or the first touch decides) */
  touch?: boolean;
  now?: () => number;
}

interface Gesture {
  x0: number;
  y0: number;
  moved: boolean; // beyond SLOP, scrolled, or cancelled by the browser's scroll
  active: boolean;
  endedAt: number;
  slider: SliderDrag | null;
}

interface SliderDrag {
  input: HTMLInputElement;
  state: "pending" | "drag" | "scroll";
  lockX: number;
  v0: number;
  vLock: number;
}

export interface TapGuard {
  readonly touchMode: boolean;
  dispose(): void;
}

/** The range a touch on `target` should drive: the range itself or the one in its `.slider` row. */
export function sliderFor(target: EventTarget | null): HTMLInputElement | null {
  const el = target as Element | null;
  if (!el || typeof el.closest !== "function") return null;
  if (el.tagName === "INPUT" && (el as HTMLInputElement).type === "range") return el as HTMLInputElement;
  const row = el.closest(".slider");
  const ranges = row ? row.querySelectorAll<HTMLInputElement>("input[type=range]") : null;
  return ranges && ranges.length === 1 ? ranges[0]! : null;
}

export function installTapGuard(doc: Document, opts: TapGuardOptions = {}): TapGuard {
  const win = doc.defaultView;
  const now = opts.now ?? (() => Date.now());
  const html = doc.documentElement;
  const coarse = (() => {
    try {
      return !!win?.matchMedia?.("(pointer: coarse)").matches;
    } catch {
      return false;
    }
  })();
  let touchMode = false;
  const setTouch = (on: boolean) => {
    touchMode = on;
    html.classList.toggle("touch-ui", on);
  };
  setTouch(!!opts.touch || coarse);
  const fixed = !!opts.touch; // the panel never has a mouse

  let g: Gesture | null = null;
  let lastDragScrollAt = -1e9; // last scroll event while/after a drag (a fling keeps scrolling)

  const begin = (x: number, y: number, target: EventTarget | null) => {
    if (!touchMode) setTouch(true);
    const flingStop = now() - lastDragScrollAt < FLING_MS;
    g = { x0: x, y0: y, moved: flingStop, active: true, endedAt: 0, slider: null };
    const input = sliderFor(target);
    if (input && !input.disabled) {
      const v = Number(input.value);
      g.slider = { input, state: "pending", lockX: x, v0: v, vLock: v };
    }
  };

  const move = (x: number, y: number) => {
    if (!g || !g.active) return;
    const dx = x - g.x0;
    const dy = y - g.y0;
    if (dx * dx + dy * dy > SLOP * SLOP) g.moved = true;
    const s = g.slider;
    if (!s) return;
    if (s.state === "pending") {
      if (Math.abs(dx) >= LOCK && Math.abs(dx) > Math.abs(dy)) {
        s.state = "drag";
        s.lockX = x;
        s.vLock = Number(s.input.value);
        g.moved = true;
      } else if (Math.abs(dy) >= LOCK) s.state = "scroll";
      return;
    }
    if (s.state !== "drag") return;
    const inp = s.input;
    const min = Number(inp.min || 0);
    const max = Number(inp.max || 100);
    const step = Number(inp.step) > 0 ? Number(inp.step) : 1;
    const width = inp.getBoundingClientRect().width || 240;
    const raw = s.vLock + ((x - s.lockX) / width) * (max - min);
    const v = Math.min(max, Math.max(min, min + Math.round((raw - min) / step) * step));
    if (String(v) !== inp.value) {
      inp.value = String(v);
      inp.dispatchEvent(new (win ?? globalThis).Event("input", { bubbles: true }));
    }
  };

  const end = (cancelled: boolean) => {
    if (!g || !g.active) return;
    g.active = false;
    g.endedAt = now();
    if (cancelled) g.moved = true;
    const s = g.slider;
    if (s && s.state === "drag" && Number(s.input.value) !== s.v0) {
      s.input.dispatchEvent(new (win ?? globalThis).Event("change", { bubbles: true }));
    }
    g.slider = null;
  };

  /** The click (or compat mousedown) that a touch drag/scroll may still produce: not a tap. */
  const notATap = (e: Event): boolean => {
    if (!g || !g.moved) return false;
    if ((e as MouseEvent).detail === 0) return false; // keyboard / programmatic click
    return g.active || now() - g.endedAt < CLICK_WINDOW_MS;
  };

  const L: [EventTarget, string, EventListener, AddEventListenerOptions][] = [];
  const on = (t: EventTarget, type: string, fn: EventListener, o: AddEventListenerOptions = {}) => {
    t.addEventListener(type, fn, { capture: true, passive: true, ...o });
    L.push([t, type, fn, { capture: true }]);
  };

  const usePointer = typeof (win as unknown as { PointerEvent?: unknown } | null)?.PointerEvent === "function";
  if (usePointer) {
    type PE = PointerEvent;
    const touchish = (e: PE) => e.pointerType === "touch" || e.pointerType === "pen";
    let pid: number | null = null;
    on(doc, "pointerdown", (ev) => {
      const e = ev as PE;
      if (!touchish(e)) return;
      if (pid !== null && g?.active) return; // a second finger: the first one's gesture stands
      pid = e.pointerId;
      begin(e.clientX, e.clientY, e.target);
    });
    on(doc, "pointermove", (ev) => {
      const e = ev as PE;
      if (e.pointerType === "mouse") {
        if (touchMode && !fixed) setTouch(false); // a real mouse on a hybrid PC: native sliders again
        return;
      }
      if (e.pointerId === pid) move(e.clientX, e.clientY);
    });
    on(doc, "pointerup", (ev) => {
      if ((ev as PE).pointerId !== pid) return;
      pid = null;
      end(false);
    });
    on(doc, "pointercancel", (ev) => {
      if ((ev as PE).pointerId !== pid) return;
      pid = null;
      end(true);
    });
  } else {
    let tid: number | null = null;
    const find = (list: TouchList) => {
      for (let i = 0; i < list.length; i++) if (list[i]!.identifier === tid) return list[i]!;
      return null;
    };
    on(doc, "touchstart", (ev) => {
      const e = ev as TouchEvent;
      if (tid !== null && g?.active) return;
      const t = e.changedTouches[0];
      if (!t) return;
      tid = t.identifier;
      begin(t.clientX, t.clientY, e.target);
    });
    on(doc, "touchmove", (ev) => {
      const t = find((ev as TouchEvent).changedTouches);
      if (t) move(t.clientX, t.clientY);
    });
    on(doc, "touchend", (ev) => {
      if (!find((ev as TouchEvent).changedTouches)) return;
      tid = null;
      end(false);
    });
    on(doc, "touchcancel", (ev) => {
      if (!find((ev as TouchEvent).changedTouches)) return;
      tid = null;
      end(true);
    });
  }

  // scrolling after a drag = a fling still running (the browser already cancels the touch when its own scroll
  // starts: pointercancel). Scroll events during a still finger (scroll anchoring as a list redraws) do not count.
  on(doc, "scroll", () => {
    if (g && g.moved) lastDragScrollAt = now();
  });

  const swallow = (e: Event) => {
    if (!notATap(e)) return;
    e.preventDefault();
    e.stopPropagation();
    if (e.type === "click") g = g && g.active ? g : null; // one click per gesture
  };
  on(doc, "mousedown", swallow, { passive: false });
  on(doc, "click", swallow, { passive: false });

  return {
    get touchMode() {
      return touchMode;
    },
    dispose() {
      for (const [t, type, fn, o] of L) t.removeEventListener(type, fn, o);
      html.classList.remove("touch-ui");
    },
  };
}
