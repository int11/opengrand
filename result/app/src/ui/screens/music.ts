// Music tab: the instrument's built-in songs - Lesson (lesson books), Concert Magic, Piano Music (listening).
// The recorder and the USB memory player are the Recorder tab (ui/recorder.ts, owner 2026-11-22) - one place each.
//
// Where those songs are (2026-11-23): not in the piano. The main board's
// SongSelect (10 52 00) reaches only the 10 recorder songs, and in MusicMode 4 (Lesson) / 6 (Piano Music) its Music
// commands do nothing - the earlier 「곡 1 … 곡 10」 picker here chose recorder songs and played nothing. The pieces are
// MIDI files inside the panel's Kawai app, which plays them itself (its shell's SMF player sends the notes to the
// piano). So a piece plays only where that shell is: our page on the panel (transport/kwmcore.ts). Elsewhere the list
// is shown with a line saying so; no SongSelect is ever sent for a built-in piece.
//
// What a tap does on the panel (the original app's own calls): selectSmf {path "Kawai/default/<file>.mid"} → for
// Lesson / Piano Music playStartSmf; for Concert Magic setConcertMagicMode {mode, smooth, triggerTrack} +
// setConcertMagicNativeTrigger {isNative: true} + MusicMode 5 (31 52 1D 05: the piano stops sounding its own keys, the
// shell turns each key into the song's next notes and reports the tap through KWM.donePlayConcertMagic).
// Hand practice (Lesson): the shell's own player leaves the hand out - setSmfTrack {array: [16 × 0/1]} mutes a part
// inside the shell (index = channel for a format 0 file, track for format 1; 0 = muted), so its notes are never sent
// and the piano's own keys are untouched. The hand parts are the first two parts selectSmf reports in existTrack:
// right hand first (channel 1), left hand second (channel 2). A new selectSmf starts with nothing muted, so the choice
// is sent again after each select.
// Song volume and balance (the original app's player sliders): Volume 10 55 01 on Global (the right hand, channel 1)
// and MIDI2ch (the left hand, channel 2) - the shell itself has no volume for its player. The piano applies a Volume
// set to the parts of the binding controller's port, and the shell's notes sound on the panel port's parts, so it is
// heard while the panel holds the binding (2026-11-26 on the piano: Global 0 → right hand silent, MIDI2ch 0 → left
// hand silent, the keys' own sound unchanged). Like the original, every piece picked sends both (song volume, with the
// balance for a lesson piece with two hand parts), so nothing needs restoring when a piece ends.
// Lesson metronome volume (manual p.39): the shell's player sends a click per beat of the piece (MIDI 99 22 vv on the
// first beat, 99 21 vv on the others, vv = its metronome volume 0..1 x 127) while its metronome is unmuted. As the
// original app: enableMetronome {on: false} (the shell then has the callback that rings the clicks) and
// setMetronomeNativeAudio {on: false} (clicks as MIDI, not panel audio) once; for a lesson piece muteMetronome
// {on: false} + setMetronomeVolume {volume: slider 0..127 in 0.1 steps}, for anything else muteMetronome {on: true}. The
// original also writes the slider to the piano's own MetronomeVolume (10 56 03) and restores it from its metronome
// screen; we leave the player's metronome setting alone (no restore point when a piece ends by itself or the page goes).
// Lesson tempo (manual p.39 「Tempo」): the shell's player speed, setSmfRelativeTempo {tempo: 0.5..2} - the original
// app's lesson slider (0..100, middle = 1×: 0.5..1 on the left half, 1..2 on the right). Like the original it is sent
// again after each selectSmf: the lesson's speed in Lesson, 1× for anything else.
import { t } from "../../i18n.ts";
import { findParam } from "../../protocol/codecs.ts";
import { builtinBooks, type BuiltinBook, type BuiltinMode, type BuiltinSong, regionOf, songKey } from "../../protocol/builtinsongs.ts";
import type { ParamStore } from "../../state/store.ts";
import { callNative, callPromise, kwm, kwmcoreAvailable } from "../../transport/kwmcore.ts";
import type { AppContext } from "../context.ts";
import { h, section } from "../dom.ts";
import { MUSIC_MODE, playerBar, playingMark } from "./songctl.ts";

