#!/usr/bin/env node
// Build a GitHub release for the online self-update: the phone APK, the Windows desktop zip and update.json,
// in one output folder, then PRINT the `gh release create` command (it does not run it, and does not push or tag).
//
//   node scripts/release_build.mjs [--tag r20261125] [--out DIR] [--skip-build] [--allow-dirty]
//                                  [--key PATH] [--sys PATH]
//
// Steps (any failure stops before update.json is written):
//   1. versions: result/app/android/app/build.gradle.kts (versionName / versionCode), result/desktop/package.json
//   2. git: a clean working tree (--allow-dirty to build anyway; the release should match a pushed commit)
//   3. builds: result/app `npm run build`, Android `./gradlew assemblePhoneDebug` (the debug signing key of this machine:
//      the same signer as the phone app installed so far — Android refuses an update from another signer),
//      result/desktop `npm run dist:win` (portable + zip + dir; the zip is what the desktop updater installs)
//   4. checks: scripts/leakcheck.py on the repository with the stock-overlap scan (--sys is required, never skipped);
//      scripts/release_scan.py on the APK and the zip (no Kawai material, no secrets, only our own files);
//      aapt2: package dev.ca79ng.shell at the gradle version
//   5. update.json: { schema, android, desktop } (format: result/desktop/src/updatepolicy.cjs), checked back with it.
//      Bridge: apps up to Android 1.2.0 / desktop 0.3.0 still require an Ed25519 "sig" over the canonical JSON
//      (below). While the old update key file exists it is added; without the key, those apps need one manual install.
// Outputs: <out>/OpenGrand-<a>-phone.apk, <out>/OpenGrand-<d>-win-x64.zip, <out>/update.json, <out>/NOTES.md
// Default out: dist-release/<tag>/ in this checkout (git-ignored).
// Old update key (optional): <main checkout>/result/stockfw/secrets/update_ed25519.pem, or CA79_UPDATE_KEY / --key.
// Stock file: <main checkout>/target/CAx9_P06.SYS, or --sys.
import { execFileSync } from "node:child_process";
import { createHash, createPrivateKey, createPublicKey, sign } from "node:crypto";
import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const P = createRequire(import.meta.url)(join(ROOT, "result/desktop/src/updatepolicy.cjs"));

const args = process.argv.slice(2);
const flag = (n) => args.includes(n);
const opt = (n) => {
  const i = args.indexOf(n);
  return i >= 0 ? args[i + 1] : undefined;
};
const die = (m) => {
  console.error(`\nrelease_build: ${m}`);
  process.exit(1);
};
const step = (m) => console.log(`\n== ${m}`);
const run = (cmd, a, o = {}) => {
  console.log(`$ ${[cmd, ...a].join(" ")}${o.cwd ? `   (in ${relative(ROOT, o.cwd) || "."})` : ""}`);
  execFileSync(cmd, a, { stdio: "inherit", cwd: ROOT, ...o });
};
const out_ = (cmd, a, o = {}) => execFileSync(cmd, a, { encoding: "utf8", cwd: ROOT, ...o }).trim();
const sha256 = (f) => createHash("sha256").update(readFileSync(f)).digest("hex");

/** The main checkout (not a worktree): where git-ignored secrets/ and target/ live. */
function mainRoot() {
  try {
    return dirname(out_("git", ["rev-parse", "--path-format=absolute", "--git-common-dir"]));
  } catch {
    return ROOT;
  }
}

// ---- 1. versions
step("versions");
const gradle = readFileSync(join(ROOT, "result/app/android/app/build.gradle.kts"), "utf8");
const aVersion = /versionName\s*=\s*"([^"]+)"/.exec(gradle)?.[1];
const aCode = Number(/versionCode\s*=\s*(\d+)/.exec(gradle)?.[1]);
const dVersion = JSON.parse(readFileSync(join(ROOT, "result/desktop/package.json"), "utf8")).version;
if (!aVersion || !aCode || !dVersion) die("cannot read the versions");
console.log(`android ${aVersion} (versionCode ${aCode}), desktop ${dVersion}`);
const today = new Date().toISOString().slice(0, 10).replaceAll("-", "");
const tag = opt("--tag") ?? `r${today}`;
if (!/^[A-Za-z0-9._-]+$/.test(tag)) die(`bad tag ${tag}`);
const out = resolve(opt("--out") ?? join(ROOT, "dist-release", tag));
const keyPath = opt("--key") ?? process.env.CA79_UPDATE_KEY ?? join(mainRoot(), "result/stockfw/secrets/update_ed25519.pem");
const sysPath = opt("--sys") ?? join(mainRoot(), "target/CAx9_P06.SYS");

// ---- 2. git
step("git");
const dirty = out_("git", ["status", "--porcelain", "--untracked-files=no"]);
const head = out_("git", ["rev-parse", "HEAD"]);
if (dirty && !flag("--allow-dirty")) die(`working tree has changes (commit first, or --allow-dirty):\n${dirty}`);
console.log(`HEAD ${head}${dirty ? " (dirty)" : ""}`);

// ---- bridge key (optional; see step 5)
const LEGACY_PUBKEY = "aP4AAJVD+jKhxbtW90VHNM7khSMV3jhgHTfHFh8DAl4=";
let priv = null;
if (existsSync(keyPath)) {
  priv = createPrivateKey(readFileSync(keyPath, "utf8"));
  const pub = priv.asymmetricKeyType === "ed25519" ? Buffer.from(createPublicKey(priv).export({ format: "jwk" }).x, "base64url").toString("base64") : "?";
  if (pub !== LEGACY_PUBKEY) die(`${keyPath} is not the old update key (public ${pub}, expected ${LEGACY_PUBKEY})`);
} else {
  console.log(`(no old update key at ${keyPath}: update.json without "sig"; apps up to Android 1.2.0 / desktop 0.3.0 will not take it)`);
}
if (!existsSync(sysPath)) die(`stock system file ${sysPath} not found - the stock-overlap scan is required for a release (--sys PATH)`);

