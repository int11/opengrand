// The device picker ("Connect to the piano"). On the Android shell there is always a Bluetooth MIDI row at the top:
// while the shell's 5 s BLE scan runs it counts the seconds, the pianos it finds are listed under it, and when
// nothing was found it says why that usually happens - the piano's BLE-MIDI module takes ONE connection, so a
// PC or another phone holding it hides the piano - with a [Search again] button. Browsers get the Web Bluetooth
// entry instead (the browser's own chooser). The desktop app (Electron shell with the in-app device list) searches
// as soon as the dialog opens and lists what it finds under the same Bluetooth MIDI row - no second window: a row
// connects, closing the dialog or picking another kind of device gives the search up. The firmware simulator entry exists only in development (a browser
// on localhost / ?sim), never on the shell or the panel (field 2026-11-01: the phone showed only the simulator).
// section 5.
import { t } from "../i18n.ts";
import type { LinkChoice } from "../state/connection.ts";
import type { BtFound } from "../transport/desktop.ts";
import type { LinkCandidate } from "../transport/link.ts";
import { h } from "./dom.ts";

export const NOT_FOUND_TEXT = "No piano found — another device (PC/phone) may be connected to it";
export const SCAN_MS = 5000;

/** A Bluetooth search whose devices this dialog lists itself (desktop app: transport/desktop.ts desktopBleScan). */
export interface BleScan {
  /** the chosen device as a link to open; rejects when the search ended without one */
  found: Promise<LinkChoice>;
  pick(deviceId: string): void;
  /** give the search up; resolves when another link may be opened */
  cancel(): Promise<void>;
}

export interface PickerDeps {
  /** Android shell: BLE scan row */
  shell: boolean;
  /** show the firmware simulator entry (development only: see devSimulator) */
  dev: boolean;
  /** every candidate now (shell devices, panel USB, Web MIDI) */
  list(): Promise<LinkCandidate[]>;
  /** shell: start a BLE scan, onChange as devices appear; returns stop */
  scan?(onChange: () => void): () => void;
  /** key of the link that is live now (connected / connecting), else null - drives the "in use" mark */
  inUseKey(): string | null;
  choose(c: LinkChoice): void;
  /** browsers: the Web Bluetooth entry */
  requestBle?: LinkChoice;
  /** desktop app: start a Bluetooth search listed in this dialog (called inside the user's click - Web Bluetooth
   *  needs the gesture); replaces the requestBle entry */
  bleScan?(onDevices: (list: BtFound[]) => void): BleScan;
  simUrl: string;
  openSim?(url: string): LinkChoice;
  scanMs?: number;
  tickMs?: number;
}

/** The simulator entry only in development: not on the shell / panel, only on a localhost origin or with ?sim. */
export function devSimulator(env: { shell: boolean; kwm: boolean; embedded: boolean; hostname: string; search: string }): boolean {
  if (env.shell || env.kwm || env.embedded) return false;
  return /^(localhost|127\.0\.0\.1|\[::1\]|::1)$/.test(env.hostname) || new URLSearchParams(env.search).has("sim");
}

type Shown = Pick<LinkCandidate, "kind" | "name" | "key">;
const isBle = (c: Shown) => /BLE|Bluetooth/i.test(c.name) || c.kind === "ble";

const ICON_BT = '<svg viewBox="0 0 24 24" focusable="false"><path d="M7 7l10 10-5 4V3l5 4L7 17"/></svg>';
const ICON_USB = '<svg viewBox="0 0 24 24" focusable="false"><path d="M12 3v15"/><path d="M9 6l3-3 3 3"/><path d="M12 14l-4-3V9"/><path d="M12 12l4-2V8"/><circle cx="12" cy="19.5" r="1.8"/></svg>';
const ICON_MIDI = '<svg viewBox="0 0 24 24" focusable="false"><circle cx="12" cy="12" r="8"/><circle cx="8.5" cy="11" r=".6"/><circle cx="15.5" cy="11" r=".6"/><circle cx="12" cy="8.5" r=".6"/><circle cx="10" cy="14.5" r=".6"/><circle cx="14" cy="14.5" r=".6"/></svg>';

/** How a candidate reaches the piano, for the row's icon and second line. */
export function transportOf(c: Shown): "ble" | "usb" | "sim" | "midi" {
  if (isBle(c)) return "ble";
  if (c.kind === "websocket") return "sim";
  if (/USB/i.test(c.name)) return "usb";
  return "midi";
}

