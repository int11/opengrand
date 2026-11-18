// Pure decisions of the desktop shell: launch options, what the app origin may use, which file a
// request maps to, and which Bluetooth device automation may pick. No Electron import, so
// tests/policy.test.cjs runs them under plain Node.
"use strict";

const path = require("node:path");

const SCHEME = "app";
const HOST = "ca79ng";
const ORIGIN = `${SCHEME}://${HOST}`;

// Permissions the app origin gets without asking. Web MIDI (+ SysEx: the whole protocol is SysEx),
// WebUSB (USB-MIDI devices that are not class compliant, future DFU), clipboard write (log copy),
// fullscreen. Everything else (camera, location, notifications …) is denied.
const ALLOWED_PERMISSIONS = new Set(["midi", "midiSysex", "usb", "clipboard-sanitized-write", "fullscreen"]);
// device grants (setDevicePermissionHandler): WebUSB only; HID / serial are not used by the app
const ALLOWED_DEVICE_TYPES = new Set(["usb"]);

const DEFAULT_BT_PATTERN = "CA79|KAWAI"; // device names the automation mode picks by default

/**
 * Field 2026-11-03: with the window minimized (document.hidden) an OTA upload took ~7 min instead of ~83 s —
 * Chromium throttles timers of a hidden page (≥ 1 s alignment, then once a minute), and the BLE write pacing and the
 * OTA stop-and-wait sleep on timers. The app is a device controller that keeps its piano link while minimized
 * (user decision 2026-11-02), so nothing of it may be throttled: the window's `backgroundThrottling: false`
 * (timers and rendering of a hidden page) plus these switches (a minimized / occluded window's renderer is not
 * backgrounded — lower process priority on Windows).
 */
const NO_THROTTLE_SWITCHES = ["disable-background-timer-throttling", "disable-renderer-backgrounding", "disable-backgrounding-occluded-windows"];

/** webPreferences of the app window: sandboxed, isolated, never throttled while hidden. */
function appWebPreferences(preload) {
  return { preload, contextIsolation: true, sandbox: true, nodeIntegration: false, backgroundThrottling: false };
}

function isAppOrigin(origin) {
  if (!origin) return false;
  try {
    // WHATWG URL gives "null" as origin for a non-special scheme, so compare scheme + host
    const u = new URL(origin);
    return u.protocol === `${SCHEME}:` && u.host === HOST && !u.username && !u.password;
  } catch {
    return false;
  }
}

function permissionAllowed(permission, origin) {
  return isAppOrigin(origin) && ALLOWED_PERMISSIONS.has(permission);
}

function devicePermissionAllowed(deviceType, origin) {
  return isAppOrigin(origin) && ALLOWED_DEVICE_TYPES.has(deviceType);
}

const truthy = (v) => v !== undefined && v !== "" && !/^(0|false|no|off)$/i.test(String(v));

/**
 * Launch options from argv + env. Remote debugging and automation are OFF unless asked for:
 *   --cdp[=PORT] or CA79_CDP_PORT=PORT          → --remote-debugging-port (127.0.0.1), default 9222
 *   --automation or CA79_AUTOMATION=1           → Bluetooth chooser auto-picks a name matching
 *                                                 PW_BT_DEVICE (regex, default CA79|KAWAI)
 *   --query=… or CA79_QUERY=…                   → query string for index.html (e.g. log&tab=info)
 *   CA79_APP_DIST=DIR                           → serve this folder instead of the built-in one
 *   CA79_USER_DATA=DIR                          → separate profile (tests; a second instance)
 */
function parseOptions(argv, env) {
  const opt = { cdpPort: null, automation: false, btPattern: DEFAULT_BT_PATTERN, query: "", appDist: null, userData: null };
  if (env.CA79_CDP_PORT) opt.cdpPort = String(env.CA79_CDP_PORT);
  if (truthy(env.CA79_AUTOMATION)) opt.automation = true;
  if (env.PW_BT_DEVICE) opt.btPattern = env.PW_BT_DEVICE;
  if (env.CA79_QUERY) opt.query = env.CA79_QUERY;
  if (env.CA79_APP_DIST) opt.appDist = env.CA79_APP_DIST;
  if (env.CA79_USER_DATA) opt.userData = env.CA79_USER_DATA;
  for (const a of argv) {
    if (a === "--cdp") opt.cdpPort = "9222";
    else if (a.startsWith("--cdp=")) opt.cdpPort = a.slice(6);
    else if (a === "--automation") opt.automation = true;
    else if (a.startsWith("--query=")) opt.query = a.slice(8);
  }
  if (opt.cdpPort !== null && !/^\d{2,5}$/.test(opt.cdpPort)) throw new Error(`bad CDP port: ${opt.cdpPort}`);
  opt.query = opt.query.replace(/^\?/, "");
  return opt;
}

