// Music tab: Recorder / Lesson / Concert Magic / Piano Music.
import { t } from "../../i18n.ts";
import { bpm, findParam, offset64 } from "../../protocol/codecs.ts";
import {
  NgClient, ngHas, NgRecorder, parseRecorderNotice, RecorderCmd, RecorderEvent, RecorderRc, type RecorderSong,
  type RecorderStatus,
} from "../../protocol/ngext.ts";
import type { ParamStore } from "../../state/store.ts";
import type { AppContext } from "../context.ts";
import { control } from "../controls.ts";
import { h, section } from "../dom.ts";
import { identifyShared } from "../identify.ts";
import { ctxFor } from "./common.ts";
import { loadUsbList } from "../usblist.ts";

/** Piano songs box: shown only when the piano reports the playback control (OpenGrand Identify feat2 0x10, 0x7D op
 * 0x1E - REMOTE_PROTOCOL.md section 10.9). The piano judges every command the same way for the panel and this app;
 * this box sends the op and shows the piano's own notices. */
const PIANO_RC: Record<number, string> = {
  [RecorderRc.Busy]: "The piano is busy, try again in a moment.",
  [RecorderRc.Recording]: "The piano is recording: playback waits until it stops.",
  [RecorderRc.Pending]: "Wait for the previous command.",
  [RecorderRc.NoData]: "Nothing recorded in this song.",
  [RecorderRc.OtherPlayer]: "Another player mode is active on the piano.",
  [RecorderRc.NotBound]: "The piano is controlled by another device.",
};

function pianoSongsBox(ctx: AppContext): HTMLElement {
  const el = h("div", { class: "piano-songs", id: "piano-songs", hidden: true });
  const s = ctx.session();
  if (!s || s.state !== "ready") return el;
  const rec = new NgRecorder(new NgClient(s));
  let st: RecorderStatus | null = null;
  let songs: RecorderSong[] | null = null;
  const sel = h("select", { name: "pianoSong" }) as HTMLSelectElement;
  const line = h("p", { class: "muted", id: "piano-songs-state" });
  const note = h("p", { class: "muted", id: "piano-songs-note" });
  const render = () => {
    const cur = st?.song ?? Number(sel.value || 0);
    sel.replaceChildren(...Array.from({ length: 10 }, (_, i) => {
      const g = songs?.[i];
      const label = !g ? t("Song {n}", { n: i + 1 })
        : g.empty ? t("Song {n} · empty", { n: i + 1 })
        : t("Song {n} · {bars} bars", { n: i + 1, bars: g.bars });
      return h("option", { value: String(i), selected: i === cur }, label);
    }));
    line.textContent = !st ? ""
      : st.recording ? t("Recording on the piano")
      : st.playing ? t("Playing · bar {bar}", { bar: st.bar })
      : st.state === 0 ? t("Paused") : t("Stopped");
  };
  const send = async (cmd: number, arg = 0) => {
    const rc = await rec.command(cmd, arg);
    note.textContent = rc === null ? t("No answer from the piano.")
      : rc <= RecorderRc.Nop ? "" : t(PIANO_RC[rc] ?? "The piano rejected this value.");
  };
  sel.addEventListener("change", () => void send(RecorderCmd.Select, Number(sel.value)));
  let attached = false;
  const off = s.onMidi((m) => {
    if (el.isConnected) attached = true;
    else if (attached) return off(); // the screen was replaced
    const n = parseRecorderNotice(m);
    if (!n) return;
    if (n.status) st = n.status;
    if (n.songs) songs = n.songs;
    if (n.event && n.event.event === RecorderEvent.Timeout) note.textContent = t("The piano did not confirm the command.");
    render();
  });
  el.append(
    h("h3", {}, t("Piano songs")),
    h("label", { class: "row" }, h("span", {}, t("Song")), sel),
    h("div", { class: "transport" },
      h("button", { "data-action": "piano-play", onclick: () => void send(RecorderCmd.Play) }, `▶ ${t("Play")}`),
      h("button", { "data-action": "piano-pause", onclick: () => void send(RecorderCmd.Pause) }, `❚❚ ${t("Pause")}`),
      h("button", { "data-action": "piano-stop", onclick: () => void send(RecorderCmd.Stop) }, `■ ${t("Stop")}`)),
    line, note,
  );
  void (async () => {
    const id = await identifyShared(s);
    if (!ngHas(id, "recorder")) return off(); // stock or a payload without the feature
    st = await rec.status();
    songs = await rec.songs();
    el.hidden = false;
    render();
  })();
  return el;
}

