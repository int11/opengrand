// Pieces the Recorder tab (ui/recorder.ts) and the Music tab (ui/screens/music.ts) share: the state line, the labelled
// transport buttons, the part buttons and the folded 「재생 옵션」 (tempo, transpose, repeat, play mode, seek, A-B).
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

export interface StateLine {
  el: HTMLElement;
  /** state: STATE_TEXT index or null (the piano has not said); text: shown instead of the state's name; sub: the right side */
  set(state: number | null, sub: string, text?: string): void;
}

/** One line: a dot, the state in large type, and what it is about (song, bar) at the right. Updated in place. */
export function stateLine(id: string): StateLine {
  const text = h("span", { class: "now-text" }, t("Stopped"));
  const sub = h("span", { class: "now-sub" });
  const el = h("div", { class: "now", id, role: "status" }, h("span", { class: "now-dot" }), text, sub);
  return {
    el,
    set(state, subText, over) {
      const st = state === null ? "" : String(state);
      if (el.getAttribute("data-state") !== st) el.setAttribute("data-state", st);
      // nothing said yet (the stock reports only changes): the idle wording, with the grey dot of "not known"
      const s = over ?? t(STATE_TEXT[state ?? 2] ?? "Stopped");
      if (text.textContent !== s) text.textContent = s;
      if (sub.textContent !== subText) sub.textContent = subText;
    },
  };
}

/** A transport button: the sign (data-ico, drawn above by styles.css) over the word. */
export function tpBtn(act: string, ico: string, label: string, run: () => void, cls = ""): HTMLButtonElement {
  return h("button", { class: `tp-btn ${cls}`.trim(), "data-act": act, "data-ico": ico, onclick: run }, label);
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

/** The folded options: song tempo, song transpose, repeat, play mode, seek, A-B repeat. */
export function playbackOptions(store: ParamStore, id: string): HTMLElement {
  const music = findParam("Music", 0x32)!;
  const cmd = (v: number) => void store.command(music, 0x7f, [v]);
  const ab = (v: number) => void store.command(findParam("ABRepeat", 0x32)!, 0x7f, [v]);
  return h("details", { class: "fold", id },
    h("summary", {}, h("span", { class: "fold-title" }, t("Playback options"), h("span", { class: "fold-note" }, t("Tempo · transpose · repeat · A-B")))),
    h("div", { class: "fold-body" },
      control(store, "MusicTempo", { label: t("Song tempo"), part: 0x7f, min: 10, max: 400, toRaw: bpm.encode, fromRaw: bpm.decode, format: (v) => `♩=${v}` }),
      // Song transpose: the original table has PartTransposeValue on the same
      // wire id (53 02) with the System part; the key transpose uses part 0x0F.
      control(store, "TransposeValue", { label: t("Song transpose"), part: 0x7f, min: -12, max: 12, toRaw: offset64.encode, fromRaw: offset64.decode }),
      control(store, "MusicRepeat", { label: t("Repeat"), part: 0x7f }),
      control(store, "PlayMode", { label: t("Play mode"), part: 0x7f }),
      h("div", { class: "row seg-row" }, h("span", {}, t("Seek")),
        h("div", { class: "segmented" },
          h("button", { "data-act": "rew", onclick: () => cmd(MUSIC_CMD.Rewind) }, `◀◀ ${t("Rewind")}`),
          h("button", { "data-act": "ff", onclick: () => cmd(MUSIC_CMD.Forward) }, `${t("Fast forward")} ▶▶`))),
      h("div", { class: "row seg-row" }, h("span", {}, t("A-B repeat")),
        h("div", { class: "segmented" },
          h("button", { "data-act": "ab-a", onclick: () => ab(1) }, t("Set A")),
          h("button", { "data-act": "ab-b", onclick: () => ab(2) }, t("Set B")),
          h("button", { "data-act": "ab-off", onclick: () => ab(0) }, t("A-B off"))))));
}
