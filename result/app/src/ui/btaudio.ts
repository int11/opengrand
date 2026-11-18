// Bluetooth audio state of the piano, one wording for the header chip and the power card: what the piano reports in
// op 0x1A PowerDiag (`btAudio` idle / connected / streaming, `holding`, standby state, ota_power2+
// `tv.btModuleOn`). Only OpenGrand slot payloads with the power port answer op 0x1A.
// The header chip (Bluetooth icon + a short state, ≥ 44 px, on every surface) opens the 「블루투스」 page
// (ui/bluetooth.ts)
import { t } from "../i18n.ts";
import { NgClient, NgPower, type PowerDiag } from "../protocol/ngext.ts";
import type { PianoSession } from "../protocol/session.ts";
import type { ConnState } from "../state/connection.ts";
import { h } from "./dom.ts";
import { pollDelay } from "./pollpace.ts";
import { tabIcon } from "./tabicons.ts";

export { BT_AUDIO_SWITCH_HINT, BT_AUDIO_TAB_HINT, CLOSE_CONFIRM_MS, logLine, sourceText } from "./bluetooth.ts";

export type BtAudioDiag = Pick<PowerDiag, "state" | "btAudio" | "holding"> & { tv?: { btModuleOn: boolean } };

/** What Bluetooth audio does right now, including standby. */
export function btAudioText(d: BtAudioDiag): string {
  if (d.state === "standby") return d.tv?.btModuleOn ? t("Standby · Bluetooth audio listening (sound wakes the piano)") : t("Standby · Bluetooth audio off");
  if (d.btAudio === "streaming") return d.holding ? t("Bluetooth audio playing · keeps the piano on") : t("Bluetooth audio playing");
  if (d.btAudio === "connected") return d.holding ? t("Bluetooth audio connected · keeps the piano on") : t("Bluetooth audio connected");
  return t("Bluetooth audio: no device");
}

/** The chip's short word (a header slot on a 480 px panel holds a few characters). */
export function btAudioShort(d: BtAudioDiag | null): string {
  if (!d) return t("Bluetooth");
  if (d.state === "standby") return t("standby");
  return d.btAudio === "streaming" ? t("playing") : d.btAudio === "connected" ? t("connected") : t("no device");
}

export interface BtAudioBadge {
  el: HTMLElement;
  /** read op 0x1A now (after a power action) */
  refresh(): void;
}

/** Header chip: reads op 0x1A on connect · focus · visibility · refresh() (periodic only if `periodMs` > 0); a tap opens the page. */
export function btAudioBadge(session: () => PianoSession | null, conn: { state: ConnState; onChange(fn: (s: ConnState) => void): unknown },
  open: () => void, periodMs = 0): BtAudioBadge {
  const lbl = h("span", { class: "lbl" }, t("Bluetooth"));
  const el = h("button", { type: "button", class: "btchip", id: "bt-audio", hidden: true, title: t("Bluetooth"), "aria-label": t("Bluetooth"), onclick: () => open() },
    tabIcon("bluetooth"), lbl);
  let timer: ReturnType<typeof setTimeout> | undefined;
  let busy = false;
  const live = () => ["ready", "waking", "displaced"].includes(conn.state.phase) && (typeof document === "undefined" || !document.hidden);
  const show = (d: BtAudioDiag | null) => {
    lbl.textContent = btAudioShort(d);
    const full = d ? btAudioText(d) : t("Bluetooth");
    el.title = full;
    el.setAttribute("aria-label", full);
    el.dataset.bt = d ? (d.state === "standby" ? "standby" : d.btAudio) : "";
  };
  const poll = async () => {
    clearTimeout(timer);
    const s = session();
    if (!s || !live()) {
      el.hidden = true;
      return;
    }
    el.hidden = false;
    if (!busy) {
      busy = true;
      try {
        const d = await new NgPower(new NgClient(s)).diag();
        if (session() === s) show(d);
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
