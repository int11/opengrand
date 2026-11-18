// Device chooser window preload: device list in, one choice out ("" = cancel).
"use strict";

const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("picker", {
  onInit: (cb) => ipcRenderer.on("picker:init", (_e, info) => cb(info)),
  onDevices: (cb) => ipcRenderer.on("picker:devices", (_e, list) => cb(list)),
  choose: (id) => ipcRenderer.send("picker:choose", String(id ?? "")),
});
