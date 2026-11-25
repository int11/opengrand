// OpenGrand controller app entry point (panel and phone).
import { installTestHelpers } from "./testhooks.ts";
import { setLang, t } from "./i18n.ts";
import { PianoSession } from "./protocol/session.ts";
import { registerServiceWorker } from "./pwa.ts";
import { attachPanelCache } from "./state/panelcache.ts";
import { TAB_IDS, type TabId } from "./state/settings.ts";
import { ParamStore } from "./state/store.ts";
import { AUTO_RECONNECT, BOOT_NOTICE, ConnectionManager, connView, type ConnState, type LinkChoice } from "./state/connection.ts";
import { androidAvailable, androidCandidate, listAndroid, onShellLifecycle, panelInfo, scanBle, setBacklight } from "./transport/android.ts";
import { bleReopenAvailable, connectBleDevice, reopenBleMidi, requestBleDevice, requestBleMidi } from "./transport/ble.ts";
import { desktopBleScan, desktopBleScanAvailable, desktopReopenBle, desktopShell } from "./transport/desktop.ts";
import { autoLink, kwm, kwmcoreAvailable, launchAndroidSettings, listKwmcore, onDeviceChange, openUrl, readAnyFile, setBacklight255, usbPath, writeAnyFile } from "./transport/kwmcore.ts";
import type { LinkCandidate, MidiLink } from "./transport/link.ts";
import { listWebMidi } from "./transport/webmidi.ts";
import { openWebSocket } from "./transport/websocket.ts";
import { chordScreen } from "./ui/chords.ts";
import { createContext } from "./ui/context.ts";
import { usbListState } from "./ui/usblist.ts";
import { usbPlayback } from "./ui/usbplay.ts";
import { bindStore } from "./ui/controls.ts";
import { DisplayController } from "./ui/display.ts";
import { h } from "./ui/dom.ts";
import { recorderScreen } from "./ui/recorder.ts";
import { APP_VERSION, infoScreen } from "./ui/info.ts";
import { confirmLoaderOnConnect } from "./ui/otaloader.ts";
import { installPanelScreenGlobals } from "./ui/panelscreenbox.ts";
import { btAudioBadge } from "./ui/btaudio.ts";
import { bluetoothPage } from "./ui/bluetooth.ts";
import { BtPort } from "./protocol/btport.ts";
import { NgClient } from "./protocol/ngext.ts";
import { PORT_BLE, PORT_PANEL, coBoundBadge } from "./ui/cobound.ts";
import { wakeOnConnect } from "./ui/power.ts";
import { powerButton } from "./ui/powerbtn.ts";
import { watchActivate, type UiLoaderApi } from "./ui/activatewatch.ts";
import { tabIcon } from "./ui/tabicons.ts";
import { takeoverBanner } from "./ui/takeover.ts";
import { UpdaterWatch } from "./protocol/updatemode.ts";
import { KAWAI_UPDATE_PAGE, updateScreen } from "./ui/updatemode.ts";
import { NOT_FOUND_TEXT, devSimulator, dropStoredSim, openPicker, reopenRemembered } from "./ui/picker.ts";
import { installHandoff } from "./state/handoff.ts";
import { installTapGuard } from "./ui/tapguard.ts";
import { TAB_TITLES, metronomeScreen, musicScreen, settingsScreen, soundScreen, vtScreen } from "./ui/screens.ts";
import { selfUpdateHub } from "./features/selfupdate.ts";
import { selfUpdateBanner } from "./ui/selfupdatebox.ts";

const store = new ParamStore();
bindStore(store);
const beatEvents = new EventTarget();
let session: PianoSession | null = null;
let connectedOnce = false;

// Inside the ORIGINAL panel shell (UI bundle): the
// chooser page passes ?embedded=0|1 from getEmbeddedMode; the panel build is embed.
const kwmShell = kwmcoreAvailable();
const embedded = kwmShell ? new URLSearchParams(location.search).get("embedded") !== "0" : panelInfo().embedded;
// installable web app: offline start from the service-worker cache (browsers only)
registerServiceWorker(androidAvailable() || kwmShell);
const ctx = createContext(store, { session: () => session, panel: embedded });
usbListState(ctx); // watch USBMemory plugs from the start: an empty list right after one is asked again (ui/usblist.ts)
setLang(ctx.settings.lang);
// a touch drag / scroll never presses a control; sliders take only a clear sideways drag (ui/tapguard.ts)
installTapGuard(document, { touch: embedded });
ctx.display = new DisplayController({ setBacklight: kwmShell ? setBacklight255 : setBacklight, root: document.documentElement, events: document, panel: embedded });
ctx.display.apply(ctx.settings);
// original shell: brightness / Auto Display Off live in the panel's user/uicache.json (shared with the Kawai UI)
if (kwmShell) void attachPanelCache(ctx);
// the loader's "start the new UI" that never reloads (field 2026-11-15): older loaders leave their notice up — ui/activatewatch.ts
watchActivate((globalThis as unknown as { CA79UI?: UiLoaderApi }).CA79UI);

