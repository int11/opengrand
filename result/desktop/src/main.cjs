// OpenGrand desktop shell (Electron main process).
// Serves the built web app (result/app/dist) from the privileged app://ca79ng origin and keeps Web
// Bluetooth, Web MIDI (SysEx) and WebUSB working: device choosers and permission handlers live here. Bluetooth
// devices are listed in the app's own connection dialog (in-app list below); the USB chooser is a small window.
// Pure decisions: ./policy.cjs.
"use strict";

const { app, BrowserWindow, dialog, ipcMain, net, protocol, session, shell } = require("electron");
const { spawn } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");
const policy = require("./policy.cjs");
const updatePolicy = require("./updatepolicy.cjs");
const { createUpdater } = require("./selfupdate.cjs");

const log = (...a) => console.log("[ca79-desktop]", ...a);

const opt = policy.parseOptions(process.argv.slice(1), process.env);
if (opt.userData) app.setPath("userData", path.resolve(opt.userData));
else {
  // builds before the rename to OpenGrand kept their profile (settings in localStorage, last Bluetooth device,
  // window bounds) under the old product name: keep using it where it exists so nothing is lost
  const legacy = path.join(app.getPath("appData"), "CA79-NG");
  if (fs.existsSync(legacy) && !fs.existsSync(app.getPath("userData"))) app.setPath("userData", legacy);
}
// Remote debugging only when asked for (--cdp / CA79_CDP_PORT; port checked in parseOptions), for test tools that
// connect over CDP. Electron binds it to 127.0.0.1 only (observed: netstat 127.0.0.1:9222 LISTENING).
if (opt.cdpPort) {
  app.commandLine.appendSwitch("remote-debugging-port", opt.cdpPort);
  log(`remote debugging on http://127.0.0.1:${opt.cdpPort} (CDP)`);
}
// Web Bluetooth is on by default on Windows / macOS; Chromium keeps it behind a flag on Linux (dev runs, smoke test)
if (process.platform === "linux") app.commandLine.appendSwitch("enable-blink-features", "WebBluetooth");
if (opt.automation) log(`automation: Bluetooth chooser auto-picks /${opt.btPattern}/i`);
// minimized / hidden: no timer throttling, no renderer backgrounding (OTA and BLE pacing, auto-reconnect) - policy.cjs
for (const sw of policy.NO_THROTTLE_SWITCHES) app.commandLine.appendSwitch(sw);

// app:// = standard + secure origin: a secure context (Web Bluetooth / MIDI / USB need one), ES modules
// and fetch with a real origin (file:// gives an opaque one), relative URLs resolve like on the web server,
// localStorage / permissions keyed to one stable origin. Service workers are allowed by the scheme; the
// app itself does not register one here (src/pwa.ts swAllowed: http(s) only).
protocol.registerSchemesAsPrivileged([
  {
    scheme: policy.SCHEME,
    privileges: { standard: true, secure: true, supportFetchAPI: true, allowServiceWorkers: true, corsEnabled: true, stream: true, codeCache: true },
  },
]);

function appRoot() {
  if (opt.appDist) return path.resolve(opt.appDist);
  if (app.isPackaged) return path.join(process.resourcesPath, "app");
  return path.resolve(__dirname, "..", "..", "app", "dist");
}

function serveApp(root) {
  protocol.handle(policy.SCHEME, async (req) => {
    const file = policy.resolveAppFile(root, req.url);
    if (!file) return new Response("not found", { status: 404 });
    try {
      const body = await fs.promises.readFile(file);
      return new Response(body, { headers: { "content-type": policy.mimeType(file), "cache-control": "no-cache" } });
    } catch {
      return new Response("not found", { status: 404 });
    }
  });
}

