// OTA loader v4 in the app (REMOTE_PROTOCOL.md section 10.12): the texts of the loader rows, the refusal texts
// (rc 22-25 and the app's own refusals), and the loader confirm after a loader upload.
//
// A loader written over Bluetooth starts on trial at the next power-on (or after "switch loader now", apply
// target 4). Stage-0 never confirms a loader by time: the app confirms it (op 0x0F) once it reaches the new
// stage-1 again and one signed exchange works. Without that, after LOADER_TRIES unconfirmed power-ons stage-0 runs
// the previous, confirmed loader again. The app remembers a loader it uploaded (this storage key) and confirms it
// on the next connect (main.ts) or when the firmware card is checked (info.ts).
import { t } from "../i18n.ts";
import {
  activeName, keyId, LOADER_TRIES, OTA_CAP2, OTA_ERRORS, OTA_KEY_STORAGE, OTA_LOCAL_HELP, OTA_RC_HELP, OtaClient,
  type OtaLocalRefusal, type OtaSlot, type OtaStatus, parseKey, S0_REASON, SLOT_FLAG,
} from "../protocol/ota.ts";
import type { PianoSession } from "../protocol/session.ts";

export const LOADER_PENDING_STORAGE = "ca79ng.otaLoaderPending";

export interface PendingLoader { keyId: number; seq: number; slot: number }

export function loadOtaKey(): Uint8Array | null {
  try {
    return parseKey(localStorage.getItem(OTA_KEY_STORAGE) ?? "");
  } catch {
    return null;
  }
}

export function pendingLoader(): PendingLoader | null {
  try {
    const p = JSON.parse(localStorage.getItem(LOADER_PENDING_STORAGE) ?? "null") as PendingLoader | null;
    return p && typeof p.seq === "number" && typeof p.slot === "number" && typeof p.keyId === "number" ? p : null;
  } catch {
    return null;
  }
}

export function rememberLoaderUpload(p: PendingLoader): void {
  try {
    localStorage.setItem(LOADER_PENDING_STORAGE, JSON.stringify(p));
  } catch {
    /* storage blocked: the confirm button on the firmware card still works */
  }
}

export function clearPendingLoader(): void {
  try {
    localStorage.removeItem(LOADER_PENDING_STORAGE);
  } catch {
    /* nothing stored */
  }
}

/** User text for a refusal code: the v4 rc texts (translated), else the protocol's short name. */
export function rcText(rc: number): string {
  return OTA_RC_HELP[rc] ? t(OTA_RC_HELP[rc]) : OTA_ERRORS[rc] ?? String(rc);
}

/** The line shown when an upload, apply or reject ends in a refusal. */
export function refusalText(why: { rc?: number; refusal?: OtaLocalRefusal; message?: string }): string {
  if (why.refusal) return t(OTA_LOCAL_HELP[why.refusal]);
  if (why.rc !== undefined && OTA_RC_HELP[why.rc]) return t(OTA_RC_HELP[why.rc]!);
  return t("Update failed: {e}", { e: why.message ?? "" });
}


/** The loader texts (i18n source strings), once: the firmware card (info.ts) uses them from here, and the
 *  translation test walks them (OTA_LOADER_STRINGS) - the app bundle goes to the panel over BLE, so no copies. */
export const LS = {
  running: "running", tries: "{n} of {m} tries", givenUp: "given up", safe: "SAFE mode", main: "main", loader: "Loader",
  stage0: "stage-0 v{v}: {r}; loader API {a}, v{iv}; key {k}",
  fellBack: "The new loader was given up; the previous loader runs. Upload it again.",
  waiting: "Switch the piano off and on to start the new loader, then connect again to confirm it.",
  confirmed: "Loader confirmed (slot {s}).", confirmFailed: "Loader confirm failed: {e}",
  file: "Loader file v{v}: {n} bytes, key {id}.",
  written: "Loader written to slot {s} (#{seq}). Switch the piano off and on (or Switch loader now) and connect again: the app confirms it. Unconfirmed, the piano returns to the current loader after 3 power-ons.",
  unconfirmed: "The running loader (slot {s}, #{seq}) is not confirmed. Unconfirmed, the piano returns to the previous loader after 3 power-ons.",
  confirm: "Confirm this loader", reselect: "Switch loader now", reselecting: "Switching loader…",
  reselected: "Now running loader slot {s}.", reselectFailed: "Switching loader failed: {e}",
  safeNote: "SAFE mode (A0 + A#0 held at power-on): the older loader runs, no main. Uploads still work.",
  rollback: "Roll back loader", noMain: "No main is running: nothing to roll back.",
} as const;

const AB = ["A", "B"] as const;

