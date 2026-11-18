// The built-in panel's WebView is Chrome 71 (field 2026-11-01).
// CSS it does not know is dropped silently — flex `gap` (Chrome 84) collapsed every spaced row on the panel.
// This test keeps such CSS out of what the panel loads (styles.css, the chooser page, inline styles in src/)
// unless the rule says how it falls back (a comment containing "c71:" in the same rule).
// The panel simulator (npm run panelsim) renders with current Chromium, so it cannot catch these.
import assert from "node:assert/strict";
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";

const APP = join(import.meta.dirname, "..");

/** Features newer than Chrome 71, as [what, test on one declaration or selector]. */
const NEWER: [string, RegExp][] = [
  ["min()/max()/clamp() (Chrome 79)", /(?<![\w-])(min|max|clamp)\(/],
  ["aspect-ratio (Chrome 88)", /(^|[;\s{])aspect-ratio\s*:/],
  ["inset (Chrome 87)", /(^|[;\s{])inset(-block|-inline)?\s*:/],
  [":is()/:where()/:has() (Chrome 88/88/105)", /:(is|where|has)\(/],
  [":focus-visible (Chrome 86)", /:focus-visible/],
  ["dynamic viewport units (Chrome 108)", /\d(dvh|svh|lvh|dvw|svw|lvw)\b/],
  ["color-mix()/lab()/lch()/oklch() (Chrome 111)", /(color-mix|oklch|oklab|(?<![\w-])lab|(?<![\w-])lch)\(/],
  ["accent-color (Chrome 93)", /(^|[;\s{])accent-color\s*:/],
  ["backdrop-filter (Chrome 76)", /(^|[;\s{])backdrop-filter\s*:/],
  ["@container / @layer (Chrome 105/99)", /@(container|layer)\b/],
  ["CSS nesting (Chrome 112)", /\{[^{}]*&[^{}]*\{/],
];

interface Rule { selector: string; body: string; marked: boolean; where: string }

/** Innermost `selector { body }` blocks (enough for our flat stylesheets with @media wrappers). */
export function rules(css: string, where: string): Rule[] {
  const out: Rule[] = [];
  const re = /([^{}]*)\{([^{}]*)\}/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(css))) {
    const raw = m[1] + m[2];
    const strip = (s: string) => s.replace(/\/\*[\s\S]*?\*\//g, "");
    out.push({ selector: strip(m[1]).trim(), body: strip(m[2]), marked: /\/\*[^*]*c71:/.test(raw), where });
  }
  return out;
}

/** Problems for Chrome 71 in one stylesheet text. */
export function c71Problems(css: string, where: string): string[] {
  const bad: string[] = [];
  const noComments = css.replace(/\/\*[\s\S]*?\*\//g, "");
  for (const [what, re] of NEWER) if (re.source.startsWith("@") && re.test(noComments)) bad.push(`${where}: ${what}`);
  for (const r of rules(css, where)) {
    if (r.marked) continue;
    const decls = r.body.split(";").map((d) => d.trim()).filter(Boolean);
    const grid = decls.some((d) => /^display\s*:\s*(inline-)?grid\b/.test(d));
    for (const d of decls) {
      // gap works on grid containers from Chrome 66; on flex containers only from Chrome 84
      if (/^(gap|row-gap|column-gap)\s*:/.test(d) && !grid) bad.push(`${where}: \`${r.selector} { ${d} }\` — gap outside a grid container (flex gap is Chrome 84); use margins on the children`);
      for (const [what, re] of NEWER) if (!re.source.startsWith("@") && re.test(d)) bad.push(`${where}: \`${r.selector} { ${d} }\` — ${what}`);
    }
    for (const [what, re] of NEWER) if (/^:/.test(re.source) && re.test(r.selector)) bad.push(`${where}: \`${r.selector}\` — ${what}`);
  }
  return [...new Set(bad)];
}

function walk(dir: string): string[] {
  return readdirSync(dir, { withFileTypes: true }).flatMap((e) => (e.isDirectory() ? walk(join(dir, e.name)) : [join(dir, e.name)]));
}

test("the checker itself: flags flex gap, min(), aspect-ratio, :is(); accepts grid gap and marked fallbacks", () => {
  const css = `.a { display: flex; gap: 8px } .b { display: grid; gap: 8px } .c { width: min(90vw, 480px) }
    .d { aspect-ratio: 1 } .e:is(.x) { color: red } .f { grid-template-columns: minmax(0, 1fr) }
    .g { /* c71: width below is the fallback */ width: 90vw; width: min(90vw, 480px) }`;
  const p = c71Problems(css, "t");
  assert.equal(p.length, 4, p.join("\n"));
  assert.ok(p.some((x) => x.includes(".a") && x.includes("gap")));
  assert.ok(!p.some((x) => x.includes(".b") || x.includes(".f") || x.includes(".g")));
});

test("panel CSS uses nothing newer than Chrome 71 without a fallback (styles.css, chooser, inline styles)", () => {
  const bad: string[] = [];
  bad.push(...c71Problems(readFileSync(join(APP, "styles.css"), "utf8"), "styles.css"));
  const chooser = readFileSync(join(APP, "panelui/chooser.html"), "utf8");
  for (const m of chooser.matchAll(/<style>([\s\S]*?)<\/style>/g)) bad.push(...c71Problems(m[1], "panelui/chooser.html"));
  for (const f of walk(join(APP, "src")).filter((x) => x.endsWith(".ts"))) {
    const src = readFileSync(f, "utf8");
    for (const m of src.matchAll(/style:\s*"([^"]*)"/g)) bad.push(...c71Problems(`x { ${m[1]} }`, f.slice(APP.length + 1)));
    if (/\.style\.(gap|rowGap|columnGap|aspectRatio|inset)\b/.test(src)) bad.push(`${f.slice(APP.length + 1)}: element.style uses a property Chrome 71 lacks`);
  }
  assert.deepEqual(bad, []);
});
