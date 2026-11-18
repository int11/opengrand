// USB memory file manager (features 43, 45, 68 on the app side; REMOTE_PROTOCOL.md §9.5):
// list / play / import / save-as-SMF / upload / delete / rename / format.
// Delete and rename go by name (24 06 / 24 07), which the stock firmware and OpenGrand both
// answer; the by-id remote parameters 32 51 3B / 3A get no reply from the stock
// On the stock the list holds paths
// relative to the root ("DIR\SONG.MID"), so a rename keeps the file in its directory.
// Play, import and save follow what the stock V2.19 accepts: play = MusicMode 3 (USB memory) + 24 08 select + Music Play (mode 6 is the
// built-in Piano Music library and ignores Play); import = 24 09 <song> <type> (stock: KSO only);
// save = 24 0A <song> 02 + name (the stock ignores 32 52 20). File-list ids are never used here.
// The tab shows both lists as soon as it opens: the USB list
// from the shared cache (ui/usblist.ts, asked again only when old, empty, from another session, or after a stick was
// inserted; an empty answer right after a plug / connect is asked again after 1.5 s and 3 s), the internal songs from OpenGrand's read-only song table (0x7D op 1E 01, Identify feat2 0x10) and its
// 5E 01 notices. On a piano without that op the songs need 「읽기」: the stock way selects each song on the piano.
// Play / stop (「USB 재생 정지」 and 「USB 재생 겹침 · 정지」,
// ui/usbplay.ts): the stock's SMF and audio players run independently and Music Reset reaches only the player of
// the type the last 24 08 selected, so a stop selects a file of each type and resets it; ▶ never starts a USB file
// while another USB playback runs (it stops that first). The playing row shows ■ from the app's own ▶ and the
// piano's word (32 52 1D, 32 52 08, MusicProgress 52 03); the USB section's ■ always stops both players.
import { FILE_TYPE, FileAccessError, resultText } from "../protocol/fileaccess.ts";
import { NgClient, ngHas, NgRecorder, parseRecorderNotice, type RecorderSong } from "../protocol/ngext.ts";
import type { PianoSession } from "../protocol/session.ts";
import { InternalSongs, SONGS, type SongInfo } from "../protocol/songs.ts";
import { findParam } from "../protocol/codecs.ts";
import { t, tl } from "../i18n.ts";
import type { AppContext } from "./context.ts";
import { confirmTap } from "./confirm2.ts";
import { h, section } from "./dom.ts";
import { identifyShared } from "./identify.ts";
import { loadUsbList, usbListGone, usbListStale, usbListState } from "./usblist.ts";
import { usbPlay, usbPlayback, usbPlayingName, usbStopAll } from "./usbplay.ts";

/** Last internal song table this app saw, per context: drawn at once when the tab opens again (same session). */
interface SongCache {
  session: PianoSession | null;
  songs: (SongInfo | null)[];
}
const songCaches = new WeakMap<AppContext, SongCache>();
function songCache(ctx: AppContext): SongCache {
  let c = songCaches.get(ctx);
  const s = ctx.session();
  if (!c || c.session !== s) songCaches.set(ctx, (c = { session: s, songs: Array.from({ length: SONGS }, () => null) }));
  return c;
}

export { MUSIC } from "./usbplay.ts";

/** op 1E 01 entry -> the row model the stock scan fills (n 0-based). */
export function songInfoOf(g: RecorderSong, n: number): SongInfo {
  return { n, hasData: !g.empty, bars: g.bars, parts: g.empty ? null : [g.part1, g.part2] };
}

