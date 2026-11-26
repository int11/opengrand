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
import type { AppContext } from "./context.ts";

type Job = () => Promise<unknown>;
interface Waiting {
  job: Job;
  done: (ran: boolean) => void;
}
interface Runner {
  session: unknown;
  running: boolean;
  next: Waiting | null;
}

export interface Transport {
  /** Carry out `job` after the one running; a job still waiting is dropped for it. Resolves true once it ran, false
   *  when a newer request replaced it before it started. Do not call it from inside a job (that job would wait for
   *  itself): a job calls the steps directly. */
  request(job: Job): Promise<boolean>;
  /** a sequence runs now */
  busy(): boolean;
}

const runners = new WeakMap<AppContext, Runner>();

async function drain(r: Runner): Promise<void> {
  r.running = true;
  try {
    while (r.next) {
      const w = r.next;
      r.next = null;
      try {
        await w.job();
      } catch {
        // the job shows its own failure; the runner goes on with the next tap
      }
      w.done(true);
    }
  } finally {
    r.running = false;
  }
}

/** The runner of this context's current session (a reconnect starts a new one). */
export function transport(ctx: AppContext): Transport {
  const s = ctx.session();
  let r = runners.get(ctx);
  if (!r || r.session !== s) {
    r = { session: s, running: false, next: null };
    runners.set(ctx, r);
  }
  const me = r;
  return {
    request(job) {
      return new Promise<boolean>((done) => {
        me.next?.done(false);
        me.next = { job, done };
        if (!me.running) void drain(me);
      });
    },
    busy: () => me.running,
  };
}
