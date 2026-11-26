// One transport runner per context and session: the only path by which a tap makes the piano play, record or stop
// something (a piano song, a USB file, the save sheet's quiet, a file put into a piano song).
//
// Why (field 2026-11-27, panel UI 0.2.34): taps in quick succession between piano songs and USB files each started
// their own multi-frame sequence on the one link - a USB file's (MusicMode 3, 24 08 select, Reset / Play) and a piano
// song's (MusicMode 2, select, Play) interleaved, the guards between them dropped a stop while letting the play go
// on, and the piano ended with the piano song playing under the USB file tapped last, or a USB row still marked playing
// under the piano song tapped last.
//
// How: a request is a whole sequence (ui/usbplay.ts usbStopAll / usbPlay with their field-tested order, the recorder's
// stop + select + play). One runs at a time, start to end - a sequence is never cut between a select and its Reset or
// Play. A request made while one runs waits; a newer request replaces one still waiting (the newest tap wins), so a
// burst of taps ends with exactly the last one carried out after the one already on the wire.
//
// What is loading (owner, panel UI 0.2.35: a tap that takes a moment must show that it loads): a request may name what
// it is for (its target); the runner says the newest named one still waiting or running and tells listeners when that
// changes - the screens mark that row and the player bar until the job is over (done, failed, replaced, or the session
// gone).
import type { AppContext } from "./context.ts";

type Job = () => Promise<unknown>;
/** What a request is for: a USB file's row, a piano song's row (n 0-based), or a stop (no row: the player bar only). */
export type TransportTarget = { kind: "usb"; name: string } | { kind: "piano"; n: number } | { kind: "stop" };
interface Waiting {
  job: Job;
  done: (ran: boolean) => void;
  target: TransportTarget | null;
}
interface Runner {
  session: unknown;
  running: boolean;
  next: Waiting | null;
  /** the running job's target */
  cur: TransportTarget | null;
  listeners: Set<() => void>;
}

export interface Transport {
  /** Carry out `job` after the one running; a job still waiting is dropped for it. Resolves true once it ran, false
   *  when a newer request replaced it before it started. Do not call it from inside a job (that job would wait for
   *  itself): a job calls the steps directly. `target`: what it is for (shown as loading until it is over). */
  request(job: Job, target?: TransportTarget): Promise<boolean>;
  /** a sequence runs now */
  busy(): boolean;
  /** What is loading: the newest named request still waiting, else the running one's; null when none (or when this
   *  runner's session is over). */
  target(): TransportTarget | null;
  /** fn is called whenever target() may have changed (a request, a job starting or ending, the session replaced).
   *  Returns the unsubscribe. */
  onChange(fn: () => void): () => void;
}

const runners = new WeakMap<AppContext, Runner>();

function notify(r: Runner): void {
  for (const fn of [...r.listeners]) {
    try {
      fn();
    } catch {
      // a screen's redraw never stops the runner
    }
  }
}

async function drain(r: Runner): Promise<void> {
  r.running = true;
  try {
    while (r.next) {
      const w = r.next;
      r.next = null;
      r.cur = w.target;
      notify(r);
      try {
        await w.job();
      } catch {
        // the job shows its own failure; the runner goes on with the next tap
      }
      w.done(true);
    }
  } finally {
    r.running = false;
    r.cur = null;
    notify(r);
  }
}

/** The runner of this context's current session (a reconnect starts a new one). */
export function transport(ctx: AppContext): Transport {
  const s = ctx.session();
  let r = runners.get(ctx);
  if (!r || r.session !== s) {
    const old = r;
    r = { session: s, running: false, next: null, cur: null, listeners: new Set() };
    runners.set(ctx, r);
    // the old session's loading marks go (its target() is null from now on)
    if (old) notify(old);
  }
  const me = r;
  return {
    request(job, target) {
      return new Promise<boolean>((done) => {
        me.next?.done(false);
        me.next = { job, done, target: target ?? null };
        if (!me.running) void drain(me);
        else notify(me);
      });
    },
    busy: () => me.running,
    target() {
      if (ctx.session() !== me.session) return null;
      return (me.next && me.next.target) || (me.running ? me.cur : null);
    },
    onChange(fn) {
      me.listeners.add(fn);
      return () => void me.listeners.delete(fn);
    },
  };
}
