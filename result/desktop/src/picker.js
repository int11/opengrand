// Device chooser page: lists what main sends, the user picks one (never automatic here).
"use strict";

const list = document.getElementById("list");
const state = document.getElementById("state");
const title = document.getElementById("title");
const PIANO = /CA79|KAWAI/i;

document.getElementById("cancel").addEventListener("click", () => window.picker.choose(""));
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape") window.picker.choose("");
});

window.picker.onInit(({ kind }) => {
  title.textContent = kind === "bluetooth" ? "Bluetooth MIDI 장치 고르기 · Choose a device" : "USB 장치 고르기 · Choose a USB device";
});

window.picker.onDevices((devices) => {
  list.replaceChildren();
  state.textContent = devices.length ? `${devices.length}개 찾음 · found — 찾는 중… · still searching` : "찾는 중… · Searching…";
  for (const d of devices) {
    const b = document.createElement("button");
    b.dataset.id = d.deviceId;
    const name = d.deviceName || "(이름 없음 · unnamed)";
    b.textContent = PIANO.test(name) ? `${name}  ★` : name;
    const s = document.createElement("small");
    s.textContent = d.deviceId;
    b.append(s);
    b.addEventListener("click", () => window.picker.choose(d.deviceId));
    const li = document.createElement("li");
    li.append(b);
    list.append(li);
  }
});