const root = document.getElementById("app")!;
const status = h("span", { class: "status" });
const main = h("main", {});
const screens: Record<TabId, () => HTMLElement> = {
  sound: () => soundScreen(store, ctx),
  music: () => musicScreen(store, ctx),
  metronome: () => metronomeScreen(store, beatEvents),
  technician: () => vtScreen(store, ctx),
  chords: () => chordScreen(ctx),
  // 「레코더」 (the tab id stays "files": stored start tabs, ?tab=files links and test drivers keep working)
  files: () => recorderScreen(ctx),
  // 「블루투스」: BT audio sources / AUTOCALL / this app's BLE-MIDI link / the piano's BT settings (ui/bluetooth.ts)
  bluetooth: () => bluetoothPage(() => (session ? new BtPort(new NgClient(session)) : null), {
    store, link: () => (session && linkKind ? { kind: linkKind, name: linkName } : null),
    // the panel shell is always connected to its piano: no connect menu there
    coBound: () => coBound.el.hidden ? "" : coBound.el.textContent ?? "", openConnect: kwmShell ? undefined : () => conn.connectButton(),
  }),
  settings: () => settingsScreen(store, ctx),
  info: () => infoScreen(ctx),
};
// Every tab on every screen size (user 2026-11-02): narrow screens (the panel, phones)
// show them in one bottom row that scrolls sideways (#tabbar), wide screens list them on the left. The row shows
// a fade on the side that has more tabs (nav.fade-l / nav.fade-r) and keeps the open tab in view.
const tabbar = h("div", { class: "tabs", id: "tabbar" });
const nav = h("nav", {}, tabbar);
function navFade(): void {
  const max = tabbar.scrollWidth - tabbar.clientWidth;
  nav.classList.toggle("fade-l", max > 2 && tabbar.scrollLeft > 2);
  nav.classList.toggle("fade-r", max > 2 && tabbar.scrollLeft < max - 2);
}
/** Scroll the tab row so the open tab sits in the middle (as far as the row goes); nothing when it does not scroll. */
function revealTab(b: HTMLElement | null): void {
  if (b && tabbar.scrollWidth > tabbar.clientWidth) tabbar.scrollLeft = Math.max(0, b.offsetLeft + b.offsetWidth / 2 - tabbar.clientWidth / 2);
  navFade();
}
tabbar.addEventListener("scroll", navFade, { passive: true });
if (typeof addEventListener === "function") addEventListener("resize", () => revealTab(tabbar.querySelector<HTMLElement>("button.on")));
// one line after the app woke the piano from OpenGrand standby on connect (wakeOnConnect)
const wakeNote = h("p", { class: "muted", id: "autowake-note", hidden: true });
// one line when the remembered piano cannot be reopened on start (Web Bluetooth without getDevices())
const reconnectNote = h("p", { class: "muted", id: "reconnect-note", hidden: true });
// one line after the app confirmed (or found given up) a loader it uploaded (ota_loader_v4, ui/otaloader.ts)
const loaderNote = h("p", { class: "muted", id: "loader-note", hidden: true });
let current: TabId = ctx.settings.startTab;

