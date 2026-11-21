// Music tab: Lesson / Concert Magic / Piano Music (the instrument's own song libraries).
// The recorder and the USB memory player are the Recorder tab (ui/recorder.ts, owner 2026-11-22) - one place each.
// Layout: a Music card with the three mode buttons and one state line (what the piano is doing), the card of the mode
// shown (song picker, labelled transport), and in it the part buttons and the folded 「재생 옵션」 (screens/songctl.ts).
// The bytes are the stock ones: MusicMode 31 52 1D, SongSelect 10 52 00, Music 32 52 08. A mode button sends its
// MusicMode; when the piano is in another mode (the Recorder tab's, the panel's) Play / Count-in / Record practice or a
// song choice sends the shown mode first.
import { t } from "../../i18n.ts";
import { findParam } from "../../protocol/codecs.ts";
import type { ParamStore } from "../../state/store.ts";
import type { AppContext } from "../context.ts";
import { h, section } from "../dom.ts";
import { usbPlayback } from "../usbplay.ts";
import { ctxFor, whileShown } from "./common.ts";
import { MUSIC_CMD, MUSIC_MODE, partSelector, playbackOptions, stateLine, tpBtn } from "./songctl.ts";

export { MUSIC_MODE } from "./songctl.ts";

/** Concert Magic result codes (§9.4, 10 52 0B 7F [code, groups hi, lo]). */
export const CM_RESULT = ["OK", "Slow", "Fast", "Too fast", "Chord", "No song / end"];

/**
 * Concert Magic settings (§9.4 [NG], 10 52 0D 7F [mode, smooth, trigger]) and
 * the result display fed by 10 52 0B notifications.
 */
function concertMagicControls(store: ParamStore): HTMLElement[] {
  let cfg = [1, 0, 0]; // Ensemble, no smoothing, trigger Part 1
  const modeSel = h("select", {}, h("option", { value: "0" }, t("Melody")), h("option", { value: "1" }, t("Ensemble"))) as HTMLSelectElement;
  const smooth = h("input", { type: "checkbox", role: "switch" }) as HTMLInputElement;
  const trig = h("select", {}, h("option", { value: "0" }, t("Part 1 (right hand)")), h("option", { value: "1" }, t("Part 2 (left hand)"))) as HTMLSelectElement;
  const show = () => {
    modeSel.value = String(cfg[0]);
    smooth.checked = cfg[1] !== 0;
    trig.value = String(cfg[2]);
  };
  const send = () => {
    cfg = [Number(modeSel.value), smooth.checked ? 1 : 0, Number(trig.value)];
    void store.send({ v1: 0x52, v2: 0x0d, data: cfg });
  };
  modeSel.addEventListener("change", send);
  smooth.addEventListener("change", send);
  trig.addEventListener("change", send);
  const result = h("div", { class: "cm-result", id: "cm-result" }, "–");
  store.onMessage((m) => {
    if (m.fn !== 0x10 || m.v1 !== 0x52) return;
    if (m.v2 === 0x0d && m.data.length >= 3) {
      cfg = m.data.slice(0, 3);
      show();
    } else if (m.v2 === 0x0b && m.data.length >= 3) {
      const code = m.data[0];
      result.textContent = t("{r} · {n} notes played", { r: t(CM_RESULT[code] ?? String(code)), n: (m.data[1] << 7) | m.data[2] });
      result.setAttribute("data-code", String(code));
    }
  });
  show();
  void store.request(0x52, 0x0d, 1500);
  return [
    h("label", { class: "row" }, h("span", {}, t("Concert Magic mode")), modeSel),
    h("label", { class: "row" }, h("span", {}, t("Smooth tempo")), smooth),
    h("label", { class: "row" }, h("span", {}, t("Trigger part")), trig),
    h("label", { class: "row" }, h("span", {}, t("Last tap")), result),
  ];
}

/**
 * SongSelect is 3 bytes in the original table: [numType, num_hi7, num_lo7]
 * (recWorker builds it by hand). The earlier 1-byte form was NACKed by the
 * firmware's length check. numType 0 = song in the current music mode.
 */
export function songSelectData(n: number, numType = 0): number[] {
  return [numType & 0x7f, (n >> 7) & 0x7f, n & 0x7f];
}