function guardPermissions(ses) {
  ses.setPermissionRequestHandler((wc, permission, callback, details) => {
    const origin = details?.requestingUrl || wc?.getURL();
    const ok = policy.permissionAllowed(permission, origin);
    if (!ok) log(`permission denied: ${permission} (${origin})`);
    callback(ok);
  });
  ses.setPermissionCheckHandler((_wc, permission, origin) => policy.permissionAllowed(permission, origin));
  ses.setDevicePermissionHandler((d) => policy.devicePermissionAllowed(d.deviceType, d.origin));
  // Windows / Linux: pairing prompts during Web Bluetooth connect (BLE-MIDI is normally "just works")
  if (typeof ses.setBluetoothPairingHandler === "function") {
    ses.setBluetoothPairingHandler((details, callback) => {
      log(`bluetooth pairing request: ${details.pairingKind} ${details.deviceId}`);
      if (details.pairingKind === "providePin") {
        callback({ confirmed: false }); // no PIN entry UI: the piano's module never asks for one
        return;
      }
      if (opt.automation && details.pairingKind === "confirm") {
        callback({ confirmed: true });
        return;
      }
      const win = BrowserWindow.getFocusedWindow() || mainWindow;
      const detail = details.pairingKind === "confirmPin" ? `PIN ${details.pin}` : details.deviceId;
      dialog
        .showMessageBox(win, { type: "question", buttons: ["페어링 · Pair", "취소 · Cancel"], defaultId: 0, cancelId: 1, message: "블루투스 장치와 페어링할까요? · Pair with this Bluetooth device?", detail })
        .then((r) => callback({ confirmed: r.response === 0 }))
        .catch(() => callback({ confirmed: false }));
    });
  }
}

// ---- device chooser window (WebUSB; Web Bluetooth only for an app build without the in-app list, see below) ----
// One request at a time per kind. The renderer's requestDevice() waits until finish() calls Electron's callback.
class Chooser {
  constructor(parent, kind, pattern) {
    this.kind = kind;
    this.pattern = pattern;
    this.done = false;
    this.onFinish = null;
    this.win = new BrowserWindow({
      parent,
      modal: true,
      width: 420,
      height: 460,
      resizable: true,
      minimizable: false,
      maximizable: false,
      autoHideMenuBar: true,
      title: kind === "bluetooth" ? "Bluetooth MIDI" : "USB",
      show: false,
      webPreferences: { preload: path.join(__dirname, "picker-preload.cjs"), contextIsolation: true, sandbox: true },
    });
    this.devices = [];
    this.ready = false;
    this.win.webContents.once("did-finish-load", () => {
      this.ready = true;
      this.win.webContents.send("picker:init", { kind });
      this.push();
      this.win.show();
    });
    this.win.on("closed", () => this.finish(""));
    this.win.loadFile(path.join(__dirname, "picker.html"));
  }
  update(devices) {
    this.devices = policy.sortDevices(devices, this.pattern);
    this.push();
  }
  push() {
    if (this.ready && !this.win.isDestroyed()) this.win.webContents.send("picker:devices", this.devices);
  }
  owns(sender) {
    return !this.win.isDestroyed() && sender === this.win.webContents;
  }
  finish(id) {
    if (this.done) return;
    this.done = true;
    if (!this.win.isDestroyed()) this.win.close();
    this.onFinish?.(id);
  }
}

const choosers = new Set();
ipcMain.on("picker:choose", (e, id) => {
  for (const c of choosers) if (c.owns(e.sender)) c.finish(typeof id === "string" ? id : "");
});

const AUTO_PICK_MS = 30000; // automation: give up (NotFoundError in the page) when no matching device shows
const RECONNECT_MS = 10000; // app-initiated reconnect: give up when the remembered piano does not show (held by another central, off)
const RECONNECT_ARM_MS = 5000; // the armed reconnect must reach select-bluetooth-device this soon, or it lapses

// ---- last chosen Bluetooth device (Electron has no getDevices(): the app cannot reopen a device after a restart) ----
// Stored in userData/last-bluetooth.json after every chooser answer; used only when the app asks to reconnect.
const lastBtFile = () => path.join(app.getPath("userData"), "last-bluetooth.json");
let lastBt; // undefined = not read yet
function lastBluetooth() {
  if (lastBt === undefined) {
    try {
      lastBt = policy.parseLastBluetooth(fs.readFileSync(lastBtFile(), "utf8"));
    } catch {
      lastBt = null;
    }
  }
  return lastBt;
}
function rememberBluetooth(dev) {
  if (!dev) return;
  lastBt = dev;
  try {
    fs.writeFileSync(lastBtFile(), JSON.stringify(dev));
  } catch (e) {
    log(`bluetooth: could not store the last device: ${e.message}`);
  }
}
let reconnectArm = null; // { deviceId, deviceName, until }: the next chooser request is the app's reconnect