/** "A: #3 (running, confirmed)" / "B: #4 (1 of 3 tries)" */
export function loaderSlotText(i: number, sl: OtaSlot): string {
  const name = AB[i] ?? String(i);
  if (!(sl.flags & SLOT_FLAG.valid)) return `${name}: ${t("empty")}`;
  const tags = [
    sl.flags & SLOT_FLAG.booted ? t(LS.running) : "",
    sl.flags & SLOT_FLAG.confirmed ? t("confirmed") : t(LS.tries, { n: sl.tries, m: LOADER_TRIES }),
    sl.flags & SLOT_FLAG.rejected ? t("rejected") : "",
    sl.flags & SLOT_FLAG.exhausted ? t(LS.givenUp) : "",
    sl.flags & SLOT_FLAG.bad ? t("failed check") : "",
  ].filter(Boolean);
  return `${name}: #${sl.seq} (${tags.join(", ")})`;
}

/** Stage-0 line of the developer view: version, why this loader runs, SAFE, the keys, what main runs. */
export function stage0Text(st: OtaStatus): string {
  const L = st.loader!;
  const hex8 = (v: number) => v.toString(16).padStart(8, "0");
  return t(LS.stage0, {
    v: L.s0Version, r: S0_REASON[L.s0Reason] ?? String(L.s0Reason), a: L.api, iv: L.version,
    k: L.rootKeyId === L.workKeyId ? hex8(L.rootKeyId) : `${hex8(L.rootKeyId)} / ${hex8(L.workKeyId)}`,
  }) + (L.safe ? ` · ${t(LS.safe)}` : "") + ` · ${t(LS.main)}: ${activeName(st)}`;
}

/** The running loader waits for its confirm op 0x0F (v4). */
export function loaderUnconfirmed(st: OtaStatus | null): boolean {
  const L = st?.loader;
  return !!L && L.caps2 & OTA_CAP2.loaderConfirm ? L.running <= 1 && !(L.slots[L.running]!.flags & SLOT_FLAG.confirmed) : false;
}

export type LoaderConfirmOutcome = { state: "confirmed" | "waiting" | "fellBack" | "failed" | "none"; message: string; status?: OtaStatus };

/** After a loader upload: confirm the new loader once it runs. "waiting" = the piano still runs the old loader
 *  (not switched off and on yet); "fellBack" = the new loader was given up (tries used, failed check, rejected) -
 *  the remembered upload is dropped then, as after a confirm. Without a remembered upload, `force` confirms the
 *  running loader anyway (the button). */
export async function confirmLoader(session: PianoSession, key: Uint8Array, force = false): Promise<LoaderConfirmOutcome> {
  // another firmware job runs on this link (an upload): the automatic confirm waits for the refresh after it
  if (!force && OtaClient.busy(session)) return { state: "none", message: "" };
  const ota = new OtaClient(session);
  const st = await ota.status();
  const L = st?.loader;
  if (!st || !L || !(L.caps2 & OTA_CAP2.loaderConfirm)) return { state: "none", message: "", status: st ?? undefined };
  const p = pendingLoader();
  if (!force && (!p || p.keyId !== keyId(key))) return { state: "none", message: "", status: st };
  if (p && !force) {
    const sl = L.slots[p.slot];
    if (!sl || !(sl.flags & SLOT_FLAG.valid) || sl.seq !== p.seq || sl.flags & (SLOT_FLAG.rejected | SLOT_FLAG.exhausted | SLOT_FLAG.bad)) {
      clearPendingLoader();
      return { state: "fellBack", message: t(LS.fellBack), status: st };
    }
    if (L.running !== p.slot) return { state: "waiting", message: t(LS.waiting), status: st };
  }
  // again right before the op (no await between this check and the lock): an overlapping automatic confirm (connect
  // + a firmware-card refresh) may have taken the link while the status was read
  if (!force && OtaClient.busy(session)) return { state: "none", message: "", status: st };
  const r = await ota.confirmLoader(key);
  if (r.ok) {
    const running = r.status?.loader?.running ?? L.running;
    if (!p || running === p.slot) clearPendingLoader();
    return { state: "confirmed", message: t(LS.confirmed, { s: AB[running] ?? "?" }), status: r.status };
  }
  return { state: "failed", message: t(LS.confirmFailed, { e: r.rc !== undefined ? rcText(r.rc) : r.message }), status: r.status ?? st };
}

/** On connect (main.ts): only when this app uploaded a loader that waits for its confirm - nothing is sent
 *  otherwise. Returns the line to show, or "" when there is nothing to say. */
export async function confirmLoaderOnConnect(session: PianoSession): Promise<string> {
  const key = loadOtaKey();
  if (!key || !pendingLoader()) return "";
  const r = await confirmLoader(session, key);
  return r.state === "confirmed" || r.state === "fellBack" || r.state === "failed" ? r.message : "";
}

/** Every loader string that only shows in some states (tests/system.dom.test.ts: all translated). */
export const OTA_LOADER_STRINGS: string[] = [...Object.values(OTA_RC_HELP), ...Object.values(OTA_LOCAL_HELP), ...Object.values(LS)];
