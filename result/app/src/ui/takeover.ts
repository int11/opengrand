// Connection banner: "another device took control" + take it back, "connection failed" + try again,
// "connection lost" + reconnect. It draws only the connection state (src/state/connection.ts); the button sends the matching event to the manager.
//
// Displacement: the stock firmware binds one controller; a CONNECT
// from another port (the panel, a second phone) sends us 7E 01 00 and from then on our requests get no
// answer. The session drops what it had queued; taking control again is CONNECT + full re-sync.
import { t } from "../i18n.ts";
import { type ConnState, type ConnectionManager, connView } from "../state/connection.ts";
import { h } from "./dom.ts";

export interface TakeoverBanner {
  el: HTMLElement;
  render(s: ConnState): void;
}

export function takeoverBanner(mgr: ConnectionManager): TakeoverBanner {
  const msg = h("span", {});
  const btn = h("button", { class: "primary" }) as HTMLButtonElement;
  const el = h("div", { class: "banner", id: "displaced", role: "alert", hidden: true }, msg, btn);
  btn.onclick = () => {
    if (btn.dataset.action === "take-control") mgr.retake();
    else if (btn.dataset.action === "retry") mgr.retry();
  };
  const render = (s: ConnState) => {
    const b = connView(s).banner;
    el.hidden = !b;
    if (!b) return;
    msg.textContent = t(...b.msg);
    btn.hidden = !b.button;
    if (b.button) {
      btn.textContent = t(b.button.label);
      btn.dataset.action = b.button.action === "retake" ? "take-control" : "retry";
      btn.disabled = b.button.disabled;
    }
  };
  mgr.onChange(render);
  render(mgr.state);
  return { el, render };
}