// ---- connection: one state machine (src/state/connection.ts); header, button and banner draw only its state ----
const LAST_LINK = "ca79ng.lastLink";
let linkKind: MidiLink["kind"] | null = null;
let linkName = "";
// the firmware simulator exists only in development (picker.ts devSimulator): never remembered / reopened elsewhere
const devSim = devSimulator({ shell: androidAvailable(), kwm: kwmShell, embedded, hostname: location.hostname, search: location.search });
const isSimKey = (key?: string | null) => !!key && key.startsWith("websocket:");
// field 2026-11-01 (phone shell): a simulator websocket remembered as the last piano - drop it
try { dropStoredSim(localStorage, LAST_LINK, devSim); } catch { /* storage unavailable */ }
const conn = new ConnectionManager({
  makeSession: (link) => {
    linkKind = link.kind;
    linkName = link.name;
    return new PianoSession(tapLink(link), { trace: (line) => logLine(`-- ${line}`) });
  },
  onSession: (s) => {
    session = s;
    if (s) attachSession(s);
  },
  wake: (s) => autoWake(s),
  // displaced right after connecting = the panel's start-up CONNECT (one-binding firmware): say so, take nothing back.
  // Not in the panel builds: they are the panel.
  bootNotice: kwmShell || embedded ? null : BOOT_NOTICE,
  // desktop app: a lost piano (power-cycled, out of range) is reopened on its own, minimized or not (field 2026-11-03).
  // Not in a browser tab / the phone shell: they let the piano go while in the background (state/handoff.ts).
  autoReconnect: desktopShell() ? AUTO_RECONNECT : null,
  // desktop app: a lost BLE piano whose held BluetoothDevice did not reconnect is looked up afresh, the way the app
  // start does (field 2026-11-03: after Windows dropped and re-made the BLE connection only a restart connected).
  // getDevices() where it exists, else the shell's reconnect (requestDevice, the remembered device auto-picked).
  reacquire: desktopShell()?.bluetooth ? (key) => (key?.startsWith("ble:") ? (bleReopenAvailable() ? reopenBleMidi(key) : desktopReopenBle(() => requestBleMidi())) : null) : undefined,
  remember: (l) => {
    if (l && isSimKey(l.key) && !devSim) return;
    try {
      if (l) localStorage.setItem(LAST_LINK, JSON.stringify(l));
      else localStorage.removeItem(LAST_LINK);
    } catch { /* storage unavailable */ }
  },
  // the panel shell has no connect button (always its piano, auto-connect below): nothing opens these there
  openPicker: () => { if (!kwmShell) void openConnect(); },
  openMenu: () => { if (!kwmShell) openConnectedMenu(); },
  trace: (line) => logLine(`-- ${line}`),
});
const connectBtn = h("button", { class: "primary", id: "connect", onclick: () => conn.connectButton() }) as HTMLButtonElement;
// Built-in panel, piano switched on with E+A+B+C: the stock USB updater (BootProgram) answers our CONNECT with
// Mode DevelopperMode/HostUpdate — and only that first CONNECT.
// Show "do not switch off" and hand over to the Kawai update page at once, as the original UI does on that reply
// (the Kawai main UI would never be answered now). The update screen stays as the fallback when the page cannot go.
const updWatch = new UpdaterWatch();
const UPDATE_HANDOVER_MS = 1200;
let updHandover: ReturnType<typeof setTimeout> | undefined;
const updScreen = updateScreen({
  kawai: () => { location.href = KAWAI_UPDATE_PAGE; },
  dismiss: () => { clearTimeout(updHandover); updWatch.state.active = false; updWatch.pianoRunning(); updScreen.render(updWatch.state); },
});
updWatch.onChange((s) => {
  updScreen.render(s);
  if (s.active && updHandover === undefined && kwmShell) {
    logLine(`-- USB firmware update mode (${s.via}): handing over to the Kawai update page`);
    updHandover = setTimeout(() => { location.href = KAWAI_UPDATE_PAGE; }, UPDATE_HANDOVER_MS);
  }
});
// 7E 01 00 (another device took the binding), failed, lost: banner with the one button that helps
const takeover = takeoverBanner(conn);
// header chip (Bluetooth icon + a short state, every surface): opens the 「블루투스」 tab
const btAudio = btAudioBadge(() => session, conn, () => show("bluetooth"));
// header power button next to connect (owner 2026-11-15): 「대기로」 (19 01) / 「깨우기」 (19 02)
const powerNote = h("p", { class: "muted", id: "power-note", hidden: true });
const power = powerButton(() => session, conn, {
  say: (m) => { powerNote.textContent = m; powerNote.hidden = !m; },
  changed: () => { btAudio.refresh(); if (current === "settings") show(current); },
});
// another controller bound at the same time (session-core payloads): "Panel also connected" / on the panel
// "App also connected". This side's stock port when the payload does not say: the panel builds are port 3,
// a Web Bluetooth link is port 2 (the BLE-MIDI module); other links are left to the payload's answer.
const coBound = coBoundBadge(() => session, conn, () => (kwmShell || embedded ? PORT_PANEL : linkKind === "ble" ? PORT_BLE : null));

