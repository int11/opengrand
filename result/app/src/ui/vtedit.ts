// User touch curve editor (§9.1) and per-note VT editors (§9.2), shown inside
// the Technician screen. Each editor reads the piano's table when opened and
// sends every edit immediately; Save stores it on the piano, Discard reverts.
import { NOTE_NAMES } from "../chords.ts";
import { FIRST_NOTE, LAST_NOTE, PER_NOTE, type PerNoteKind, PerNoteModel, TouchCurveModel } from "../features/vtedit.ts";
import { t } from "../i18n.ts";
import type { ParamStore } from "../state/store.ts";
import { h, section } from "./dom.ts";

const SVG = "http://www.w3.org/2000/svg";
function svg(tag: string, attrs: Record<string, string | number>): SVGElement {
  const el = document.createElementNS(SVG, tag);
  for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, String(v));
  return el;
}

const noteName = (n: number) => `${NOTE_NAMES[n % 12]}${Math.floor(n / 12) - 1}`;

/** "User edit" section: buttons that open one editor at a time below them. */
export function userEditSection(store: ParamStore): HTMLElement {
  const host = h("div", { class: "editor-host" });
  let dispose: (() => void) | null = null;
  const open = (make: () => { el: HTMLElement; dispose: () => void }) => {
    dispose?.();
    const e = make();
    dispose = e.dispose;
    host.replaceChildren(e.el);
  };
  const buttons = h(
    "div",
    { class: "transport" },
    h("button", { "data-editor": "touch", onclick: () => open(() => touchCurveEditor(store)) }, t("User touch curve")),
    ...PER_NOTE.map((k) => h("button", { "data-editor": k.id, onclick: () => open(() => perNoteEditor(store, k)) }, t(`User ${k.label.toLowerCase()}`))),
  );
  return section(t("User edit"), buttons, host);
}

export function touchCurveEditor(store: ParamStore): { el: HTMLElement; dispose: () => void } {
  const m = new TouchCurveModel(store);
  const plot = svg("svg", { viewBox: "-2 -2 132 132", class: "curve", role: "img" }) as SVGSVGElement;
  plot.append(svg("rect", { x: 0, y: 0, width: 128, height: 128, class: "curve-bg" }), svg("line", { x1: 0, y1: 128, x2: 128, y2: 0, class: "curve-diag" }));
  const line = svg("polyline", { class: "curve-line", points: "" });
  const dots = svg("g", {});
  plot.append(line, dots);
  const status = h("p", { class: "muted", id: "curve-status" }, t("Loading…"));
  const nameIn = h("input", { type: "text", maxlength: 16, placeholder: t("Curve name") }) as HTMLInputElement;
  const xIn = h("input", { type: "number", min: 1, max: 126, value: 64 }) as HTMLInputElement;
  const yIn = h("input", { type: "number", min: 1, max: 127, value: 64 }) as HTMLInputElement;
  const pts = h("div", { class: "chips" });
  let selected: number | null = null;

  const draw = () => {
    line.setAttribute("points", m.vel.map((v, i) => `${i},${127 - v}`).join(" "));
    dots.replaceChildren(...m.points().map((x) => svg("circle", { cx: x, cy: 127 - m.vel[x], r: 2.5, class: x === selected ? "curve-pt on" : "curve-pt" })));
    pts.replaceChildren(...m.points().map((x) => h("button", { class: x === selected ? "chip on" : "chip", onclick: () => { selected = x; xIn.value = String(x); yIn.value = String(m.vel[x]); draw(); } }, `${x}→${m.vel[x]}`)));
    if (document.activeElement !== nameIn) nameIn.value = m.name;
  };
  const off = m.onChange(draw);

  // tap on the plot: move the selected point there, or add a new one
  plot.addEventListener("click", (e: MouseEvent) => {
    const r = plot.getBoundingClientRect();
    if (!r.width) return;
    const x = Math.round(((e.clientX - r.left) / r.width) * 132 - 2);
    const y = Math.round(127 - (((e.clientY - r.top) / r.height) * 132 - 2));
    void m.setPoint(x, y, selected ?? Math.max(1, Math.min(126, x)));
    selected = Math.max(1, Math.min(126, x));
  });

  let learning = false;
  const learnBtn = h("button", { "data-act": "learn" }, t("Learn from playing")) as HTMLButtonElement;
  learnBtn.addEventListener("click", () => {
    learning = !learning;
    learnBtn.textContent = learning ? t("Finish learning") : t("Learn from playing");
    status.textContent = learning ? t("Play softly and strongly (at least 4 notes), then finish.") : t("The piano made a curve from your playing.");
    void m.learn(learning);
  });

  void m.load().then((ok) => {
    status.textContent = ok ? t("Touch curve set to User. Edits apply immediately.") : t("The piano did not send the curve.");
    draw();
  });
  draw();

  const el = h(
    "div",
    { class: "editor", id: "touch-editor" },
    h("h4", {}, t("User touch curve")),
    plot,
    h("p", { class: "muted" }, t("Horizontal: key velocity · vertical: sound velocity")),
    pts,
    h("label", { class: "row" }, h("span", {}, t("Input velocity")), xIn),
    h("label", { class: "row" }, h("span", {}, t("Output velocity")), yIn),
    h("div", { class: "transport" },
      h("button", { "data-act": "set", onclick: () => { const x = Number(xIn.value); void m.setPoint(x, Number(yIn.value), selected ?? x); selected = x; } }, t("Set point")),
      h("button", { "data-act": "remove", onclick: () => { if (selected !== null) void m.removePoint(selected); selected = null; } }, t("Remove point")),
      h("button", { "data-act": "clear", onclick: () => { selected = null; void m.clear(); } }, t("Clear all")),
      learnBtn,
    ),
    h("label", { class: "row" }, h("span", {}, t("Name")), nameIn),
    h("div", { class: "transport" },
      h("button", { "data-act": "name", onclick: () => void m.setName(nameIn.value) }, t("Rename")),
      h("button", { class: "primary", "data-act": "save", onclick: () => void m.save().then(() => (status.textContent = t("Saved on the piano."))) }, t("Save")),
      h("button", { "data-act": "discard", onclick: () => void m.exit().then(() => (status.textContent = t("Unsaved changes discarded."))) }, t("Discard")),
    ),
    status,
  );
  return { el, dispose: () => { off(); m.dispose(); } };
}

