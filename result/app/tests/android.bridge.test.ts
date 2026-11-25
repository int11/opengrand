// Static contract between the Android shell (android/app/src/main/java/…/*.kt) and the web side
// (src/transport/android.ts): WebView has no Web MIDI / Web Bluetooth, so everything goes through
// the `ca79ng` JavascriptInterface and the `ca79ngNative` callbacks. A renamed or missing method
// on either side fails here instead of silently on the phone.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";

const root = new URL("../", import.meta.url);
const ktDir = new URL("android/app/src/main/java/dev/ca79ng/shell/", root);
const kt = Object.fromEntries(readdirSync(ktDir).filter((f) => f.endsWith(".kt")).map((f) => [f, readFileSync(new URL(f, ktDir), "utf8")]));
const ts = readFileSync(new URL("src/transport/android.ts", root), "utf8");

await import("../src/transport/android.ts");
const native = (globalThis as unknown as { ca79ngNative: Record<string, unknown> }).ca79ngNative;

test("every JavascriptInterface method is declared in NativeBridge and vice versa", () => {
  const shell = [...kt["MainActivity.kt"]!.matchAll(/@JavascriptInterface\s+fun\s+(\w+)\s*\(/g)].map((m) => m[1]!).sort();
  const iface = ts.slice(ts.indexOf("interface NativeBridge"), ts.indexOf("}", ts.indexOf("interface NativeBridge")));
  const web = [...iface.matchAll(/^\s+(\w+)\??\(/gm)].map((m) => m[1]!).sort();
  assert.ok(shell.length >= 14, `found ${shell.length} shell methods`);
  assert.deepEqual(shell, web);
});

test("every callback the shell evaluates exists on ca79ngNative", () => {
  const all = Object.values(kt).join("\n");
  const names = new Set<string>();
  for (const m of all.matchAll(/ca79ngNative\.(\w+)\(/g)) names.add(m[1]!);
  for (const m of all.matchAll(/callback\("(\w+)"/g)) names.add(m[1]!);
  assert.deepEqual([...names].sort(), ["onDetached", "onDevices", "onDfu", "onLifecycle", "onMidi", "onSelfUpdate", "onUpdate"]);
  for (const n of names) assert.equal(typeof native[n], "function", n);
});

test("the shell loads the bundle where the build copies dist", () => {
  const gradle = readFileSync(new URL("android/app/build.gradle.kts", root), "utf8");
  assert.match(gradle, /from\("\.\.\/\.\.\/dist"\)/);
  assert.match(gradle, /into\("build\/generated\/www\/www"\)/);
  assert.match(kt["MainActivity.kt"]!, /"https:\/\/appassets\.androidplatform\.net\/assets\/www\/index\.html"/);
  // index.html references its assets relatively, so the /assets/www/ prefix works
  const html = readFileSync(new URL("index.html", root), "utf8");
  assert.doesNotMatch(html, /(src|href)="\//);
});

test("the shell handles what WebView drops without a WebChromeClient", () => {
  // confirm()/prompt() (delete, format, factory reset, rename) and <input type=file> (OTA key, .c79, MIDI upload)
  const a = kt["MainActivity.kt"]!;
  assert.match(a, /webChromeClient\s*=/);
  assert.match(a, /onShowFileChooser/);
  assert.match(a, /onActivityResult/);
});