function renderConn(s: ConnState = conn.state): void {
  const v = connView(s);
  header.dataset.phase = s.phase; // the status dot (styles.css)
  // panel shell: only the state — 「피아노 연결됨」 once linked, the progress / failure text otherwise
  status.textContent = kwmShell && (s.phase === "ready" || s.phase === "waking") ? t("Piano connected") : t(...v.status);
  connectBtn.textContent = t(v.button.label) + (v.button.action === "menu" ? " ▾" : "");
  connectBtn.disabled = v.button.action === "none";
  connectBtn.dataset.phase = s.phase;
}
let lastPhase = conn.phase;
conn.onChange((s) => {
  renderConn(s);
  if (s.phase !== "idle") reconnectNote.hidden = true;
  if (s.phase === "ready" && lastPhase !== "waking" && lastPhase !== "ready") {
    connectedOnce = true;
    updWatch.pianoRunning();
    usbPlayback(ctx); // follow the USB players from the connect on, not only while the Recorder tab is open (ui/usbplay.ts)
    // panel UI loader (panelui/uiloader.js): this UI version reached the piano → keep it
    (globalThis as unknown as { CA79UI?: { confirm(): void } }).CA79UI?.confirm();
    // the same for a loader this app uploaded over Bluetooth: it reached the app again → confirm it (op 0x0F);
    // sends nothing unless such an upload is remembered
    const s0 = session;
    if (s0) void confirmLoaderOnConnect(s0).then((m) => { loaderNote.textContent = m; loaderNote.hidden = !m; }).catch(() => {});
    show(current); // screens read the session state: draw them for the synced piano
  }
  lastPhase = s.phase;
});

function renderChrome(): void {
  tabbar.replaceChildren(...TAB_IDS.map((id) => h("button", {
    "data-tab": id, id: `tab-${id}`, title: t(TAB_TITLES[id]), onclick: () => show(id),
  }, tabIcon(id), h("span", { class: "lbl" }, t(TAB_TITLES[id])))));
  logBtn.textContent = t("Close MIDI log");
  renderConn();
  takeover.render(conn.state);
}

function show(id: TabId): void {
  current = id;
  main.replaceChildren(screens[id]());
  let on: HTMLElement | null = null;
  tabbar.querySelectorAll<HTMLElement>("button[data-tab]").forEach((b) => {
    b.classList.toggle("on", b.dataset.tab === id);
    if (b.dataset.tab === id) on = b;
  });
  revealTab(on);
}

ctx.rerender = () => {
  renderChrome();
  power.redraw(); // field 2026-11-15: 「To standby」 stayed English after switching the panel to Korean
  show(current);
};

// header: name · status (+ another controller) · Bluetooth chip · connect (not in the panel shell: the panel is
// always connected to its piano). One line high on every screen size — the panel is 480 CSS px wide
// Language, MIDI log, and inside the original shell the stock
// Kawai UI (../ui/html) and the chooser (../index.html) are in the Settings tab's 「앱 · 화면」 card (ctx.app).
const header = h("header", {}, h("strong", { class: "brand" }, "OpenGrand"), h("div", { class: "hinfo" }, status, coBound.el), btAudio.el, power.el, ...(kwmShell ? [] : [connectBtn]));
// online self-update (phone app, Windows desktop app): checks on its own schedule, banner once a new version is ready
const upHub = selfUpdateHub();
root.append(header, takeover.el, ...(upHub ? [selfUpdateBanner(upHub)] : []), wakeNote, powerNote, reconnectNote, loaderNote, nav, main, updScreen.el);
if (embedded) document.body.classList.add("panel");
if (kwmShell) document.body.classList.add("kwm");

// the Settings tab's 「앱 · 화면」 card (screens/settings.ts appSection)
// test drivers: read USB files / song exports without a screen (automated device tests)
(globalThis as unknown as { __files?: () => unknown }).__files = () => ctx.files();
// the whole app context (session · store · files · settings) for CDP test bodies, so a new probe needs no rebuild
(globalThis as unknown as { __ca79?: unknown }).__ca79 = { ctx, session: () => ctx.session(), files: () => ctx.files() };
installTestHelpers();

