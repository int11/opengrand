// Online self-update, page side (src/features/selfupdate.ts, src/ui/selfupdatebox.ts): which shell has an updater,
// when the page asks it to check (setting, start delay, 24 h, online), and what the Info block / banner show.
import { dom, tick } from "./_dom.ts";
import { beforeEach, test } from "node:test";
import assert from "node:assert/strict";

const U = await import("../src/features/selfupdate.ts");
const { selfUpdateBox, selfUpdateBanner, selfUpdateText } = await import("../src/ui/selfupdatebox.ts");
type State = import("../src/features/selfupdate.ts").SelfUpdateState;

beforeEach(() => localStorage.clear());

/** A fake shell updater: check() resolves to `next` states, records calls. */
function fakeUpdater(initial: State) {
  let st = initial;
  const ls = new Set<(s: State) => void>();
  const calls = { check: 0, apply: 0, open: 0 };
  const u = {
    kind: "desktop" as const,
    state: async () => st,
    check: async () => {
      calls.check++;
    },
    apply: async () => (calls.apply++, true),
    openPage: () => void calls.open++,
    onChange: (cb: (s: State) => void) => (ls.add(cb), () => ls.delete(cb)),
  };
  const push = (s: State) => {
    st = s;
    for (const l of ls) l(s);
  };
  return { u, calls, push };
}

/** Manual clock and timers. */
function clock(start = 1_000_000) {
  let now = start;
  const timers: { at: number; f: () => void; every?: number }[] = [];
  return {
    now: () => now,
    setTimeout: (f: () => void, ms: number) => timers.push({ at: now + ms, f }),
    setInterval: (f: () => void, ms: number) => timers.push({ at: now + ms, f, every: ms }),
    clearTimeout: () => {},
    clearInterval: () => {},
    advance(ms: number) {
      const end = now + ms;
      for (;;) {
        const due = timers.filter((t) => t.at <= end).sort((a, b) => a.at - b.at)[0];
        if (!due) break;
        now = due.at;
        if (due.every) due.at += due.every;
        else timers.splice(timers.indexOf(due), 1);
        due.f();
      }
      now = end;
    },
  };
}

function eventsBox() {
  const ls = new Map<string, Set<() => void>>();
  return {
    addEventListener: (t: string, f: () => void) => (ls.get(t) ?? ls.set(t, new Set()).get(t)!).add(f),
    removeEventListener: (t: string, f: () => void) => ls.get(t)?.delete(f),
    fire: (t: string) => ls.get(t)?.forEach((f) => f()),
  };
}

test("detection: desktop preload, Android phone shell; the panel shell, a browser and old shells have none", () => {
  assert.equal(U.selfUpdater({}), null);
  assert.equal(U.selfUpdater({ ca79Desktop: { shell: "electron", bluetooth: {} } }), null); // 0.2.0 shell
  assert.equal(U.selfUpdater({ ca79Desktop: { shell: "electron", update: { state: async () => null, check: async () => null, apply: async () => false, openPage: async () => true, onChange: () => () => {} } } })?.kind, "desktop");
  const android = (state: string) => ({ ca79ng: { selfUpdateState: () => state, selfUpdateCheck: () => true, selfUpdateApply: () => true } });
  assert.equal(U.selfUpdater(android('{"phase":"idle","current":"1.2.0"}'))?.kind, "android");
  assert.equal(U.selfUpdater(android("")), null); // panel build
  assert.equal(U.selfUpdater({ ca79ng: { checkUpdate: () => "{}" } }), null); // shell before the online update
});

test("schedule: one check shortly after start, then every 24 h, only online and only when on", async () => {
  const c = clock();
  const { u, calls } = fakeUpdater({ phase: "idle", current: "0.2.0" });
  let online = true;
  const hub = U.startSelfUpdate(u, { ...c, online: () => online, events: null });
  await tick(0);
  assert.equal(calls.check, 0, "not at once");
  c.advance(U.START_DELAY_MS);
  assert.equal(calls.check, 1, "start check");
  c.advance(U.CHECK_EVERY_MS - U.START_DELAY_MS); // the hourly tick at 24 h: 20 s short of a day since the check
  assert.equal(calls.check, 1, "not before 24 h");
  c.advance(U.TICK_MS);
  assert.equal(calls.check, 2, "24 h later");
  online = false;
  c.advance(U.CHECK_EVERY_MS);
  assert.equal(calls.check, 2, "offline: no check");
  online = true;
  c.advance(U.TICK_MS);
  assert.equal(calls.check, 3, "back online at the next tick");
  hub.setEnabled(false);
  c.advance(3 * U.CHECK_EVERY_MS);
  assert.equal(calls.check, 3, "off: no check");
  await hub.checkNow();
  assert.equal(calls.check, 4, "「지금 확인」 works while off");
  assert.equal(U.loadAutoUpdatePrefs().enabled, false);
  assert.equal(U.loadAutoUpdatePrefs().lastCheck, c.now());
  hub.stop();
});