ipcMain.on("bt:remembered", (e) => {
  const d = mainWindow && e.sender === mainWindow.webContents ? lastBluetooth() : null;
  e.returnValue = d ? d.deviceName || d.deviceId : null;
});
// The app's reconnect on start (result/app/src/transport/desktop.ts). requestDevice needs a user gesture, so the
// page's hook runs from here with one; the chooser request it makes is answered with the remembered device only.
ipcMain.handle("bt:reconnect", async (e) => {
  if (!mainWindow || e.sender !== mainWindow.webContents || !policy.isAppOrigin(e.sender.getURL())) return "denied";
  // the app's connection dialog is searching (its requestDevice() is the running one): a second one would replace it
  if (inApp.listening) return "busy";
  const dev = lastBluetooth();
  if (!dev) return "none";
  reconnectArm = { ...dev, until: Date.now() + RECONNECT_ARM_MS };
  const hook = "globalThis.__ca79ngDesktopReconnect";
  let r;
  try {
    r = await e.sender.executeJavaScript(`typeof ${hook} === "function" ? (${hook}(), "started") : "no-hook"`, true);
  } catch (err) {
    r = `error: ${err.message}`;
  }
  if (r !== "started") reconnectArm = null;
  log(`bluetooth: reconnect to ${dev.deviceName || dev.deviceId} ${r}`);
  return r;
});

// ---- Bluetooth device list inside the app's own connection dialog (no chooser window) ----
// The page (result/app/src/ui/picker.ts through preload.cjs onDevices / choose) listens while its dialog searches;
// a request that starts then is answered from there. cancelUntil: the page gave a request up before Electron
// reported any device for it (Electron raises select-bluetooth-device only once a device shows, so there was nothing
// to answer yet) - the next request that shows up is that one and gets "" (policy.bluetoothRoute "cancel").
const CANCEL_NEXT_MS = 5 * 60 * 1000;
const inApp = { listening: false, cancelUntil: 0 };
let btPending = null; // { callback, devices, chooser?, timer?, reconnect?, inApp? }: the one running requestDevice()
const fromMainWindow = (e) => !!mainWindow && e.sender === mainWindow.webContents;
const fromApp = (e) => fromMainWindow(e) && policy.isAppOrigin(e.senderFrame ? e.senderFrame.url : e.sender.getURL());

function finishBluetooth(id) {
  if (!btPending) return;
  const p = btPending;
  btPending = null;
  clearTimeout(p.timer);
  if (p.chooser) {
    choosers.delete(p.chooser);
    p.chooser.onFinish = null;
    p.chooser.finish(id);
  }
  log(id ? `bluetooth: selected ${id}${p.reconnect ? " (reconnect)" : ""}` : `bluetooth: ${p.reconnect ? "reconnect gave up" : "chooser cancelled"}`);
  rememberBluetooth(policy.lastBluetoothOf(id, p.devices));
  p.callback(id);
}

ipcMain.on("bt:listen", (e) => {
  if (!fromApp(e)) {
    e.returnValue = false;
    return;
  }
  inApp.listening = true;
  inApp.cancelUntil = 0; // a new search of the page replaces the request it gave up
  e.returnValue = true;
});
ipcMain.on("bt:unlisten", (e) => {
  if (!fromMainWindow(e)) return;
  inApp.listening = false;
  if (btPending?.inApp) finishBluetooth(""); // nobody is left to choose (dialog gone, page reloaded)
});
ipcMain.on("bt:choose", (e, id) => {
  if (!fromApp(e)) return;
  const p = btPending;
  if (!p) {
    if (id === "" && inApp.listening) inApp.cancelUntil = Date.now() + CANCEL_NEXT_MS;
    return;
  }
  if (p.chooser || p.reconnect) return; // the chooser window's / the reconnect's request: not the page's to answer
  // automation's request (no list was sent): the page may only cancel it
  const pick = policy.inAppChoice(id, p.inApp ? p.devices : []);
  if (pick === null) log("bluetooth: in-app choice ignored (not in the list)");
  else finishBluetooth(pick);
});

