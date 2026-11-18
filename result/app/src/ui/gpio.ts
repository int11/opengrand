// Settings > Power > Developer: 「GPIO 읽기」 — the read-only GPIO diagnostic op 0x17 (REMOTE_PROTOCOL.md section 10.11). One button reads the snapshot and the change log and lists every
// pin the records name with its direction and level; a checkbox switches the piano's change notices on for this app
// (bound controller) and appends them as they come. Nothing here changes a pin: the piano has no op for that.
// Plain text rows (data-pin / data-signal attributes) so an automated test driving the app can read the result.
import { t } from "../i18n.ts";
import type { PianoSession } from "../protocol/session.ts";
import { type GpioEvent, type GpioSnapshot, gpioPins, GPIO_PINS, NgClient, NgGpio, parseGpioNotice } from "../protocol/ngext.ts";
import { h } from "./dom.ts";

const hex8 = (v: number) => "0x" + v.toString(16).toUpperCase().padStart(2, "0");
const hex32 = (v: number) => "0x" + (v >>> 0).toString(16).toUpperCase().padStart(8, "0");
const roleOf = (name: string) => GPIO_PINS.find((p) => p.name === name)?.role;

/** One change-log line: "12.345 s  GP3[8] (mute request input) → 1". */
export function gpioEventText(e: GpioEvent): string {
  const role = roleOf(e.pin);
  return t("{s} s  {pin} ({role}) → {level}", { s: (e.ms / 1000).toFixed(3), pin: e.pin, role: role ? t(role) : "?", level: e.level });
}

/** The stock bytes of a snapshot in one line. */
export function gpioStockText(s: GpioSnapshot): string {
  return t("Stock: headphones {hp} · GP3[8] sample {lv} · GP3[8] event {ev} · model byte {model} · chip A base {chip}", {
    hp: s.headphones, lv: s.gp3Level, ev: s.gp3Event, model: hex8(s.model), chip: hex8(s.chipA),
  });
}

export function gpioSection(s: PianoSession): HTMLElement {
  const gpio = new NgGpio(new NgClient(s));
  const status = h("p", { class: "muted", id: "gpio-status" });
  const pins = h("div", { id: "gpio-pins" });
  const stock = h("p", { class: "muted", id: "gpio-stock" });
  const raw = h("p", { class: "muted", id: "gpio-raw" });
  const log = h("div", { id: "gpio-log" });
  let unsub: (() => void) | null = null;

  const showLog = (events: GpioEvent[], live = false) => {
    const rows = events.map((e) => h("p", { class: "gpio-event", "data-signal": e.pin, "data-seq": e.seq }, (live ? "● " : "") + gpioEventText(e)));
    if (live) log.prepend(...rows.reverse());
    else log.replaceChildren(...(rows.length ? rows.reverse() : [h("p", { class: "muted" }, t("No change since the payload started."))]));
  };

  const read = async () => {
    status.textContent = t("Reading…");
    const snap = await gpio.snapshot();
    if (!snap) {
      status.textContent = t("No answer from the piano (this firmware may not have the GPIO diagnostic).");
      return;
    }
    pins.replaceChildren(...gpioPins(snap).map((p) =>
      h("div", { class: "row gpio-pin", "data-pin": p.name, "data-dir": p.dir, "data-level": p.level },
        h("span", {}, `${p.name} ${t(p.role)}${p.open ? " *" : ""}`),
        h("span", {}, t("{dir} · now {level} (OUT {out} · IN {in})", { dir: p.dir === "in" ? t("input") : t("output"), level: p.level, out: p.out, in: p.in })))));
    stock.textContent = gpioStockText(snap);
    raw.textContent = ([23, 45, 67] as const).map((pr) => `${pr}: DIR ${hex32(snap.dir[pr])} OUT ${hex32(snap.out[pr])} IN ${hex32(snap.in[pr])}`).join(" · ")
      + " · PINMUX " + snap.pinmux.map((v, i) => `${i}=${hex32(v)}`).join(" ");
    const l = await gpio.log();
    if (l) showLog(l.events);
    status.textContent = t("Read at {s} s after the payload started (* = meaning not confirmed yet).", { s: (snap.ms / 1000).toFixed(1) });
  };

  const notifyBox = h("input", { type: "checkbox", name: "gpioNotify", "data-action": "gpio-notify" }) as HTMLInputElement;
  notifyBox.addEventListener("change", async () => {
    const on = notifyBox.checked;
    const r = await gpio.notify(on);
    if (!r || r.rc !== 0) {
      notifyBox.checked = false;
      status.textContent = !r ? t("No answer from the piano (this firmware may not have the GPIO diagnostic).")
        : r.rc === 2 ? t("The piano is controlled by another device.") : t("The piano rejected this value.");
      unsub?.();
      unsub = null;
      return;
    }
    if (on && !unsub) {
      unsub = s.onMidi((m) => {
        const n = parseGpioNotice(m);
        if (n) showLog(n.events, true);
      });
    } else if (!on) {
      unsub?.();
      unsub = null;
    }
    status.textContent = on ? t("Change notices on: pin changes appear below as they happen.") : t("Change notices off.");
  });

  return h("div", { id: "gpio-diag" },
    h("div", { class: "row" }, h("span", {}, t("GPIO (read only)")),
      h("button", { "data-action": "gpio-read", onclick: () => void read() }, t("Read GPIO"))),
    h("label", { class: "row" }, h("span", {}, t("Change notices")), notifyBox),
    status, pins, stock, raw, log);
}
