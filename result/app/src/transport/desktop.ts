// Windows desktop app (result/desktop, Electron): reconnect to the last Bluetooth piano on start, and the Bluetooth
// search of the connection dialog (desktopBleScan below: the shell hands the page the device list of a running
// requestDevice() instead of opening its own chooser window).
// Electron has no navigator.bluetooth.getDevices(), so a remembered piano cannot be reopened the Chrome
// way. Instead the shell's main process remembers the device the user last chose; the app asks it to
// reconnect, the shell runs the hook below with a user gesture (requestDevice needs one) and answers the
// chooser itself with exactly that device.

/** What the desktop preload (result/desktop/src/preload.cjs) exposes as window.ca79Desktop. */
export interface DesktopBridge {
  shell: "electron";
  bluetooth?: {
    /** name of the device the shell remembers (picked in its chooser), or null */
    remembered: string | null;
    /** "started" = the hook ran; "none" = nothing remembered; anything else = not started */
    reconnect(): Promise<string>;
    /** shells with the in-app device list: every list update of a running requestDevice(); returns "stop listening" */
    onDevices?(cb: (list: BtFound[]) => void): () => void;
    /** answer the running requestDevice(): a deviceId from the list, "" = cancel */
    choose?(id: string): void;
  };
}

/** One Bluetooth device the shell found (deviceId = the shell's id, not the page's BluetoothDevice.id). */
export interface BtFound {
  deviceId: string;
  deviceName: string;
}

/** A running Bluetooth search whose devices the page lists itself. */
export interface DesktopBleScan<T> {
  /** what `request` gave once a device was chosen; rejects when the search ended without one (cancel included) */
  found: Promise<T>;
  pick(deviceId: string): void;
  /** give the search up; resolves once the request is over (at once when the shell has not seen a device yet) */
  cancel(): Promise<void>;
}

/** cancel(): how long to wait for the cancelled request to end before going on anyway */
export const CANCEL_WAIT_MS = 1500;

/** The page global the shell calls (executeJavaScript with a user gesture). */
export const DESKTOP_RECONNECT_HOOK = "__ca79ngDesktopReconnect";

type Global = Record<string, unknown>;

export function desktopShell(g: Global = globalThis as unknown as Global): DesktopBridge | null {
  const d = g.ca79Desktop as DesktopBridge | undefined;
  return d && d.shell === "electron" ? d : null;
}

/**
 * Reconnect through the shell: `request` (requestBleMidi) runs inside the shell's gesture call and the
 * shell picks its remembered device. Rejects when the shell remembers nothing / cannot start, or when
 * `request` fails (the piano did not show up: NotFoundError).
 */
export function desktopReopenBle<T>(request: () => Promise<T>, g: Global = globalThis as unknown as Global): Promise<T> {
  const bt = desktopShell(g)?.bluetooth;
  if (!bt) return Promise.reject(new Error("not the desktop app"));
  return new Promise<T>((resolve, reject) => {
    const drop = () => {
      if (g[DESKTOP_RECONNECT_HOOK] === hook) delete g[DESKTOP_RECONNECT_HOOK];
    };
    const hook = () => {
      drop();
      request().then(resolve, reject);
    };
    g[DESKTOP_RECONNECT_HOOK] = hook;
    bt.reconnect().then(
      (r) => {
        if (r === "started") return;
        drop();
        reject(new Error(`desktop reconnect not started: ${r}`));
      },
      (e: unknown) => {
        drop();
        reject(e instanceof Error ? e : new Error(String(e)));
      },
    );
  });
}

/** Does this shell list Bluetooth devices in the page (preload onDevices / choose)? Older shells open their own window. */
export function desktopBleScanAvailable(g: Global = globalThis as unknown as Global): boolean {
  const bt = desktopShell(g)?.bluetooth;
  return typeof bt?.onDevices === "function" && typeof bt.choose === "function";
}

/**
 * Bluetooth search for the connection dialog: `request` (navigator.bluetooth.requestDevice - call this inside the
 * user's click) runs now, `onDevices` gets the shell's list on every update, pick(id) / cancel() answer the request.
 * Null when the shell has no in-app list.
 */
export function desktopBleScan<T>(request: () => Promise<T>, onDevices: (list: BtFound[]) => void, g: Global = globalThis as unknown as Global): DesktopBleScan<T> | null {
  const bt = desktopShell(g)?.bluetooth;
  if (!bt || typeof bt.onDevices !== "function" || typeof bt.choose !== "function") return null;
  const choose = bt.choose.bind(bt);
  let over = false; // settled or given up
  let listed = false; // the shell has sent a list = it holds the request and can answer it
  let off: (() => void) | null = bt.onDevices((list) => {
    if (over || !Array.isArray(list)) return;
    listed = true;
    onDevices(list.filter((d) => d && typeof d.deviceId === "string").map((d) => ({ deviceId: d.deviceId, deviceName: typeof d.deviceName === "string" ? d.deviceName : "" })));
  });
  const stop = () => {
    over = true;
    off?.();
    off = null;
  };
  let found: Promise<T>;
  try {
    found = request();
  } catch (e) {
    found = Promise.reject(e instanceof Error ? e : new Error(String(e)));
  }
  const settled = found.then(stop, stop);
  return {
    found,
    pick: (id) => {
      if (!over) choose(id);
    },
    cancel: () => {
      if (over) return settled;
      const wait = listed;
      choose("");
      stop();
      // no list yet: the shell has nothing to answer until a device shows (it cancels that request then) - do not wait
      return wait ? Promise.race([settled, new Promise<void>((r) => setTimeout(r, CANCEL_WAIT_MS))]) : Promise.resolve();
    },
  };
}
