// Panel UI / APK update (feature 86), web side.
// The Android shell looks for <usb>/CA79NG/manifest.json + app.apk, checks the
// SHA-256 and installs (panel: PackageInstaller session as a system app; phone:
// ACTION_VIEW on a FileProvider URI). The original app used its own package
// format; here the package is plain and integrity comes from the manifest hash.
import { bridge, onUpdateStatus, shellInfo } from "../transport/android.ts";

export interface UpdateManifest {
  version: string;
  versionCode?: number;
  sha256: string;
  file?: string; // default "app.apk"
}

export interface UpdateCheck {
  found: boolean;
  version?: string; // version offered on the USB memory
  current?: string; // installed shell version
  newer?: boolean;
  error?: string;
}

/** Compare dotted numeric versions ("1.10.0" > "1.9.3"). Non-numeric parts count as 0. */
export function compareVersions(a: string, b: string): number {
  const pa = a.split(/[.\-+]/).map((x) => parseInt(x, 10) || 0);
  const pb = b.split(/[.\-+]/).map((x) => parseInt(x, 10) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] ?? 0) - (pb[i] ?? 0);
    if (d !== 0) return d < 0 ? -1 : 1;
  }
  return 0;
}

/** Validate a manifest (same rules as the Kotlin UpdateManager). */
export function parseManifest(json: string): UpdateManifest | null {
  try {
    const m = JSON.parse(json) as UpdateManifest;
    if (typeof m.version !== "string" || !/^\d+(\.\d+)*$/.test(m.version)) return null;
    if (typeof m.sha256 !== "string" || !/^[0-9a-fA-F]{64}$/.test(m.sha256)) return null;
    if (m.file !== undefined && (typeof m.file !== "string" || /[\\/]|\.\./.test(m.file))) return null;
    return m;
  } catch {
    return null;
  }
}

export function checkApkUpdate(): UpdateCheck {
  const nb = bridge();
  if (!nb?.checkUpdate) return { found: false, error: "unsupported" };
  try {
    const r = JSON.parse(nb.checkUpdate()) as UpdateCheck;
    const cur = r.current ?? shellInfo()?.version;
    if (r.found && r.version && cur && r.newer === undefined) r.newer = compareVersions(r.version, cur) > 0;
    return { ...r, current: cur };
  } catch {
    return { found: false, error: "bad reply" };
  }
}

/** Start the install; `onStatus` receives {state:"verifying"|"installing"|"done"|"error", message?}. */
export function installApkUpdate(onStatus: (s: Record<string, unknown>) => void): boolean {
  const nb = bridge();
  if (!nb?.installUpdate) return false;
  const off = onUpdateStatus((s) => {
    onStatus(s);
    if (s.state === "done" || s.state === "error") off();
  });
  const ok = nb.installUpdate();
  if (!ok) off();
  return ok;
}