ctx.app = {
  shell: kwmShell,
  midiLogOpen: () => !logPre.hidden,
  setMidiLog: (open) => setLogOpen(open),
  go: (href) => { location.href = href; },
  androidSettings: () => launchAndroidSettings(),
  openUsb: async (rel) => {
    const root = await usbPath();
    if (!root) return "no USB path (stick in [USB for UPDATE]?)";
    const url = "file://" + root.replace(/\/+$/, "") + "/" + rel;
    try { await openUrl(url); return "ok · " + url; } catch { return "no app for " + url; }
  },
  openUri: async (uri) => {
    let url = uri;
    if (url.includes("{usb}")) {
      const root = await usbPath();
      const id = root.replace(/\/+$/, "").split("/").pop() ?? "";
      if (!id) return "no USB path (stick in [USB for UPDATE]?)";
      url = url.split("{usb}").join(id);
    }
    try { await openUrl(url); return "ok · " + url; } catch { return "no app for " + url; }
  },
  readFile: async (path) => { try { return await readAnyFile(path); } catch { return null; } },
  dumpToUsb: async (name, text) => {
    const root = (await usbPath()).replace(/\/+$/, "");
    if (!root) return "no USB path";
    const file = root + "/ca79ng_dump/" + name.replace(/[^A-Za-z0-9._-]+/g, "_") + ".txt";
    try { await writeAnyFile(file, text); return "saved " + file; } catch { return "could not write " + file; }
  },
};

// MIDI traffic log: always recorded (window.__midiLog, last MIDI_LOG_MAX lines, read by test drivers);
// shown in a panel toggled by the "MIDI log" switch in Settings (open state remembered; ?log opens it).
// TX lines carry the session queue at the send (` | ready inflight=55 08 00#1 queued=2`), and session
// events (state changes, sends dropped while displaced, given-up messages) are logged as `-- ...` lines -
// field: a 400-line ring lost the minutes that mattered.
const MIDI_LOG_MAX = 5000;
const midiLog: string[] = [];
(window as unknown as { __midiLog: string[] }).__midiLog = midiLog;
const logPre = h("pre", { id: "midi-log", hidden: true, style: "position:fixed;left:0;right:0;bottom:0;height:35vh;overflow:auto;margin:0;padding:4px;background:#000;color:#8f8;font-size:11px;z-index:98;user-select:text" });
// the close button floats only while the log is open (it is opened from Settings → 「앱 · 화면」)
const logBtn = h("button", { id: "midi-log-toggle", hidden: true, style: "position:fixed;right:8px;bottom:calc(35vh + 8px);z-index:99;opacity:.85" }, t("Close MIDI log"));
function setLogOpen(open: boolean): void {
  logPre.hidden = !open;
  logBtn.hidden = !open;
  document.body.style.paddingBottom = open ? "36vh" : "";
  try { localStorage.setItem("midiLogOpen", open ? "1" : "0"); } catch { /* storage unavailable */ }
  if (open) { logPre.textContent = midiLog.join("\n"); logPre.scrollTop = logPre.scrollHeight; }
  // the Settings switch follows the floating close button
  const sw = document.getElementById("midi-log-switch") as HTMLInputElement | null;
  if (sw) sw.checked = open;
}
logBtn.onclick = () => setLogOpen(logPre.hidden);
document.body.append(logPre, logBtn);
renderChrome();
show(current);
setLogOpen(new URLSearchParams(location.search).has("log") || (() => { try { return localStorage.getItem("midiLogOpen") === "1"; } catch { return false; } })());
const t0Log = performance.now();
function logLine(line: string): void {
  midiLog.push(`${((performance.now() - t0Log) / 1000).toFixed(3)} ${line}`);
  if (midiLog.length > MIDI_LOG_MAX) midiLog.splice(0, midiLog.length - MIDI_LOG_MAX);
  if (!logPre.hidden) { logPre.textContent = midiLog.join("\n"); logPre.scrollTop = logPre.scrollHeight; }
}
function logMidi(dir: string, m: Uint8Array, note = ""): void {
  if (m.length === 1 && m[0] >= 0xf8) return;
  logLine(`${dir} ${[...m].map((x) => x.toString(16).padStart(2, "0")).join(" ")}${note ? ` | ${note}` : ""}`);
}
function tapLink(link: MidiLink): MidiLink {
  // debugging hook for test drivers: send raw bytes on the current link (replies land in __midiLog)
  (window as unknown as { __sendHex: (hex: string) => void }).__sendHex = (hex) => {
    const m = new Uint8Array(hex.trim().split(/\s+/).map((x) => parseInt(x, 16)));
    logMidi("TX", m);
    link.send(m);
  };
  return {
    kind: link.kind,
    name: link.name,
    send: (m) => { logMidi("TX", m, session?.describe() ?? ""); link.send(m); },
    onMessage: (hd) => link.onMessage((m) => {
      logMidi("RX", m);
      if ((kwmShell || embedded) && updWatch.feed(m)) return; // the USB updater's frames are not the piano's
      hd(m);
    }),
    onClose: (hd) => link.onClose(hd),
    close: () => link.close(),
  };
}

