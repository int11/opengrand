// The player bar the Recorder tab (ui/recorder.ts) and the Music tab (ui/screens/music.ts) share (owner 2026-11-23:
// 「보통 곡 재생은 이런 바를 쓴다」 - a media player's bar instead of a form):
//   a thin progress line with a position dot (a slider only where the piano can jump - see `seekable`),
//   one row of round buttons: play / pause (one button), previous / next song, stop, the position as a pill
//   ("12 / 24 bars", "0:07 / 3:20"), what is loaded as a pill, and at the right Rec (recorder only), the part and the
//   options button. The options (part, tempo, transpose, repeat, play mode, rewind / fast forward, A-B) are a sheet.
// The bar is updated in place: a notice or the 250 ms tick never replaces a button under a finger.
// The bytes are the stock ones: Music 32 52 08, IntSongPart 10 52 01, ABRepeat 32 52 06.
import { t } from "../../i18n.ts";
import { bpm, findParam, offset64 } from "../../protocol/codecs.ts";
import type { ParamStore } from "../../state/store.ts";
import { control } from "../controls.ts";
import { h } from "../dom.ts";

/** The song player's state as the piano reports it (Music 32 52 08 / OpenGrand op 1E status byte 1): 0 paused,
 *  1 playing, 2 stopped, 3 / 5 record standby, 4 recording, 6 count-in. */
export const STATE_TEXT = ["Paused", "Playing", "Stopped", "Ready to record", "Recording", "Ready to record", "Count-in"];
export const isRecState = (s: number | null): boolean => s !== null && s >= 3 && s <= 6;

/** MusicMode values (fn 0x31 52 1D, original enum). */
export const MUSIC_MODE = { Exit: 0, Recorder: 2, UsbMemory: 3, Lesson: 4, ConcertMagic: 5, PianoMusic: 6 } as const;

/** Music command values (32 52 08). */
export const MUSIC_CMD = { Pause: 0, Play: 1, Stop: 2, Rec: 3, CountIn: 5, Forward: 7, Rewind: 8 } as const;

/** Icons (24×24, drawn for this app; filled with currentColor, `.s` = stroked). Plain SVG markup through innerHTML,
 *  as ui/tabicons.ts: the panel's Chrome 71 has no glyphs for ⏮ ⏭ in its fonts. */
const ICONS = {
  play: '<path d="M8 5.2v13.6L19 12z"/>',
  pause: '<path d="M6.8 5h3.6v14H6.8zM13.6 5h3.6v14h-3.6z"/>',
  stop: '<rect x="6.5" y="6.5" width="11" height="11" rx="1.6"/>',
  prev: '<path d="M6 6h2.4v12H6zM19 6v12l-9-6z"/>',
  next: '<path d="M15.6 6H18v12h-2.4zM5 6v12l9-6z"/>',
  rew: '<path d="M11.6 6.5v11L3.5 12zM20 6.5v11L11.9 12z"/>',
  ff: '<path d="M12.4 6.5v11l8.1-5.5zM4 6.5v11l8.1-5.5z"/>',
  rec: '<circle cx="12" cy="12" r="6.2"/>',
  options: '<path class="s" d="M4 7h9M17 7h3M4 12h3M11 12h9M4 17h11M19 17h1"/><circle class="s" cx="15" cy="7" r="2"/><circle class="s" cx="9" cy="12" r="2"/><circle class="s" cx="17" cy="17" r="2"/>',
  more: '<circle cx="5.5" cy="12" r="1.8"/><circle cx="12" cy="12" r="1.8"/><circle cx="18.5" cy="12" r="1.8"/>',
} as const;
export type IconName = keyof typeof ICONS;

export function icon(name: IconName): HTMLElement {
  const el = h("span", { class: "pl-ico", "aria-hidden": "true", "data-ico": name });
  el.innerHTML = `<svg viewBox="0 0 24 24" focusable="false">${ICONS[name]}</svg>`;
  return el;
}

/** A round button holding one icon; the name is its title and aria-label. */
export function iconButton(name: IconName, label: string, attrs: Record<string, string | EventListener | undefined> = {}): HTMLButtonElement {
  return h("button", { type: "button", title: label, "aria-label": label, ...attrs, class: `pl-btn ${attrs.class ?? ""}`.trim() }, icon(name));
}

/** The three bars a playing row shows (animated by styles.css). */
export function playingMark(): HTMLElement {
  return h("span", { class: "eq", "aria-hidden": "true" }, h("i", {}), h("i", {}), h("i", {}));
}