function handleBluetooth(win) {
  win.webContents.on("select-bluetooth-device", (event, devices, callback) => {
    event.preventDefault();
    if (!btPending) {
      const arm = reconnectArm && Date.now() < reconnectArm.until ? reconnectArm : null;
      reconnectArm = null;
      const route = policy.bluetoothRoute({ armed: !!arm, automation: opt.automation, cancelNext: Date.now() < inApp.cancelUntil, listening: inApp.listening });
      inApp.cancelUntil = 0;
      if (route === "cancel") {
        log("bluetooth: request given up by the app before a device showed - cancelled");
        callback("");
        return;
      }
      btPending = { callback };
      const pending = btPending;
      if (route === "reconnect") {
        // the app's reconnect: no chooser window, only the remembered device, give up after RECONNECT_MS
        pending.reconnect = arm;
        pending.timer = setTimeout(() => {
          log(`bluetooth: reconnect - ${arm.deviceName || arm.deviceId} did not show in ${RECONNECT_MS / 1000} s`);
          finishBluetooth("");
        }, RECONNECT_MS);
      } else if (route === "auto") {
        pending.timer = setTimeout(() => {
          log(`bluetooth: automation found no device matching /${opt.btPattern}/i in ${AUTO_PICK_MS / 1000} s`);
          finishBluetooth("");
        }, AUTO_PICK_MS);
      } else if (route === "inapp") {
        pending.inApp = true; // listed in the app's connection dialog
      } else {
        const c = new Chooser(win, "bluetooth", opt.btPattern);
        c.onFinish = (id) => finishBluetooth(id);
        choosers.add(c);
        pending.chooser = c;
      }
    }
    const pending = btPending;
    pending.callback = callback; // Electron hands a fresh callback with every list update
    pending.devices = devices;
    if (pending.reconnect) {
      const id = policy.reconnectPick(devices, pending.reconnect);
      if (id) finishBluetooth(id);
    } else if (pending.chooser) {
      pending.chooser.update(devices);
    } else if (pending.inApp) {
      win.webContents.send("bt:devices", policy.inAppDevices(devices, opt.btPattern));
    } else {
      const id = policy.autoPick(devices, opt.btPattern);
      if (id) finishBluetooth(id);
    }
  });
  win.webContents.on("destroyed", () => finishBluetooth(""));
}

function handleUsb(ses) {
  ses.on("select-usb-device", (event, details, callback) => {
    event.preventDefault();
    const parent = BrowserWindow.getFocusedWindow() || mainWindow;
    const list = details.deviceList.map((d) => ({ deviceId: d.deviceId, deviceName: d.productName || `USB ${d.vendorId.toString(16)}:${d.productId.toString(16)}` }));
    const c = new Chooser(parent, "usb", opt.btPattern);
    choosers.add(c);
    c.onFinish = (id) => {
      choosers.delete(c);
      callback(id || undefined);
    };
    c.update(list);
  });
}

