// Node-only: runs the firmware simulator as a child process and exposes its
// PANEL port as a MidiLink (used by the e2e test and the dev server).
import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { existsSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { createInterface } from "node:readline";

import { fromHex, toHex } from "../src/protocol/sysex.ts";
import { BaseLink } from "../src/transport/link.ts";

const HERE = dirname(fileURLToPath(import.meta.url));
export const SIM_BIN = resolve(HERE, "../../firmware/build/ca79ng_sim");

export class SimProcessLink extends BaseLink {
  readonly kind = "loopback" as const;
  readonly name = "OpenGrand simulator";
  readonly proc: ChildProcessWithoutNullStreams;
  readonly port: string;
  readonly lines: string[] = [];
  private readyResolve!: () => void;
  readonly ready: Promise<void>;
  private otherListeners: ((line: string) => void)[] = [];

  constructor(opts: { port?: string; realtime?: boolean; args?: string[] } = {}) {
    super();
    if (!existsSync(SIM_BIN)) throw new Error(`simulator not built: ${SIM_BIN} (run make -C firmware sim)`);
    this.port = opts.port ?? "panel";
    this.ready = new Promise((r) => (this.readyResolve = r));
    const args = [...(opts.realtime === false ? [] : ["--realtime"]), ...(opts.args ?? [])];
    this.proc = spawn(SIM_BIN, args, { stdio: ["pipe", "pipe", "pipe"] });
    createInterface({ input: this.proc.stdout }).on("line", (line) => {
      this.lines.push(line);
      if (line.startsWith("ready")) this.readyResolve();
      const m = /^tx (\w+) (.*)$/.exec(line);
      if (m && m[1] === this.port) this.emit(fromHex(m[2]));
      else for (const l of this.otherListeners) l(line);
    });
    this.proc.on("exit", () => this.closed());
    // a page timer may still send after close() quit the simulator (seen under load in tests/navpanel.dom.test.ts:
    // "write EPIPE" after the test ended): the link is gone, the late frame has nowhere to go
    this.proc.stdin.on("error", (e: NodeJS.ErrnoException) => {
      if (e.code !== "EPIPE") throw e;
    });
  }

  /** Lines that are not panel-port MIDI (status, other ports, errors). */
  onLine(l: (line: string) => void): void {
    this.otherListeners.push(l);
  }

  command(line: string): void {
    this.proc.stdin.write(line + "\n");
  }

  send(msg: Uint8Array): void {
    this.command(`midi ${this.port} ${toHex(msg)}`);
  }

  close(): void {
    if (!this.proc.killed) {
      this.command("quit");
      setTimeout(() => this.proc.kill(), 200).unref();
    }
  }
}