/** The file under `root` that an app:// request URL names, or null (other host, escapes root). */
function resolveAppFile(root, url) {
  let u;
  try {
    u = new URL(url);
  } catch {
    return null;
  }
  if (u.protocol !== `${SCHEME}:` || u.host !== HOST) return null;
  let rel;
  try {
    rel = decodeURIComponent(u.pathname);
  } catch {
    return null;
  }
  if (rel === "/" || rel === "") rel = "/index.html";
  if (rel.includes("\0")) return null;
  const base = path.resolve(root);
  const file = path.resolve(base, "." + rel);
  if (file !== base && !file.startsWith(base + path.sep)) return null;
  return file;
}

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".mjs": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json",
  ".map": "application/json",
  ".webmanifest": "application/manifest+json",
  ".png": "image/png",
  ".svg": "image/svg+xml",
  ".ico": "image/x-icon",
  ".wasm": "application/wasm",
  ".bin": "application/octet-stream",
};

function mimeType(file) {
  return MIME[path.extname(file).toLowerCase()] || "application/octet-stream";
}

/**
 * Automation pick: the first device whose name matches the pattern, else null (keep waiting).
 * Devices without a name never match, so an unknown device is never picked.
 */
function autoPick(devices, pattern) {
  const re = new RegExp(pattern || DEFAULT_BT_PATTERN, "i");
  const d = devices.find((x) => x && x.deviceName && re.test(x.deviceName));
  return d ? d.deviceId : null;
}

/**
 * The Bluetooth device the user last chose (main process, userData/last-bluetooth.json), parsed back, or
 * null when the file is missing / damaged. Electron's deviceId (the adapter address on Windows) is stable
 * across restarts, unlike the page's BluetoothDevice.id.
 */
function parseLastBluetooth(text) {
  try {
    const v = JSON.parse(text);
    if (!v || typeof v.deviceId !== "string" || !v.deviceId) return null;
    return { deviceId: v.deviceId, deviceName: typeof v.deviceName === "string" ? v.deviceName : "" };
  } catch {
    return null;
  }
}

/** What to store after a chooser answer: the chosen device (name from the latest list), or null for a cancel. */
function lastBluetoothOf(id, devices) {
  if (!id) return null;
  const d = (devices || []).find((x) => x && x.deviceId === id);
  return { deviceId: id, deviceName: (d && d.deviceName) || "" };
}

/**
 * Reconnect pick (an app-initiated reconnect only): exactly the remembered device, matched by deviceId, else
 * null (keep waiting). A device with the same name but another id is never picked.
 */
function reconnectPick(devices, remembered) {
  if (!remembered || !remembered.deviceId) return null;
  const d = (devices || []).find((x) => x && x.deviceId === remembered.deviceId);
  return d ? d.deviceId : null;
}

/** Picker order: names that look like the piano first, then named devices, then unnamed. */
function sortDevices(devices, pattern = DEFAULT_BT_PATTERN) {
  const re = new RegExp(pattern, "i");
  const rank = (d) => (d.deviceName && re.test(d.deviceName) ? 0 : d.deviceName ? 1 : 2);
  return [...devices].sort((a, b) => rank(a) - rank(b) || String(a.deviceName).localeCompare(String(b.deviceName)));
}

module.exports = {
  SCHEME,
  HOST,
  ORIGIN,
  DEFAULT_BT_PATTERN,
  NO_THROTTLE_SWITCHES,
  appWebPreferences,
  isAppOrigin,
  permissionAllowed,
  devicePermissionAllowed,
  parseOptions,
  resolveAppFile,
  mimeType,
  autoPick,
  sortDevices,
  parseLastBluetooth,
  lastBluetoothOf,
  reconnectPick,
};
