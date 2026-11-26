// App window preload (sandboxed). Exposes who the host is (logs, automation checks), the online self-update and two desktop-only
// services (the app's side: result/app/src/transport/desktop.ts): reconnect to the Bluetooth piano the user last
// chose (Electron has no getDevices()), and the Bluetooth device list of a running requestDevice() for the app's own
// connection dialog (onDevices / choose - no separate chooser window). The app works without them, like in Chrome.
"use strict";

const { contextBridge, ipcRenderer } = require("electron");

// once per page load: the device the main process remembers (name only; the id stays in the main process)
let remembered = null;
try {
  remembered = ipcRenderer.sendSync("bt:remembered") || null;
} catch {
  remembered = null;
}

// In-app Bluetooth list: while the page listens, the main process sends it the devices of a requestDevice() call
// instead of opening its chooser window. A fresh page listens to nothing (a reload forgets the old page's listeners).
const btListeners = new Set();
ipcRenderer.on("bt:devices", (_e, list) => {
  for (const cb of [...btListeners]) cb(list);
});
ipcRenderer.send("bt:page"); // a new document: the main process drops what the previous page left running

// Online self-update (main: selfupdate.cjs): state pushes while a check / download runs
const updListeners = new Set();
ipcRenderer.on("upd:changed", (_e, st) => {
  for (const cb of [...updListeners]) cb(st);
});

contextBridge.exposeInMainWorld("ca79Desktop", Object.freeze({
  shell: "electron",
  platform: process.platform,
  electron: process.versions.electron,
  chrome: process.versions.chrome,
  bluetooth: Object.freeze({
    remembered,
    // the main process arms the chooser for that one device and runs the page's reconnect hook with a gesture
    reconnect: () => ipcRenderer.invoke("bt:reconnect"),
    // cb(list of { deviceId, deviceName }, piano-like names first) on every update of a running requestDevice();
    // returns the function that stops listening. Synchronous on purpose: the main process knows the page listens
    // before the requestDevice() call that follows can reach it.
    onDevices: (cb) => {
      if (typeof cb !== "function") return () => {};
      if (!btListeners.size) ipcRenderer.sendSync("bt:listen");
      btListeners.add(cb);
      return () => {
        if (btListeners.delete(cb) && !btListeners.size) ipcRenderer.send("bt:unlisten");
      };
    },
    // answer the running request: a deviceId from the list, or "" to cancel it
    choose: (id) => ipcRenderer.send("bt:choose", String(id ?? "")),
  }),
  // online self-update (the app's side: result/app/src/features/selfupdate.ts)
  update: Object.freeze({
    state: () => ipcRenderer.invoke("upd:state"),
    check: () => ipcRenderer.invoke("upd:check"),
    apply: () => ipcRenderer.invoke("upd:apply"),
    openPage: () => ipcRenderer.invoke("upd:open"),
    onChange: (cb) => {
      if (typeof cb !== "function") return () => {};
      updListeners.add(cb);
      return () => updListeners.delete(cb);
    },
  }),
}));
