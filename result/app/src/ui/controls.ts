// Generic parameter controls generated from the protocol definitions:
// enum -> segmented/select, boolean -> switch, range ("min") -> slider in UI
// units, everything else -> raw slider bounded by the descriptor's data max.
import { enumLabel, enumOptions, findParam, rangeDecode, rangeEncode } from "../protocol/codecs.ts";
import type { ParamDef } from "../protocol/params.gen.ts";
import { keyOf, type ParamStore } from "../state/store.ts";
import { t, tl } from "../i18n.ts";
import { h } from "./dom.ts";
import { onDispose } from "./scope.ts";

export interface ControlOptions {
  label?: string;
  part?: number;
  /** custom UI range for raw sliders, e.g. signed offsets */
  min?: number;
  max?: number;
  toRaw?: (v: number) => number[];
  fromRaw?: (d: number[]) => number;
  format?: (v: number) => string;
  /** after the user picked a value in this control and the piano took it (never on a value the piano reports) */
  onPick?: (data: number[]) => void;
}

const refreshers = new Map<string, Set<() => void>>();

/** Wire the store once: every change re-renders the controls bound to that key. */
export function bindStore(store: ParamStore): void {
  store.subscribe((key) => refreshers.get(key)?.forEach((f) => f()));
}

/** controls bound to the store right now (tests: replaced screens must not leave theirs behind) */
export function boundControls(): number {
  let n = 0;
  refreshers.forEach((set) => (n += set.size));
  return n;
}

/** Bind `f` to the key for the life of the screen being built (ui/scope.ts: main.ts disposes it when the tab is
 * replaced). A control built outside a scope (a screen built directly, as tests do) stays bound. */
function onKey(key: string, f: () => void): void {
  let set = refreshers.get(key);
  if (!set) refreshers.set(key, (set = new Set()));
  set.add(f);
  onDispose(() => {
    const now = refreshers.get(key);
    if (!now) return;
    now.delete(f);
    if (!now.size) refreshers.delete(key);
  });
}

/** labels of a two-value setting that read as off / on (the last `/` part) - only those are drawn as a switch */
const ONOFF = new Set(["Off", "On", "Stop", "Run", "Detach", "Attach"]);

function defaultPart(p: ParamDef): number {
  return p.scope === "global" ? 0x7f : p.scope === "system" ? 0x0f : 0x00;
}

