// Tab icons (24×24 line drawings, drawn for this app — no images or glyphs taken from the original UI).
// Strokes use currentColor (styles.css `nav button .ico svg`); paths with class "f" are filled.
// Plain SVG markup: the panel's Chrome 71 parses it through innerHTML like any other element.
import type { TabId } from "../state/settings.ts";
import { h } from "./dom.ts";

const SVG: Record<TabId, string> = {
  sound: '<path d="M3 12h2.5l2-6 3 12 3-14 3 13 2-5H21"/>',
  music: '<circle cx="12" cy="12" r="9"/><path class="f" d="M10 8.2v7.6l6-3.8z"/>',
  metronome: '<path d="M8.5 3h7l4 18h-15z"/><path d="M5.5 16.5h13M12 16l5-11"/>',
  technician: '<path d="M4 6h8M16 6h4M4 12h3M11 12h9M4 18h11M19 18h1"/><circle cx="14" cy="6" r="2"/><circle cx="9" cy="12" r="2"/><circle cx="17" cy="18" r="2"/>',
  chords: '<ellipse class="f" cx="9.5" cy="18" rx="3" ry="2.2"/><ellipse class="f" cx="9.5" cy="12.5" rx="3" ry="2.2"/><ellipse class="f" cx="9.5" cy="7" rx="3" ry="2.2"/><path d="M12.5 18V2.5"/>',
  files: '<path d="M9 2.5h6v5H9z"/><rect x="6" y="7.5" width="12" height="14" rx="2"/><path d="M10.5 4.5v1M13.5 4.5v1"/>',
  settings: '<circle cx="12" cy="12" r="3.2"/><path d="M12 2.5v3M12 18.5v3M2.5 12h3M18.5 12h3M5.3 5.3l2.1 2.1M16.6 16.6l2.1 2.1M5.3 18.7l2.1-2.1M16.6 7.4l2.1-2.1"/>',
  info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v6"/><circle class="f" cx="12" cy="7.6" r="1.2"/>',
  bluetooth: '<path d="M7 7.5l10 9-5 4.5V3l5 4.5-10 9"/>',
};

/** `<span class="ico">` holding the tab's icon (aria-hidden: the label next to it names the tab). */
export function tabIcon(id: TabId): HTMLElement {
  const el = h("span", { class: "ico", "aria-hidden": "true" });
  el.innerHTML = `<svg viewBox="0 0 24 24" focusable="false">${SVG[id]}</svg>`;
  return el;
}
