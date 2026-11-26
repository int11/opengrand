// "Panel UI over Bluetooth" box on the Information screen: sends a signed panel UI package
// (.c79u, `npm run panelui:pack`) through the piano's relay to the built-in panel
// (src/protocol/panelui.ts, REMOTE_PROTOCOL.md section 10.7). Not shown on the panel itself.
import {
  keyIdHex, PanelUiClient, type PanelUiJob, type PanelUiStatus, parsePackage, PUI_RUNNING, PuiControl, type UploadProgress,
} from "../protocol/panelui.ts";
import { xferLock } from "../protocol/xferlock.ts";
import { t } from "../i18n.ts";
import type { AppContext } from "./context.ts";
import { h } from "./dom.ts";

export function panelUiBox(ctx: AppContext): HTMLElement {
  const state = h("output", { id: "pui-state" }, "–");
  const msg = h("p", { class: "muted", id: "pui-msg" });
  const bar = h("progress", { id: "pui-progress", max: 100, value: 0, hidden: true }) as HTMLProgressElement;
  const activate = h("input", { id: "pui-activate", type: "checkbox", checked: true }) as HTMLInputElement;
  let file: Uint8Array | null = null;

  const show = (st: PanelUiStatus | null) => {
    if (!st) {
      state.textContent = t("No reply — the piano needs the panel relay and the panel the OpenGrand UI bundle.");
      return;
    }
    const slot = (s: PanelUiStatus["slots"][number]) => (s.state === "empty" ? "–" : `${s.version} (${s.state})`);
    state.textContent = t("{running} · A {a} · B {b} · bundled {c}", {
      running: PUI_RUNNING[st.running] ?? String(st.running), a: slot(st.slots[0]), b: slot(st.slots[1]), c: st.bundledVersion,
    });
  };
  const client = () => {
    const s = ctx.session();
    if (!s) msg.textContent = t("Connect to the piano first.");
    return s ? new PanelUiClient(s) : null;
  };
  const refresh = async () => {
    const c = client();
    if (c) show(await c.status());
  };

  const pick = h("input", { id: "pui-file", type: "file", accept: ".c79u" }) as HTMLInputElement;
  pick.addEventListener("change", async () => {
    const f = pick.files?.[0];
    file = f ? new Uint8Array(await f.arrayBuffer()) : null;
    const p = file ? parsePackage(file) : null;
    msg.textContent = !file ? "" : p
      ? t("{n} bytes, version {v}, seq {s}, key {k}.", { n: file.length, v: p.header.version, s: p.header.seq, k: keyIdHex(p.header.keyId) })
      : t("Not a panel UI package (.c79u).");
  });
  /** What an upload's progress says; only from the progress, so a rebuilt screen shows the same (no screen state). */
  const uploadText = (p: UploadProgress): string =>
    (p.phase === "upload" || p.phase === "begin") && p.restarts ? t("Resending (a frame was damaged on the way)… {a}/{b} bytes", { a: p.sent, b: p.total })
    : p.phase === "upload" || p.phase === "begin" ? t("Sending… {a}/{b} bytes", { a: p.sent, b: p.total })
    : p.phase === "commit" ? t("The panel is checking and storing it…")
    : p.phase === "done" && p.rc === 14 ? t("Failed: {e}", { e: p.message ?? "" })
    : p.phase === "done" && p.activated === true ? t("Stored in slot {slot}; the panel is restarting its UI.", { slot: p.message ?? "" })
    : p.phase === "done" && p.activated === false ? t("Stored in slot {slot}, but the panel did not restart its UI ({e}).", {
      slot: p.message ?? "", e: p.activateRc === null || p.activateRc === undefined ? t("No reply") : String(p.activateRc),
    })
    : p.phase === "done" ? t("Stored in slot {slot}.", { slot: p.message ?? "" })
    : p.phase === "error" ? t("Failed: {e}", { e: p.message ?? "" })
    : t("Working…");
  const setBusy = (on: boolean) => {
    (send as HTMLButtonElement).disabled = on;
    (rollback as HTMLButtonElement).disabled = on;
  };
  /** Follow a panel UI job on this link - this screen's, or one a replaced screen started (a tab switch, language
   *  change or reconnect rebuilds the screen): its progress with Send / Roll back off, then its result. */
  const follow = (job: PanelUiJob, own: boolean) => {
    setBusy(true);
    if (job.what === "upload") bar.hidden = false;
    job.watch((j) => {
      if (j.what === "upload") {
        const p = (j.finished ? j.result : j.progress) as UploadProgress | undefined;
        if (p) bar.value = Math.round((p.sent / Math.max(1, p.total)) * 100);
        msg.textContent = p ? uploadText(p) : t("Working…");
        if (j.finished && p?.phase !== "done") bar.hidden = true;
      } else if (!j.finished) msg.textContent = t("Working…");
      else if (typeof j.result === "string") msg.textContent = j.result;
      if (j.finished) {
        setBusy(false);
        if (!own && msg.isConnected) void refresh();    // only a screen still shown asks the panel
      }
    });
  };
  const send = h("button", {
    id: "pui-send", class: "primary",
    onclick: async () => {
      const s = ctx.session();
      const c = client();
      if (!s || !c) return;
      if (!file) {
        msg.textContent = t("Pick a package first.");
        return;
      }
      const up = c.upload(file, { activate: activate.checked });
      // the lock is taken before upload() returns: this is its job (or the one that refused it)
      const job = PanelUiClient.running(s);
      if (job) follow(job, true);
      await up;
      await refresh();
    },
  }, t("Send to panel"));
  const rollback = h("button", {
    id: "pui-rollback",
    onclick: async () => {
      const s = ctx.session();
      if (!s) {
        msg.textContent = t("Connect to the piano first.");
        return;
      }
      // one panel UI job per link (src/protocol/xferlock.ts): not during an upload (this or a rebuilt screen's), not
      // twice for a double tap - a second Control 2 would mark the fallback version bad too
      const lease = xferLock(s, "panelui", "rollback");
      if (!lease) {
        msg.textContent = t("Busy");
        return;
      }
      follow(lease.job, true);
      try {
        await rollbackNow(new PanelUiClient(s));
      } finally {
        lease.release(msg.textContent);                // a rebuilt screen shows the same result
      }
    },
  }, t("Roll back"));
  async function rollbackNow(c: PanelUiClient): Promise<void> {
    const rc = await c.control(PuiControl.Rollback);
    if (rc !== 0) {
      msg.textContent = t("Failed: {e}", { e: rc === null ? t("No reply") : String(rc) });
      await refresh();
      return;
    }
    // the version on the screen was the one rolled back (e.g. a dark or stuck panel):
    // restart the panel UI now (Control 1 = reload into the newest usable version), any loader since 0.2.0
    const st = await c.status();
    const run = st && (st.running === 2 || st.running === 3) ? st.slots[st.running - 2] : null;
    if (run?.state === "bad" && (await c.control(PuiControl.Activate)) === 0) {
      show(st);
      msg.textContent = t("Rolled back: the panel is restarting into the previous UI.");
      return;
    }
    show(st);
    msg.textContent = t("Rolled back: the panel starts the previous UI next time.");
  }

  // a job a replaced screen started is still running on this link: follow it instead of offering a second one
  const cur = ctx.session();
  const running = cur ? PanelUiClient.running(cur) : null;
  if (running) follow(running, false);

  // laid out like 「펌웨어 업데이트」 on the Information screen: state under its label,
  // one labelled file picker, the primary send with its switch, then roll back; the card title is the caller's
  return h("div", { id: "panel-ui" },
    h("p", { class: "muted" }, t("Sends a signed UI package (.c79u) through the piano to the built-in panel. The panel must show the OpenGrand UI or its chooser; it keeps the previous version and falls back to it if the new one fails.")),
    h("div", { class: "row stack" }, h("span", { class: "lbl" }, t("Panel")), h("div", { class: "val" }, state, h("button", { id: "pui-check", onclick: () => void refresh() }, t("Check")))),
    h("label", { class: "row stack pick" }, h("span", { class: "lbl" }, t("UI package file (.c79u)")), pick),
    h("div", { class: "inline acts" }, send, h("label", { class: "inline" }, activate, " ", t("Start it right away"))),
    h("div", { class: "inline acts" }, rollback),
    bar,
    msg,
  );
}
