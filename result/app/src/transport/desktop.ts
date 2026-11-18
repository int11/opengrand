// Windows desktop app (result/desktop, Electron): reconnect to the last Bluetooth piano on start.
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
  };
}

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
