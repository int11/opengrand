// "Panel also connected" / "App also connected": another controller bound to the piano's session core at
// the same time (OpenGrand slot payloads with feat2 0x02 - ota_session, ota_main). Both stay in control: with the stage 2 payload a change made on one shows on the
// other (the piano sends the change frame; session.ts ACKs it and store.ts applies it like a dump value).
// Source: op 0x1B 03 (binding table, polled lightly). Stage 2 builds say which port asked (byte 45); older
// ones do not, and this side's port is then inferred (panel builds = port 3, a BLE link = port 2).
import { t } from "../i18n.ts";
import { NgClient, ngHas, type SessionCore } from "../protocol/ngext.ts";
import type { PianoSession } from "../protocol/session.ts";
import type { ConnState } from "../state/connection.ts";
import { h } from "./dom.ts";
import { pollDelay } from "./pollpace.ts";

/** Stock port numbers (REMOTE_PROTOCOL.md section 10.5): 0 USB-MIDI, 1 / 2 Bluetooth MIDI, 3 the built-in panel. */
export const PORT_PANEL = 3;
export const PORT_BLE = 2;

/** The header line for a binding table seen from port `me` (null = unknown); null = nobody else. */
export function coBoundText(c: SessionCore, me: number | null): string | null {
  const self = c.port ?? me;
  const bits = c.bound & 0x0f;
  if (self === null || self === undefined) {
    // which bit is ours is unknown: say something only when there are clearly two
    let n = 0;
    for (let p = 0; p < 4; p++) if (bits & (1 << p)) n++;
    return n >= 2 ? t("Another controller is also connected") : null;
  }
  const others = bits & ~(1 << self);
  if (!others) return null;
  const names: string[] = [];
  if (others & (1 << PORT_PANEL)) names.push(t("the panel"));
  if (others & (1 << PORT_BLE)) names.push(t("the app (Bluetooth)"));
  if (others & (1 << 1)) names.push(t("a Bluetooth device"));
  if (others & (1 << 0)) names.push(t("a USB device"));
  if (others === 1 << PORT_PANEL) return t("Panel also connected");
  if (others === 1 << PORT_BLE) return t("App also connected (Bluetooth)");
  return t("Also connected: {devices}", { devices: names.join(", ") });
}

export interface CoBoundBadge {
  el: HTMLElement;
  /** read op 0x1B 03 now */
  refresh(): void;
}

/**
 * Header line: shown only with a session-core payload (Identify feat2 0x02, asked once per session), polls
 * op 0x1B 03 every `periodMs` while connected and the page is visible. `myPort` is this side's stock port when
 * the payload does not say (older stage 1 builds).
 */
export function coBoundBadge(
  session: () => PianoSession | null,
  conn: { state: ConnState; onChange(fn: (s: ConnState) => void): unknown },
  myPort: () => number | null,
  periodMs = 0,
): CoBoundBadge {
  const el = h("span", { class: "cobound", id: "cobound", hidden: true });
  let timer: ReturnType<typeof setTimeout> | undefined;
  let busy = false;
  let checked: { s: PianoSession; ok: boolean } | null = null;
  const live = () => ["ready", "waking", "displaced"].includes(conn.state.phase) && (typeof document === "undefined" || !document.hidden);
  const poll = async () => {
    clearTimeout(timer);
    const s = session();
    if (!s || !live()) {
      el.hidden = true;
      return;
    }
    if (!busy) {
      busy = true;
      try {
        const ng = new NgClient(s);
        if (checked?.s !== s) {
          const id = await ng.identify();
          // no answer (busy link): ask again next time; an answer decides for the whole session
          checked = id ? { s, ok: ngHas(id, "session") } : null;
        }
        if (checked && !checked.ok) {
          el.hidden = true;
          return; // stock or a payload without the session core: never shown, no more polls this session
        }
        if (checked) {
          const c = await ng.sessionCore();
          if (session() === s) {
            const text = c ? coBoundText(c, myPort()) : null;
            el.hidden = !text;
            if (text) el.textContent = text;
          }
        }
      } finally {
        busy = false;
      }
    }
    // no periodic poll by default (owner 2026-11-16: no unneeded traffic) - read on connect, focus, visibility, refresh()
    if (periodMs > 0 && live()) timer = setTimeout(() => void poll(), pollDelay(periodMs));
  };
  let was = "";
  conn.onChange((st) => {
    const now = live() ? `${st.gen}` : "";
    if (now === was) return;
    was = now;
    if (now) void poll();
    else {
      clearTimeout(timer);
      el.hidden = true;
    }
  });
  if (typeof document !== "undefined") document.addEventListener("visibilitychange", () => { if (!document.hidden && live()) void poll(); });
  if (typeof addEventListener === "function") addEventListener("focus", () => { if (live()) void poll(); });
  return { el, refresh: () => void poll() };
}