export { MUSIC_MODE } from "./songctl.ts";

/** Concert Magic tap result codes (the shell's donePlayConcertMagic cmr). */
export const CM_RESULT = ["OK", "Slow", "Fast", "Too fast", "Chord", "No song / end"];

/**
 * SongSelect is 3 bytes in the original table: [numType, num_hi7, num_lo7]. numType 0 = a recorder song (0-9) - the
 * stock firmware's only songs (ui/recorder.ts).
 */
export function songSelectData(n: number, numType = 0): number[] {
  return [numType & 0x7f, (n >> 7) & 0x7f, n & 0x7f];
}

/**
 * [Global, MIDI2ch] Volume for a song volume (0..127) and balance (0..100, 50 = both hands at the song volume) as the
 * original app computes them: below 50 the right hand (Global) is scaled down, above 50 the left hand (MIDI2ch).
 */
export function handVolumes(vol: number, balance: number): [number, number] {
  const v = Math.max(0, Math.min(127, Math.round(vol)));
  const b = Math.max(0, Math.min(100, Math.round(balance)));
  if (b < 50) return [Math.floor((v * Math.floor((100 * b) / 50)) / 100), v];
  if (b > 50) return [v, Math.floor((v * Math.floor((100 * (100 - b)) / 50)) / 100)];
  return [v, v];
}
/** The balance slider's words: which hand is softer. */
function balanceText(b: number): string {
  return b < 50 ? t("Right hand softer") : b > 50 ? t("Left hand softer") : t("Both hands equal");
}

/** Hand practice: which hand the piano leaves out (you play it). */
export type Hands = "both" | "right" | "left";
/**
 * The [right, left] hand parts of a loaded piece as setSmfTrack indexes: the first two parts holding channel messages
 * in selectSmf's existTrack (16 flags). Without that answer: channels 1 and 2 (index 0, 1).
 */
export function handParts(existTrack: unknown): [number, number] {
  const on: number[] = [];
  if (Array.isArray(existTrack)) existTrack.slice(0, 16).forEach((x, i) => x && on.push(i)); // no flatMap on Chrome 71
  const r = on[0] ?? 0;
  return [r, on[1] ?? Math.min(r + 1, 15)];
}
/** setSmfTrack's array for a choice: 1 = plays, 0 = muted (the shell reads numbers). */
export function trackArray(hands: Hands, [r, l]: [number, number]): number[] {
  const off = hands === "right" ? r : hands === "left" ? l : -1;
  return Array.from({ length: 16 }, (_, i) => (i === off ? 0 : 1));
}

type Status = "idle" | "loading" | "playing" | "paused" | "magic" | "error" | "missing";

const MODES: [label: string, mode: number, key: BuiltinMode, line: string][] = [
  ["Lesson", MUSIC_MODE.Lesson, "lesson", "Play along with lesson-book pieces: one hand at a time, slower, a passage repeated."],
  ["Concert Magic", MUSIC_MODE.ConcertMagic, "cm", "Tap any keys in rhythm and the piece plays."],
  ["Piano Music", MUSIC_MODE.PianoMusic, "piano", "Listen to the built-in piano pieces."],
];

/**
 * Concert Magic as the panel's original app starts it: setConcertMagicMode {mode 1, smooth 0, triggerTrack 0}. In the
 * shell mode 1 advances one beat per tap (0 = every note of any part, 2 / 3 = the trigger part's notes), and every tap
 * turns smoothing on regardless of `smooth`; the original app has no settings for it, so neither do we.
 */
export const CM_SETUP = { mode: 1, smooth: 0, triggerTrack: 0 } as const;

/** The lesson metronome slider (0..127) as the shell's metronome volume: 0..1 in 0.1 steps, as the original app rounds it. */
export function metroVolume(v: number): number {
  return Math.round((Math.max(0, Math.min(127, v)) * 10) / 127) / 10;
}