export function musicScreen(store: ParamStore, progressText: () => string, ctx?: AppContext): HTMLElement {
  const c = ctxFor(store, ctx);
  const music = findParam("Music", 0x32)!;
  const songP = findParam("SongSelect")!;
  const modeP = findParam("MusicMode", 0x31)!;
  const modeState = findParam("MusicMode", 0x10)!;
  // the piano's MusicMode as it last said or this app set (ui/usbplay.ts follows 32 52 1D; the store has the sync's)
  const pb = usbPlayback(c);
  const pianoMode = () => pb.mode ?? store.get(modeState, 0x7f)?.[0];
  const modes: [string, number][] = [["Lesson", MUSIC_MODE.Lesson], ["Concert Magic", MUSIC_MODE.ConcertMagic], ["Piano Music", MUSIC_MODE.PianoMusic]];
  const here = (m: number | undefined): m is number => modes.some(([, v]) => v === m);
  // the mode shown: the piano's when it is one of this tab's, else Lesson (nothing is sent until a button is used)
  const m0 = pianoMode();
  let view: number = here(m0) ? m0 : MUSIC_MODE.Lesson;
  const sendMode = (v: number) => {
    pb.mode = v;
    void store.command(modeP, 0x7f, [v]);
  };
  const ensureMode = () => {
    if (pianoMode() !== view) sendMode(view);
  };
  const cmd = (v: number) => {
    if (v !== MUSIC_CMD.Stop && v !== MUSIC_CMD.Pause) ensureMode();
    void store.command(music, 0x7f, [v]);
  };

  // --- what the piano is doing: one line (the Music report 32 52 08 while the piano is in one of this tab's modes) ---
  let state: number | null = null;
  const now = stateLine("music-state");
  const renderNow = () => {
    const st = here(pianoMode()) ? state : null;
    el.setAttribute("data-play", st === null ? "" : String(st)); // lights the transport button of that state (styles.css)
    const p = progressText();
    now.set(st, st !== null && st !== 2 && p && p !== "–" ? p : "");
  };
  let attached = false;
  const timer = setInterval(() => {
    // stop polling once the screen has been replaced
    if (now.el.isConnected) attached = true;
    else if (attached) return clearInterval(timer);
    renderNow();
  }, 250);

  const modeBar = h("div", { class: "segmented modes" });
  const sections: Record<number, HTMLElement> = {};
  const showMode = () => {
    modeBar.querySelectorAll("button").forEach((b) => b.classList.toggle("on", Number(b.getAttribute("data-mode")) === view));
    for (const [m, sec] of Object.entries(sections)) sec.hidden = Number(m) !== view;
    // one part selector and one options fold for every mode: they sit in the card of the mode shown
    if (sections[view] && shared.parentElement !== sections[view]) sections[view].append(shared);
    renderNow();
  };
  for (const [label, v] of modes) {
    modeBar.append(h("button", { "data-mode": v, onclick: () => { view = v; sendMode(v); showMode(); } }, t(label)));
  }

  const transport = () =>
    h("div", { class: "tp three" },
      tpBtn("play", "▶", t("Play"), () => cmd(MUSIC_CMD.Play), "tp-play"),
      tpBtn("pause", "❚❚", t("Pause"), () => cmd(MUSIC_CMD.Pause), "tp-pause"),
      tpBtn("stop", "■", t("Stop"), () => cmd(MUSIC_CMD.Stop)));
  // song picker: the mode's songs (numType 0, 0-based), section 9.3
  const songNumber = (label: string) => {
    const list = h("select", {}, ...Array.from({ length: 10 }, (_, i) => h("option", { value: String(i) }, t("Song {n}", { n: i + 1 })))) as HTMLSelectElement;
    list.addEventListener("change", () => {
      ensureMode();
      void store.set(songP, 0x7f, songSelectData(Number(list.value)));
    });
    return h("label", { class: "row" }, h("span", {}, label), list);
  };

  const parts = partSelector(store);
  const shared = h("div", { class: "music-shared" },
    h("div", { class: "row seg-row" }, h("span", {}, t("Part")), parts.el),
    playbackOptions(store, "music-more"));

  sections[MUSIC_MODE.Lesson] = section(
    t("Lesson"),
    songNumber(t("Lesson song")),
    h("p", { class: "muted" }, t("Practise one hand: choose only the part the piano should play.")),
    transport(),
    h("div", { class: "tp-row" },
      h("button", { "data-act": "count-in", onclick: () => cmd(MUSIC_CMD.CountIn) }, t("Count-in")),
      h("button", { "data-act": "rec-practice", class: "rec", onclick: () => cmd(MUSIC_CMD.Rec) }, `● ${t("Record practice")}`)),
  );
  sections[MUSIC_MODE.ConcertMagic] = section(
    t("Concert Magic"),
    songNumber(t("Concert Magic song")),
    h("p", { class: "muted" }, t("Tap any keys in rhythm; the piano plays the melody and accompaniment.")),
    ...concertMagicControls(store),
    transport(),
  );
  sections[MUSIC_MODE.PianoMusic] = section(t("Piano Music"), songNumber(t("Piece")), transport());

  const el = h("div", { class: "screen music" }, section(t("Music"), modeBar, now.el), ...Object.values(sections));

  // --- the piano's word: the parameter mirror and the Music state report ---
  whileShown(el, store, (k) => {
    if (k === "MusicMode@127") {
      const m = store.get(modeState, 0x7f)?.[0];
      if (here(m)) view = m;
      showMode();
    } else if (k === "IntSongPart@127") parts.sync();
  });
  let seen = false;
  const offMsg = store.onMessage((m) => {
    if (el.isConnected) seen = true;
    else if (seen) return void offMsg(); // the screen was replaced
    if (m.v1 === 0x52 && m.v2 === 0x08 && (m.fn === 0x10 || m.fn === 0x32) && m.data[0] !== undefined && m.data[0] <= 6) {
      state = m.data[0];
      renderNow();
    }
  });
  showMode();
  return el;
}