/** The piano's own name: the driver suffix ("(BLE-MIDI)", "(USB-MIDI)", "(KWMcore …)") moves to the second line. */
export function deviceName(c: Shown): string {
  return c.name.replace(/\s*\((?:BLE|USB|KWMcore)[^)]*\)\s*$/i, "").trim() || c.name;
}

/**
 * One device in the list (owner 2026-11-15: the old row "CA79 (BLE-MIDI) — android" read like a settings button, not
 * a device to pick): icon · name · how it connects · a 「연결」 pill on the right (「✓ 사용 중」 for the live link).
 */
export function deviceRow(c: Shown, inUse: boolean, onPick: () => void): HTMLButtonElement {
  const tr = transportOf(c);
  const ico = h("span", { class: "dev-ico", "aria-hidden": "true" });
  ico.innerHTML = tr === "ble" ? ICON_BT : tr === "usb" ? ICON_USB : ICON_MIDI;
  const sub = tr === "ble" ? t("Bluetooth MIDI") : tr === "usb" ? t("USB MIDI") : tr === "sim" ? t("Firmware simulator") : t("MIDI");
  return h("button", { class: inUse ? "item dev on" : "item dev", "data-transport": tr, "data-key": c.key, onclick: onPick },
    ico,
    h("span", { class: "dev-txt" }, h("span", { class: "dev-name" }, deviceName(c)), h("span", { class: "dev-sub" }, sub)),
    h("span", { class: "dev-go" }, inUse ? `✓ ${t("in use")}` : t("Connect")));
}

