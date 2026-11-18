// App window preload (sandboxed). Exposes who the host is (logs, automation checks) and one desktop-only
// service: reconnect to the Bluetooth piano the user last chose (Electron has no getDevices(); the app's
// branch is result/app/src/transport/desktop.ts). The app works without it, like in Chrome.
"use strict";

const { contextBridge, ipcRenderer } = require("electron");

// once per page load: the device the main process remembers (name only; the id stays in the main process)
let remembered = null;
try {
  remembered = ipcRenderer.sendSync("bt:remembered") || null;
} catch {
  remembered = null;
}

contextBridge.exposeInMainWorld("ca79Desktop", Object.freeze({
  shell: "electron",
  platform: process.platform,
  electron: process.versions.electron,
  chrome: process.versions.chrome,
  bluetooth: Object.freeze({
    remembered,
    // the main process arms the chooser for that one device and runs the page's reconnect hook with a gesture
    reconnect: () => ipcRenderer.invoke("bt:reconnect"),
  }),
}));