/** The lesson tempo slider (0..100, 50 = 1×) as the shell's relative speed 0.5..2. */
export function relativeSpeed(v: number): number {
  const x = Math.max(0, Math.min(100, v));
  return x <= 50 ? 0.5 + (0.5 * x) / 50 : (2 * x) / 100;
}

/** What the selectSmf answer holds that the start needs (the shell's KWMSmf info). */
interface SmfInfo { division?: number; beatInfo?: { numra?: number; denom?: number }[]; existTrack?: unknown }
/** playStartSmf's interval as the original app computes it: ticks per beat (a dotted beat in 6/8, 9/8, 12/8). */
export function beatTicks(info: SmfInfo | null): number {
  const div = info?.division ?? 480;
  const numra = info?.beatInfo?.[0]?.numra ?? 4;
  const denom = info?.beatInfo?.[0]?.denom ?? 4;
  let d = 0;
  for (let de = denom; de > 1; de /= 2) d++;
  const bar = d >= 2 ? (div >> (d - 2)) * numra : (div << (2 - d)) * numra;
  return bar / (numra / (denom === 8 && numra > 3 ? 3 : 1));
}

/** What plays, kept across screens: a tab switch builds a new screen while the shell keeps playing. */
const live = {
  status: "idle" as Status,
  cur: null as { mode: number; book: BuiltinBook; i: number } | null,
  magicOn: false, // MusicMode 5 + native trigger are set (undo them when Concert Magic ends)
  hands: "both" as Hands,
  handsSent: "both" as Hands, // what the shell's player has muted for the loaded piece
  parts: [0, 1] as [number, number], // the loaded piece's right / left hand parts (setSmfTrack indexes)
  interval: 480,
  speed: 50, // the lesson tempo slider (relativeSpeed), kept across pieces as the original app keeps it
  metroVol: 0, // the lesson metronome slider 0..127 (the original's default 0 = silent), kept across pieces
  metroInit: false, // enableMetronome / setMetronomeNativeAudio sent to this shell
  metroLesson: false, // the shell's clicks are unmuted for a lesson piece
  songVol: 100, // the song volume slider 0..127 (the original's default 100), kept across pieces
  balance: 50, // the lesson balance slider 0..100 (50 = both hands equal), kept across pieces
  view: MUSIC_MODE.Lesson as number,
  redraw: null as (() => void) | null, // the newest screen's
};
const LIVE0 = JSON.stringify({ ...live, redraw: undefined });
/** Tests: forget what plays. */
export function _resetMusicForTests(): void {
  Object.assign(live, JSON.parse(LIVE0), { redraw: null });
  shellFiles = null;
}

// The panel shell's own list of its song files (getSmfList {bundle: true} = files/smf, where the shell unpacks its
// song set at start), as key -> path. Asked once; a failure is asked again next time.
let shellFiles: Promise<Map<string, string>> | null = null;
function shellPaths(): Promise<Map<string, string>> {
  return (shellFiles ??= callPromise<{ name?: unknown }[]>("getSmfList", { bundle: true }, 8000)
    .then((list) => {
      const m = new Map<string, string>();
      for (const e of Array.isArray(list) ? list : []) {
        if (typeof e?.name === "string" && e.name.startsWith("Kawai/default/")) m.set(songKey(e.name), e.name);
      }
      return m;
    })
    .catch(() => {
      shellFiles = null;
      return new Map<string, string>();
    }));
}

