// In-page two-step confirm for destructive actions — no native window.confirm (field 2026-11-15: an automated test drove the
// desktop app over CDP and accepted the page's confirm, yet Electron's native confirm box stayed on the user's screen). The first tap arms the button and shows the question under it,
// a second deliberate tap (not within CONFIRM.gapMs — a double tap or a queued tap is not two decisions) runs the
// action; CONFIRM.ms without the second tap disarms it. Same pattern as the panel chooser's 「한 번 더 누르면 …」.
import { t } from "../i18n.ts";
import { h } from "./dom.ts";

export const CONFIRM = { ms: 6000, gapMs: 350 };

type Armed = { timer: ReturnType<typeof setTimeout>; ask: HTMLElement; label: string; at: number };
const armed = new WeakMap<HTMLElement, Armed>();

/** Back to the unarmed button (no action). */
export function disarm(btn: HTMLElement): void {
  const a = armed.get(btn);
  if (!a) return;
  armed.delete(btn);
  clearTimeout(a.timer);
  a.ask.remove();
  btn.textContent = a.label;
  delete btn.dataset.armed;
  btn.classList.remove("armed");
}

/** Call from the button's click: the first call arms (returns false), the second runs `run` (returns true). */
export function confirmTap(target: EventTarget | HTMLElement | null, question: string, run: () => void): boolean {
  const btn = target as HTMLElement | null;
  if (!btn || typeof (btn as HTMLElement).insertAdjacentElement !== "function") return false;
  const a = armed.get(btn);
  if (a) {
    if (Date.now() - a.at < CONFIRM.gapMs) return false; // the same gesture twice: still one decision
    disarm(btn);
    run();
    return true;
  }
  const ask = h("p", { class: "confirm-ask", role: "alert", "data-confirm": "ask" }, question, " ", h("b", {}, t("Tap the button again to confirm.")));
  const label = btn.textContent ?? "";
  // under the button's row (a .transport / .row flex line) rather than inside it
  const host = btn.parentElement && btn.parentElement.children.length > 1 ? btn.parentElement : btn;
  host.insertAdjacentElement("afterend", ask);
  btn.dataset.armed = "1";
  btn.classList.add("armed");
  btn.textContent = t("Tap again to confirm");
  armed.set(btn, { ask, label, at: Date.now(), timer: setTimeout(() => disarm(btn), CONFIRM.ms) });
  return false;
}
