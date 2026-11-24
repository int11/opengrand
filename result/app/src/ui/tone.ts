// Settings tab, Speaker / Headphone: tone control with what each type brings along (manual p.65-66, stock
// settings menu): the Brilliance level only while the type is Brilliance, the User EQ editor only while it is
// User, and Low Volume Balance (the stock menu writes it as EqualizeMasterVolume `51 31 7F`, Off / Type 1-5).
// User EQ bands share one address per kind (UserEqGain / UserEqFreq) - the store keeps them per band.
import { findParam } from "../protocol/codecs.ts";
import { EQ_BANDS, EQ_FREQ_BANDS, EQ_FREQS, EQ_GAIN_MAX, EQ_GAIN_MIN, eqFreq, eqGain, formatHz, nearestFreq } from "../features/usereq.ts";
import type { ParamStore } from "../state/store.ts";
import { t } from "../i18n.ts";
import { control } from "./controls.ts";
import { h } from "./dom.ts";
import { whileShown } from "./screens/common.ts";

const SYS = 0x7f;
/** ToneControl raw values (protocol table) */
const TONE_BRILLIANCE = 1;
const TONE_USER = 64;

const signed = (v: number, unit: string) => `${v > 0 ? "+" : ""}${v} ${unit}`;

/** The rows that follow Tone control: Brilliance level, User EQ, Low Volume Balance. */
export function toneRows(store: ParamStore): HTMLElement[] {
  const tone = findParam("ToneControl")!;
  const gain = findParam("UserEqGain")!;
  const freq = findParam("UserEqFreq")!;

  const brilliance = control(store, "Brilliance", { label: t("Brilliance level"), format: (v) => signed(v, "dB") });

  const gainRows = EQ_BANDS.map((name, band) => {
    const input = h("input", { type: "range", min: EQ_GAIN_MIN, max: EQ_GAIN_MAX, step: 1 }) as HTMLInputElement;
    const out = h("output", {}, "–");
    const row = h("label", { class: "row slider", "data-band": String(band) }, h("span", {}, t(`${name} band`)), input, out);
    const sync = () => {
      const d = store.getBand(gain, SYS, band);
      row.classList.toggle("unknown", !d);
      if (!d) return void (out.textContent = "–");
      const v = eqGain.decode(d);
      input.value = String(v);
      out.textContent = signed(v, "dB");
    };
    const send = () => {
      const data = eqGain.encode(band, Number(input.value));
      const now = store.getBand(gain, SYS, band);
      if (!now || now[1] !== data[1]) void store.set(gain, SYS, data);
    };
    input.addEventListener("input", () => (out.textContent = signed(Number(input.value), "dB")));
    input.addEventListener("change", send);
    return { row, sync };
  });
  const freqRows = EQ_FREQ_BANDS.map((band) => {
    const sel = h("select", {}, ...EQ_FREQS.map((f, i) => h("option", { value: String(i) }, formatHz(f)))) as HTMLSelectElement;
    const row = h("label", { class: "row", "data-band": String(band) }, h("span", {}, t(`${EQ_BANDS[band]} frequency`)), sel);
    const sync = () => {
      const d = store.getBand(freq, SYS, band);
      row.classList.toggle("unknown", !d);
      if (d) sel.value = String(nearestFreq(eqFreq.decode(d)));
      else sel.selectedIndex = -1;
    };
    sel.addEventListener("change", () => void store.set(freq, SYS, eqFreq.encode(band, EQ_FREQS[Number(sel.value)])));
    return { row, sync };
  });
  const eq = h("div", { class: "user-eq", id: "user-eq" }, h("p", { class: "muted row-note" }, t("User tone control: level of four bands, centre of the two middle ones.")),
    ...gainRows.map((r) => r.row), ...freqRows.map((r) => r.row));

  let asked = false;
  const sync = () => {
    const d = store.get(tone, SYS);
    brilliance.hidden = d?.[0] !== TONE_BRILLIANCE;
    eq.hidden = d?.[0] !== TONE_USER;
    for (const r of [...gainRows, ...freqRows]) r.sync();
    // the dump brings every band; a band still missing when the editor shows is asked for once
    // (whether one get returns all bands is not known - each kind is asked once)
    if (!eq.hidden && !asked && store.connected && (gainRows.some((_, b) => !store.getBand(gain, SYS, b)) || EQ_FREQ_BANDS.some((b) => !store.getBand(freq, SYS, b)))) {
      asked = true;
      void store.reread(gain, SYS).then(() => store.reread(freq, SYS));
    }
  };
  whileShown(eq, store, sync);
  sync();
  return [brilliance, eq, control(store, "EqualizeMasterVolume", { label: t("Low volume balance") })];
}
