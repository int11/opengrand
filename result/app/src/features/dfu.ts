// BLE-MIDI module firmware update (feature 87), web side.
// Same flow as the original app, but the steps are
// driven from here so the UI can show each one:
//   1. ask the module for its firmware version (F0 00 02 08 10 55 10 04 00 F7 -> 20 04 <ASCII>)
//   2. ask the shell for a DFU package (USB memory <usb>/CA79NG/blemidi.zip, else app storage)
//   3. send the buttonless enter-DFU SysEx (… 7F 00 01 F7) over the current MIDI link
//   4+5. the shell scans for the bootloader (DfuTarg legacy / DFUMI1P secure, with the
//      matching DFU service UUID) and runs Nordic DfuServiceInitiator
//   6. progress comes back through ca79ngNative.onDfu(json)
import { BleModule } from "../protocol/blemidi.ts";
import { appFrame, Fn } from "../protocol/sysex.ts";
import type { PianoSession } from "../protocol/session.ts";
import { bridge, onDfuStatus } from "../transport/android.ts";
import { compareVersions } from "./update.ts";

export type DfuPhase = "query" | "package" | "enter" | "scan" | "upload" | "done" | "uptodate" | "error";

export interface DfuState {
  phase: DfuPhase;
  progress?: number; // 0..100 during upload
  moduleVersion?: string | null;
  packageVersion?: string;
  message?: string;
}

export interface DfuPackage {
  path: string;
  version?: string;
  source?: string;
}

/**
 * Ask for the BLE-MIDI module's firmware version; null when nothing answers.
 * Two routes, first answer wins:
 *  - the module itself (F0 00 02 08 10 55 10 04 00 F7 -> 20 04 <ASCII>), works over BLE-MIDI;
 *  - the main board, REMOTE_PROTOCOL.md §9.7: 7E 04 41 02 -> 7E 04 01 02 <ASCII> (on the panel the
 *    module's own replies never reach the controller).
 */
export function queryModuleVersion(session: Pick<PianoSession, "onMidi" | "sendRaw"> & Partial<Pick<PianoSession, "onFrame" | "deviceId">>, timeoutMs = 2000): Promise<string | null> {
  return new Promise((resolve) => {
    let done = false;
    const offs: (() => void)[] = [];
    const finish = (v: string | null) => {
      if (done) return;
      done = true;
      offs.forEach((o) => o());
      clearTimeout(timer);
      resolve(v);
    };
    offs.push(session.onMidi((m) => {
      const v = BleModule.parseFirmwareReply(m);
      if (v !== null) finish(v.trim());
    }));
    if (session.onFrame) {
      offs.push(session.onFrame((f) => {
        if (f.fn === Fn.System && f.body[0] === 0x04 && f.body[1] === 0x01 && f.body[2] === 0x02) {
          const v = String.fromCharCode(...f.body.slice(3).filter((b) => b >= 0x20)).trim();
          if (v) finish(v);
        }
      }));
      session.sendRaw(appFrame(Fn.System, [0x04, 0x41, 0x02], session.deviceId));
    }
    const timer = setTimeout(() => finish(null), timeoutMs);
    session.sendRaw(BleModule.requestFirmware());
  });
}

export function findDfuPackage(): DfuPackage | null {
  const nb = bridge();
  if (!nb?.findDfuPackage) return null;
  try {
    const p = JSON.parse(nb.findDfuPackage()) as Partial<DfuPackage>;
    return typeof p.path === "string" && p.path ? (p as DfuPackage) : null;
  } catch {
    return null;
  }
}

export function dfuSupported(): boolean {
  const nb = bridge();
  return !!(nb?.startDfu && nb.findDfuPackage);
}

export interface DfuOptions {
  force?: boolean; // update even when the versions match
  queryTimeoutMs?: number;
}

/** One module update at a time, app-wide (the shell runs one DFU): the Information screen is rebuilt on every tab
 *  switch / reconnect with a fresh, enabled button, so the button alone does not keep a second run out. */
let dfuRunning = false;

/** A module update is running (started by any screen). */
export function dfuBusy(): boolean {
  return dfuRunning;
}

/** Run the whole update. Resolves with the final state (done / uptodate / error); "busy" (nothing sent) while
 *  another run is going. */
export async function runDfu(session: Pick<PianoSession, "onMidi" | "sendRaw"> & Partial<Pick<PianoSession, "onFrame" | "deviceId">>, onState: (s: DfuState) => void, opt: DfuOptions = {}): Promise<DfuState> {
  if (dfuRunning) {
    const s: DfuState = { phase: "error", message: "busy" };
    onState(s);
    return s;
  }
  dfuRunning = true;
  try {
    return await runDfuNow(session, onState, opt);
  } finally {
    dfuRunning = false;
  }
}

async function runDfuNow(session: Pick<PianoSession, "onMidi" | "sendRaw"> & Partial<Pick<PianoSession, "onFrame" | "deviceId">>, onState: (s: DfuState) => void, opt: DfuOptions): Promise<DfuState> {
  const report = (s: DfuState) => {
    onState(s);
    return s;
  };
  const nb = bridge();
  if (!nb?.startDfu || !nb.findDfuPackage) return report({ phase: "error", message: "unsupported" });

  report({ phase: "query" });
  const moduleVersion = await queryModuleVersion(session, opt.queryTimeoutMs);

  report({ phase: "package", moduleVersion });
  const pkg = findDfuPackage();
  if (!pkg) return report({ phase: "error", moduleVersion, message: "no package" });
  if (!opt.force && moduleVersion && pkg.version && compareVersions(pkg.version, moduleVersion) <= 0) {
    return report({ phase: "uptodate", moduleVersion, packageVersion: pkg.version });
  }

  report({ phase: "enter", moduleVersion, packageVersion: pkg.version });
  session.sendRaw(BleModule.enterDfu());

  return new Promise<DfuState>((resolve) => {
    const off = onDfuStatus((st) => {
      const base = { moduleVersion, packageVersion: pkg.version };
      if (typeof st.error === "string" || (typeof st.progress === "number" && st.progress < 0)) {
        off();
        resolve(report({ ...base, phase: "error", message: String(st.error ?? "failed") }));
      } else if (st.complete === 1 || st.state === "done") {
        off();
        resolve(report({ ...base, phase: "done", progress: 100 }));
      } else if (st.state === "scan") {
        report({ ...base, phase: "scan" });
      } else if (typeof st.progress === "number") {
        report({ ...base, phase: "upload", progress: st.progress });
      }
    });
    if (!nb.startDfu!(pkg.path)) {
      off();
      resolve(report({ phase: "error", moduleVersion, packageVersion: pkg.version, message: "start failed" }));
    }
  });
}