/** m:ss (h:mm:ss from an hour on). */
export function clock(ms: number): string {
  const s = Math.max(0, Math.floor(ms / 1000));
  const m = Math.floor(s / 60);
  const ss = String(s % 60).padStart(2, "0");
  return m >= 60 ? `${Math.floor(m / 60)}:${String(m % 60).padStart(2, "0")}:${ss}` : `${m}:${ss}`;
}

/** Part 1 / Part 2 / Both as one segmented control (IntSongPart: what plays, and the part a recording goes to). */
export function partSelector(store: ParamStore): { el: HTMLElement; sync(): void } {
  const partP = findParam("IntSongPart")!;
  const el = h("div", { class: "segmented", "data-part-select": "" },
    ...([["Part 1", 0], ["Part 2", 1], ["Both", 0x7f]] as const).map(([label, v]) =>
      h("button", { "data-part": v, onclick: () => void store.set(partP, 0x7f, [v]) }, t(label))));
  const sync = () => {
    const v = store.get(partP, 0x7f)?.[0]; // undefined (not read yet): none lit
    el.querySelectorAll("button").forEach((b) => b.classList.toggle("on", Number(b.getAttribute("data-part")) === v));
  };
  sync();
  return { el, sync };
}

/** What the bar shows. */
export interface PlayerView {
  /** STATE_TEXT index; null = the piano has not said (shown as stopped) */
  state: number | null;
  /** read out instead of the state's name (「USB 재생 중」) */
  stateText?: string;
  /** what is loaded: a song ("Song 2"), a file name; and what it holds (muted) */
  title: string;
  sub: string;
  /** the position's unit: bars (1-based) or milliseconds */
  unit: "bar" | "ms";
  /** null = not known */
  pos: number | null;
  total: number | null;
  /** the progress line is a slider (the piano can jump to a bar): needs pos and total in bars */
  seekable: boolean;
  /** what the main button does now */
  main: "play" | "pause" | "stop";
  prev: boolean;
  next: boolean;
  /** the part means something for what is loaded (a piano song; not a USB file) - default true */
  part?: boolean;
}

export interface PlayerActions {
  play(): void;
  pause(): void;
  stop(): void;
  prev(): void;
  next(): void;
  /** jump to a bar (1-based); without it the line is never a slider */
  seek?(bar: number): void;
  /** the Rec button; without it there is none */
  rec?(): void;
  /** false: the chosen part is never said on the bar (it is in the options sheet only) - default true */
  partPill?: boolean;
}

export interface PlayerBar {
  el: HTMLElement;
  set(v: PlayerView): void;
  /** the part pill and the part buttons follow IntSongPart */
  syncPart(): void;
  /** where a screen puts its own lines (a question, a hint, the piano's refusal) */
  foot: HTMLElement;
  options: HTMLDialogElement;
}

const MAIN_LABEL = { play: "Play", pause: "Pause", stop: "Stop" } as const;

/** The position pill's text. */
export function positionText(v: Pick<PlayerView, "state" | "unit" | "pos" | "total">): string {
  if (v.state === 4) return v.pos ? `${t("Recording")} · ${t("Bar {bar}", { bar: v.pos })}` : t("Recording");
  if (v.state === 3 || v.state === 5) return t("Ready to record");
  if (v.state === 6) return t("Count-in");
  if (v.unit === "ms") {
    if (v.pos === null) return v.total ? clock(v.total) : "–:––";
    return v.total ? `${clock(v.pos)} / ${clock(v.total)}` : clock(v.pos);
  }
  if (v.pos && v.total) return t("{a} / {n} bars", { a: v.pos, n: v.total });
  if (v.pos) return t("Bar {bar}", { bar: v.pos });
  return v.total ? t("{n} bars", { n: v.total }) : "–";
}

/**
 * The player bar. `id` names the card (its data-play = the state, for styles.css) and `${id}-state` the line a screen
 * reader hears; the options sheet is `${id}-options`.
 */