function attachSession(s: PianoSession): void {
  store.attach(s);
  s.onParam((p) => {
    if (p.v1 === 0x56 && p.v2 === 0x0b) beatEvents.dispatchEvent(new CustomEvent("beat", { detail: p.data[0] }));
  });
  s.onMidi((m) => ctx.midi.dispatchEvent(new CustomEvent("midi", { detail: m })));
}

// App only (not the built-in panel): opening the app wakes a piano in OpenGrand standby (setting, default
// on), then shows one line. Called by the connection
// manager at the first contact of a session (ready: the phase "waking" until this ends; or displaced while
// connecting — 19 00 / 19 02 answer on any port, the panel holding the binding does not matter).
function autoWake(s: PianoSession): Promise<void> | null {
  wakeNote.hidden = true;
  if (kwmShell || embedded || !ctx.settings.autoWake) return null;
  return wakeOnConnect(s).then((woke) => {
    if (!woke || session !== s) return;
    wakeNote.textContent = t("Woke the piano from standby.");
    wakeNote.hidden = false;
    btAudio.refresh();
    power.refresh();
    if (current === "settings") show(current); // the power card shows the new state
  }, () => {});
}

/** Pick a device: the piano already in use opens nothing (connection.ts `pick`). */
function choose(c: LinkChoice): Promise<void> {
  return conn.choose(c).then(() => {}, (e: Error) => {
    logLine(`-- open failed: ${e.message}`);
    // Web Bluetooth chooser closed without a piano (NotFoundError): usually another device holds the BLE link
    if (e.name === "NotFoundError" || /cancel/i.test(e.message)) {
      reconnectNote.textContent = t(NOT_FOUND_TEXT);
      reconnectNote.hidden = false;
    }
  });
}

/** live phases: the "in use" mark of the picker (not for a link that failed to open / was lost) */
const LIVE = ["connecting", "syncing", "ready", "waking", "displaced", "retaking"];

// Connected: "Connect" does not open a second link — it offers Disconnect / another device.
function openConnectedMenu(): void {
  const dlg = h("dialog", { class: "sheet", id: "conn-menu" });
  const close = () => { dlg.close(); dlg.remove(); };
  dlg.append(
    h("h3", {}, t(...connView(conn.state).status)),
    h("div", { class: "list" },
      // a real CONNECT handshake + re-sync on this link (field 2026-11-09: the piano had dropped the binding, nothing re-sent CONNECT)
      h("button", { class: "item", "data-action": "reconnect", onclick: () => { close(); conn.retry(); } }, t("Reconnect")),
      h("button", { class: "item", "data-action": "disconnect", onclick: () => { close(); conn.disconnect(); } }, t("Disconnect")),
      h("button", { class: "item", "data-action": "other-device", onclick: () => { close(); void openConnect(); } }, t("Connect another device…"))),
    h("button", { onclick: close }, t("Close")));
  document.body.append(dlg);
  dlg.showModal();
}