function guardNavigation(win) {
  const external = (url) => {
    if (/^https?:\/\//i.test(url)) void shell.openExternal(url);
  };
  win.webContents.on("will-navigate", (e, url) => {
    if (!policy.isAppOrigin(url)) {
      e.preventDefault();
      external(url);
    }
  });
  win.webContents.setWindowOpenHandler(({ url }) => {
    external(url);
    return { action: "deny" };
  });
}

// ---- online self-update (./selfupdate.cjs; format and key: ./updatepolicy.cjs) ----
// The page (result/app/src/features/selfupdate.ts) decides when to check (setting, 24 h, online) and shows the result;
// this side fetches, verifies, downloads, stages and runs the swap helper. Automation and dev runs never check on their own
// (noAuto; 「지금 확인」 still works).
let updater = null;
function startUpdater() {
  const plan = updatePolicy.updatePlan({
    platform: process.platform,
    packaged: app.isPackaged,
    execPath: process.execPath,
    argv: process.argv,
    portableFile: process.env.PORTABLE_EXECUTABLE_FILE,
  });
  const ps = path.join(process.env.SystemRoot || "C:\\Windows", "System32", "WindowsPowerShell", "v1.0", "powershell.exe");
  updater = createUpdater({
    fetch: (url, init) => net.fetch(url, init),
    currentVersion: app.getVersion(),
    plan,
    dir: path.join(app.getPath("userData"), "update"),
    log,
    // A detached Windows PowerShell (DETACHED_PROCESS) does not run at all, and a plain child is killed with the app
    // (libuv puts children in a kill-on-close job). So a short launcher starts the helper with Start-Process, which
    // leaves that job; the app waits for the launcher to finish before it quits.
    spawnHelper: (script, env, cwd) => new Promise((resolve) => {
      const launch = "Start-Process -FilePath $env:OG_PS -WindowStyle Hidden -ArgumentList @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',('\"' + $env:OG_SCRIPT + '\"'))";
      const child = spawn(ps, ["-NoProfile", "-NonInteractive", "-EncodedCommand", Buffer.from(launch, "utf16le").toString("base64")], {
        cwd, env: { ...process.env, ...env, OG_PS: ps, OG_SCRIPT: script }, stdio: "ignore", windowsHide: true,
      });
      const done = () => {
        clearTimeout(timer);
        resolve();
      };
      const timer = setTimeout(done, 15000);
      child.on("exit", done);
      child.on("error", (e) => {
        log(`update launcher: ${e.message}`);
        done();
      });
    }),
    quit: () => app.quit(),
  });
  updater.onChange((st) => {
    if (mainWindow && !mainWindow.isDestroyed()) mainWindow.webContents.send("upd:changed", { ...st, noAuto: noAuto() });
  });
  log(`self-update: ${plan.kind}${plan.reason ? ` (${plan.reason})` : ""}, version ${app.getVersion()}`);
}
const noAuto = () => opt.automation || !app.isPackaged;
const updState = () => ({ ...updater.state(), noAuto: noAuto() });
ipcMain.handle("upd:state", (e) => (fromApp(e) && updater ? updState() : null));
ipcMain.handle("upd:check", async (e) => {
  if (!fromApp(e) || !updater) return null;
  await updater.check();
  return updState();
});
ipcMain.handle("upd:apply", (e) => (fromApp(e) && updater ? updater.apply({ relaunch: true }) : false));
ipcMain.handle("upd:open", (e) => {
  if (!fromApp(e)) return false;
  const page = updater && updater.state().page;
  void shell.openExternal(typeof page === "string" && page.startsWith("https://github.com/") ? page : updatePolicy.RELEASES_PAGE);
  return true;
});

let mainWindow = null;

// Window bounds survive restarts; the default is wide enough for the desktop layout (side nav at >= 900 px).
const boundsFile = () => path.join(app.getPath("userData"), "window.json");

function loadBounds() {
  try {
    const b = JSON.parse(fs.readFileSync(boundsFile(), "utf8"));
    if (Number.isFinite(b.width) && Number.isFinite(b.height) && b.width >= 360 && b.height >= 480) return b;
  } catch {
    /* first run or unreadable: defaults */
  }
  return { width: 1280, height: 820 };
}

function saveBounds(win) {
  if (win.isMinimized() || win.isMaximized()) return;
  try {
    fs.writeFileSync(boundsFile(), JSON.stringify(win.getBounds()));
  } catch (e) {
    log(`window bounds not saved: ${e}`);
  }
}

function createWindow(root) {
  const icon = path.join(root, "icon-512.png");
  const b = loadBounds();
  mainWindow = new BrowserWindow({
    ...b,
    minWidth: 360,
    minHeight: 480,
    title: "OpenGrand",
    autoHideMenuBar: true,
    backgroundColor: "#111418",
    icon: fs.existsSync(icon) ? icon : undefined,
    webPreferences: policy.appWebPreferences(path.join(__dirname, "preload.cjs")),
  });
  guardNavigation(mainWindow);
  handleBluetooth(mainWindow);
  mainWindow.on("close", () => saveBounds(mainWindow));
  mainWindow.on("closed", () => (mainWindow = null));
  const url = `${policy.ORIGIN}/index.html${opt.query ? `?${opt.query}` : ""}`;
  log(`loading ${url} from ${root}`);
  void mainWindow.loadURL(url);
}

// One instance per profile: BLE holds one central; a second window would fight the first for the piano.
if (!app.requestSingleInstanceLock()) {
  log("another instance is running (same profile) - exiting");
  app.quit();
} else {
  app.on("second-instance", () => {
    if (mainWindow) {
      if (mainWindow.isMinimized()) mainWindow.restore();
      mainWindow.focus();
    }
  });
  app.whenReady().then(() => {
    const root = appRoot();
    if (!fs.existsSync(path.join(root, "index.html"))) {
      dialog.showErrorBox("OpenGrand", `앱 빌드가 없습니다 · app build not found:\n${root}\n\ncd result/app && npm run build`);
      app.quit();
      return;
    }
    serveApp(root);
    const ses = session.defaultSession;
    guardPermissions(ses);
    handleUsb(ses);
    startUpdater();
    createWindow(root);
  });
  app.on("window-all-closed", () => app.quit());
  // a staged update not applied by 「다시 시작해서 업데이트」 is swapped in when the app quits (no relaunch); not in
  // automation runs, whose test tool may start the exe again right away
  let helperPending = false;
  app.on("will-quit", (e) => {
    if (helperPending || !updater || opt.automation) return;
    const started = updater.apply({ relaunch: false });
    if (!started) return;
    helperPending = true;
    e.preventDefault();
    started.then(() => app.quit());
  });
}