export function perNoteEditor(store: ParamStore, kind: PerNoteKind): { el: HTMLElement; dispose: () => void } {
  const m = new PerNoteModel(store, kind);
  const status = h("p", { class: "muted" }, t("Loading…"));
  const temperament = kind.size === 12;
  let oct = 5; // C4..B4
  const octSel = h("select", {}, ...Array.from({ length: 9 }, (_, i) => i + 1).map((o) => h("option", { value: String(o) }, `${noteName(Math.max(FIRST_NOTE, o * 12))} – ${noteName(Math.min(LAST_NOTE, o * 12 + 11))}`))) as HTMLSelectElement;
  octSel.value = String(oct);
  const overview = h("div", { class: "pn-overview" });
  const rows = h("div", { class: "pn-rows" });
  const fmt = (v: number) => `${v > 0 ? "+" : ""}${v}${kind.unit ? " " + t(kind.unit) : ""}`;

  const indices = () => (temperament ? Array.from({ length: 12 }, (_, i) => i) : Array.from({ length: 12 }, (_, i) => oct * 12 + i).filter((n) => n >= FIRST_NOTE && n <= LAST_NOTE));
  const build = () => {
    rows.replaceChildren(
      ...indices().map((i) => {
        const input = h("input", { type: "range", min: -kind.limit, max: kind.limit, step: 1, value: m.values[i], "data-index": i }) as HTMLInputElement;
        const out = h("output", {}, fmt(m.values[i]));
        input.addEventListener("input", () => (out.textContent = fmt(Number(input.value))));
        input.addEventListener("change", () => void m.set(i, Number(input.value)));
        return h("label", { class: "row slider" }, h("span", {}, temperament ? NOTE_NAMES[i] : noteName(i)), input, out);
      }),
    );
  };
  const drawOverview = () => {
    if (temperament) return;
    overview.replaceChildren(
      ...Array.from({ length: LAST_NOTE - FIRST_NOTE + 1 }, (_, k) => {
        const v = m.values[FIRST_NOTE + k];
        const bar = h("span", { class: Math.floor((FIRST_NOTE + k) / 12) === oct ? "pn-bar on" : "pn-bar" });
        bar.style.height = `${50 + (v / kind.limit) * 50}%`;
        return bar;
      }),
    );
  };
  const refresh = () => {
    rows.querySelectorAll<HTMLInputElement>("input").forEach((inp) => {
      const i = Number(inp.getAttribute("data-index"));
      inp.value = String(m.values[i]);
      (inp.nextElementSibling as HTMLElement).textContent = fmt(m.values[i]);
    });
    drawOverview();
  };
  const off = m.onChange(refresh);
  octSel.addEventListener("change", () => {
    oct = Number(octSel.value);
    build();
    drawOverview();
  });
  void m.load().then((ok) => {
    status.textContent = ok ? t("{name} set to User. Edits apply immediately.", { name: t(kind.label) }) : t("The piano did not send the table.");
    refresh();
  });
  build();
  drawOverview();

  const el = h(
    "div",
    { class: "editor", id: `pn-${kind.id}` },
    h("h4", {}, t(`User ${kind.label.toLowerCase()}`)),
    temperament ? null : overview,
    temperament ? null : h("label", { class: "row" }, h("span", {}, t("Octave")), octSel),
    rows,
    h("div", { class: "transport" },
      h("button", { "data-act": "reset", onclick: () => void m.reset() }, t("Reset all to 0")),
      h("button", { class: "primary", "data-act": "save", onclick: () => void m.save().then(() => (status.textContent = t("Saved on the piano."))) }, t("Save")),
      h("button", { "data-act": "discard", onclick: () => void m.exit().then(() => (status.textContent = t("Unsaved changes discarded."))) }, t("Discard")),
    ),
    status,
  );
  return { el, dispose: () => { off(); m.dispose(); } };
}
