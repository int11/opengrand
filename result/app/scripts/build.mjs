// Bundles the app into dist/: content-hashed ES module + stylesheet, index.html
// pointing at them, and the PWA pieces (manifest, icons, service worker).
import { build } from "esbuild";
import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { basename, join } from "node:path";

import { THEME, emitPwa } from "./pwa.ts";

const pkg = JSON.parse(readFileSync("package.json", "utf8"));
const DIST = process.env.CA79_DIST || "dist"; // tests build into a temporary folder (tests/pwa.test.ts)

mkdirSync(DIST, { recursive: true });
// drop the previous build's hashed files (and the old unhashed names)
for (const f of readdirSync(DIST)) {
  if (/^(app|styles)(-[\w]+)?\.(js|css)(\.map)?$/.test(f)) rmSync(join(DIST, f));
}

const res = await build({
  entryPoints: ["src/main.ts"],
  bundle: true,
  format: "esm",
  target: ["es2020", "chrome80"], // the panel's Android WebView is not current
  minify: true,
  sourcemap: true,
  outdir: DIST,
  entryNames: "app-[hash]",
  metafile: true,
  logLevel: "info",
  define: { __APP_VERSION__: JSON.stringify(pkg.version) },
});
const appJs = basename(Object.keys(res.metafile.outputs).find((f) => f.endsWith(".js")));

const css = readFileSync("styles.css");
const cssName = `styles-${createHash("sha256").update(css).digest("hex").slice(0, 8).toUpperCase()}.css`;
writeFileSync(join(DIST, cssName), css);

let html = readFileSync("index.html", "utf8");
for (const [from, to] of [['href="styles.css"', `href="${cssName}"`], ['src="app.js"', `src="${appJs}"`], ["__THEME__", THEME]]) {
  if (!html.includes(from)) throw new Error(`index.html: ${from} not found`);
  html = html.replaceAll(from, to);
}
writeFileSync(join(DIST, "index.html"), html);

const pwa = emitPwa(DIST, { version: pkg.version, assets: ["index.html", appJs, cssName] });
console.log(`dist: index.html ${appJs} ${cssName} ${pwa.join(" ")}`);
