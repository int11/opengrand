// Information screen (feature 78) with the panel/APK update (86) and the
// BLE-MIDI module firmware update (87).
import { type DfuState, dfuSupported, queryModuleVersion, runDfu } from "../features/dfu.ts";
import { NgClient, type NgDumpStatus, ngHas } from "../protocol/ngext.ts";
import { PROBE_FEATURE, probeReport } from "./probe.ts";
import { btDiagBox } from "./btdiag.ts";
import { panelUiBox } from "./paneluibox.ts";
import { panelScreenBox } from "./panelscreenbox.ts";
import { panelToolsBox } from "./paneltools.ts";
import { powerDevSection } from "./power.ts";
import { ramCanarySection } from "./ramcanary.ts";
import { kwmcoreAvailable } from "../transport/kwmcore.ts";
import {
  activeName, ApplyTarget, keyId, OTA_APPLY_STATE, OTA_CAP, OTA_CAP2, OTA_KEY_STORAGE, OTA_REASON, OtaClient, OtaTarget, type OtaStatus,
  loaderRollbackMask, parseKey, parseOtaFile, SLOT_FLAG,
} from "../protocol/ota.ts";
import {
  confirmLoader, LS, loadOtaKey, loaderSlotText, loaderUnconfirmed, pendingLoader, rcText, refusalText, rememberLoaderUpload, stage0Text,
} from "./otaloader.ts";
import { type UpdateCheck, checkApkUpdate, installApkUpdate } from "../features/update.ts";
import { selfUpdateHub } from "../features/selfupdate.ts";
import { selfUpdateBox } from "./selfupdatebox.ts";
import { t } from "../i18n.ts";
import { shellInfo } from "../transport/android.ts";
import type { AppContext } from "./context.ts";
import { h, section } from "./dom.ts";

declare const __APP_VERSION__: string | undefined;
export const APP_VERSION: string = typeof __APP_VERSION__ === "string" ? __APP_VERSION__ : "dev";

// Layout (user 2026-11-02; owner 2026-11-14): one card per job — 정보(OpenGrand 포함) ·
// 펌웨어 업데이트 · 패널 UI 업데이트 · 앱 / BLE-MIDI 모듈 업데이트 · 개발자용 (collapsed, 패널 화면 보기 포함). Short labels never break (`.lbl`), long values
// sit under their label and wrap inside the card (`.row.stack` + `.val`).

/** label · short value on one line (versions). */
function row(label: string, value: string | Node, id?: string): HTMLElement {
  return h("div", { class: "row" }, h("span", { class: "lbl" }, label), typeof value === "string" ? h("output", { id }, value) : value);
}

/** label above; below it a long value that wraps inside the card, with an optional button at its right. */
function stackRow(label: string, value: Node, action?: Node): HTMLElement {
  return h("div", { class: "row stack" }, h("span", { class: "lbl" }, label), h("div", { class: "val" }, value, action ?? null));
}

/** A whole card that folds (개발자용): its summary looks like a card title. */
function cardFold(d: HTMLDetailsElement): HTMLDetailsElement {
  d.classList.add("card-fold");
  return d;
}

/** A section whose body opens on its title (개발자용, 업데이트 키): closed unless `open`. */
function fold(id: string, title: Node | string, open: boolean, ...children: (Node | null)[]): HTMLDetailsElement {
  return h("details", { id, class: "fold", open }, h("summary", {}, title), ...children) as HTMLDetailsElement;
}

