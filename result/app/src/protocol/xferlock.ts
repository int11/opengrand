// One long job per link and family (OTA ops, panel UI ops). The screens are rebuilt on every tab switch,
// language change and reconnect, and every client is made per call, so a disabled button or a client field does
// not keep a second job off the link: a rebuilt screen has a fresh, enabled button. The lock is keyed on the
// session / port object, taken before the first await and released in `finally`.
//  - OTA: a second Begin resets the piano's reception under the first upload, and every authenticated op changes
//    the challenge (REMOTE_PROTOCOL.md section 10.3), so two overlapping status + op pairs make one fail with rc 2.
//  - panel UI: Data acks carry no tag, so two uploads would read each other's acks; a roll back during an upload
//    changes the panel's slots under it.
// The lock holds the running job (XferJob): what it is, its last progress and its result, with listeners. A screen
// built while a job runs follows it (progress, buttons off, the result) instead of offering a second one.
export type XferFamily = "ota" | "panelui";

type Watcher<P, R> = (job: XferJob<P, R>) => void;

/** A running (or just ended) long job: the last progress it reported, then its result. */
export class XferJob<P = unknown, R = unknown> {
  readonly what: string;
  progress: P | undefined = undefined;
  result: R | undefined = undefined;
  finished = false;
  private readonly watchers = new Set<Watcher<P, R>>();
  constructor(what: string) {
    this.what = what;
  }

  /** Calls `fn` now (the state so far), on every progress and once when the job ends; returns the unsubscribe. */
  watch(fn: Watcher<P, R>): () => void {
    fn(this);
    if (this.finished) return () => {};
    this.watchers.add(fn);
    return () => void this.watchers.delete(fn);
  }

  report(p: P): void {
    if (this.finished) return;
    this.progress = p;
    this.emit();
  }

  finish(r?: R): void {
    if (this.finished) return;
    this.finished = true;
    this.result = r;
    this.emit();
    this.watchers.clear();
  }

  private emit(): void {
    for (const fn of [...this.watchers]) {
      try {
        fn(this);
      } catch (e) {
        // a screen's listener must not break the transfer; the error still shows
        queueMicrotask(() => { throw e; });
      }
    }
  }
}

/** A held lock: report progress on its job, release it (with the result) at the end. */
export interface XferLease<P = unknown, R = unknown> {
  job: XferJob<P, R>;
  release(result?: R): void;
}

const held = new WeakMap<object, Map<XferFamily, XferJob>>();

/** What holds `family` on this link, or null. */
export function xferBusy(port: object, family: XferFamily): string | null {
  return held.get(port)?.get(family)?.what ?? null;
}

/** The job holding `family` on this link, or null. */
export function xferJob(port: object, family: XferFamily): XferJob | null {
  return held.get(port)?.get(family) ?? null;
}

/** Take `family` on this link for `what`; null when another job holds it (nothing is taken then). */
export function xferLock<P = unknown, R = unknown>(port: object, family: XferFamily, what: string): XferLease<P, R> | null {
  let m = held.get(port);
  if (m?.has(family)) return null;
  if (!m) held.set(port, (m = new Map()));
  const job = new XferJob<P, R>(what);
  m.set(family, job as XferJob);
  return {
    job,
    release: (result?: R) => {
      if (job.finished) return;
      // free first: a listener told of the end may start (or look for) the next job
      if (m!.get(family) === (job as XferJob)) m!.delete(family);
      job.finish(result);
    },
  };
}

/** Run `fn` holding `family` (its job gets `fn`'s result); `refused` (nothing sent) when another job holds it. */
export async function withXferLock<T, P = unknown>(port: object, family: XferFamily, what: string,
  fn: (job: XferJob<P, T>) => Promise<T>, refused: () => T): Promise<T> {
  const lease = xferLock<P, T>(port, family, what);
  if (!lease) return refused();
  let result: T | undefined;
  try {
    result = await fn(lease.job);
    return result;
  } finally {
    lease.release(result);
  }
}