/** Strings the piano songs box shows (for the translation check: the box needs a feat2 0x10 piano). */
export const PIANO_SONGS_STRINGS = [
  ...Object.values(PIANO_RC), "Song {n} · empty", "Song {n} · {bars} bars", "Recording on the piano", "Playing · bar {bar}",
  "Paused", "Stopped", "The piano did not confirm the command.", "Piano songs", "Song", "No answer from the piano.",
  "The piano rejected this value.",
];

/** MusicMode values (fn 0x31 52 1D, original enum). */
export const MUSIC_MODE = { Exit: 0, Recorder: 2, UsbMemory: 3, Lesson: 4, ConcertMagic: 5, PianoMusic: 6 } as const;

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
  const cmd = (v: number) => void store.command(music, 0x7f, [v]);
  const ab = (v: number) => void store.command(findParam("ABRepeat", 0x32)!, 0x7f, [v]);
  const prog = h("div", { class: "progress" }, "–");
  let attached = false;
  const timer = setInterval(() => {
    // stop polling once the screen has been replaced
    if (prog.isConnected) attached = true;
    else if (attached) return clearInterval(timer);
    prog.textContent = progressText();
  }, 250);

  // --- mode (Recorder / Lesson / Concert Magic / Piano Music) ---
  const modeP = findParam("MusicMode", 0x31)!;
  const modeState = findParam("MusicMode", 0x10)!;
  let mode: number = store.get(modeState, 0x7f)?.[0] ?? MUSIC_MODE.Recorder;
  const modes: [string, number][] = [["Recorder", MUSIC_MODE.Recorder], ["Lesson", MUSIC_MODE.Lesson], ["Concert Magic", MUSIC_MODE.ConcertMagic], ["Piano Music", MUSIC_MODE.PianoMusic]];
  const modeBar = h("div", { class: "segmented" });
  const sections: Record<number, HTMLElement> = {};
  const showMode = () => {
    modeBar.querySelectorAll("button").forEach((b) => b.classList.toggle("on", Number(b.getAttribute("data-mode")) === mode));
    for (const [m, el] of Object.entries(sections)) el.hidden = Number(m) !== mode;
  };
  for (const [label, v] of modes) {
    modeBar.append(h("button", { "data-mode": v, onclick: () => { mode = v; void store.command(modeP, 0x7f, [v]); showMode(); } }, t(label)));
  }
  store.subscribe((k, d) => {
    if (k === `MusicMode@127` && d[0] !== undefined && d[0] !== MUSIC_MODE.Exit) {
      mode = d[0];
      showMode();
    }
  });

  const transport = () =>
    h(
      "div",
      { class: "transport" },
      h("button", { onclick: () => cmd(1) }, `▶ ${t("Play")}`),
      h("button", { onclick: () => cmd(0) }, `❚❚ ${t("Pause")}`),
      h("button", { onclick: () => cmd(2) }, `■ ${t("Stop")}`),
      h("button", { onclick: () => cmd(8), title: t("Rewind") }, "◀◀"),
      h("button", { onclick: () => cmd(7), title: t("Fast forward") }, "▶▶"),
    );
  const abRow = () =>
    h(
      "div",
      { class: "transport" },
      h("button", { onclick: () => ab(1) }, t("Set A")),
      h("button", { onclick: () => ab(2) }, t("Set B")),
      h("button", { onclick: () => ab(0) }, t("A-B off")),
    );
  // song picker: internal songs (numType 0, 0-based) and, for the playback
  // modes, files on the USB memory (numType 1 = file-list id), §9.3
  const songNumber = (label: string, withUsb: boolean) => {
    const sel = h("select", {}) as HTMLSelectElement;
    const fill = () => {
      const cur = sel.value;
      sel.replaceChildren(
        ...Array.from({ length: 10 }, (_, i) => h("option", { value: `0:${i}` }, t("Song {n}", { n: i + 1 }))),
        ...(withUsb ? c.usbFiles.map((f, id) => h("option", { value: `1:${id}` }, `USB · ${f}`)) : []),
      );
      if (cur) sel.value = cur;
    };
    fill();
    sel.addEventListener("change", () => {
      const [type, n] = sel.value.split(":").map(Number);
      void store.set(findParam("SongSelect")!, 0x7f, songSelectData(n, type));
    });
    const usbBtn = withUsb
      ? h("button", { class: "star", title: t("Read the USB memory"), onclick: async () => {
          // the Files tab's cache (ui/usblist.ts): one request at a time, a failure keeps the last list
          await loadUsbList(c);
          fill();
        } }, "↻")
      : null;
    return h("label", { class: "row" }, h("span", {}, label), sel, usbBtn);
  };

  // --- song playback options shared by all modes (features 11, 52) ---
  const partP = findParam("IntSongPart")!;
  const p1 = h("input", { type: "checkbox", role: "switch" }) as HTMLInputElement;
  const p2 = h("input", { type: "checkbox", role: "switch" }) as HTMLInputElement;
  const syncParts = () => {
    const d = store.get(partP, 0x7f);
    const v = d?.[0] ?? 0x7f;
    p1.checked = v === 0 || v === 0x7f;
    p2.checked = v === 1 || v === 0x7f;
    p1.indeterminate = p2.indeterminate = !d; // not read yet
  };
  const sendParts = (changed: HTMLInputElement) => {
    if (!p1.checked && !p2.checked) changed.checked = true; // at least one part plays
    void store.set(partP, 0x7f, [p1.checked && p2.checked ? 0x7f : p1.checked ? 0 : 1]);
  };
  p1.addEventListener("change", () => sendParts(p1));
  p2.addEventListener("change", () => sendParts(p2));
  store.subscribe((k) => k === "IntSongPart@127" && syncParts());
  syncParts();

  const playback = section(
    t("Song playback"),
    control(store, "MusicTempo", { label: t("Song tempo"), part: 0x7f, min: 10, max: 400, toRaw: bpm.encode, fromRaw: bpm.decode, format: (v) => `♩=${v}` }),
    // Song transpose: the original table has PartTransposeValue on the same
    // wire id (53 02) with the System part; the key transpose uses part 0x0F.
    control(store, "TransposeValue", { label: t("Song transpose"), part: 0x7f, min: -12, max: 12, toRaw: offset64.encode, fromRaw: offset64.decode }),
    h("label", { class: "row" }, h("span", {}, t("Part 1 (right hand)")), p1),
    h("label", { class: "row" }, h("span", {}, t("Part 2 (left hand)")), p2),
    control(store, "MusicRepeat", { label: t("Repeat"), part: 0x7f }),
    control(store, "PlayMode", { label: t("Play mode"), part: 0x7f }),
  );

  sections[MUSIC_MODE.Recorder] = section(
    t("Recorder"),
    pianoSongsBox(c),
    songNumber(t("Internal song"), false),
    control(store, "IntSongPart", { label: t("Part"), part: 0x7f }),
    h(
      "div",
      { class: "transport" },
      h("button", { onclick: () => cmd(3), class: "rec" }, `● ${t("Rec")}`),
      h("button", { onclick: () => cmd(1) }, `▶ ${t("Play")}`),
      h("button", { onclick: () => cmd(0) }, `❚❚ ${t("Pause")}`),
      h("button", { onclick: () => cmd(2) }, `■ ${t("Stop")}`),
      h("button", { onclick: () => cmd(8), title: t("Rewind") }, "◀◀"),
      h("button", { onclick: () => cmd(7), title: t("Fast forward") }, "▶▶"),
    ),
    abRow(),
  );
  sections[MUSIC_MODE.Lesson] = section(
    t("Lesson"),
    songNumber(t("Lesson song"), true),
    h("p", { class: "muted" }, t("Practise one hand: switch the other part off under Song playback.")),
    transport(),
    h("div", { class: "transport" }, h("button", { onclick: () => cmd(5) }, t("Count-in")), h("button", { onclick: () => cmd(3), class: "rec" }, `● ${t("Record practice")}`)),
    abRow(),
  );
  sections[MUSIC_MODE.ConcertMagic] = section(
    t("Concert Magic"),
    songNumber(t("Concert Magic song"), true),
    h("p", { class: "muted" }, t("Tap any keys in rhythm; the piano plays the melody and accompaniment.")),
    ...concertMagicControls(store),
    transport(),
  );
  sections[MUSIC_MODE.PianoMusic] = section(t("Piano Music"), songNumber(t("Piece"), true), transport());

  const el = h("div", { class: "screen" }, section(t("Music"), modeBar, prog), ...Object.values(sections), playback);
  showMode();
  return el;
}
