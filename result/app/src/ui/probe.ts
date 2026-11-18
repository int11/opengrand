// Text reports for the phase2c probe ops (REMOTE_PROTOCOL.md section 10.1), shown as-is in the
// Information screen so the owner can read or screenshot the raw values on the phone.
import { CORE_TEST_CHECKS, type NgClient, type NgFeatureName, NG_PAYLOAD_MAX } from "../protocol/ngext.ts";

export type ProbeKind = "port" | "link" | "apo" | "btlog" | "arm" | "canary" | "core";

/** Identify feature name each probe op needs (an image answers only the ops its bits list). */
export const PROBE_FEATURE: Record<ProbeKind, NgFeatureName> = {
  port: "port", link: "echo", apo: "apo", btlog: "btlog", arm: "canary", canary: "canary", core: "core",
};

const hex = (v: number, w = 8) => `0x${v.toString(16).toUpperCase().padStart(w, "0")}`;
const APO_SETTING = ["off", "15 min", "60 min", "120 min"];
const CANARY_STATE = ["not armed", "fill pending", "armed"];

/** Runs one probe op and formats the reply; null = no reply (stock firmware or no probe). */
export async function probeReport(ng: NgClient, kind: ProbeKind, now: () => number = () => performance.now()): Promise<string | null> {
  switch (kind) {
    case "port": {
      const p = await ng.port();
      return p && `port ${p.port} · binding ${p.binding} · port3 forwarding ${p.forwarding} · length ${p.length}`;
    }
    case "link": {
      const rows = await ng.linkTest([64, 256, 1024, NG_PAYLOAD_MAX], now);
      if (!rows[0]?.replied) return null;
      return rows
        .map((r) => `${r.mode.padEnd(8)} ${String(r.bytes).padStart(4)} B  ${!r.replied ? "no reply" : r.ok ? "CRC ok " : "FAILED "} ${r.ms.toFixed(0).padStart(5)} ms` +
          (r.ok && r.ms > 0 ? `  ~${Math.round((r.bytes * 1000) / r.ms)} B/s` : ""))
        .join("\n");
    }
    case "apo": {
      const a = await ng.apo();
      return a && [
        `APO counter ${a.counter} (setting ${APO_SETTING[a.apoSetting] ?? a.apoSetting}) · state ${a.state}`,
        `A2DP link ${a.a2dp} · BT power ${a.btPower} · READY ${a.btReady} · dialect ${a.dialect}`,
        `BLE power ${a.blePower} · switch hold ${a.switchHold} · watchdog ${a.watchdog} · special mode ${a.specialMode}`,
        `raw ${Array.from(a.raw, (b) => b.toString(16).padStart(2, "0")).join(" ")}`,
      ].join("\n");
    }
    case "btlog": {
      const l = await ng.btLog();
      return l && [`wr ${l.wr} · rd ${l.rd} · line ${l.lineLen} · snapshot ${l.seq}`, ...l.lines].join("\n");
    }
    case "arm": {
      const s = await ng.canaryArm();
      return s === null ? null : `canary ${CANARY_STATE[s] ?? s} — check again after using the piano`;
    }
    case "canary": {
      const c = await ng.canary();
      return c && [
        `canary ${CANARY_STATE[c.state] ?? c.state} · ${hex(c.lo)}–${hex(c.hi)} · ${c.ticks} ticks since fill`,
        `changed words ${c.changed}` + (c.changed ? ` · first ${hex(c.first)} · last ${hex(c.last)}` : ""),
      ].join("\n");
    }
    case "core": {
      const c = await ng.coreTest();
      if (!c) return null;
      const failed = CORE_TEST_CHECKS.filter((_, i) => i < c.checks && !(c.passMask & (1 << i)));
      return [
        `core self-test ${c.passed}/${c.checks} passed` + (failed.length ? ` · FAILED: ${failed.join(", ")}` : ""),
        `store CRC ${hex(c.storeCrcDefaults)} -> ${hex(c.storeCrcAfter)} · kproto out ${c.txFrames} frames ${c.txBytes} B CRC ${hex(c.txCrc)}`,
        `core ticks ${c.ticks} · ${c.ms} ms` + (c.ticks > 1 ? ` (~${(c.ms / (c.ticks - 1)).toFixed(1)} ms/tick)` : "") +
          ` · time ${c.timeUs} us · R1 guards ${c.guardsOk ? "ok" : "BROKEN"}`,
      ].join("\n");
    }
  }
}