// ---- 3. builds
const apkBuilt = join(ROOT, "result/app/android/app/build/outputs/apk/phone/debug/app-phone-debug.apk");
const zipBuilt = join(ROOT, `result/desktop/dist-electron/OpenGrand-${dVersion}-win-x64.zip`);
if (!flag("--skip-build")) {
  step("build");
  run("npm", ["--prefix", "result/app", "run", "build"]);
  const env = { ...process.env };
  if (!env.ANDROID_HOME && !env.ANDROID_SDK_ROOT && existsSync(join(process.env.HOME ?? "", "Android/Sdk"))) env.ANDROID_HOME = join(process.env.HOME, "Android/Sdk");
  rmSync(apkBuilt, { force: true });
  run("./gradlew", ["assemblePhoneDebug"], { cwd: join(ROOT, "result/app/android"), env });
  rmSync(zipBuilt, { force: true });
  run("npm", ["--prefix", "result/desktop", "run", "dist:win"]);
}
for (const f of [apkBuilt, zipBuilt]) if (!existsSync(f)) die(`missing build output ${relative(ROOT, f)}`);

// ---- 4. checks
step("leak check (repository)");
try {
  run("python3", ["-I", "scripts/leakcheck.py", "--sys", sysPath]);
} catch {
  die("scripts/leakcheck.py found something");
}
step("artifact scan (APK, desktop zip)");
try {
  run("python3", ["-I", "scripts/release_scan.py", "--sys", sysPath, "--apk", apkBuilt, "--zip", zipBuilt]);
} catch {
  die("scripts/release_scan.py refused the artifacts");
}
step("APK identity");
const sdk = process.env.ANDROID_HOME ?? process.env.ANDROID_SDK_ROOT ?? join(process.env.HOME ?? "", "Android/Sdk");
const bt = existsSync(join(sdk, "build-tools")) ? readdirSync(join(sdk, "build-tools")).sort((a, b) => P.compareVersions(a, b)).pop() : null;
if (!bt) die(`no Android build-tools under ${sdk}`);
const badging = out_(join(sdk, "build-tools", bt, "aapt2"), ["dump", "badging", apkBuilt]);
const pkg = /^package: name='([^']+)' versionCode='(\d+)' versionName='([^']+)'/m.exec(badging);
if (!pkg || pkg[1] !== "dev.ca79ng.shell" || Number(pkg[2]) !== aCode || pkg[3] !== aVersion) die(`APK is ${pkg ? pkg.slice(1).join(" ") : "?"}, expected dev.ca79ng.shell ${aCode} ${aVersion}`);
const certs = out_(join(sdk, "build-tools", bt, "apksigner"), ["verify", "--print-certs", apkBuilt]);
const signer = /certificate SHA-256 digest: ([0-9a-f]+)/.exec(certs)?.[1] ?? "?";
console.log(`${pkg[1]} ${pkg[3]} (${pkg[2]}), signer SHA-256 ${signer}`);

// ---- 5. outputs + update.json
step("update.json");
mkdirSync(out, { recursive: true });
const apkName = `OpenGrand-${aVersion}-phone.apk`;
const zipName = `OpenGrand-${dVersion}-win-x64.zip`;
copyFileSync(apkBuilt, join(out, apkName));
copyFileSync(zipBuilt, join(out, zipName));
const entry = (name, extra) => ({ ...extra, file: name, sha256: sha256(join(out, name)), size: statSync(join(out, name)).size });
const manifest = { schema: 1, android: entry(apkName, { version: aVersion, versionCode: aCode }), desktop: entry(zipName, { version: dVersion }) };
// canonical JSON for the bridge signature: sorted keys, no white space (only plain strings and integers occur here)
const canonical = (v) => (typeof v === "object" ? `{${Object.keys(v).sort().map((k) => `"${k}":${canonical(v[k])}`).join(",")}}` : JSON.stringify(v));
if (priv) manifest.sig = sign(null, Buffer.from(canonical(manifest), "ascii"), priv).toString("base64");
const text = JSON.stringify(manifest, null, 2) + "\n";
P.verifyManifest(text); // round trip with the apps' own check
writeFileSync(join(out, "update.json"), text);
const title = `OpenGrand ${today.slice(0, 4)}-${today.slice(4, 6)}-${today.slice(6)} (Android ${aVersion}, Windows ${dVersion})`;
writeFileSync(join(out, "NOTES.md"), [
  `Phone app (Android) ${aVersion} and Windows desktop app ${dVersion}.`,
  "",
  "Installed apps update themselves from this release (Info screen, Automatic updates). Manual install:",
  `- Android: ${apkName} (installs over an earlier OpenGrand phone app)`,
  `- Windows: unzip ${zipName} and run OpenGrand.exe`,
  "",
  "| file | sha256 |",
  "|---|---|",
  `| ${apkName} | ${manifest.android.sha256} |`,
  `| ${zipName} | ${manifest.desktop.sha256} |`,
  "",
  `Built from ${head}.`,
  "",
].join("\n"));
console.log(text);

const q = (s) => `'${s.replace(/'/g, "'\\''")}'`;
console.log(`== done: ${out}
Before publishing: push ${head.slice(0, 12)} to main (the tag is created on that commit), and run make check.
Then:

gh release create ${tag} --repo ${P.REPO} --target ${head} --latest --title ${q(title)} --notes-file ${q(join(out, "NOTES.md"))} \\
  ${q(join(out, "update.json"))} ${q(join(out, apkName))} ${q(join(out, zipName))}
`);
