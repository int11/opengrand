// Music tab: Lesson / Concert Magic / Piano Music (the instrument's own song libraries).
// The recorder and the USB memory player are the Recorder tab (ui/recorder.ts, owner 2026-11-22) - one place each.
// Layout (owner 2026-11-23: a media player's bar): a Music card with the three mode buttons, the player bar
// (screens/songctl.ts playerBar: what is loaded, the position, play / pause, previous / next song, stop, and behind
// its buttons the part and the playback options), then the card of the mode shown (song picker and what only that
// mode has - count-in and record practice, the Concert Magic settings).
// The bytes are the stock ones: MusicMode 31 52 1D, SongSelect 10 52 00, Music 32 52 08. A mode button sends its
// MusicMode; when the piano is in another mode (the Recorder tab's, the panel's) Play / Count-in / Record practice or a
// song choice sends the shown mode first.
// The bar's position is the piano's MusicProgress (32 52 03, bar form) and - only if the piano says one in these modes
// - its MaxTime (10 52 18); the line is never a slider here (no jump is known for these modes; rewind / fast forward
// are in the options).
import { t } from "../../i18n.ts";
import { findParam, progress } from "../../protocol/codecs.ts";
import type { ParamStore } from "../../state/store.ts";
import type { AppContext } from "../context.ts";
import { h, section } from "../dom.ts";
import { usbPlayback } from "../usbplay.ts";
import { ctxFor, whileShown } from "./common.ts";
import { MUSIC_CMD, MUSIC_MODE, playerBar } from "./songctl.ts";

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

/** How many songs a mode's picker lists (numType 0, 0-based). */
const PICKER_SONGS = 10;

export function musicScreen(store: ParamStore, ctx?: AppContext): HTMLElement {
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

  // --- what the piano is doing (the Music report 32 52 08 while the piano is in one of this tab's modes), where the
  //     song is (MusicProgress, bar form) and - if the piano says it - how long it is (MaxTime) ---
  let state: number | null = null;
  let bar = 0;
  let maxBars = 0;
  // song pickers: the mode's songs (numType 0, 0-based), section 9.3 - one per mode, the bar's ⏮ ⏭ step the shown one
  const pickers: Record<number, HTMLSelectElement> = {};
  const chooseSong = (n: number) => {
    bar = 0;
    maxBars = 0;
    ensureMode();
    void store.set(songP, 0x7f, songSelectData(n));
    renderNow();
  };
  const songNumber = (mode: number, label: string) => {
    const list = h("select", {}, ...Array.from({ length: PICKER_SONGS }, (_, i) => h("option", { value: String(i) }, t("Song {n}", { n: i + 1 })))) as HTMLSelectElement;
    list.addEventListener("change", () => chooseSong(Number(list.value)));
    pickers[mode] = list;
    return h("label", { class: "row" }, h("span", {}, label), list);
  };
  const step = (d: -1 | 1) => {
    const list = pickers[view];
    const n = Number(list.value) + d;
    if (n < 0 || n >= PICKER_SONGS) return;
    list.value = String(n);
    chooseSong(n);
  };
  const player = playerBar(store, "music-player", {
    play: () => cmd(MUSIC_CMD.Play), pause: () => cmd(MUSIC_CMD.Pause), stop: () => cmd(MUSIC_CMD.Stop),
    prev: () => step(-1), next: () => step(1),
  });
  const renderNow = () => {
    const st = here(pianoMode()) ? state : null;
    const n = Number(pickers[view]?.value ?? 0);
    const moving = st === 0 || st === 1;
    const total = maxBars || null;
    player.set({
      state: st,
      title: t(modes.find(([, v]) => v === view)![0]),
      sub: t("Song {n}", { n: n + 1 }),
      unit: "bar",
      pos: st !== null && st >= 3 ? bar || null : moving && bar ? (total ? Math.min(bar, total) : bar) : total ? 1 : null,
      total: st !== null && st >= 3 ? null : total,
      seekable: false,
      main: st === 1 || st === 4 || st === 6 ? "pause" : "play",
      prev: n > 0, next: n < PICKER_SONGS - 1,
    });
  };
  let attached = false;
  const timer = setInterval(() => {
    // stop polling once the screen has been replaced
    if (player.el.isConnected) attached = true;
    else if (attached) return clearInterval(timer);
    renderNow();
  }, 250);

  const modeBar = h("div", { class: "segmented modes" });
  const sections: Record<number, HTMLElement> = {};
  const showMode = () => {
    modeBar.querySelectorAll("button").forEach((b) => b.classList.toggle("on", Number(b.getAttribute("data-mode")) === view));
    for (const [m, sec] of Object.entries(sections)) sec.hidden = Number(m) !== view;
    renderNow();
  };
  for (const [label, v] of modes) {
    modeBar.append(h("button", { "data-mode": v, onclick: () => { view = v; bar = 0; maxBars = 0; sendMode(v); showMode(); } }, t(label)));
  }

  sections[MUSIC_MODE.Lesson] = section(
    t("Lesson"),
    songNumber(MUSIC_MODE.Lesson, t("Lesson song")),
    h("p", { class: "muted" }, t("Practise one hand: choose only the part the piano should play.")),
    h("div", { class: "tp-row" },
      h("button", { "data-act": "count-in", onclick: () => cmd(MUSIC_CMD.CountIn) }, t("Count-in")),
      h("button", { "data-act": "rec-practice", class: "rec", onclick: () => cmd(MUSIC_CMD.Rec) }, `● ${t("Record practice")}`)),
  );
  sections[MUSIC_MODE.ConcertMagic] = section(
    t("Concert Magic"),
    songNumber(MUSIC_MODE.ConcertMagic, t("Concert Magic song")),
    h("p", { class: "muted" }, t("Tap any keys in rhythm; the piano plays the melody and accompaniment.")),
    ...concertMagicControls(store),
  );
  sections[MUSIC_MODE.PianoMusic] = section(t("Piano Music"), songNumber(MUSIC_MODE.PianoMusic, t("Piece")));

  const el = h("div", { class: "screen music" }, h("section", { class: "card", id: "music-modes" }, h("h3", {}, t("Music")), modeBar), player.el, ...Object.values(sections));

  // --- the piano's word: the parameter mirror, the Music state report, the position ---
  whileShown(el, store, (k) => {
    if (k === "MusicMode@127") {
      const m = store.get(modeState, 0x7f)?.[0];
      if (here(m)) view = m;
      showMode();
    } else if (k === "IntSongPart@127") player.syncPart();
  });
  let seen = false;
  const offMsg = store.onMessage((m) => {
    if (el.isConnected) seen = true;
    else if (seen) return void offMsg(); // the screen was replaced
    if (m.v1 !== 0x52 || (m.fn !== 0x10 && m.fn !== 0x32)) return;
    if (m.v2 === 0x08 && m.data[0] !== undefined && m.data[0] <= 6) {
      state = m.data[0];
      if (state === 2) bar = 0;
      renderNow();
    } else if (m.v2 === 0x03 && m.data.length >= 4 && here(pianoMode())) {
      const at = progress.decode(m.data);
      if ("bar" in at) bar = at.bar;
    } else if (m.v2 === 0x18 && m.fn === 0x10 && m.data.length >= 3 && m.data[0] === 0 && here(pianoMode())) {
      maxBars = (m.data[1] << 7) | m.data[2];
    }
  });
  showMode();
  return el;
}
