// Online self-update, pure parts (no Electron import; tests/updater.test.cjs runs them under plain Node). The
// release tool (scripts/release_build.mjs) uses the same canonical form and checks, so the format has one definition.
//
// Source: the latest GitHub release of int11/opengrand (https://api.github.com/repos/int11/opengrand/releases/latest).
// Every release carries an asset "update.json":
//   { "schema": 1,
//     "android": { "version": "1.2.0", "versionCode": 14, "file": "OpenGrand-1.2.0-phone.apk", "sha256": "<hex>", "size": n },
//     "desktop": { "version": "0.3.0", "file": "OpenGrand-0.3.0-win-x64.zip", "sha256": "<hex>", "size": n } }
// No signature: trust is HTTPS to api.github.com / github.com and this repository's release. Other keys (e.g. a "sig"
// that older releases carry) are ignored. The downloaded asset must have exactly `size` bytes and the `sha256`.
"use strict";

const path = require("node:path");

const REPO = "int11/opengrand";
const LATEST_URL = `https://api.github.com/repos/${REPO}/releases/latest`;
const RELEASES_PAGE = `https://github.com/${REPO}/releases/latest`;
const MANIFEST_NAME = "update.json";
const MAX_MANIFEST = 64 * 1024;
const MAX_ASSET = 1024 * 1024 * 1024;

const RE_VERSION = /^\d+(\.\d+){0,3}$/;
const RE_FILE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/;
const RE_SHA = /^[0-9a-f]{64}$/;

function compareVersions(a, b) {
  const pa = String(a).split(".").map((x) => parseInt(x, 10) || 0);
  const pb = String(b).split(".").map((x) => parseInt(x, 10) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] || 0) - (pb[i] || 0);
    if (d) return d < 0 ? -1 : 1;
  }
  return 0;
}

function checkEntry(e, what, android) {
  if (!e || typeof e !== "object" || Array.isArray(e)) throw new Error(`${what}: not an object`);
  if (typeof e.version !== "string" || !RE_VERSION.test(e.version)) throw new Error(`${what}.version`);
  if (typeof e.file !== "string" || !RE_FILE.test(e.file) || e.file.includes("..") || e.file === MANIFEST_NAME) throw new Error(`${what}.file`);
  if (typeof e.sha256 !== "string" || !RE_SHA.test(e.sha256)) throw new Error(`${what}.sha256`);
  if (!Number.isSafeInteger(e.size) || e.size <= 0 || e.size > MAX_ASSET) throw new Error(`${what}.size`);
  if (android && (!Number.isSafeInteger(e.versionCode) || e.versionCode <= 0)) throw new Error(`${what}.versionCode`);
}

/** Parse and check update.json text. Returns { schema, android?, desktop? } or throws with err.code "manifest". */
function verifyManifest(text) {
  const fail = (msg) => Object.assign(new Error(msg), { code: "manifest" });
  if (typeof text !== "string" || text.length > MAX_MANIFEST) throw fail("update.json too large");
  let m;
  try {
    m = JSON.parse(text);
  } catch {
    throw fail("update.json is not JSON");
  }
  if (!m || typeof m !== "object" || Array.isArray(m)) throw fail("update.json is not an object");
  try {
    if (m.schema !== 1) throw new Error("schema");
    if (m.android !== undefined) checkEntry(m.android, "android", true);
    if (m.desktop !== undefined) checkEntry(m.desktop, "desktop", false);
  } catch (e) {
    throw fail(`update.json: bad ${e.message}`);
  }
  return { schema: 1, android: m.android, desktop: m.desktop };
}

/**
 * What a failed request means for the user. Offline, rate limit and "no release yet" are quiet conditions, not errors.
 *   status: HTTP status, or undefined when the request itself failed (no network, DNS, timeout)
 */
function httpProblem(status) {
  if (status === undefined) return "offline";
  if (status === 403 || status === 429) return "ratelimit";
  if (status === 404) return "norelease";
  return status >= 200 && status < 300 ? null : "http";
}

