// Information > Custom firmware: "BT module diagnostics" (slot payload ota_bt, Identify feat2 0x01).
// Reads the BT audio module's configuration / links / version (read-only iWRAP SET, LIST, INFO) and the
// BLE-MIDI port 2 robustness counters. REMOTE_PROTOCOL.md section 10.6.
import { BT_RC, BtPort, type BtStatus } from "../protocol/btport.ts";
import { NgClient } from "../protocol/ngext.ts";
import { t } from "../i18n.ts";
import type { AppContext } from "./context.ts";
import { h } from "./dom.ts";

function statusText(st: BtStatus): string {
  return t("Module {m} · port 2 latch cleared {u}× · active sensing disarmed {a}× · port 2 flag {f}", {
    m: st.moduleOn ? t("on") : t("off"),
    u: String(st.p2Unlatches),
    a: String(st.asDisarms),
    f: `0x${st.p2Flag.toString(16)}`,
  });
}

/**
 * 1D 01 got no answer: ask the heartbeat (1D 0F, answered on task 19 even while the engine loop is stuck) twice,
 * a second apart - the same pass count means the piano's engine loop has stopped (field 2026-11-01, record
 * section 1).
 */
export async function noReplyText(port: BtPort, waitMs = 1000, sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms))): Promise<string> {
  const a = await port.heartbeat();
  if (!a) return t("No reply.");
  await sleep(waitMs);
  const b = await port.heartbeat();
  if (b && b.passes === a.passes) return t("The piano's engine loop has stopped (stage {s}, TX count {c}) — switch the piano off and on.", { s: b.stage, c: b.txCount });
  return t("No reply.");
}

/** Hidden until show(true) (ngCheck: Identify lists "bt"). */
export function btDiagBox(ctx: AppContext): HTMLElement & { show: (on: boolean) => void } {
  const out = h("pre", { id: "bt-diag-out" }, "");
  const info = h("p", { class: "muted", id: "bt-diag-status" });
  const btn = h("button", { id: "bt-diag-run" }, t("BT module diagnostics")) as HTMLButtonElement;
  btn.addEventListener("click", async () => {
    const cur = ctx.session();
    if (!cur) return;
    btn.disabled = true;
    out.textContent = t("Asking the Bluetooth audio module… (a few seconds)");
    try {
      const port = new BtPort(new NgClient(cur));
      const r = await port.query("all");
      if (!r) out.textContent = await noReplyText(port);
      else if (r.rc !== 0) out.textContent = t("Refused: {e}", { e: BT_RC[r.rc] ?? String(r.rc) });
      else out.textContent = r.text || t("The module did not answer.");
      if (r?.status) info.textContent = statusText(r.status);
    } finally {
      btn.disabled = false;
    }
  });
  const box = h("div", { id: "bt-diag", hidden: true },
    h("h4", {}, t("BT module diagnostics")),
    h("p", { class: "muted" }, t("Sends the read-only module queries SET, LIST and INFO and shows the answer. Nothing is changed on the module.")),
    btn, info, out) as unknown as HTMLElement & { show: (on: boolean) => void };
  box.show = (on: boolean) => {
    box.hidden = !on;
  };
  return box;
}