export function infoScreen(ctx: AppContext): HTMLElement {
  const s = ctx.session();
  const id = s?.identity ?? null;
  const shell = shellInfo();
  const upHub = selfUpdateHub();
  const bleOut = h("output", { id: "ble-version" }, "–");

  const checkBle = async () => {
    const cur = ctx.session();
    if (!cur) return;
    bleOut.textContent = t("Checking…");
    const v = await queryModuleVersion(cur);
    bleOut.textContent = v ?? t("No reply");
  };

  // ---- panel UI / APK update from USB memory ----
  const updBox = h("div", { id: "apk-update" });
  const showUpdate = (r: UpdateCheck) => {
    if (r.error === "unsupported") {
      updBox.replaceChildren(h("p", { class: "muted" }, t("Updates are installed by the Android shell (not available in a browser).")));
      return;
    }
    if (!r.found) {
      updBox.replaceChildren(
        h("p", { class: "muted" }, r.error ? t("Update check failed: {e}", { e: r.error }) : t("No update package on the USB memory (CA79NG/manifest.json).")),
        h("button", { onclick: () => showUpdate(checkApkUpdate()) }, t("Check again")),
      );
      return;
    }
    if (!r.newer) {
      updBox.replaceChildren(h("p", { class: "muted" }, t("Version {v} on the USB memory is not newer.", { v: r.version ?? "?" })));
      return;
    }
    const status = h("p", { class: "muted" });
    const btn = h("button", { class: "primary", id: "apk-update-btn" }, t("Update to {v}", { v: r.version ?? "?" }));
    btn.addEventListener("click", () => {
      (btn as HTMLButtonElement).disabled = true;
      const ok = installApkUpdate((st) => {
        status.textContent = st.state === "error" ? t("Update failed: {e}", { e: String(st.message ?? "") }) : st.state === "done" ? t("Update installed.")
          : st.state === "needs-source" ? t("Turn on 「Allow from this source」 for OpenGrand in the settings that just opened, then come back and install again.") : t("Installing…");
        if (st.state === "error" || st.state === "needs-source") (btn as HTMLButtonElement).disabled = false;
      });
      if (!ok) {
        status.textContent = t("Update failed: {e}", { e: "start" });
        (btn as HTMLButtonElement).disabled = false;
      }
    });
    updBox.replaceChildren(btn, status);
  };
  showUpdate(checkApkUpdate());

  // ---- BLE-MIDI module DFU ----
  const bar = h("progress", { id: "dfu-progress", max: 100, value: 0, hidden: true }) as HTMLProgressElement;
  const dfuStatus = h("p", { class: "muted", id: "dfu-status" });
  const phaseText = (st: DfuState): string => {
    switch (st.phase) {
      case "query": return t("Reading module version…");
      case "package": return t("Looking for the update package…");
      case "enter": return t("Restarting the module in update mode…");
      case "scan": return t("Searching for the module bootloader…");
      case "upload": return t("Updating… {p}%", { p: st.progress ?? 0 });
      case "done": return t("Module updated.");
      case "uptodate": return t("Module firmware {v} is up to date.", { v: st.moduleVersion ?? "?" });
      case "error": return t("Module update failed: {e}", { e: st.message ?? "" });
    }
  };
  const dfuBtn = h("button", { id: "dfu-btn" }, t("Update BLE module")) as HTMLButtonElement;
  dfuBtn.addEventListener("click", async () => {
    const cur = ctx.session();
    if (!cur) {
      dfuStatus.textContent = t("Connect to the piano first.");
      return;
    }
    dfuBtn.disabled = true;
    bar.hidden = false;
    const final = await runDfu(cur, (st) => {
      dfuStatus.textContent = phaseText(st);
      if (st.moduleVersion) bleOut.textContent = st.moduleVersion;
      bar.value = st.phase === "upload" ? (st.progress ?? 0) : st.phase === "done" ? 100 : bar.value;
    });
    dfuBtn.disabled = false;
    if (final.phase !== "done") bar.hidden = true;
  });

  // ---- OpenGrand custom firmware (manufacturer ID 0x7D, REMOTE_PROTOCOL.md section 10) ----
  // Stock firmware never answers this family, so "no reply" is a result, not a failure.
  const ngOut = h("output", { id: "ng-firmware" }, "–");
  const ngDumpBox = h("div", { id: "ng-dump", hidden: true });
  const ngBar = h("progress", { id: "ng-progress", max: 96, value: 0, hidden: true }) as HTMLProgressElement;
  const ngDumpStatus = h("output", { id: "ng-dump-status" }, "");
  let ngPoll: ReturnType<typeof setInterval> | null = null;

  const stopPoll = () => {
    if (ngPoll !== null) clearInterval(ngPoll);
    ngPoll = null;
  };
  const showDump = (st: NgDumpStatus) => {
    ngBar.hidden = st.state !== "running" && st.state !== "closing";
    ngBar.value = st.chunk;
    const pct = Math.round((st.chunk / st.chunks) * 100);
    ngDumpStatus.textContent =
      st.state === "running" ? t("Dumping… {pct}% ({a}/{b} chunks)", { pct: String(pct), a: String(st.chunk), b: String(st.chunks) })
      : st.state === "done" ? t("Done — CA79NOR.BIN written to the USB memory.")
      : st.state === "error" ? t("Failed: {e}", { e: st.errorText })
      : st.state === "idle" ? t("Idle.")
      : t("Working…");
    if (st.state === "done" || st.state === "error" || st.state === "idle") stopPoll();
  };

  const ngCheck = async () => {
    const cur = ctx.session();
    if (!cur) {
      ngOut.textContent = t("Connect to the piano first.");
      return;
    }
    ngOut.textContent = t("Checking…");
    const ng = new NgClient(cur);
    const info = await ng.identify();
    if (!info) {
      ngOut.textContent = t("Stock firmware (no reply)");
      ngDumpBox.hidden = true;
      ngProbeBox.hidden = true;
      canaryBox.hidden = true;
      otaBox.hidden = true;
      btBox.show(false);
      devShown();
      return;
    }
    // headline, then one chip per feature; the ", " between chips is hidden but stays in textContent, so
    // automation and tests still read "identify, status, …"
    const names = info.featureNames.length ? info.featureNames : [t("no features")];
    const chips = h("span", { class: "fchips", id: "ng-features" });
    names.forEach((n, i) => chips.append(...(i ? [h("span", { class: "sep" }, ", ")] : []), h("span", { class: "fchip" }, n))); // no flatMap: Chrome 71
    ngOut.replaceChildren(
      h("span", { class: "ng-head" }, t("OpenGrand rev {r} on {v}", { r: String(info.protocol), v: info.baseVersion })),
      h("span", { class: "sep" }, " — "),
      chips,
    );
    ngDumpBox.hidden = !ngHas(info, "nordump");
    let anyProbe = false;
    for (const [btn, kind] of probeBtns) {
      btn.hidden = !ngHas(info, PROBE_FEATURE[kind]);
      anyProbe ||= !btn.hidden;
    }
    ngProbeBox.hidden = !anyProbe;
    canaryBox.hidden = !ngHas(info, "ramcanary");
    btBox.show(ngHas(info, "bt"));
    devShown();
    // The OTA ops live in the resident loader, not in the payload: a slot payload that does not
    // advertise "ota" (e.g. a demo) still runs on a loader that can roll it back - ask the loader.
    otaBox.hidden = !ngHas(info, "ota") && !(await new OtaClient(cur).status(800).catch(() => null));
    devShown();
    if (!otaBox.hidden) await otaRefresh();
    const st = await ng.status();
    if (st) showDump(st);
  };

  const ngDumpBtn = h("button", {
    onclick: async () => {
      const cur = ctx.session();
      if (!cur) return;
      const ng = new NgClient(cur);
      const rc = await ng.startDump();
      if (rc === null) {
        ngDumpStatus.textContent = t("No reply.");
        return;
      }
      if (rc !== 0) {
        const st = await ng.status();
        ngDumpStatus.textContent = st ? t("Failed: {e}", { e: st.errorText }) : t("Rejected ({c}).", { c: String(rc) });
        return;
      }
      stopPoll();
      ngPoll = setInterval(() => {
        void (async () => {
          const st = await ng.status();
          if (st) showDump(st);
        })();
      }, 1000);
    },
  }, t("Dump NOR flash to USB"));

  const ngCancelBtn = h("button", {
    onclick: async () => {
      const cur = ctx.session();
      if (!cur) return;
      stopPoll();
      await new NgClient(cur).cancel();
      const st = await new NgClient(cur).status();
      if (st) showDump(st);
    },
  }, t("Cancel"));

  ngDumpBox.append(
    h("p", { class: "muted" }, t("Reads the whole 6 MB NOR flash and writes CA79NOR.BIN to the USB memory. Nothing is written to the flash.")),
    h("div", { class: "inline" }, ngDumpBtn, " ", ngCancelBtn),
    ngBar,
    ngDumpStatus,
  );

  // ---- diagnostic probe ops (phase2c_probe, baseline; REMOTE_PROTOCOL.md section 10.1) ----
  // Each button shows only when identify lists its feature bit.
  const ngProbeBox = h("div", { id: "ng-probe", hidden: true });
  const btBox = btDiagBox(ctx); // BT module diagnostics (ota_bt, feat2 "bt")
  const probeOut = h("pre", { id: "ng-probe-out" }, "");
  const probeBtns: [HTMLElement, Parameters<typeof probeReport>[1]][] = [];
  const probeBtn = (id: string, label: string, kind: Parameters<typeof probeReport>[1]) => {
    const btn = h("button", {
      id,
      onclick: async () => {
        const cur = ctx.session();
        if (!cur) return;
        probeOut.textContent = t("Checking…");
        probeOut.textContent = (await probeReport(new NgClient(cur), kind)) ?? t("No reply.");
      },
    }, label);
    probeBtns.push([btn, kind]);
    return btn;
  };
  ngProbeBox.append(
    h("p", { class: "muted" }, t("Diagnostic probe: reads piano state only. Nothing is written to the flash.")),
    h("div", { class: "inline" },
      probeBtn("ng-probe-port", t("Port"), "port"), " ",
      probeBtn("ng-probe-link", t("Link test"), "link"), " ",
      probeBtn("ng-probe-apo", t("APO / Bluetooth"), "apo"), " ",
      probeBtn("ng-probe-btlog", t("Bluetooth log"), "btlog"), " ",
      probeBtn("ng-probe-arm", t("Arm canary"), "arm"), " ",
      probeBtn("ng-probe-canary", t("Check canary"), "canary"), " ",
      probeBtn("ng-probe-core", t("Core self-test"), "core")),
    probeOut,
  );

  // ---- remote cave update over Bluetooth (ota_loader; REMOTE_PROTOCOL.md section 10.3) ----
  // Shown only when identify lists "ota". The per-device key (result/stockfw/secrets/ota_key.bin)
  // is imported once and kept in this browser's storage; payload files are build/ota_*.c79.
  const otaBox = h("section", { class: "card", id: "ng-ota", hidden: true });
  const otaState = h("output", { id: "ota-state" }, "–");
  const otaKeyOut = h("output", { id: "ota-key" }, "");
  const otaMsg = h("p", { class: "muted", id: "ota-status" });
  const otaBar = h("progress", { id: "ota-progress", max: 100, value: 0, hidden: true }) as HTMLProgressElement;
  let otaFile: Uint8Array | null = null;
  const loadKey = loadOtaKey;
  const hex8 = (v: number) => v.toString(16).padStart(8, "0");
  // 업데이트 키: its own fold, closed once a key is in (its file picker is not the payload's)
  const keyFold = fold("ota-keys", h("span", { class: "fold-title" }, t("Update key"), h("span", { class: "fold-note" }, otaKeyOut)), true);
  const showKey = (collapse = false) => {
    const k = loadKey();
    otaKeyOut.textContent = k ? t("Key {id} imported.", { id: hex8(keyId(k)) }) : t("No key imported.");
    if (!k) keyFold.open = true;
    else if (collapse) keyFold.open = false;
  };
  const slotText = (name: string, sl: OtaStatus["slots"][number]) => {
    // the loader reads the slots only on a slot boot: after the escape keys it goes built-in first, so an unread
    // slot is "not read", not "empty" (field 2026-11-15)
    if (!(sl.flags & (SLOT_FLAG.scanned | SLOT_FLAG.valid))) return `${name}: ${t("not read")}`;
    if (!(sl.flags & SLOT_FLAG.valid)) return `${name}: ${t("empty")}`;
    const tags = [
      sl.flags & SLOT_FLAG.confirmed ? t("confirmed") : t("unconfirmed"),
      sl.flags & SLOT_FLAG.liveOk && !(sl.flags & SLOT_FLAG.confirmed) ? t("ran live") : "",
      sl.flags & SLOT_FLAG.rejected ? t("rejected") : "",
      sl.flags & SLOT_FLAG.bad ? t("failed check") : "",
    ].filter(Boolean);
    return `${name}: #${sl.seq} (${tags.join(", ")})`;
  };
  const otaDiag = h("p", { class: "muted", id: "ota-diag", hidden: true });
  // ---- ota_loader_v4 (section 10.12): loader slots, stage-0, SAFE mode, the loader confirm; hidden below layout 3
  const otaLoaderState = h("output", { id: "ota-loader-state" }, "–");
  const otaS0 = h("p", { class: "muted", id: "ota-s0", hidden: true });
  const otaSafe = h("p", { class: "muted", id: "ota-safe", hidden: true },
    t(LS.safeNote));
  const otaLoaderNote = h("p", { class: "muted", id: "ota-loader-note" });
  /** image versions of the slots (op 0x0E), "#seq v<version>" in the slot rows */
  const slotVersion = new Map<number, number>();
  const withVersion = (text: string, target: number) => {
    const v = slotVersion.get(target);
    return v ? text.replace(/#(\d+)/, `#$1 v${v}`) : text;
  };
  const showOta = (st: OtaStatus) => {
    otaState.textContent = `${activeName(st)} — ${OTA_REASON[st.reason] ?? st.reason} · ${withVersion(slotText("A", st.slots[0]), OtaTarget.MainA)} · ${withVersion(slotText("B", st.slots[1]), OtaTarget.MainB)}`;
    (otaApply as HTMLButtonElement).hidden = !(st.caps & OTA_CAP.applyNow);
    (otaApplyAfterRow as HTMLElement).hidden = !(st.caps & OTA_CAP.applyNow);
    otaDiag.hidden = !st.diag;
    if (st.diag) {
      const d = st.diag;
      otaDiag.textContent = t("Boot: mailbox {m0} {m1}, copy {c}, escape {e}, keys down {k}", {
        m0: hex8(d.mbox0), m1: hex8(d.mbox1), c: hex8(d.copy), e: d.esc.toString(16), k: d.keysDown,
      });
    }
    const L = st.loader;
    otaLoaderRow.hidden = !L;
    otaS0.hidden = !L;
    (otaLoaderRollback as HTMLButtonElement).hidden = !L;
    otaSafe.hidden = !L?.safe;
    if (L) {
      otaLoaderState.textContent = `${withVersion(loaderSlotText(0, L.slots[0]), OtaTarget.LoaderA)} · ${withVersion(loaderSlotText(1, L.slots[1]), OtaTarget.LoaderB)}`;
      otaS0.textContent = stage0Text(st);
    }
    const unconfirmed = loaderUnconfirmed(st);
    otaConfirmRow.hidden = !unconfirmed;
    if (L && unconfirmed) {
      otaLoaderNote.textContent = t(LS.unconfirmed,
        { s: L.running ? "B" : "A", seq: L.slots[L.running]?.seq ?? "?" });
    }
    // after a loader upload, until the new loader runs: switch to it without a power cycle (apply target 4)
    const p = pendingLoader();
    (otaReselect as HTMLButtonElement).hidden = !(L && L.caps2 & OTA_CAP2.reselect && p && L.running !== p.slot
      && L.slots[p.slot]?.seq === p.seq && !(L.slots[p.slot]!.flags & (SLOT_FLAG.rejected | SLOT_FLAG.exhausted)));
  };
  const otaRefresh = async (autoConfirm = true) => {
    const cur = ctx.session();
    if (!cur) return;
    const ota = new OtaClient(cur);
    const st = await ota.status();
    if (!st) {
      otaState.textContent = t("No reply.");
      return;
    }
    slotVersion.clear();
    if (st.loader && st.loader.caps2 & OTA_CAP2.slotInfo) {
      for (const tg of [OtaTarget.MainA, OtaTarget.MainB, OtaTarget.LoaderA, OtaTarget.LoaderB]) {
        const inf = await ota.info(tg, 600);
        if (inf?.image?.version) slotVersion.set(tg, inf.image.version);
      }
    }
    showOta(st);
    // a loader this app uploaded: confirm it as soon as it runs (section 10.12 - stage-0 never confirms by time)
    const key = loadKey();
    if (autoConfirm && key && pendingLoader() && st.loader) {
      const r = await confirmLoader(cur, key);
      if (r.state !== "none") otaMsg.textContent = r.message;
      if (r.status) showOta(r.status);
    }
  };
  const keyText = h("input", { id: "ota-key-text", type: "text", placeholder: t("64 hex digits") }) as HTMLInputElement;
  const keyFile = h("input", { id: "ota-key-file", type: "file" }) as HTMLInputElement;
  const storeKey = (k: Uint8Array | null) => {
    if (!k) {
      otaMsg.textContent = t("That is not a 32-byte key.");
      return;
    }
    try {
      localStorage.setItem(OTA_KEY_STORAGE, Array.from(k, (b) => b.toString(16).padStart(2, "0")).join(""));
    } catch {
      /* storage blocked: key stays unavailable */
    }
    showKey(true);
  };
  keyFile.addEventListener("change", async () => {
    const f = keyFile.files?.[0];
    if (f) storeKey(parseKey(new Uint8Array(await f.arrayBuffer())));
  });
  const payloadFile = h("input", { id: "ota-file", type: "file", accept: ".c79" }) as HTMLInputElement;
  payloadFile.addEventListener("change", async () => {
    const f = payloadFile.files?.[0];
    otaFile = f ? new Uint8Array(await f.arrayBuffer()) : null;
    const info = otaFile ? parseOtaFile(otaFile) : null;
    otaMsg.textContent = !otaFile ? "" : !info ? t("Not a OpenGrand payload file (.c79).")
      : info.kind === "loader" ? t(LS.file, { v: info.version, n: info.length, id: hex8(info.keyId) })
      : t("{n} bytes, features {f0}/{f1}, key {id}.", { n: info.length, f0: info.feat0.toString(16), f1: info.feat1.toString(16), id: hex8(info.keyId) });
  });
  const setOtaFile = (data: Uint8Array) => {
    otaFile = data;
  };
  const applyAfter = h("input", { id: "ota-apply-after", type: "checkbox" }) as HTMLInputElement;
  const otaApplyAfterRow = h("label", { class: "inline", hidden: true }, applyAfter, " ", t("Apply right after writing"));
  const runApply = async (cur: NonNullable<ReturnType<typeof ctx.session>>, key: Uint8Array, target: number) => {
    otaMsg.textContent = t("Applying…");
    const r = await new OtaClient(cur).apply(key, target, (s) => {
      otaMsg.textContent = t("Applying… ({s})", { s: OTA_APPLY_STATE[s.applyState] ?? s.applyState });
      showOta(s);
    });
    otaMsg.textContent = r.ok ? t("Now running: {c}. Not boot-tested yet - it is tried at the next power-on.", { c: r.message })
      : t("Apply failed: {e}", { e: r.rc !== undefined && r.rc >= 22 ? rcText(r.rc) : r.message });
    return r.ok;
  };
  // switch to the loader just written without a power cycle (v4 apply target 4); the new stage-1 is confirmed
  // right after (otaRefresh), as after a power cycle and a reconnect
  const otaReselect = h("button", {
    id: "ota-reselect", hidden: true,
    onclick: async () => {
      const cur = ctx.session();
      const key = loadKey();
      if (!cur) return;
      if (!key) {
        otaMsg.textContent = t("Import the key first.");
        return;
      }
      (otaReselect as HTMLButtonElement).disabled = true;
      otaMsg.textContent = t(LS.reselecting);
      const r = await new OtaClient(cur).apply(key, ApplyTarget.Reselect, (s) => showOta(s));
      const note = r.ok ? t(LS.reselected, { s: r.status?.loader?.running ? "B" : "A" })
        : t(LS.reselectFailed, { e: r.rc !== undefined && r.rc >= 22 ? rcText(r.rc) : r.message });
      otaMsg.textContent = note;
      (otaReselect as HTMLButtonElement).disabled = false;
      await otaRefresh();                              // confirms the new loader (it was remembered at the upload)
      const after = otaMsg.textContent;
      otaMsg.textContent = after && after !== note ? `${note} ${after}` : note;
    },
  }, t(LS.reselect));
  const otaConfirm = h("button", {
    id: "ota-loader-confirm",
    onclick: async () => {
      const cur = ctx.session();
      const key = loadKey();
      if (!cur) return;
      if (!key) {
        otaMsg.textContent = t("Import the key first.");
        return;
      }
      (otaConfirm as HTMLButtonElement).disabled = true;
      const r = await confirmLoader(cur, key, true);
      otaMsg.textContent = r.message;
      (otaConfirm as HTMLButtonElement).disabled = false;
      if (r.status) showOta(r.status);
    },
  }, t(LS.confirm));
  const otaConfirmRow = h("div", { id: "ota-loader-confirm-row", hidden: true }, otaLoaderNote, h("div", { class: "inline acts" }, otaConfirm));
  const otaLoaderRow = stackRow(t(LS.loader), otaLoaderState);
  otaLoaderRow.id = "ota-loader-row";
  otaLoaderRow.hidden = true;
  const otaLoaderRollback = h("button", {
    id: "ota-loader-rollback", hidden: true,
    onclick: async () => {
      const cur = ctx.session();
      const key = loadKey();
      if (!cur || !key) {
        otaMsg.textContent = t("Import the key first.");
        return;
      }
      const ota = new OtaClient(cur);
      const st = await ota.status();
      if (!st?.loader) return;
      // the piano keeps one confirmed loader (rc 23): refuse here with the same words
      const m = loaderRollbackMask(st);
      const rc = m.ok ? await ota.reject(key, m.mask) : m.rc;
      otaMsg.textContent = rc === 0 ? t("Slot rejected. Turn the piano off and on to go back.")
        : rc !== null && rc >= 22 ? rcText(rc) : t("Rejected ({c}).", { c: String(rc) });
      await otaRefresh(false);
    },
  }, t(LS.rollback));
  const otaApply = h("button", {
    id: "ota-apply", hidden: true,
    onclick: async () => {
      const cur = ctx.session();
      const key = loadKey();
      if (!cur) return;
      if (!key) {
        otaMsg.textContent = t("Import the key first.");
        return;
      }
      (otaApply as HTMLButtonElement).disabled = true;
      await runApply(cur, key, ApplyTarget.Newest);
      (otaApply as HTMLButtonElement).disabled = false;
      await otaRefresh();
    },
  }, t("Apply now"));
  const otaUpload = h("button", {
    id: "ota-upload", class: "primary",
    onclick: async () => {
      const cur = ctx.session();
      const key = loadKey();
      if (!cur) return;
      if (!key) {
        otaMsg.textContent = t("Import the key first.");
        return;
      }
      if (!otaFile) {
        otaMsg.textContent = t("Pick a payload file first.");
        return;
      }
      (otaUpload as HTMLButtonElement).disabled = true;
      otaBar.hidden = false;
      const fin = await new OtaClient(cur).update(otaFile, key, (p) => {
        otaBar.value = Math.round((p.sent / Math.max(1, p.total)) * 100);
        otaMsg.textContent =
          p.phase === "upload" ? t("Uploading… {a}/{b} bytes", { a: p.sent, b: p.total })
          : p.phase === "commit" ? t("Writing to the slot… ({s})", { s: p.message ?? "" })
          : p.phase === "done" && p.kind === "loader" ? t(LS.written,
            { s: p.target === OtaTarget.LoaderB ? "B" : "A", seq: p.seq ?? "?" })
          : p.phase === "done" ? t("Written to {slot}. Turn the piano off and on to start it.", { slot: p.message ?? "" })
          : p.phase === "error" ? (p.rc !== undefined && p.rc >= 22) || p.refusal ? refusalText(p) : t("Update failed: {e}", { e: p.message ?? "" })
          : p.phase === "begin" && p.message ? t("Sending again from the start ({e})", { e: p.message })
          : t("Working…");
        if (p.status) showOta(p.status);
      });
      const loaderDone = fin.phase === "done" && fin.kind === "loader";
      // remembered: the next connect (main.ts) or check confirms it once it runs
      if (loaderDone) rememberLoaderUpload({ keyId: keyId(key), seq: fin.seq!, slot: fin.target! - OtaTarget.LoaderA });
      // "apply right after writing" applies the newest main: not for a loader (that is "switch loader now")
      if (fin.phase === "done" && !loaderDone && applyAfter.checked && !(otaApplyAfterRow as HTMLElement).hidden) await runApply(cur, key, ApplyTarget.Newest);
      (otaUpload as HTMLButtonElement).disabled = false;
      if (fin.phase !== "done") otaBar.hidden = true;
      const msg = otaMsg.textContent;
      await otaRefresh();
      if (loaderDone) otaMsg.textContent = msg;       // the old loader still runs: keep the instructions
    },
  }, t("Upload and commit"));
  const otaRollback = h("button", {
    id: "ota-rollback",
    onclick: async () => {
      const cur = ctx.session();
      const key = loadKey();
      if (!cur || !key) {
        otaMsg.textContent = t("Import the key first.");
        return;
      }
      const ota = new OtaClient(cur);
      const st = await ota.status();
      if (!st || st.active > 1) {
        otaMsg.textContent = st?.loader ? t(LS.noMain) : t("The built-in cave is running: nothing to roll back.");
        return;
      }
      const rc = await ota.reject(key, 1 << st.active);
      otaMsg.textContent = rc === 0 ? t("Slot rejected. Turn the piano off and on to go back.") : t("Rejected ({c}).", { c: String(rc) });
      await otaRefresh();
    },
  }, t("Roll back"));
  keyFold.append(
    h("div", { class: "fold-body" },
      h("p", { class: "muted" }, t("The piano's own key (ota_key.bin) signs every update. It is kept in this app's storage only.")),
      h("div", { class: "keyline" }, keyText, h("button", { id: "ota-key-save", onclick: () => storeKey(parseKey(keyText.value)) }, t("Save key"))),
      h("label", { class: "pick" }, h("span", { class: "lbl" }, t("Key file")), keyFile)),
  );
  otaBox.append(
    h("h3", {}, t("Firmware update")),
    h("p", { class: "muted" }, t("Installs a new OpenGrand firmware file (.c79) over Bluetooth. It starts at the next power-on.")),
    h("details", { class: "more", id: "ota-more" }, h("summary", {}, t("Details")),
      h("p", { class: "muted" }, t("Writes a signed cave payload into the free NOR slot; the piano starts it at the next power-on and falls back to the previous one if it fails. Hold A0, A#0 and B0 while switching on to start the built-in cave."))),
    h("label", { class: "row stack pick" }, h("span", { class: "lbl" }, t("Payload file (.c79)")), payloadFile),
    h("div", { class: "inline acts" }, otaUpload, otaReselect),
    otaBar,
    otaMsg,
    otaSafe,
    otaConfirmRow,
    keyFold,
  );
  // the slot tools (what runs + boot diagnostics, apply now / right after writing, roll back) are field-test tools: the
  // 「개발자용」 card below (owner 2026-11-16); shown with the update card, results in #ota-status
  const otaDev = h("div", { id: "ota-dev", hidden: true },
    h("h4", {}, t("Firmware update (slots)")),
    stackRow(t("Running"), otaState, h("button", { id: "ota-refresh", onclick: () => void otaRefresh() }, t("Check"))),
    otaLoaderRow,
    otaS0,
    h("div", { class: "inline acts" }, otaApply, otaRollback, otaLoaderRollback, otaApplyAfterRow),
    otaDiag);
  (otaBox as HTMLElement & { setOtaFile?: (d: Uint8Array) => void }).setOtaFile = setOtaFile;
  showKey(true);

  // 개발자용 (owner 2026-11-14): what the panel shows (phone / PC only),
  // probe ops, BT module diagnostics and the NOR dump - in one closed fold at the bottom, shown when any of them exists
  const panelShot = kwmcoreAvailable() ? null : h("div", { id: "ng-panel-screen" }, h("h4", {}, t("Panel screen")), panelScreenBox(() => ctx.session()));
  // owner 2026-11-16: every field-test tool lives here - the firmware slot tools, the power
  // tests (remote off, auto power-off test, GPIO; from Settings › 전원) and the panel USB tools (from Settings › 앱 · 화면)
  const powerDev = powerDevSection(ctx, () => devShown());
  // 「RAM 카나리」 (op 0x20, ngver 4): the field canary over the unreferenced SDRAM blocks, shown when Identify says ramcanary
  const canaryBox = ramCanarySection(() => ctx.session());
  const appDev = panelToolsBox(ctx.app);
  const devCard = h("section", { class: "card", id: "ng-dev", hidden: !panelShot && !appDev },
    cardFold(fold("ng-dev-fold", h("span", { class: "fold-title" }, t("Developer")), false,
      h("div", { class: "fold-body" }, panelShot, otaDev, powerDev, canaryBox, ngProbeBox, btBox, ngDumpBox, appDev))));
  function devShown(): void {
    otaDev.hidden = otaBox.hidden;
    devCard.hidden = !panelShot && !appDev && otaDev.hidden && powerDev.hidden && canaryBox.hidden && ngProbeBox.hidden && btBox.hidden
      && ngDumpBox.hidden;
  }

  // Order: what runs (versions, the custom firmware) · the updates people use (firmware, panel UI, app / module) ·
  // the developer fold last.
  return h(
    "div",
    { class: "screen info" },
    section(
      t("Information"),
      row(t("App version"), APP_VERSION, "app-version"),
      shell ? row(t("Android shell"), `${shell.version}${shell.flavor ? ` (${shell.flavor})` : ""}`, "shell-version") : null,
      row(t("Model"), id?.model ?? t("Not connected"), "model"),
      row(t("Firmware version"), id?.version ?? "–", "fw-version"),
      row(t("BLE module version"), h("span", { class: "inline" }, bleOut, h("button", { onclick: () => void checkBle(), disabled: !s }, t("Check")))),
      stackRow(t("OpenGrand"), ngOut, h("button", { id: "ng-check", onclick: () => void ngCheck(), disabled: !s }, t("Check"))),
    ),
    otaBox,
    // remote panel UI update: from a phone / PC only (the panel cannot send to itself)
    kwmcoreAvailable() ? null : section(t("Panel UI update"), panelUiBox(ctx)),
    section(
      t("App and BLE-MIDI module update"),
      // online self-update from GitHub releases (phone app, Windows desktop app); absent in a browser and on the panel
      upHub ? h("h4", {}, t("Automatic updates")) : null,
      upHub ? selfUpdateBox(upHub) : null,
      h("h4", {}, t("App update")),
      updBox,
      h("h4", {}, t("BLE-MIDI module update")),
      dfuSupported()
        ? h("div", {}, h("p", { class: "muted" }, t("Reads CA79NG/blemidi.zip from the USB memory (or the app's storage) and updates the Bluetooth MIDI module.")), dfuBtn, bar, dfuStatus)
        : h("p", { class: "muted" }, t("Module updates need the Android app.")),
    ),
    devCard,
  );
}
