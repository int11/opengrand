// One long job per link and family (OTA ops, panel UI ops). The screens are rebuilt on every tab switch,
// language change and reconnect, and every client is made per call, so a disabled button or a client field does
// not keep a second job off the link: a rebuilt screen has a fresh, enabled button. The lock is keyed on the
// session / port object, taken before the first await and released in `finally`.
//  - OTA: a second Begin resets the piano's reception under the first upload, and every authenticated op changes
//    the challenge (REMOTE_PROTOCOL.md section 10.3), so two overlapping status + op pairs make one fail with rc 2.
//  - panel UI: Data acks carry no tag, so two uploads would read each other's acks; a roll back during an upload
//    changes the panel's slots under it.
export type XferFamily = "ota" | "panelui";

const held = new WeakMap<object, Map<XferFamily, string>>();

/** What holds `family` on this link, or null. */
export function xferBusy(port: object, family: XferFamily): string | null {
  return held.get(port)?.get(family) ?? null;
}

/** Take `family` on this link for `what`; null when another job holds it (nothing is taken then). */
export function xferLock(port: object, family: XferFamily, what: string): (() => void) | null {
  let m = held.get(port);
  if (m?.has(family)) return null;
  if (!m) held.set(port, (m = new Map()));
  m.set(family, what);
  let done = false;
  return () => {
    if (done) return;
    done = true;
    m!.delete(family);
  };
}

/** Run `fn` holding `family`; `refused` (nothing sent) when another job holds it. */
export async function withXferLock<T>(port: object, family: XferFamily, what: string, fn: () => Promise<T>, refused: () => T): Promise<T> {
  const release = xferLock(port, family, what);
  if (!release) return refused();
  try {
    return await fn();
  } finally {
    release();
  }
}