export function openPicker(deps: PickerDeps): HTMLDialogElement {
  const dlg = h("dialog", { class: "sheet", id: "picker" }) as HTMLDialogElement;
  const list = h("div", { class: "list" }, h("p", { class: "muted" }, t("Searching…")));
  dlg.append(h("h3", {}, t("Connect to the piano")), list, h("button", { onclick: () => dlg.close() }, t("Close")));
  document.body.append(dlg);
  dlg.showModal();
  const scanMs = deps.scanMs ?? SCAN_MS;
  const tickMs = deps.tickMs ?? 1000;
  let scanning = deps.shell && !!deps.scan; // the first draw already says "searching"
  let started = Date.now();
  let stopScan: (() => void) | null = null;
  let timer: ReturnType<typeof setInterval> | undefined;
  let cands: LinkCandidate[] = [];
  // desktop app: the Bluetooth search of this dialog (null = none running: not started, ended, given up)
  const inApp = !deps.shell && !!deps.bleScan;
  let bleScan: BleScan | null = null;
  let bleGen = 0; // answers of a search that was given up are ignored
  let bleFound: BtFound[] = [];
  let bleHint = false;
  let hintTimer: ReturnType<typeof setTimeout> | undefined;
  const stopBle = (): Promise<void> => {
    const s = bleScan;
    bleScan = null;
    bleGen++;
    clearTimeout(hintTimer);
    return s ? s.cancel() : Promise.resolve();
  };
  // another row: the running Bluetooth search is given up first, its link opens after that request is over
  const pick = (c: LinkChoice) => () => {
    const searching = bleScan !== null;
    const stopped = stopBle();
    dlg.close();
    if (searching) void stopped.then(() => deps.choose(c));
    else deps.choose(c); // browsers: synchronous (requestBle needs the click's gesture)
  };
  const mark = (key?: string) => (key && key === deps.inUseKey() ? ` ✓ ${t("in use")}` : "");
  const item = (c: LinkCandidate) => deviceRow(c, !!c.key && c.key === deps.inUseKey(), pick(c));

  const draw = () => {
    const rows: HTMLElement[] = [];
    const ble = cands.filter(isBle);
    const other = cands.filter((c) => !isBle(c));
    if (deps.shell) {
      // the Bluetooth MIDI row: same place and look as the browsers' "Bluetooth MIDI (pair new)…"
      let head: HTMLElement;
      if (scanning) {
        const s = Math.max(1, Math.ceil((scanMs - (Date.now() - started)) / 1000));
        head = h("div", { class: "pick-sec", id: "bt-scan", "data-state": "scanning" }, t("Searching for Bluetooth MIDI… {n} s", { n: s }));
      } else if (!ble.length) {
        head = h("div", { class: "pick-sec pick-none", id: "bt-scan", "data-state": "none" },
          h("p", { class: "muted" }, t(NOT_FOUND_TEXT)),
          h("button", { id: "bt-rescan", onclick: () => startScan() }, t("Search again")));
      } else {
        head = h("div", { class: "pick-sec", id: "bt-scan", "data-state": "found" }, t("Bluetooth MIDI"));
      }
      rows.push(head, ...ble.map(item));
    } else if (inApp) {
      // same row as the Android shell; the search has no fixed end here (the shell's scan goes on until answered)
      if (bleScan) {
        rows.push(h("div", { class: "pick-sec", id: "bt-scan", "data-state": "scanning" }, `${t("Bluetooth MIDI")} — ${t("Searching…")}`));
        if (bleHint && !bleFound.length) rows.push(h("p", { class: "muted", id: "bt-hint" }, t(NOT_FOUND_TEXT)));
      } else {
        rows.push(h("div", { class: "pick-sec pick-none", id: "bt-scan", "data-state": "none" },
          h("p", { class: "muted" }, t(NOT_FOUND_TEXT)),
          h("button", { id: "bt-rescan", onclick: () => startBle() }, t("Search again"))));
      }
      for (const d of bleScan ? bleFound : []) {
        const row = deviceRow({ kind: "ble", name: d.deviceName || d.deviceId }, false, () => bleScan?.pick(d.deviceId));
        row.dataset.id = d.deviceId;
        rows.push(row);
      }
      rows.push(...ble.map(item));
    } else {
      rows.push(...ble.map(item));
    }
    rows.push(...other.map(item));
    if (!deps.shell && !inApp && deps.requestBle) rows.push(h("button", { class: "item", id: "bt-pair", onclick: pick(deps.requestBle) }, t("Bluetooth MIDI (pair new)…")));
    if (deps.dev && deps.openSim) {
      const key = `websocket:${deps.simUrl}`;
      rows.push(h("button", { class: "item", id: "sim-item", onclick: pick(deps.openSim(deps.simUrl)) }, t("Firmware simulator ({url})", { url: deps.simUrl }) + mark(key)));
    }
    list.replaceChildren(...rows);
  };
  const refresh = async () => {
    try {
      cands = await deps.list();
    } catch {
      cands = [];
    }
    draw();
  };
  const endScan = () => {
    scanning = false;
    clearInterval(timer);
    stopScan?.();
    stopScan = null;
    void refresh();
  };
  function startScan(): void {
    if (!deps.scan) return;
    stopScan?.();
    scanning = true;
    started = Date.now();
    stopScan = deps.scan(() => void refresh());
    clearInterval(timer);
    timer = setInterval(() => {
      if (Date.now() - started >= scanMs) endScan();
      else draw();
    }, tickMs);
    draw();
  }
  function startBle(): void {
    if (!deps.bleScan) return;
    void stopBle();
    const gen = bleGen;
    bleFound = [];
    bleHint = false;
    const s = deps.bleScan((l) => {
      if (gen !== bleGen) return;
      bleFound = l;
      draw();
    });
    bleScan = s;
    s.found.then((c) => {
      if (gen !== bleGen) return; // given up meanwhile
      bleScan = null;
      dlg.close();
      deps.choose(c);
    }, () => {
      // ended without a device (nothing in range, no gesture when the dialog opened by itself, Bluetooth off):
      // the "not found" row with [Search again] - a click there is a gesture
      if (gen !== bleGen) return;
      bleScan = null;
      clearTimeout(hintTimer);
      draw();
    });
    hintTimer = setTimeout(() => {
      bleHint = true;
      draw();
    }, scanMs);
    draw();
  }
  dlg.addEventListener("close", () => {
    clearInterval(timer);
    stopScan?.();
    void stopBle();
    dlg.remove();
  });
  if (inApp) startBle(); // now, not after the list: still inside the click that opened the dialog
  void refresh().then(() => { if (deps.shell) startScan(); });
  return dlg;
}

/** A stored last link that is the simulator: dropped outside development (field 2026-11-01, phone shell). */
export function dropStoredSim(storage: Pick<Storage, "getItem" | "removeItem">, key: string, dev: boolean): boolean {
  if (dev) return false;
  try {
    const l = JSON.parse(storage.getItem(key) ?? "null") as { key?: string } | null;
    if (l?.key?.startsWith("websocket:")) {
      storage.removeItem(key);
      return true;
    }
  } catch { /* storage unavailable / not JSON */ }
  return false;
}

/** App start: reopen the remembered piano; if that fails (another device holds its BLE link, out of range) open the
 *  picker instead of leaving a silent "failed". */
export async function reopenRemembered(conn: { choose(c: LinkChoice): Promise<unknown>; readonly phase: string }, c: LinkChoice, pick: () => void): Promise<void> {
  try {
    await conn.choose(c);
  } catch { /* the phase says it */ }
  if (conn.phase === "failed" || conn.phase === "idle") pick();
}
