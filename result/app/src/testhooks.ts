// Helpers for automated device tests that drive the app from outside (e.g. the desktop app over CDP).
// `window.__H` — no UI, no effect unless a test calls it. Logs come from window.__midiLog (ring, "t RX|TX hex").
type W = { __midiLog: unknown[]; __sendHex: (hex: string) => void; __ca79?: { session: () => { takeControl(): Promise<unknown> } | null; files: () => unknown } };

export function installTestHelpers(): void {
  const w = globalThis as unknown as W & { __H?: unknown };
  const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
  const H = {
    sleep,
    lastT: () => parseFloat(String(w.__midiLog[w.__midiLog.length - 1] ?? "0")),
    send: (hex: string) => w.__sendHex(hex),
    hex: (n: number) => (n & 0x7f).toString(16).padStart(2, "0"),
    /** RX line matching re after log time t (ring buffer: filter by timestamp, not index) */
    waitRx: async (re: RegExp, t: number, ms = 3000): Promise<string | null> => {
      const t0 = performance.now();
      while (performance.now() - t0 < ms) {
        const l = w.__midiLog.slice(-40).map(String).find((x) => parseFloat(x) > t && x.includes(" RX ") && re.test(x));
        if (l) return l.split(" RX ")[1];
        await sleep(5);
      }
      return null;
    },
    /** send and wait for the reply in one go */
    ask: async (hex: string, re: RegExp, ms = 3000) => { const t = H.lastT(); w.__sendHex(hex); return H.waitRx(re, t, ms); },
    ng: (body: string) => "F0 7D 43 41 37 39 " + body + " F7",
    conn: () => (document.querySelector("#connect")?.textContent || "").includes("연결됨"),
    waitConn: async (ms = 30000) => { const t0 = performance.now(); while (performance.now() - t0 < ms) { if (H.conn()) return true; await sleep(100); } return false; },
    beep: async (note: string) => { w.__sendHex("90 " + note + " 60"); await sleep(150); w.__sendHex("80 " + note + " 00"); },
    /** song table (NG 1E 01 -> 5E 01): one 8-byte hex string per slot */
    table: async () => {
      const r = await H.ask(H.ng("1E 01"), /5e 01/i, 3000);
      if (!r) return null;
      const b = r.split(" ").slice(8, -1), o: string[] = [];
      for (let i = 0; i + 8 <= b.length; i += 8) o.push(b.slice(i, i + 8).join(" "));
      return o;
    },
    fa: () => w.__ca79!.files(),
    /** InternalSongDelete 32 52 21 7F [04 hi lo 7F] -> Result */
    delSong: (song: number) => H.ask("F0 40 7F 32 08 02 52 21 7F 04 " + H.hex(song >> 7) + " " + H.hex(song) + " 7F F7", /32 04 1e 51 7e/i, 8000),
    err: (e: unknown) => String((e as Error)?.message || e),
    /** file channel wedged after a fn24 timeout: CONNECT + re-sync, no BLE reconnect */
    recover: () => w.__ca79!.session()!.takeControl(),
  };
  w.__H = H;
}
