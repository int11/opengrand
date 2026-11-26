// Info › self-update: the Install / Restart button after the shell's apply failed by throwing (an Electron IPC
// invoke rejects when its handler throws) - error paths must not leave the button disabled for good.
import { tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const U = await import("../src/features/selfupdate.ts");
const { selfUpdateBox } = await import("../src/ui/selfupdatebox.ts");
const i18n = await import("../src/i18n.ts");
type State = import("../src/features/selfupdate.ts").SelfUpdateState;

test("self-update: an apply that throws gives the button back", async () => {
  i18n.setLang("en");
  localStorage.clear();
  const st: State = { phase: "ready", current: "1.2.0", available: "1.3.0", action: "install" };
  let calls = 0;
  const u = {
    kind: "desktop" as const,
    state: async () => st,
    check: async () => {},
    apply: async (): Promise<boolean> => {
      calls++;
      throw new Error("handler failed");
    },
    openPage: () => {},
    onChange: () => () => {},
  };
  const hub = U.startSelfUpdate(u, { online: () => false, events: null, setTimeout: () => 0, setInterval: () => 0 });
  hub.state = st;
  const box = selfUpdateBox(hub);
  document.body.append(box);
  const apply = box.querySelector<HTMLButtonElement>("#self-update-apply")!;
  const unhandled: unknown[] = [];
  const onUnhandled = (e: unknown) => unhandled.push(e);
  process.on("unhandledRejection", onUnhandled);
  apply.click();
  await tick(20);
  process.off("unhandledRejection", onUnhandled);
  assert.equal(calls, 1);
  assert.equal(apply.disabled, false, "the button stayed disabled after the failed apply");
  assert.equal(unhandled.length, 0);
  box.remove();
  hub.stop();
});
