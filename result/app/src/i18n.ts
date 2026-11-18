// UI string table (feature 77). English source strings are the keys
// (gettext style), so a missing translation degrades to English instead of a
// key name. Two entry points:
//   t(s)  — UI text written in this code base; must be translated (a test
//           renders every screen in each language and checks for gaps)
//   tl(s) — labels that come from the protocol tables (parameter names, enum
//           values, sound names); translated when an entry exists, otherwise
//           shown as-is (instrument sound names stay English on the original too)
import { STRINGS } from "./i18n.strings.ts";

export type Lang = "en" | "ja" | "ko";
export const LANGS: { id: Lang; name: string }[] = [
  { id: "en", name: "English" },
  { id: "ja", name: "日本語" },
  { id: "ko", name: "한국어" },
];

let cur: Lang = "en";
const used = new Set<string>();

export function setLang(l: Lang): void {
  cur = LANGS.some((x) => x.id === l) ? l : "en";
  const doc = (globalThis as { document?: Document }).document;
  if (doc) doc.documentElement.lang = cur;
}

export function getLang(): Lang {
  return cur;
}

function fill(s: string, vars?: Record<string, string | number>): string {
  return vars ? s.replace(/\{(\w+)\}/g, (m, k: string) => (k in vars ? String(vars[k]) : m)) : s;
}

export function t(s: string, vars?: Record<string, string | number>): string {
  used.add(s);
  const tr = cur === "en" ? s : (STRINGS[cur][s] ?? s);
  return fill(tr, vars);
}

export function tl(s: string): string {
  if (cur === "en") return s;
  const tr = STRINGS[cur][s];
  if (tr !== undefined) return tr;
  // "<name>/<key>" enum labels (Temperament "Equal/C#"): translate the name, keep the note
  const m = /^(.+)\/([A-G]#?)$/.exec(s);
  return m && m[1] in STRINGS[cur] ? `${STRINGS[cur][m[1]]}/${m[2]}` : s;
}

/** Keys passed to t() so far that have no entry for `lang` (test helper). */
export function untranslated(lang: Exclude<Lang, "en">): string[] {
  return [...used].filter((k) => !(k in STRINGS[lang]));
}

export function usedKeys(): string[] {
  return [...used];
}