export function control(store: ParamStore, name: string, opts: ControlOptions = {}): HTMLElement {
  const p = findParam(name);
  if (!p) return h("div", { class: "row muted" }, t("{name}: not supported", { name }));
  const part = opts.part ?? defaultPart(p);
  const key = keyOf(p, part);
  const label = opts.label ?? tl(humanize(p.name));
  const cur = () => store.get(p, part);
  // not in the dump (or a dump cut short): read it back with a single get once connected
  store.want(p, part);

  if ((p.kind === "enum" || p.kind === "boolean") && p.values?.length && !opts.toRaw) {
    const options = enumOptions(p);
    // a switch only for a real on/off pair (labels ONOFF, last byte 0 = off / 1 = on) - field 2026-11-13 C17: the old
    // rule drew every two-value setting as a switch with `options[1]` = on: Speaker volume (Normal 1 before Low 0) sent
    // Normal on unchecking, Headphone volume (Normal 1 / High 2) always looked on - any other two-value setting is a select
    const same = (a: number[], b: readonly number[]) => a.length === b.length && a.every((x, k) => x === b[k]);
    const last = (o: { data: number[] }) => o.data[o.data.length - 1];
    const raw01 = options.length === 2 && options[0].data.length === options[1].data.length &&
      options[0].data.slice(0, -1).every((x, k) => x === options[1].data[k]) && new Set(options.map(last)).size === 2 &&
      options.every((o) => last(o) === 0 || last(o) === 1) &&   // e.g. Program Change Off 5 0 / On 5 1
      // ... and named as an on/off pair: Speaker volume Normal / Low, Metronome / Rhythm, WAV / MP3 are choices (a select)
      options.every((o) => ONOFF.has(o.label.split("/").pop() ?? ""));
    if (raw01) {
      const on = options.find((o) => last(o) !== 0) ?? options[1];
      const off = options.find((o) => last(o) === 0) ?? options[0];
      const input = h("input", { type: "checkbox", role: "switch" }) as HTMLInputElement;
      const row = h("label", { class: "row" }, h("span", {}, label), input);
      const sync = () => {
        const d = cur();
        // value not known yet: indeterminate, not a default that looks like the piano's value
        input.indeterminate = !d;
        row.classList.toggle("unknown", !d);
        input.checked = d ? same(on.data, d.slice(0, on.data.length)) : false;
      };
      input.addEventListener("change", () => void store.set(p, part, [...(input.checked ? on : off).data]));
      onKey(key, sync);
      sync();
      return row;
    }
    const sel = h("select", {}) as HTMLSelectElement;
    // blank (no option selected) until the piano's value is known or when it is none of the options
    options.forEach((o, i) => sel.append(h("option", { value: String(i) }, tl(o.label))));
    const row = h("label", { class: "row" }, h("span", {}, label), sel);
    const sync = () => {
      const d = cur();
      const i = d ? options.findIndex((o) => o.data.every((b, k) => d[k] === b)) : -1;
      if (i >= 0) sel.value = String(i);
      else sel.selectedIndex = -1;
      row.classList.toggle("unknown", !d);
    };
    sel.addEventListener("change", () => {
      if (sel.selectedIndex < 0) return;
      const data = options[Number(sel.value)].data;
      void store.set(p, part, data).then((ok) => ok && opts.onPick?.(data));
    });
    onKey(key, sync);
    sync();
    return row;
  }

  // sliders
  const r = p.range;
  const min = opts.min ?? r?.uiMin ?? 0;
  const max = opts.max ?? r?.uiMax ?? p.dataMax[0] ?? 127;
  const toRaw = opts.toRaw ?? ((v: number) => [r ? rangeEncode(p, v) : v]);
  const fromRaw = opts.fromRaw ?? ((d: number[]) => (r ? rangeDecode(p, d[0] ?? 0) : (d[0] ?? 0)));
  const fmt = opts.format ?? ((v: number) => String(v));
  const input = h("input", { type: "range", min, max, step: 1 }) as HTMLInputElement;
  const out = h("output", {}, "–");
  const row = h("label", { class: "row slider" }, h("span", {}, label), input, out);
  const sync = () => {
    const d = cur();
    row.classList.toggle("unknown", !d);
    if (!d) {
      out.textContent = "–";
      return;
    }
    const v = fromRaw(d);
    input.value = String(v);
    out.textContent = fmt(v);
  };
  // Send the slider value unless the store already holds it. "input" (drags) is coalesced to
  // one message per animation frame; "change" (end of a drag, keyboard, a value set by a test
  // driver) sends at once, so an edit never depends on the frame callback alone.
  let pending = 0;
  const send = () => {
    cancelAnimationFrame(pending);
    const data = toRaw(Number(input.value));
    const now = cur();
    if (now && now.length === data.length && now.every((b, i) => b === data[i])) return;
    void store.set(p, part, data);
  };
  input.addEventListener("input", () => {
    out.textContent = fmt(Number(input.value));
    cancelAnimationFrame(pending);
    pending = requestAnimationFrame(send);
  });
  input.addEventListener("change", () => {
    out.textContent = fmt(Number(input.value));
    send();
  });
  onKey(key, sync);
  sync();
  return row;
}

export function valueLabel(store: ParamStore, name: string, part?: number): string {
  const p = findParam(name);
  if (!p) return "";
  const d = store.get(p, part ?? defaultPart(p));
  if (!d) return "–";
  return p.values ? tl(enumLabel(p, d[0], d[1])) : String(p.range ? rangeDecode(p, d[0]) : d[0]);
}

export function humanize(name: string): string {
  return name
    .replace(/([a-z])([A-Z])/g, "$1 $2")
    .replace(/([A-Z]+)([A-Z][a-z])/g, "$1 $2")
    .replace(/^SHS/, "Spatial Headphone Sound")
    .replace(/^DCA /, "");
}