export function playerBar(store: ParamStore, id: string, acts: PlayerActions): PlayerBar {
  const partP = findParam("IntSongPart")!;
  const music = findParam("Music", 0x32)!;
  const cmd = (v: number) => void store.command(music, 0x7f, [v]);
  const ab = (v: number) => void store.command(findParam("ABRepeat", 0x32)!, 0x7f, [v]);

  // ---- the options sheet (built once: its controls stay bound to the store) ----
  const parts = partSelector(store);
  const options = h("dialog", { class: "sheet pl-sheet", id: `${id}-options` }) as HTMLDialogElement;
  const close = () => {
    if (typeof options.close === "function") options.close();
    else options.removeAttribute("open");
  };
  options.append(
    h("h3", {}, t("Playback options")),
    h("div", { class: "pl-opts" },
      h("div", { class: "pl-part-row" },
        h("div", { class: "row seg-row" }, h("span", {}, t("Part")), parts.el),
        // owner 2026-11-23: 「파트 기능이 뭔지 모르겠다」 - say what it is where it is chosen
        h("p", { class: "muted pl-part-hint" }, t("A song has two parts, so each hand can be recorded on its own. Recording and playback use the part chosen here."))),
      control(store, "MusicTempo", { label: t("Song tempo"), part: 0x7f, min: 10, max: 400, toRaw: bpm.encode, fromRaw: bpm.decode, format: (v) => `♩=${v}` }),
      // Song transpose: the original table has PartTransposeValue on the same
      // wire id (53 02) with the System part; the key transpose uses part 0x0F.
      control(store, "TransposeValue", { label: t("Song transpose"), part: 0x7f, min: -12, max: 12, toRaw: offset64.encode, fromRaw: offset64.decode }),
      control(store, "MusicRepeat", { label: t("Repeat"), part: 0x7f }),
      control(store, "PlayMode", { label: t("Play mode"), part: 0x7f }),
      // rewind / fast forward are the piano's own steps (Music 8 / 7): not a jump to a place
      h("div", { class: "row seg-row" }, h("span", {}, t("Seek")),
        h("div", { class: "segmented" },
          h("button", { "data-act": "rew", onclick: () => cmd(MUSIC_CMD.Rewind) }, icon("rew"), t("Rewind")),
          h("button", { "data-act": "ff", onclick: () => cmd(MUSIC_CMD.Forward) }, t("Fast forward"), icon("ff")))),
      h("div", { class: "row seg-row" }, h("span", {}, t("A-B repeat")),
        h("div", { class: "segmented" },
          h("button", { "data-act": "ab-a", onclick: () => ab(1) }, t("Set A")),
          h("button", { "data-act": "ab-b", onclick: () => ab(2) }, t("Set B")),
          h("button", { "data-act": "ab-off", onclick: () => ab(0) }, t("A-B off"))))),
    h("button", { "data-act": "options-close", onclick: close }, t("Close")));
  // a tap on the dimmed page behind the sheet closes it (the dialog itself is the backdrop's target)
  options.addEventListener("click", (e) => {
    if (e.target === options) close();
  });
  const open = () => {
    if (options.open) return;
    if (typeof options.showModal === "function") options.showModal();
    else options.setAttribute("open", "");
  };

  const partRow = options.querySelector(".pl-part-row") as HTMLElement;

  // ---- the bar ----
  const status = h("span", { class: "sr-only", id: `${id}-state`, role: "status" }, t("Stopped"));
  const title = h("b", { class: "pl-title" });
  const sub = h("span", { class: "pl-sub" });
  const time = h("span", { class: "pl-pill pl-time", id: `${id}-pos` }, "–");
  const fill = h("div", { class: "pl-fill" });
  const dot = h("div", { class: "pl-dot" });
  const range = h("input", { type: "range", class: "pl-range", min: 1, max: 1, step: 1, "aria-label": t("Go to bar"), hidden: true }) as HTMLInputElement;
  const seek = h("div", { class: "pl-seek slider" }, h("div", { class: "pl-track" }, fill), dot, range);
  const main = iconButton("play", t("Play"), { class: "pl-main", "data-act": "play" });
  const prev = iconButton("prev", t("Previous song"), { "data-act": "prev", onclick: () => acts.prev() });
  const next = iconButton("next", t("Next song"), { "data-act": "next", onclick: () => acts.next() });
  const stop = iconButton("stop", t("Stop"), { "data-act": "stop", onclick: () => acts.stop() });
  const partVal = h("b", {}, "–");
  const partBtn = h("button", { type: "button", class: "pl-part", "data-act": "part", title: t("Part"), onclick: open }, h("span", {}, t("Part")), partVal);
  const rec = acts.rec
    ? h("button", { type: "button", class: "pl-rec", "data-act": "rec", title: t("Rec"), "aria-label": t("Rec"), onclick: () => acts.rec!() }, icon("rec"), h("span", { class: "pl-rec-txt" }, t("Rec")))
    : null;
  const foot = h("div", { class: "pl-foot" });
  const el = h("div", { class: "card player", id, "data-play": "" },
    status,
    h("div", { class: "pl-head" }, time, h("span", { class: "pl-pill pl-info", id: `${id}-info` }, title, sub)),
    seek,
    h("div", { class: "pl-left" }, main, h("span", { class: "pl-skip" }, prev, next), stop),
    h("div", { class: "pl-right" }, rec, partBtn, iconButton("options", t("Playback options"), { "data-act": "options", onclick: open })),
    foot,
    options);

  let shown: PlayerView | null = null;
  let mainAct: PlayerView["main"] = "play";
  main.addEventListener("click", () => acts[mainAct]());
  // the slider: the dot follows the finger, the piano is asked once when it lets go (a jump per step would be refused
  // as "wait for the previous command"); the piano's own position does not move the dot meanwhile
  let dragging = false;
  let canSeek = false;
  let dragEnd: ReturnType<typeof setTimeout> | undefined;
  const place = (pos: number | null, total: number | null, unit: "bar" | "ms") => {
    // bars are counted from 1: bar 1 is the start of the line, the last bar its end
    const f = pos === null || !total ? null : unit === "bar" ? (total > 1 ? (pos - 1) / (total - 1) : 0) : pos / total;
    const pct = f === null ? "0%" : `${(Math.max(0, Math.min(1, f)) * 100).toFixed(2)}%`;
    if (fill.style.width !== pct) fill.style.width = pct;
    if (dot.style.left !== pct) dot.style.left = pct;
    dot.hidden = f === null;
  };
  range.addEventListener("input", () => {
    if (!shown || !canSeek) return;
    dragging = true;
    // a drag that ends without a change (the finger came back to where it started, a cancelled touch) lets go too
    clearTimeout(dragEnd);
    dragEnd = setTimeout(() => (dragging = false), 3000);
    const bar = Number(range.value);
    place(bar, shown.total, "bar");
    time.textContent = positionText({ ...shown, pos: bar });
  });
  range.addEventListener("change", () => {
    clearTimeout(dragEnd);
    dragging = false;
    if (!shown || !canSeek) return;
    acts.seek!(Number(range.value)); // the screen shows that bar until the piano's next position notice
  });

  const text = (e: HTMLElement, s: string) => {
    if (e.textContent !== s) e.textContent = s;
  };
  const attr = (e: HTMLElement, k: string, v: string) => {
    if (e.getAttribute(k) !== v) e.setAttribute(k, v);
  };
  // the pill is there only while one part alone is chosen (and the loaded thing has parts): both parts - the usual
  // case - needs no word on the bar (owner 2026-11-23: 「파트 1+2」 meant nothing to a first look)
  let partMeant = true;
  const syncPart = () => {
    parts.sync();
    const v = store.get(partP, 0x7f)?.[0];
    text(partVal, v === 0 ? "1" : v === 1 ? "2" : "1+2");
    partBtn.hidden = acts.partPill === false || !partMeant || (v !== 0 && v !== 1);
  };
  syncPart();

  return {
    el, foot, options, syncPart,
    set(v) {
      shown = v;
      attr(el, "data-play", v.state === null ? "" : String(v.state)); // styles.css: the recording colours
      // nothing said yet (the stock reports only changes): the idle wording
      text(status, v.stateText ?? t(STATE_TEXT[v.state ?? 2] ?? "Stopped"));
      text(title, v.title);
      text(sub, v.sub);
      if (mainAct !== v.main) {
        mainAct = v.main;
        attr(main, "data-act", v.main);
        attr(el, "data-main", v.main);
        attr(main, "title", t(MAIN_LABEL[v.main]));
        attr(main, "aria-label", t(MAIN_LABEL[v.main]));
        main.replaceChildren(icon(v.main));
      }
      prev.disabled = !v.prev;
      next.disabled = !v.next;
      if (partMeant !== (v.part ?? true)) {
        partMeant = v.part ?? true;
        partRow.hidden = !partMeant;
        syncPart();
      }
      const seekable = v.seekable && !!acts.seek && v.unit === "bar" && !!v.total && v.total > 1;
      if (range.hidden === seekable) range.hidden = !seekable;
      attr(seek, "data-seek", seekable ? "1" : "");
      // no total: nothing to draw a share of (an empty line, no dot)
      attr(seek, "data-known", v.total ? "1" : "");
      canSeek = seekable;
      if (!seekable) dragging = false;
      if (dragging) return;
      if (seekable) {
        attr(range, "max", String(v.total));
        const at = String(Math.max(1, Math.min(v.total!, v.pos ?? 1)));
        if (range.value !== at) range.value = at;
      }
      place(v.pos, v.total, v.unit);
      text(time, positionText(v));
    },
  };
}
