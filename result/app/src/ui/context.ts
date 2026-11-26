// Shared state handed to screens that need more than the parameter store.
import { setLang } from "../i18n.ts";
import { FileAccessClient } from "../protocol/fileaccess.ts";
import type { PianoSession } from "../protocol/session.ts";
import { type AppSettings, SoundLibrary, loadSettings, saveSettings } from "../state/settings.ts";
import type { ParamStore } from "../state/store.ts";
import type { DisplayController } from "./display.ts";

export interface AppContext {
  store: ParamStore;
  settings: AppSettings;
  library: SoundLibrary;
  display: DisplayController | null;
  /** current piano session (null when not connected) */
  session(): PianoSession | null;
  /** non-Kawai MIDI from the piano: CustomEvent<Uint8Array> "midi" */
  midi: EventTarget;
  /** running on the instrument's built-in panel */
  panel: boolean;
  /** persist + apply a settings change; language changes re-render the UI */
  updateSettings(patch: Partial<AppSettings>): void;
  /** re-render the current screen (after a language change) */
  rerender(): void;
  /** fn 0x24 FileAccess client for the current session (null when not connected) */
  files(): FileAccessClient | null;
  /** CONNECT + full re-sync of `s` through the connection manager (main.ts: ConnectionManager.rejoin); absent in
   *  screen tests, where the session does it alone */
  retake?(s: PianoSession): Promise<unknown>;
  /** last USB file list (index = file-list id, §9.5) */
  usbFiles: string[];
  /** the app entry's pieces the Settings tab's 「앱 · 화면」 card works (main.ts); absent in screen tests */
  app?: AppHooks;
}

export interface AppHooks {
  /** inside the original panel shell (kwmcore): the Kawai UI and the UI chooser are reachable */
  shell: boolean;
  midiLogOpen(): boolean;
  setMidiLog(open: boolean): void;
  /** leave this page (the Kawai UI ../ui/html/index.html, the chooser ../index.html?stay=1) */
  go(href: string): void;
  /** panel shell: open Android Settings (the original hidden "OS Settings" button) */
  androidSettings?(): void;
  /** panel shell: ask Android to VIEW a file:// path on the [USB for UPDATE] stick (kwmcore openURL → ACTION_VIEW, no
   *  MIME type); resolves to "ok" when some app took it, else the error text. `rel` "" = the stick's root folder. */
  openUsb?(rel: string): Promise<string>;
  /** panel shell: VIEW any URI through openURL; `{usb}` in it becomes the stick's volume id (e.g. 1E71-DA63) */
  openUri?(uri: string): Promise<string>;
  /** panel shell: read a text file anywhere the Kawai app may read (kwmcore loadJson ../ escape); null = missing /
   *  unreadable / timeout. `dumpToUsb` copies that text to <stick>/ca79ng_dump/<name>.txt (saveJson), "" or an error. */
  readFile?(path: string): Promise<string | null>;
  dumpToUsb?(name: string, text: string): Promise<string>;
}

const clients = new WeakMap<PianoSession, FileAccessClient>();

export function createContext(store: ParamStore, over: Partial<AppContext> = {}): AppContext {
  const ctx: AppContext = {
    store,
    settings: loadSettings(),
    library: new SoundLibrary(),
    display: null,
    session: () => null,
    midi: new EventTarget(),
    panel: false,
    rerender: () => {},
    usbFiles: [],
    files() {
      const s = ctx.session();
      if (!s) return null;
      let c = clients.get(s);
      // a fn 0x24 timeout = the stock has likely unbound us: CONNECT + re-sync once (not while displaced)
      // through the manager when there is one: a re-take it does not know of could fail and leave the session
      // "connecting" under a header that says Connected, or run alongside its own
      const recover = () => (s.displaced || s.state === "closed" ? undefined : ctx.retake ? ctx.retake(s) : s.takeControl());
      if (!c) clients.set(s, (c = new FileAccessClient(s, undefined, { recover })));
      return c;
    },
    updateSettings(patch) {
      const langChanged = patch.lang !== undefined && patch.lang !== ctx.settings.lang;
      ctx.settings = { ...ctx.settings, ...patch };
      saveSettings(ctx.settings);
      setLang(ctx.settings.lang);
      ctx.display?.apply(ctx.settings);
      if (langChanged) ctx.rerender();
    },
    ...over,
  };
  return ctx;
}