async function openConnect(): Promise<void> {
  const simUrl = `ws://${location.hostname || "localhost"}:${location.port || 8079}/midi`;
  openPicker({
    shell: androidAvailable(),
    dev: devSim,
    list: async () => {
      const cands: LinkCandidate[] = [];
      if (androidAvailable()) cands.push(...listAndroid());
      if (kwmShell) cands.push(...(await listKwmcore()));
      try {
        cands.push(...(await listWebMidi()));
      } catch {
        /* Web MIDI denied */
      }
      return cands;
    },
    // Android shell: BLE-MIDI pianos show up as the 5 s scan finds them
    scan: androidAvailable() ? (fn) => scanBle(fn) : undefined,
    inUseKey: () => (LIVE.includes(conn.phase) ? conn.state.key : null),
    choose: (c) => void choose(c),
    // browsers: Web Bluetooth does its own device picker; the piano already open comes back as the same link
    requestBle: androidAvailable() ? undefined : { name: "Bluetooth MIDI", open: () => requestBleMidi() },
    // desktop app: the search runs in this dialog (the shell sends the device list; no chooser window)
    bleScan: !androidAvailable() && desktopBleScanAvailable() ? (onDevices) => {
      const s = desktopBleScan(() => requestBleDevice(), onDevices)!;
      return { found: s.found.then((d) => ({ name: d.name ?? "Bluetooth MIDI", open: () => connectBleDevice(d) })), pick: s.pick, cancel: s.cancel };
    } : undefined,
    simUrl,
    openSim: (url) => ({ key: `websocket:${url}`, name: `Simulator (${url})`, open: () => openWebSocket(url) }),
  });
}

// Auto-connect on the built-in panel: the main board is the only USB-MIDI device.
if (kwmShell) {
  // original shell: wait for deviceready, take device "USB"; retry when the shell
  // reports a device change (the original UI reloaded the whole page instead)
  let linking = false;
  let kwmLink: MidiLink | null = null;
  let retry: ReturnType<typeof setTimeout> | undefined;
  const tryLink = async (): Promise<void> => {
    clearTimeout(retry);
    // live (incl. "displaced": the banner decides, never auto-steal): nothing to do
    if (linking || updWatch.state.active || !["idle", "lost", "failed"].includes(conn.phase)) return;
    linking = true;
    let ph = conn.phase;
    try {
      kwmLink?.close(); // a failed attempt's link
      kwmLink = await autoLink();
      if (kwmLink) ph = await conn.attach(kwmLink);
    } finally {
      linking = false;
    }
    // piano not answering yet (still booting): retry like the original's 3 s reload
    if (ph !== "ready" && ph !== "waking" && ph !== "displaced") retry = setTimeout(() => void tryLink(), 3000);
  };
  onDeviceChange(() => void tryLink());
  void tryLink();
  // no piano answer at all → back to the chooser, which counts down to the Kawai UI
  // (through the UI loader: a new remote UI version that never reaches the piano is marked failed)
  const fail = (globalThis as unknown as { __ca79ngFail?: (r: string, d?: string) => void }).__ca79ngFail;
  // the chooser shows the detail: a piano that never answers may be in its USB updater with the Mode reply lost
  const NOCONNECT_DETAIL = "피아노가 응답하지 않습니다 — 펌웨어 업데이트(E+A+B+C) 중일 수 있으니 전원을 끄지 마세요 · the piano may be updating its firmware: do not switch it off";
  if (embedded) setTimeout(() => {
    if (connectedOnce || updWatch.state.active || conn.phase === "displaced") return;
    if (fail) fail("noconnect", NOCONNECT_DETAIL);
    else location.replace("../index.html?reason=noconnect&detail=" + encodeURIComponent(NOCONNECT_DETAIL));
  }, 30000);
} else if (embedded) {
  const first = listAndroid().find((c) => /USB/i.test(c.name));
  if (first) choose(first);
} else {
  // phone / PC: take the last piano back on start (no chooser), and let it go while the app is away
  const rem = rememberedChoice();
  if (rem && !new URLSearchParams(location.search).has("sim")) {
    // the remembered piano did not come back (another device holds its BLE link, out of range): open the picker
    if ("open" in rem) void reopenRemembered(conn, rem, () => void openConnect());
    else if (desktopShell()) {
      // desktop app whose shell has not remembered a Bluetooth device yet (first start of this version)
      reconnectNote.textContent = t("{name} was not reconnected automatically: choose it once under Connect, and the app will reconnect to it on start from then on.", { name: rem.name });
      reconnectNote.hidden = false;
      logLine("-- auto-reconnect skipped: the desktop shell remembers no Bluetooth device yet");
    } else {
      // M2 (2026-11-01): PC Chrome without getDevices() — say why nothing happens and what to do
      reconnectNote.textContent = t("{name} was not reconnected automatically: this browser cannot reopen a Bluetooth device after a reload. Tap Connect and choose it again.", { name: rem.name });
      reconnectNote.hidden = false;
      logLine("-- auto-reconnect skipped: navigator.bluetooth.getDevices() is not available (Chrome: chrome://flags/#enable-web-bluetooth-new-permissions-backend)");
    }
  }
  installLifecycle();
}