export function musicScreen(store: ParamStore, ctx?: AppContext): HTMLElement {
  const canPlay = kwmcoreAvailable();
  // the CA79's EU and US lists differ (Concert Magic, Piano Music, hymns): the piano's destination picks one
  const region = regionOf(ctx?.session()?.identity?.destination);
  const modeP = findParam("MusicMode", 0x31)!;
  const keyOf = (m: number) => MODES.find(([, v]) => v === m)![2];

  const redraw = () => live.redraw?.();

  // the shell's player mutes the hand left out; it needs a loaded piece (a new select clears every mute)
  const sendHands = (want: Hands) => {
    if (!canPlay || want === live.handsSent) return;
    live.handsSent = want;
    callNative("setSmfTrack", { array: trackArray(want, live.parts) });
  };
  /** Which hand the current piece should leave out: only a lesson piece from a book with two hand parts. */
  const handsFor = (c: typeof live.cur) => (c && c.mode === MUSIC_MODE.Lesson && c.book.hands ? live.hands : "both");
  const endMagic = () => {
    if (!live.magicOn) return;
    live.magicOn = false;
    callNative("setConcertMagicNativeTrigger", { isNative: false });
    void store.command(modeP, 0x7f, [MUSIC_MODE.Exit]);
  };
  // the shell's beat clicks: unmuted at the slider's volume for a lesson piece, muted for anything else
  const metroFor = (lesson: boolean) => {
    if (!canPlay) return;
    if (!live.metroInit) {
      live.metroInit = true;
      callNative("enableMetronome", { on: false });
      callNative("setMetronomeNativeAudio", { on: false });
    }
    live.metroLesson = lesson;
    if (lesson) callNative("setMetronomeVolume", { volume: metroVolume(live.metroVol) });
    callNative("muteMetronome", { on: !lesson });
  };
  // song volume / balance: Volume on Global (right hand) and MIDI2ch (left hand) for the piece now loaded
  const volFor = () => {
    const c = live.cur;
    const [g, l] = handVolumes(live.songVol, c && c.mode === MUSIC_MODE.Lesson && c.book.hands ? live.balance : 50);
    void store.send({ v1: 0x55, v2: 0x01, part: 0x0f, data: [g] });
    void store.send({ v1: 0x55, v2: 0x01, part: 0x31, data: [l] });
  };
  let volTimer: ReturnType<typeof setTimeout> | null = null;
  const volSoon = () => {
    if (!live.cur || !(live.status === "playing" || live.status === "paused" || live.status === "magic")) return;
    if (volTimer) return;
    volTimer = setTimeout(() => { volTimer = null; volFor(); }, 120);
  };
  const stop = () => {
    // the shell may play a piece started from an earlier screen: always stop it, end Concert Magic, both hands again
    if (canPlay) callNative("playStopSmf");
    endMagic();
    if (live.cur) live.status = "idle";
    redraw();
  };
  const play = async (mode: number, b: BuiltinBook, i: number) => {
    if (!canPlay) return;
    if (live.status === "playing" || live.status === "paused") callNative("playStopSmf");
    const song = b.songs[i];
    live.cur = { mode, book: b, i };
    live.status = "loading";
    redraw();
    let info: SmfInfo | null = null;
    const path = (await shellPaths()).get(song.key);
    if (live.cur?.book.id !== b.id || live.cur.i !== i) return; // another piece was picked meanwhile
    if (!path) {
      live.status = "missing";
      return redraw();
    }
    try {
      info = await callPromise<SmfInfo>("selectSmf", { path, quantise: 256 }, 8000);
    } catch {
      if (live.cur?.book.id === b.id && live.cur.i === i) live.status = "error";
      return redraw();
    }
    if (live.cur?.book.id !== b.id || live.cur.i !== i) return; // another piece was picked meanwhile
    // the new piece starts with nothing muted: hand practice (a lesson piece with two hand parts) is set again
    live.parts = handParts(info?.existTrack);
    live.handsSent = "both";
    sendHands(handsFor(live.cur));
    callNative("setSmfRelativeTempo", { tempo: mode === MUSIC_MODE.Lesson ? relativeSpeed(live.speed) : 1 });
    metroFor(mode === MUSIC_MODE.Lesson);
    volFor();
    if (mode === MUSIC_MODE.ConcertMagic) {
      await callPromise("setConcertMagicMode", { ...CM_SETUP }, 3000).catch(() => {});
      callNative("setConcertMagicNativeTrigger", { isNative: true });
      if (!live.magicOn) void store.command(modeP, 0x7f, [MUSIC_MODE.ConcertMagic]);
      live.magicOn = true;
      live.status = "magic";
    } else {
      endMagic();
      live.interval = beatTicks(info);
      callNative("playStartSmf", { interval: live.interval });
      live.status = "playing";
    }
    redraw();
  };
  const pause = () => {
    if (live.status !== "playing") return;
    callNative("playPauseSmf");
    live.status = "paused";
    redraw();
  };
  const resume = () => {
    if (!live.cur) return;
    if (live.status === "paused") {
      callNative("playStartSmf", { interval: live.interval });
      live.status = "playing";
      redraw();
    } else if (live.status !== "playing" && live.status !== "loading") void play(live.cur.mode, live.cur.book, live.cur.i);
  };
  const step = (d: -1 | 1) => {
    if (!live.cur) return;
    const n = live.cur.i + d;
    if (n >= 0 && n < live.cur.book.songs.length) void play(live.cur.mode, live.cur.book, n);
  };

  const player = playerBar(store, "music-player", { play: resume, pause, stop, prev: () => step(-1), next: () => step(1) });
  // the bar's options and part are the piano recorder's (tempo, IntSongPart, A-B on the piano): not these pieces
  player.el.querySelectorAll<HTMLElement>('[data-act="options"], [data-act="part"]').forEach((b) => (b.hidden = true));

  // ---- the lists ----
  const lists: Record<number, { pick: HTMLSelectElement; rows: HTMLElement; book(): BuiltinBook }> = {};
  const songRow = (mode: number, b: BuiltinBook, s: BuiltinSong, i: number) => {
    const on = live.cur?.book.id === b.id && live.cur.i === i;
    const playing = on && (live.status === "playing" || live.status === "magic");
    return h("div", { class: "song-row", "data-key": s.key, "data-on": on ? "1" : undefined, "data-playing": playing ? "1" : undefined },
      h("div", { class: "file-main" },
        h("button", { class: "song-pick", "data-act": "builtin-pick", "aria-pressed": String(on), disabled: canPlay ? undefined : true, onclick: () => void play(mode, b, i) },
          h("span", { class: "song-num" }, i + 1),
          h("span", { class: "song-txt" }, h("span", { class: "song-name" }, s.title), h("span", { class: "song-info" }, s.composer || t(b.title))),
          playing ? playingMark() : null)));
  };
  const fillRows = (mode: number) => {
    const l = lists[mode];
    const b = l.book();
    l.rows.replaceChildren(...b.songs.map((s, i) => songRow(mode, b, s, i)));
  };
  const listFor = (mode: number): [HTMLElement | null, HTMLElement] => {
    const books = builtinBooks(keyOf(mode), region);
    const pick = h("select", { "data-books": keyOf(mode) }, ...books.map((b) => h("option", { value: b.id }, t(b.title)))) as HTMLSelectElement;
    const rows = h("div", { class: "list", "data-list": keyOf(mode) });
    lists[mode] = { pick, rows, book: () => books.find((b) => b.id === pick.value) ?? books[0] };
    pick.addEventListener("change", () => fillRows(mode));
    fillRows(mode);
    return [books.length > 1 ? h("label", { class: "row" }, h("span", {}, mode === MUSIC_MODE.Lesson ? t("Lesson book") : t("Group")), pick) : null, rows];
  };

  // hand practice: shown in Lesson for a book whose pieces have two live.hands
  const handSeg = h("div", { class: "segmented", "data-hands": "" },
    ...([["Both hands", "both"], ["Right hand", "right"], ["Left hand", "left"]] as const).map(([label, v]) =>
      h("button", { "data-hand": v, onclick: () => { live.hands = v; if (live.status === "playing" || live.status === "paused") sendHands(handsFor(live.cur)); redraw(); } }, t(label))));
  const handRow = h("div", { class: "pl-part-row", id: "hand-practice" },
    h("div", { class: "row seg-row" }, h("span", {}, t("One-hand practice")), handSeg),
    h("p", { class: "muted" }, t("Choose the hand you play: the piano leaves that hand out and plays the other.")));

  // lesson tempo: the shell's speed for the piece now playing (or the next one picked)
  const speedIn = h("input", { type: "range", min: 0, max: 100, step: 1, value: String(live.speed) }) as HTMLInputElement;
  const speedOut = h("output", {}, `${relativeSpeed(live.speed).toFixed(2)}×`);
  const speedRow = h("label", { class: "row slider", id: "lesson-tempo" }, h("span", {}, t("Tempo")), speedIn, speedOut);
  const speedSet = () => {
    live.speed = Number(speedIn.value);
    speedOut.textContent = `${relativeSpeed(live.speed).toFixed(2)}×`;
    if (live.cur?.mode === MUSIC_MODE.Lesson && (live.status === "playing" || live.status === "paused")) {
      callNative("setSmfRelativeTempo", { tempo: relativeSpeed(live.speed) });
    }
  };
  speedIn.addEventListener("input", speedSet);
  speedIn.addEventListener("change", speedSet);

  // song volume: every mode's pieces (one value; a row in each section, kept in step)
  const volIns: HTMLInputElement[] = [], volOuts: HTMLElement[] = [];
  const volRow = () => {
    const i = h("input", { type: "range", min: 0, max: 127, step: 1, value: String(live.songVol) }) as HTMLInputElement;
    const o = h("output", {}, String(live.songVol));
    volIns.push(i);
    volOuts.push(o);
    const set = () => {
      live.songVol = Number(i.value);
      for (const x of volIns) if (x !== i) x.value = i.value;
      for (const x of volOuts) x.textContent = String(live.songVol);
      volSoon();
    };
    i.addEventListener("input", set);
    i.addEventListener("change", set);
    return h("label", { class: "row slider song-volume" }, h("span", {}, t("Song volume")), i, o);
  };
  const volNote = () => h("p", { class: "muted" }, t("Song volume and balance are heard while the panel controls the piano (not while a phone or PC does)."));
  // lesson balance: one hand softer (a book with two hand parts)
  const balIn = h("input", { type: "range", min: 0, max: 100, step: 1, value: String(live.balance) }) as HTMLInputElement;
  const balOut = h("output", {}, balanceText(live.balance));
  const balRow = h("label", { class: "row slider", id: "lesson-balance" }, h("span", {}, t("Balance")), balIn, balOut);
  const balSet = () => {
    live.balance = Number(balIn.value);
    balOut.textContent = balanceText(live.balance);
    if (live.cur?.mode === MUSIC_MODE.Lesson) volSoon();
  };
  balIn.addEventListener("input", balSet);
  balIn.addEventListener("change", balSet);

  // lesson metronome volume: the shell's beat clicks for the lesson piece
  const metroIn = h("input", { type: "range", min: 0, max: 127, step: 1, value: String(live.metroVol) }) as HTMLInputElement;
  const metroOut = h("output", {}, String(live.metroVol));
  const metroRow = h("label", { class: "row slider", id: "lesson-metronome" }, h("span", {}, t("Metronome volume")), metroIn, metroOut);
  const metroSet = () => {
    live.metroVol = Number(metroIn.value);
    metroOut.textContent = String(live.metroVol);
    if (live.metroLesson && live.cur?.mode === MUSIC_MODE.Lesson && (live.status === "playing" || live.status === "paused")) {
      callNative("setMetronomeVolume", { volume: metroVolume(live.metroVol) });
    }
  };
  metroIn.addEventListener("input", metroSet);
  metroIn.addEventListener("change", metroSet);

  // Concert Magic: the last tap's result (the shell's own settings stay as the original app sets them, CM_SETUP)
  const cmResult = h("div", { class: "cm-result", id: "cm-result" }, "–");
  if (canPlay) {
    const k = kwm();
    // the shell: KWM.donePlayConcertMagic(noteno, velocity, sec, success, cmr)
    k.donePlayConcertMagic = (...a: unknown[]) => {
      const code = Number(a[4] ?? 0);
      cmResult.textContent = t(CM_RESULT[code] ?? String(code));
      cmResult.setAttribute("data-code", String(code));
    };
  }

  const sections: Record<number, HTMLElement> = {
    // hand practice above the list (a book can be 126 pieces long)
    [MUSIC_MODE.Lesson]: ((pick, rows) => section(t("Lesson"), pick, speedRow, metroRow, volRow(), balRow, volNote(), handRow, rows))(...listFor(MUSIC_MODE.Lesson)),
    [MUSIC_MODE.ConcertMagic]: section(t("Concert Magic"), volRow(), ...listFor(MUSIC_MODE.ConcertMagic),
      h("label", { class: "row" }, h("span", {}, t("Last tap")), cmResult)),
    [MUSIC_MODE.PianoMusic]: section(t("Piano Music"), volRow(), volNote(), ...listFor(MUSIC_MODE.PianoMusic)),
  };

  // ---- the mode card: three buttons, each with its one line ----
  const modeBar = h("div", { class: "segmented modes" });
  const modeLine = h("p", { class: "muted", id: "music-mode-line" });
  for (const [label, v] of MODES) modeBar.append(h("button", { "data-mode": v, onclick: () => { live.view = v; render(); } }, t(label)));
  const where = canPlay ? null : h("p", { class: "muted", id: "builtin-where" }, t("These pieces are inside the piano's panel app, so they play only on the panel's screen."));

  function render() {
    const [, , , line] = MODES.find(([, v]) => v === live.view)!;
    modeBar.querySelectorAll("button").forEach((b) => b.classList.toggle("on", Number(b.getAttribute("data-mode")) === live.view));
    modeLine.textContent = t(line);
    for (const [m, sec] of Object.entries(sections)) sec.hidden = Number(m) !== live.view;
    handRow.hidden = !(canPlay && lists[MUSIC_MODE.Lesson].book().hands);
    balRow.hidden = handRow.hidden;
    speedRow.hidden = !canPlay;
    metroRow.hidden = !canPlay;
    if (metroIn.value !== String(live.metroVol)) metroIn.value = String(live.metroVol);
    metroOut.textContent = String(live.metroVol);
    if (speedIn.value !== String(live.speed)) speedIn.value = String(live.speed);
    speedOut.textContent = `${relativeSpeed(live.speed).toFixed(2)}×`;
    handSeg.querySelectorAll("button").forEach((b) => b.classList.toggle("on", b.getAttribute("data-hand") === live.hands));
    for (const m of Object.keys(lists)) fillRows(Number(m));
    const song = live.cur ? live.cur.book.songs[live.cur.i] : null;
    const st = live.status === "playing" || live.status === "magic" ? 1 : live.status === "paused" ? 0 : 2;
    player.set({
      state: live.cur ? st : null,
      stateText: live.status === "loading" ? t("Loading…") : live.status === "error" ? t("The panel app could not open this piece.") : live.status === "missing" ? t("This piece is not in the panel app.") : live.status === "magic" ? t("Tap any keys") : undefined,
      title: song ? song.title : t(MODES.find(([, v]) => v === live.view)![0]),
      sub: song ? song.composer || t(live.cur!.book.title) : canPlay ? t("Pick a piece") : "",
      unit: "ms", pos: null, total: null, seekable: false,
      main: live.status === "playing" ? "pause" : live.status === "magic" ? "stop" : "play",
      prev: !!live.cur && live.cur.i > 0, next: !!live.cur && live.cur.i < live.cur.book.songs.length - 1,
      part: false,
    });
  }

  const el = h("div", { class: "screen music" }, h("section", { class: "card", id: "music-modes" }, h("h3", {}, t("Music")), modeBar, modeLine, where), player.el, ...Object.values(sections));
  live.redraw = render;
  render();
  return el;
}
