// Recorder tab (ui/recorder.ts): the USB memory as a player source - what the bar shows while a USB file plays or after
// one was chosen in the USB list, and ⏮ ⏭ through the files the USB player plays itself.
import { t } from "../i18n.ts";
import type { AppContext } from "./context.ts";
import type { UsbPane } from "./files.ts";
import type { ListId, PlayerSource } from "./recmodel.ts";
import { isRecState, type PlayerView } from "./screens/songctl.ts";
import { KINDS, kindOf } from "./songkinds.ts";
import { playerOf, usbAnyRunning, usbNow, type UsbPlayback } from "./usbplay.ts";

export interface UsbSource extends PlayerSource {
  /** The bar is the USB player's: a USB file plays (never while a recording runs or waits), or one was chosen last
   *  and the piano song is at rest - a song that plays or is paused (started on the panel, the piano's buttons) is
   *  what the bar must show and its ■ must stop. */
  active(): boolean;
}

/** state: the piano recorder's state (STATE_TEXT index, null = not said); src: what the bar last held */
export function usbSource(ctx: AppContext, pb: UsbPlayback, usb: UsbPane, state: () => number | null, src: () => ListId): UsbSource {
  /** The files the USB player plays itself - what ⏮ ⏭ step through (a KSO is played through a piano song). */
  const direct = () => ctx.usbFiles.filter((f) => { const k = kindOf(f); return !k || KINDS[k].howPlayed === "direct"; });
  /** The USB file the bar is about: the one that plays, else the one chosen in the list. */
  const usbItem = () => (usbAnyRunning(pb) ? usbNow(pb)?.name ?? null : usb.selected());
  return {
    active: () => (usbAnyRunning(pb) ? !isRecState(state()) : src() === "usb" && (state() === null || state() === 2)),
    view(): PlayerView {
      const on = usbAnyRunning(pb);
      const now = on ? usbNow(pb) : null;
      const name = usbItem();
      const files = direct();
      const k = name === null ? -1 : files.indexOf(name);
      const kind = name === null ? null : kindOf(name);
      return {
        state: on ? 1 : 2, stateText: on ? t("USB playing") : undefined,
        // the whole name with its type; "USB" yields first when the name is long
        title: name !== null ? name.slice(name.lastIndexOf("\\") + 1) : on ? t("USB memory") : t("Choose a song below."),
        sub: name !== null ? "USB" : "",
        // a file that does not play has no position; its kind says which unit will come
        unit: now ? now.unit : name !== null && playerOf(name) === "smf" ? "bar" : "ms",
        pos: now ? now.pos : null, total: now ? now.total : null,
        seekable: false, main: on ? "stop" : "play",
        prev: k > 0, next: files.length > 0 && k < files.length - 1,
        part: kind ? KINDS[kind].parts : false,
      };
    },
    step(d) {
      const name = usbItem() ?? usb.selected();
      const files = direct();
      const to = files[(name === null ? (d > 0 ? -1 : files.length) : files.indexOf(name)) + d];
      if (to !== undefined) usb.pick(to, false); // plays only if a USB file plays now
    },
  };
}