/**
 * The piano this app last synced with (localStorage), as a choice that opens without a chooser —
 * or `{ unavailable }` when this browser cannot reopen it on its own (Web Bluetooth without getDevices(); the
 * desktop app reopens through its shell instead, transport/desktop.ts).
 */
function rememberedChoice(): LinkChoice | { unavailable: true; name: string } | null {
  let l: { key?: string; name?: string } | null;
  try { l = JSON.parse(localStorage.getItem(LAST_LINK) ?? "null") as { key?: string; name?: string } | null; } catch { return null; }
  if (!l?.key) return null;
  const key = l.key;
  const name = l.name ?? key;
  if (key.startsWith("android:")) return androidCandidate(key, name);
  if (key.startsWith("ble:")) {
    if (bleReopenAvailable()) return { key, name, open: () => reopenBleMidi(key) };
    // desktop app (no getDevices()): the shell picks the device it remembers; the key is known once open
    const desk = desktopShell()?.bluetooth;
    if (desk?.remembered) return { name: desk.remembered, open: () => desktopReopenBle(() => requestBleMidi()) };
    return { unavailable: true, name };
  }
  if (key.startsWith("webmidi:")) return { key, name, open: async () => {
    const c = (await listWebMidi()).find((x) => x.key === key);
    if (!c) throw new Error(`${name} is not available`);
    return c.open();
  } };
  return null;
}

/** Hand the piano over cleanly (state/handoff.ts): page closed / hidden a while / shell stopped → let it go.
 *  The desktop app keeps the link while minimized. */
function installLifecycle(): void {
  installHandoff(conn, { doc: document, win: window, onShell: onShellLifecycle, keepWhenHidden: desktopShell() !== null });
}

// Development shortcuts: ?sim connects to the firmware simulator, ?tab=<id|index> opens a tab.
const qs = new URLSearchParams(location.search);
const tabArg = (): TabId => {
  const q = qs.get("tab");
  // the Piano tab is in the Sound tab since 2026-11-09; the Recorder tab is the tab that was 「USB 메모리」 (id "files")
  const v = q === "piano" ? "sound" : q === "recorder" ? "files" : q ?? current;
  return (TAB_IDS as readonly string[]).includes(v) ? (v as TabId) : (TAB_IDS[Number(v)] ?? current);
};
if (qs.has("sim")) void openWebSocket(`ws://${location.host}/midi`).then((l) => conn.attach(l)).then(() => show(tabArg()));
else if (qs.has("tab")) show(tabArg());

// What the panel shows, for a phone / PC (ops 0x35 / 0x36):
// in the panel shell the screen reporter (panelui/panelscreen.js, prepended to this script by the bundle tool)
// answers and this page adds its tab, connection and backlight; elsewhere window.__panelReport / __panelSnapshot
// read the panel through the piano (the 「패널 화면 보기」 card on the Information tab does the same).
interface ScreenReporter { install(k: unknown): unknown; provide(fn: () => Record<string, unknown>): void }
const screenReporter = (globalThis as unknown as { CA79Screen?: ScreenReporter }).CA79Screen;
if (kwmShell && screenReporter) {
  screenReporter.install(kwm());
  screenReporter.provide(() => ({
    pg: "app", ver: APP_VERSION, tab: current, con: conn.phase, lang: ctx.settings.lang,
    bl: ctx.display?.dimmed ? 0 : ctx.settings.brightness * 10, dim: !!ctx.display?.dimmed, off: ctx.settings.autoOffMin,
    // the Settings tab's probe outputs (「이 주소 열기」 / 「Android 탐색」), so a remote driver reads them even off-screen
    ...(() => {
      const o = document.getElementById("usb-open-result")?.textContent, x = document.getElementById("peek-out")?.textContent;
      return { ...(o ? { uo: o.slice(0, 300) } : {}), ...(x ? { px: x.slice(0, 2400) } : {}) };
    })(),
  }));
} else if (!kwmShell) installPanelScreenGlobals(() => session);

// boot guard in the original-shell bundle (ng/index.html): reaching here means the bundle ran
(globalThis as unknown as { __ca79ngBooted?: boolean }).__ca79ngBooted = true;
