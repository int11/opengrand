// 「패널 화면 보기」 on the Information screen (phone / PC only): what the built-in panel shows, read through the
// piano's relay (src/protocol/panelscreen.ts, REMOTE_PROTOCOL.md §10.7 ops 0x35 / 0x36). For test drivers the same is on window:
//   await __panelReport()    → the report object (or { error })
//   await __panelSnapshot()  → a data: URL of the picture (or null; the reason in __panelLast.error)
//   __panelLast              → the last result of either (bytes, timings, retries)
import { t } from "../i18n.ts";
import { estimateScreenTransfer, PanelScreenClient, type PanelReport, reportLines, type ScreenProgress } from "../protocol/panelscreen.ts";
import type { PuiPort } from "../protocol/panelui.ts";
import { h } from "./dom.ts";

export interface PanelScreenGlobals {
  __panelReport?: (maxBytes?: number) => Promise<PanelReport | { error: string }>;
  __panelSnapshot?: (o?: { width?: number; kb?: number }) => Promise<string | null>;
  __panelLast?: Record<string, unknown>;
  __panelAct?: (id: string, value?: string) => Promise<string>;
  /** the same by CSS selector (panelscreen.js v3) */
  __panelActSel?: (selector: string, value?: string) => Promise<string>;
}

/** window.__panelReport / __panelSnapshot over whatever session is current. */
export function installPanelScreenGlobals(session: () => PuiPort | null, w: PanelScreenGlobals = globalThis as unknown as PanelScreenGlobals): void {
  const act = async (id: string, value: string | undefined, bySel: boolean) => {
    const s = session();
    if (!s) return "not connected";
    const rc = await new PanelScreenClient(s).act(id, value, 2500, bySel);
    return rc === null ? "no answer" : rc === 0 ? "ok" : rc === 4 ? "no such element" : `rc ${rc}`;
  };
  w.__panelAct = (id: string, value?: string) => act(id, value, false);
  w.__panelActSel = (selector: string, value?: string) => act(selector, value, true);
  w.__panelReport = async (maxBytes?: number) => {
    const s = session();
    if (!s) return { error: "not connected" };
    const r = await new PanelScreenClient(s).report(maxBytes ? { maxBytes } : {});
    w.__panelLast = { kind: "report", ok: r.ok, error: r.error, ms: r.ms, bytes: r.bytes?.length, retries: r.retries, report: r.report };
    return r.report ?? { error: r.error ?? "?" };
  };
  w.__panelSnapshot = async (o = {}) => {
    const s = session();
    if (!s) {
      w.__panelLast = { kind: "snapshot", ok: false, error: "not connected" };
      return null;
    }
    const r = await new PanelScreenClient(s).snapshot(o);
    w.__panelLast = { kind: "snapshot", ok: r.ok, error: r.error, ms: r.ms, readMs: r.readMs, renderMs: r.info?.ms, bytes: r.bytes?.length, width: r.info?.width, height: r.info?.height, retries: r.retries, chunk: r.chunk, dataUrl: r.dataUrl };
    return r.dataUrl ?? null;
  };
}

export function panelScreenBox(session: () => PuiPort | null): HTMLElement {
  const msg = h("p", { class: "muted", id: "ps-msg" });
  const pre = h("pre", { id: "ps-report", hidden: true, style: "white-space:pre-wrap;word-break:break-word;font-size:12px;max-height:50vh;overflow:auto;margin:8px 0" });
  const img = h("img", { id: "ps-image", hidden: true, alt: t("Panel screen"), style: "display:block;max-width:100%;border:1px solid #474647;margin:8px 0;image-rendering:auto" }) as HTMLImageElement;
  const save = h("a", { id: "ps-save", hidden: true, download: "panel.jpg" }, t("Save image")) as HTMLAnchorElement;
  const busy = (on: boolean) => {
    (readBtn as HTMLButtonElement).disabled = on;
    (shotBtn as HTMLButtonElement).disabled = on;
  };
  const client = () => {
    const s = session();
    if (!s) msg.textContent = t("Connect to the piano first.");
    return s ? new PanelScreenClient(s) : null;
  };
  const onProgress = (p: ScreenProgress) => {
    msg.textContent = p.phase === "capture" ? t("The panel is preparing it…")
      : p.phase === "read" ? t("Receiving… {a}/{b} bytes", { a: p.got, b: p.total }) + (p.retries ? ` (${t("retries {n}", { n: p.retries })})` : "")
      : p.phase === "error" ? t("Failed: {e}", { e: p.message ?? "" })
      : "";
  };
  const readBtn = h("button", {
    id: "ps-read",
    onclick: async () => {
      const c = client();
      if (!c) return;
      busy(true);
      const r = await c.report({ onProgress });
      busy(false);
      if (!r.report) return void (msg.textContent = t("Failed: {e}", { e: r.error ?? "" }));
      pre.textContent = reportLines(r.report).join("\n");
      pre.hidden = false;
      msg.textContent = t("{n} bytes in {s} s.", { n: r.bytes?.length ?? 0, s: (r.ms / 1000).toFixed(1) });
    },
  }, t("Read screen info"));
  const shotBtn = h("button", {
    id: "ps-shot", class: "primary",
    onclick: async () => {
      const c = client();
      if (!c) return;
      busy(true);
      const r = await c.snapshot({ onProgress: (p) => {
        onProgress(p);
        if (p.phase === "read" && p.total) msg.textContent += ` · ~${Math.ceil(estimateScreenTransfer(p.total).seconds)} s`;
      } });
      busy(false);
      if (!r.dataUrl || !r.info) return void (msg.textContent = t("Failed: {e}", { e: r.error ?? "" }));
      img.src = r.dataUrl;
      img.hidden = false;
      save.href = r.dataUrl;
      save.download = `panel-${new Date().toISOString().replace(/[:.]/g, "-").slice(0, 19)}.${r.info.fmt === 3 ? "png" : "jpg"}`;
      save.hidden = false;
      msg.textContent = t("{w}×{h}, {n} bytes: drawn in {r} ms, received in {s} s.", { w: r.info.width, h: r.info.height, n: r.bytes?.length ?? 0, r: r.info.ms, s: (r.readMs / 1000).toFixed(1) });
    },
  }, t("Capture screen"));
  return h("div", { id: "panel-screen" },
    h("p", { class: "muted" }, t("What the built-in panel shows now, read through the piano: the page, its text and settings, or a picture the panel draws of itself (about 10–20 s).")),
    h("div", { class: "inline acts" }, shotBtn, readBtn),
    msg, img, save, pre,
  );
}