/** From the GitHub "latest release" JSON: the release page, and the download URL of each asset by name. */
function releaseAssets(rel) {
  if (!rel || typeof rel !== "object" || !Array.isArray(rel.assets)) throw Object.assign(new Error("release: no assets"), { code: "manifest" });
  const assets = new Map();
  for (const a of rel.assets) {
    if (a && typeof a.name === "string" && typeof a.browser_download_url === "string" && /^https:\/\//.test(a.browser_download_url)) {
      assets.set(a.name, { url: a.browser_download_url, size: a.size });
    }
  }
  return { tag: typeof rel.tag_name === "string" ? rel.tag_name : "", page: typeof rel.html_url === "string" && /^https:\/\/github\.com\//.test(rel.html_url) ? rel.html_url : RELEASES_PAGE, assets };
}

/**
 * How this copy of the app can update itself:
 *   { kind: "dir", installDir, exe, newDir, oldDir, args }  unpacked build in a writable folder: stage next to it, swap
 *   { kind: "manual", reason }                               tell the user (portable exe, dev run, not Windows)
 * input: { platform, packaged, execPath, argv (process.argv), portableFile (env PORTABLE_EXECUTABLE_FILE) }
 */
function updatePlan({ platform, packaged, execPath, argv, portableFile }) {
  if (platform !== "win32") return { kind: "manual", reason: "platform" };
  if (!packaged) return { kind: "manual", reason: "dev" };
  if (portableFile) return { kind: "manual", reason: "portable" };
  const p = path.win32;
  const exe = p.resolve(execPath);
  const installDir = p.dirname(exe);
  const parent = p.dirname(installDir);
  const name = p.basename(installDir);
  if (!name || parent === installDir) return { kind: "manual", reason: "root" };
  return {
    kind: "dir",
    installDir,
    exe,
    newDir: p.join(parent, `${name}.new`),
    oldDir: p.join(parent, `${name}.old`),
    // the same launch options again (--cdp, --automation, --query …); argv[0] is the exe of a packaged app
    args: (argv || []).slice(1).filter((a) => typeof a === "string"),
  };
}

/** Environment for the swap helper: everything it needs, nothing quoted into the script. */
function helperEnv(plan, { pid, log, result, relaunch }) {
  const env = {
    OG_PID: String(pid),
    OG_INSTALL: plan.installDir,
    OG_NEW: plan.newDir,
    OG_OLD: plan.oldDir,
    OG_EXE: plan.exe,
    OG_LOG: log,
    OG_RESULT: result,
    OG_RELAUNCH: relaunch ? "1" : "0",
    OG_ARGC: String(plan.args.length),
  };
  plan.args.forEach((a, i) => (env[`OG_ARG${i}`] = a));
  return env;
}

/**
 * The swap helper (Windows PowerShell 5.1, run detached after the app quits). Waits for the app's process to end,
 * renames <install> -> <install>.old (an older .old is removed first) and <install>.new -> <install>, retrying while
 * files are still locked (Electron's helper processes outlive the main one by a moment); on failure it renames the
 * old folder back. Then it starts the exe again with the same arguments (OG_RELAUNCH=1) and writes the outcome to
 * OG_RESULT, which the app reads on its next start.
 */
const HELPER_PS1 = String.raw`$ErrorActionPreference = 'Stop'
function Log([string]$m) { try { Add-Content -LiteralPath $env:OG_LOG -Value ((Get-Date -Format o) + ' ' + $m) -Encoding UTF8 } catch {} }
function Retry([scriptblock]$b) {
  for ($i = 0; $i -lt 120; $i++) { try { & $b; return $true } catch { $script:lastErr = $_.Exception.Message; Start-Sleep -Milliseconds 500 } }
  return $false
}
$inst = $env:OG_INSTALL; $new = $env:OG_NEW; $old = $env:OG_OLD; $exe = $env:OG_EXE
Log ('helper start, waiting for pid ' + $env:OG_PID)
try { Wait-Process -Id ([int]$env:OG_PID) -Timeout 120 -ErrorAction SilentlyContinue } catch {}
$state = 'failed'
if (-not (Test-Path -LiteralPath (Join-Path $new (Split-Path $exe -Leaf)))) {
  $script:lastErr = 'staged folder missing'
} else {
  if (Test-Path -LiteralPath $old) { [void](Retry { Remove-Item -LiteralPath $old -Recurse -Force }) }
  if (Test-Path -LiteralPath $old) {
    $script:lastErr = 'cannot remove ' + $old
  } elseif (Retry { Rename-Item -LiteralPath $inst -NewName (Split-Path $old -Leaf) }) {
    if (Retry { Rename-Item -LiteralPath $new -NewName (Split-Path $inst -Leaf) }) { $state = 'ok' }
    else { [void](Retry { Rename-Item -LiteralPath $old -NewName (Split-Path $inst -Leaf) }) }
  }
}
Log ('swap ' + $state + $(if ($state -ne 'ok') { ': ' + $script:lastErr } else { '' }))
try { Set-Content -LiteralPath $env:OG_RESULT -Value (@{ state = $state; error = [string]$script:lastErr } | ConvertTo-Json -Compress) -Encoding UTF8 } catch {}
if ($env:OG_RELAUNCH -eq '1') {
  $list = @()
  for ($i = 0; $i -lt [int]$env:OG_ARGC; $i++) { $a = [string](Get-Item -LiteralPath ('env:OG_ARG' + $i)).Value; $list += ('"' + ($a -replace '"', '\"') + '"') }
  try {
    if ($list.Count -gt 0) { Start-Process -FilePath $exe -ArgumentList $list -WorkingDirectory (Split-Path $exe) }
    else { Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) }
    Log 'relaunched'
  } catch { Log ('relaunch failed: ' + $_.Exception.Message) }
}
`;

/**
 * Zip entry names a staged update may contain: relative, no drive / root / "..", no backslash tricks.
 * Returns the safe relative path (forward slashes) or null.
 */
function safeEntryName(name) {
  if (typeof name !== "string" || !name || name.includes("\0") || name.includes("\\")) return null;
  if (name.startsWith("/") || /^[A-Za-z]:/.test(name)) return null;
  const parts = name.split("/");
  if (parts.some((p) => p === ".." || p === ".")) return null;
  return name;
}

module.exports = {
  REPO,
  LATEST_URL,
  RELEASES_PAGE,
  MANIFEST_NAME,
  MAX_MANIFEST,
  compareVersions,
  verifyManifest,
  httpProblem,
  releaseAssets,
  updatePlan,
  helperEnv,
  HELPER_PS1,
  safeEntryName,
};