export function filesScreen(ctx: AppContext): HTMLElement {
  const store = ctx.store;
  const status = h("p", { class: "muted", id: "usb-status" });
  const result = h("p", { class: "muted", id: "usb-result" }, "");
  const list = h("div", { class: "list", id: "usb-files" });
  const usb = usbListState(ctx);
  const attached = () => store.get(findParam("USBMemory"), 0x7f)?.[0];
  const showStatus = () => {
    const a = attached();
    status.textContent = a === 1 ? t("USB memory attached") : a === 0 || usb.noMedia ? t("No USB memory") : t("USB memory state unknown");
  };

  let busy = false;
  const run = async (what: string, op: () => Promise<unknown>, refresh = true): Promise<boolean> => {
    const fa = ctx.files();
    if (!fa) {
      result.textContent = t("Connect to the piano first.");
      return false;
    }
    busy = true;
    result.textContent = t("{op}…", { op: what });
    try {
      await op();
      result.textContent = t("{op}: done", { op: what });
      if (refresh) await load(false, true); // the op worked, so the stick is mounted: an empty list is final
      return true;
    } catch (e) {
      const msg = e instanceof FileAccessError ? tl(resultText(e.code)) : (e as Error).message;
      result.textContent = t("{op} failed: {e}", { op: what, e: msg });
      return false;
    } finally {
      busy = false;
    }
  };

  const render = () => {
    const files = ctx.usbFiles;
    shownKey = playKey();
    if (!files.length) {
      list.replaceChildren(h("p", { class: "muted" }, usb.noMedia ? t("No USB memory") : t("No files.")));
      return;
    }
    list.replaceChildren(
      ...files.map((name) =>
        h(
          "div",
          { class: "file-item", "data-file": name },
          h("span", { class: "file-name" }, name),
          usbPlayingName(playing, name)
            ? h("button", { title: t("Stop"), "data-act": "stop", onclick: () => void stop() }, "■")
            : h("button", { title: t("Play"), "data-act": "play", onclick: () => void play(name) }, "▶"),
          h("button", { "data-act": "import", onclick: () => void run(t("Import"), () => ctx.files()!.load(name, Number(songSel.value)), false) }, t("Import")),
          h("button", { "data-act": "rename", onclick: () => void rename(name) }, t("Rename")),
          h("button", { class: "danger", "data-act": "delete", onclick: (e: Event) => del(name, e.currentTarget) }, t("Delete")),
        ),
      ),
    );
  };

  // report: show "reading…" and then the count (tab open, ↻, stick inserted); false after a file operation,
  // whose own "done" line stays. A request already running is shared (loadUsbList).
  const load = async (report = true, trustEmpty = false) => {
    if (!ctx.files()) return;
    if (report) result.textContent = t("{op}…", { op: t("Read the USB memory") });
    const ok = await loadUsbList(ctx, trustEmpty);
    if (!ok) result.textContent = usb.error ?? t("Connect to the piano first.");
    else if (usb.noMedia) result.textContent = t("No USB memory");
    else if (report) result.textContent = t("{n} files", { n: ctx.usbFiles.length });
    showStatus();
    render();
  };
  /** Tab open / reconnect: the cached list at once; ask the piano only when the cache is old or from another
   *  session, the stick is not known to be out, and the page is visible (else when it becomes visible). */
  const autoLoad = () => {
    if (!ctx.files()) return;
    if (attached() === 0) {
      usbListGone(ctx);
      showStatus();
      render();
      return;
    }
    if (!usbListStale(ctx)) return;
    if (typeof document !== "undefined" && document.visibilityState === "hidden") {
      const onVis = () => {
        if (document.visibilityState === "hidden") return;
        document.removeEventListener("visibilitychange", onVis);
        if (list.isConnected) autoLoad();
      };
      document.addEventListener("visibilitychange", onVis);
      return;
    }
    void load();
  };

  // play: USB memory mode (MusicMode 3), select the file into the USB player, then Play (ui/usbplay.ts) - after
  // stopping whatever USB playback runs; refused (with the panel way to stop) when it does not stop.
  const playing = usbPlayback(ctx);
  // the rows' ▶/■ as last drawn (onPlay below redraws only when it changes)
  const playKey = () => ctx.usbFiles.map((n) => (usbPlayingName(playing, n) ? "■" : "▶") + n).join("\n");
  let shownKey = "";
  const stillText = () => t("The piano still plays from the USB memory. Stop it on the piano: on the panel's USB memory screen choose the playing file and press Stop; if it still plays, switch the piano off and on.");
  const play = async (name: string) => {
    if (busy || playing.stopping) return;
    let refused: string | null = null;
    await run(t("Play"), async () => {
      const r = await usbPlay(ctx, name, () => (result.textContent = t("Stopping the USB playback that runs first…")));
      if (!r.ok && r.why === "not-stopped") refused = t("{name} was not started: the USB playback that runs did not stop.", { name }) + " " + stillText();
      else if (!r.ok && r.why === "offline") throw new Error(t("Connect to the piano first."));
    }, false);
    if (refused) result.textContent = refused;
    render();
  };
  // stop: both USB players (the selected type's, the audio one, the SMF one), then the piano must stay quiet
  const stop = async () => {
    if (busy || playing.stopping) return;
    if (!ctx.files()) {
      result.textContent = t("Connect to the piano first.");
      return;
    }
    busy = true;
    result.textContent = t("{op}…", { op: t("Stop") });
    try {
      const r = await usbStopAll(ctx);
      result.textContent = r === "stopped" ? t("{op}: done", { op: t("Stop") }) : r === "still" ? stillText() : t("Connect to the piano first.");
    } finally {
      busy = false;
    }
    render();
  };

  const rename = (name: string) => {
    const to = typeof prompt === "function" ? prompt(t("New name for {name}", { name }), name) : null;
    if (!to || to === name) return;
    const dir = name.includes("\\") ? name.slice(0, name.lastIndexOf("\\") + 1) : "";
    void run(t("Rename"), () => ctx.files()!.rename(name, to.includes("\\") ? to : dir + to));
  };
  const del = (name: string, btn: EventTarget | null) => {
    confirmTap(btn, t("Delete {name}?", { name }), () => void run(t("Delete"), () => ctx.files()!.delete(name)));
  };

  const songSel = h("select", {}, ...Array.from({ length: 10 }, (_, i) => h("option", { value: String(i) }, t("Song {n}", { n: i + 1 })))) as HTMLSelectElement;
  const upload = h("input", { type: "file", accept: ".mid,.MID,.smf,audio/midi" }) as HTMLInputElement;
  const readUpload = async (): Promise<{ name: string; data: Uint8Array } | null> => {
    const f = upload.files?.[0];
    if (!f) {
      result.textContent = t("Choose a MIDI file first.");
      return null;
    }
    return { name: f.name.replace(/[\\/]/g, "_"), data: new Uint8Array(await f.arrayBuffer()) };
  };

  // --- internal recorder songs: the stock has no directory, so a
  // scan selects each song and reads its 0xEC / MaxTime answers (protocol/songs.ts)
  const cache = songCache(ctx);
  const songs = cache.songs;
  const recordedText = () => t("{n} of 10 songs recorded", { n: songs.filter((i) => i?.hasData).length });
  const songsStatus = h("p", { class: "muted", id: "songs-status" }, songs.some((i) => i) ? recordedText() : t("Not scanned yet."));
  const songList = h("div", { class: "list", id: "songs-list" });
  // OpenGrand song table (op 1E 01): read-only, nothing on the piano changes, not APO activity; null = no such op
  let ngRec: NgRecorder | null = null;
  // Delete only where the piano's core deletes (5E 00 byte 31 bit 0, firmware recdel.c): the stock's own InternalSongDelete
  // re-writes the song from the last take - nothing deleted, and another song overwritten with that take
  let canDelete = false;
  const fillFromNg = (list: RecorderSong[]) => {
    list.slice(0, SONGS).forEach((g, n) => (songs[n] = songInfoOf(g, n)));
    renderSongs();
    songsStatus.textContent = recordedText();
  };
  const readNg = async (): Promise<boolean> => {
    if (!ngRec) return false;
    const list = await ngRec.songs();
    if (!list) return false;
    fillFromNg(list);
    return true;
  };
  const songsClient = () => {
    const s = ctx.session();
    return s ? new InternalSongs(s) : null;
  };
  const songRun = async (what: string, op: (c: InternalSongs) => Promise<unknown>): Promise<boolean> => {
    const c = songsClient();
    if (!c) {
      songsStatus.textContent = t("Connect to the piano first.");
      return false;
    }
    busy = true;
    songsStatus.textContent = t("{op}…", { op: what });
    try {
      const r = await op(c);
      if (r === false) throw new Error(t("no answer"));
      songsStatus.textContent = t("{op}: done", { op: what });
      return true;
    } catch (e) {
      songsStatus.textContent = t("{op} failed: {e}", { op: what, e: (e as Error).message });
      return false;
    } finally {
      busy = false;
    }
  };
  const songText = (i: SongInfo | null): string => {
    if (!i) return t("not scanned");
    if (!i.hasData) return t("Empty");
    const parts = i.parts ? [i.parts[0] ? t("Part 1") : "", i.parts[1] ? t("Part 2") : ""].filter(Boolean).join(" + ") : "";
    return [i.bars !== null ? t("{n} bars", { n: i.bars }) : "", parts].filter(Boolean).join(" · ");
  };
  const restoreOf = () => {
    const sel = store.get(findParam("SongSelect"), 0x7f);
    const part = store.get(findParam("IntSongPart"), 0x7f)?.[0];
    return { song: sel && sel[0] === 0 ? (sel[1] << 7) | sel[2] : 0, part };
  };
  const rescanOne = (n: number) => songRun(t("Read song {n}", { n: n + 1 }), async (c) => {
    if (ngRec && (await readNg())) return;
    songs[n] = await c.info(n);
    renderSongs();
  });
  const delTitle = () => (canDelete ? "" : t("Needs OpenGrand song delete: the piano's own delete does not remove the song."));
  const delSong = (n: number, part: 0 | 1 | 0x7f, btn: EventTarget | null) => {
    if (!canDelete) return;
    const msg = part === 0x7f ? t("Delete song {n} on the piano?", { n: n + 1 }) : t("Delete part {p} of song {n} on the piano?", { p: part + 1, n: n + 1 });
    confirmTap(btn, msg, () => void songRun(t("Delete"), (c) => c.delete(n, part)).then((ok) => ok && rescanOne(n)));
  };
  const renderSongs = () => {
    songList.replaceChildren(
      ...songs.map((i, n) => {
        const empty = i !== null && !i.hasData;
        const both = !!i?.parts && i.parts[0] && i.parts[1];
        return h(
          "div",
          { class: "file-item", "data-song": String(n + 1), "data-state": i === null ? "unknown" : i.hasData ? "data" : "empty" },
          h("span", { class: "song-name" }, t("Song {n}", { n: n + 1 })),
          h("span", { class: "muted song-info" }, songText(i)),
          h("button", { title: t("Play"), "data-act": "song-play", disabled: empty, onclick: () => void songRun(t("Play"), (c) => c.play(n)) }, "▶"),
          h("button", { "data-act": "song-save", title: t("To USB (SMF)"), "aria-label": t("To USB (SMF)"), disabled: empty, onclick: () => void run(t("Save as SMF"), () => ctx.files()!.saveSong(n, `SONG_${String(n + 1).padStart(2, "0")}.MID`, FILE_TYPE.MID)) }, "SMF"),
          // KSO = the piano's own song format, a lossless backup that loads back (24 09); owner 2026-11-15 (R2 backup)
          h("button", { "data-act": "song-save-kso", title: t("To USB (KSO)"), "aria-label": t("To USB (KSO)"), disabled: empty, onclick: () => void run(t("Save as KSO"), () => ctx.files()!.saveSong(n, `SONG_${String(n + 1).padStart(2, "0")}.KSO`, FILE_TYPE.KSO)) }, "KSO"),
          both ? h("button", { class: "danger", "data-act": "song-delete-p1", disabled: !canDelete, title: delTitle(), onclick: (e: Event) => delSong(n, 0, e.currentTarget) }, t("Delete part 1")) : null,
          both ? h("button", { class: "danger", "data-act": "song-delete-p2", disabled: !canDelete, title: delTitle(), onclick: (e: Event) => delSong(n, 1, e.currentTarget) }, t("Delete part 2")) : null,
          h("button", { class: "danger", "data-act": "song-delete", disabled: empty || !canDelete, title: delTitle(), onclick: (e: Event) => delSong(n, 0x7f, e.currentTarget) }, t("Delete")),
        );
      }),
    );
  };
  const scanSongs = async () => {
    // OpenGrand: the song table; the stock way below only when the op does not answer
    if (ngRec) {
      songsStatus.textContent = t("{op}…", { op: t("Scan") });
      if (await readNg()) return;
    }
    const ok = await songRun(t("Scan"), async (c) => {
      songs.fill(null);
      renderSongs();
      await c.scan(restoreOf(), (i) => {
        songs[i.n] = i;
        renderSongs();
      });
    });
    if (ok) songsStatus.textContent = recordedText();
  };
  renderSongs();
  /** Tab open / reconnect: on OpenGrand with the playback-control op, read the song table and follow its notices. */
  const autoSongs = () => {
    const s = ctx.session();
    if (!s || s.state !== "ready") return; // main.ts draws the tab again when the session becomes ready
    void (async () => {
      const id = await identifyShared(s);
      if (!ngHas(id, "recorder") || ctx.session() !== s) return;
      ngRec = new NgRecorder(new NgClient(s));
      let seen = false;
      const off = s.onMidi((m) => {
        if (songList.isConnected) seen = true;
        else if (seen) return off(); // the screen was replaced
        const n = parseRecorderNotice(m);
        if (n?.songs) fillFromNg(n.songs);
        if (n?.status && n.status.deletes !== canDelete) {
          canDelete = n.status.deletes;
          renderSongs();
        }
      });
      if (!songs.some((i) => i)) songsStatus.textContent = t("{op}…", { op: t("Scan") });
      // a 1E request also subscribes this controller to 5E 01 (song table changed) while it stays bound
      if (!(await readNg()) && !songs.some((i) => i)) songsStatus.textContent = t("Not scanned yet.");
      const st = await ngRec.status();
      if (st && st.deletes !== canDelete && ctx.session() === s) {
        canDelete = st.deletes;
        renderSongs();
      }
    })();
  };

  // The piano's word on the USB players (ui/usbplay.ts) redraws the rows while this tab is shown - only when a row's
  // ▶/■ changes: the bar notices (52 03) come about twice a second while a song plays, and redrawing on each one
  // replaced the ■ under the finger (field 2026-11-15, F13a: the stop button kept detaching, a tap could be lost).
  const onPlay = () => {
    if (!list.isConnected) {
      if (seenPlay) playing.listeners.delete(onPlay);
      return;
    }
    seenPlay = true;
    if (playKey() === shownKey) return;
    render();
  };
  let seenPlay = false;
  playing.listeners.add(onPlay);
  // The piano's USBMemory notice (51 2E): a stick inserted -> ask (as the stock panel does, kawaipiano.js
  // onUsbStatus -> FileListRequest), removed -> empty. Only a change counts: the connect sync repeats the
  // value, and a reconnect is handled by the new screen (another session -> stale cache).
  let lastAttach = attached();
  let seenUsb = false;
  const offUsb = store.subscribe((k) => {
    if (k !== "USBMemory@127") return;
    if (status.isConnected) seenUsb = true;
    else {
      if (seenUsb) offUsb();
      return;
    }
    const a = attached();
    showStatus();
    if (a === lastAttach) return;
    lastAttach = a;
    if (a === 1 && ctx.session()?.state !== "syncing") void load();
    if (a === 0) {
      usbListGone(ctx);
      showStatus();
      render();
    }
  });
  showStatus();
  render();
  autoLoad();
  autoSongs();

  return h(
    "div",
    { class: "screen" },
    section(
      t("Internal songs"),
      h("p", { class: "muted" }, t("The piano's recorder memory (10 songs). Scanning selects each song on the piano; song names are not sent by the piano and cannot be changed.")),
      h("div", { class: "transport" },
        h("button", { "data-act": "songs-scan", onclick: () => void scanSongs() }, `↻ ${t("Scan")}`),
        h("button", { "data-act": "songs-stop", onclick: () => void songRun(t("Stop"), (c) => c.stop()) }, `■ ${t("Stop")}`),
      ),
      songList,
      songsStatus,
    ),
    section(
      t("USB memory"),
      status,
      h("div", { class: "transport" },
        h("button", { onclick: () => void load(), "data-act": "refresh" }, `↻ ${t("Refresh")}`), // always asks
        // always there: playback the app did not start (panel, piano buttons) or whose notice never came
        h("button", { "data-act": "usb-stop", onclick: () => void stop() }, `■ ${t("Stop")}`),
        h("button", { class: "danger", "data-act": "format", onclick: (e: Event) => {
          confirmTap(e.currentTarget, t("Format the USB memory? All files will be erased."), () => void run(t("Format"), () => ctx.files()!.format()));
        } }, t("Format")),
      ),
      list,
      result,
    ),
    // saving an internal song to USB is the per-song 「USB로 (SMF)」 button above; this picks where 「가져오기」 goes
    section(
      t("Import into"),
      h("label", { class: "row" }, h("span", {}, t("Internal song")), songSel),
    ),
    section(
      t("Upload from this device"),
      upload,
      h("div", { class: "transport" },
        h("button", { "data-act": "upload", onclick: async () => { const u = await readUpload(); if (u) void run(t("Upload"), () => ctx.files()!.write(u.name, u.data)); } }, t("Copy to USB memory")),
        h("button", { "data-act": "play-data", onclick: async () => { const u = await readUpload(); if (u) void run(t("Play"), () => ctx.files()!.playData(u.data), false); } }, t("Play without saving")),
      ),
    ),
  );
}
