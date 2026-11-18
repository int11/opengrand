// One Identify for the cards one screen builds at once (Settings: power policy, startup settings): they all ask
// "which OpenGrand features?" right after rendering, and a stock piano answers none of them (700 ms timeout each).
import { NgClient, type NgIdentity } from "../protocol/ngext.ts";
import type { PianoSession } from "../protocol/session.ts";

const SHARE_MS = 3000;
const recent = new WeakMap<PianoSession, { at: number; p: Promise<NgIdentity | null> }>();

export function identifyShared(s: PianoSession): Promise<NgIdentity | null> {
  const now = Date.now();
  const r = recent.get(s);
  if (r && now - r.at < SHARE_MS) return r.p;
  const p = new NgClient(s).identify();
  recent.set(s, { at: now, p });
  return p;
}