test("schedule: default on; a disabled setting survives a restart; automation runs and a ready update never auto-check", async () => {
  assert.equal(U.loadAutoUpdatePrefs().enabled, true);
  const c = clock();
  const a = fakeUpdater({ phase: "idle", noAuto: true });
  const h1 = U.startSelfUpdate(a.u, { ...c, online: () => true, events: null });
  await tick(0);
  c.advance(U.START_DELAY_MS + U.CHECK_EVERY_MS);
  assert.equal(a.calls.check, 0);
  h1.stop();
  const b = fakeUpdater({ phase: "ready", available: "0.3.0", action: "restart" });
  const h2 = U.startSelfUpdate(b.u, { ...c, online: () => true, events: null });
  await tick(0);
  c.advance(U.START_DELAY_MS + U.CHECK_EVERY_MS);
  assert.equal(b.calls.check, 0);
  h2.stop();
});

test("schedule: after an offline failure, the browser's online event checks again", async () => {
  const c = clock();
  const ev = eventsBox();
  const { u, calls, push } = fakeUpdater({ phase: "idle" });
  const hub = U.startSelfUpdate(u, { ...c, online: () => true, events: ev });
  await tick(0);
  c.advance(U.START_DELAY_MS);
  assert.equal(calls.check, 1);
  push({ phase: "error", error: "offline" });
  ev.fire("online");
  assert.equal(calls.check, 2);
  push({ phase: "error", error: "ratelimit" });
  ev.fire("online");
  assert.equal(calls.check, 2, "rate limit waits for the next tick");
  hub.stop();
});

test("status texts: quiet problems read as such, refusals say why", () => {
  assert.match(selfUpdateText({ phase: "error", error: "offline" }), /internet/);
  assert.match(selfUpdateText({ phase: "error", error: "ratelimit" }), /request limit/);
  assert.match(selfUpdateText({ phase: "error", error: "norelease" }), /No release/);
  assert.match(selfUpdateText({ phase: "ready", action: "install", available: "1.2.2", hint: "source" }), /ready to install\. Turn on 「Allow from this source」/, "Android: the install source switch was opened");
  assert.doesNotMatch(selfUpdateText({ phase: "ready", action: "install", available: "1.2.2" }), /Allow from this source/);
  assert.match(selfUpdateText({ phase: "error", error: "signature" }), /signature/);
  assert.match(selfUpdateText({ phase: "error", error: "sha256" }), /checksum/);
  assert.match(selfUpdateText({ phase: "error", error: "http", message: "HTTP 500" }), /HTTP 500/);
  assert.match(selfUpdateText({ phase: "downloading", available: "0.3.0", progress: 42 }), /0\.3\.0.*42%/);
  assert.match(selfUpdateText({ phase: "ready", available: "0.3.0", action: "manual", reason: "portable" }), /portable/);
  assert.match(selfUpdateText({ phase: "uptodate", current: "0.3.0" }), /Up to date/);
});

test("Info block: toggle, 「지금 확인」, and the install / restart button when ready", async () => {
  const c = clock();
  const { u, calls, push } = fakeUpdater({ phase: "uptodate", current: "1.2.0", checkedAt: Date.now() });
  const hub = U.startSelfUpdate(u, { ...c, online: () => true, events: null });
  await tick(0);
  const box = selfUpdateBox(hub);
  dom.window.document.body.append(box);
  assert.match(box.querySelector("#self-update-status")!.textContent!, /Up to date/);
  assert.equal(box.querySelector("#self-update-apply"), null);
  const auto = box.querySelector<HTMLInputElement>("#self-update-auto")!;
  assert.equal(auto.checked, true);
  auto.checked = false;
  auto.dispatchEvent(new dom.window.Event("change"));
  assert.equal(U.loadAutoUpdatePrefs().enabled, false);
  box.querySelector<HTMLButtonElement>("#self-update-check")!.click();
  await tick(0);
  assert.equal(calls.check, 1);
  push({ phase: "downloading", current: "1.2.0", available: "1.3.0", progress: 10 });
  assert.equal(box.querySelector<HTMLButtonElement>("#self-update-check")!.disabled, true);
  push({ phase: "ready", current: "1.2.0", available: "1.3.0", action: "install" });
  const apply = box.querySelector<HTMLButtonElement>("#self-update-apply")!;
  assert.equal(apply.textContent, "Install");
  apply.click();
  await tick(0);
  assert.equal(calls.apply, 1);
  box.remove();
  hub.stop();
});

test("banner: shows when ready, 「나중에」 hides it until a newer version; manual opens the release page", async () => {
  const c = clock();
  const { u, calls, push } = fakeUpdater({ phase: "idle" });
  const hub = U.startSelfUpdate(u, { ...c, online: () => true, events: null });
  await tick(0);
  const el = selfUpdateBanner(hub);
  assert.equal(el.hidden, true);
  push({ phase: "ready", available: "0.3.0", action: "restart" });
  assert.equal(el.hidden, false);
  assert.match(el.textContent!, /0\.3\.0/);
  assert.equal(el.querySelector("#self-update-banner-apply")!.textContent, "Restart to update");
  el.querySelector<HTMLButtonElement>("#self-update-later")!.click();
  assert.equal(el.hidden, true);
  push({ phase: "ready", available: "0.3.0", action: "restart" });
  assert.equal(el.hidden, true, "same version stays dismissed");
  push({ phase: "ready", available: "0.4.0", action: "manual", reason: "portable" });
  assert.equal(el.hidden, false);
  el.querySelector<HTMLButtonElement>("#self-update-banner-apply")!.click();
  assert.equal(calls.open, 1);
  assert.equal(calls.apply, 0);
  hub.stop();
});
