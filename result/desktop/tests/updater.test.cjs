// Unit tests for the online self-update (no Electron, no network): node --test
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const zlib = require("node:zlib");
const P = require("../src/updatepolicy.cjs");
const { listZip, extractZip } = require("../src/unzip.cjs");
const { createUpdater } = require("../src/selfupdate.cjs");

const FIX = JSON.parse(fs.readFileSync(path.join(__dirname, "fixtures", "update.json"), "utf8"));
const sha256 = (b) => crypto.createHash("sha256").update(b).digest("hex");

/** A zip of [name, content, deflate?] entries (local headers + central directory, no zip64). */
function makeZip(entries) {
  const locals = [];
  const central = [];
  let off = 0;
  for (const [name, content, deflate] of entries) {
    const data = Buffer.from(content);
    const comp = deflate ? zlib.deflateRawSync(data) : data;
    const nm = Buffer.from(name, "utf8");
    const crc = zlib.crc32(data) >>> 0;
    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0);
    lh.writeUInt16LE(20, 4);
    lh.writeUInt16LE(0x800, 6);
    lh.writeUInt16LE(deflate ? 8 : 0, 8);
    lh.writeUInt32LE(crc, 14);
    lh.writeUInt32LE(comp.length, 18);
    lh.writeUInt32LE(data.length, 22);
    lh.writeUInt16LE(nm.length, 26);
    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0);
    ch.writeUInt16LE(20, 4);
    ch.writeUInt16LE(20, 6);
    ch.writeUInt16LE(0x800, 8);
    ch.writeUInt16LE(deflate ? 8 : 0, 10);
    ch.writeUInt32LE(crc, 16);
    ch.writeUInt32LE(comp.length, 20);
    ch.writeUInt32LE(data.length, 24);
    ch.writeUInt16LE(nm.length, 28);
    ch.writeUInt32LE(off, 42);
    locals.push(lh, nm, comp);
    central.push(ch, nm);
    off += 30 + nm.length + comp.length;
  }
  const cd = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(entries.length, 8);
  end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(cd.length, 12);
  end.writeUInt32LE(off, 16);
  return Buffer.concat([...locals, cd, end]);
}

const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), "og-upd-"));

// ---------------------------------------------------------------- format

test("the shared fixture parses; a \"sig\" from older releases is ignored", () => {
  const v = P.verifyManifest(FIX.manifest);
  assert.equal(v.desktop.version, "0.3.0");
  assert.equal(v.android.versionCode, 14);
  assert.equal(v.sig, undefined);
  const m = JSON.parse(FIX.manifest);
  delete m.sig;
  assert.equal(P.verifyManifest(JSON.stringify(m)).desktop.version, "0.3.0");
});

test("bad contents are refused as \"manifest\"", () => {
  const ok = { version: "1.0.0", file: "a.zip", sha256: "a".repeat(64), size: 10 };
  const bad = [
    { schema: 2, desktop: ok },
    { schema: 1, desktop: { ...ok, file: "../a.zip" } },
    { schema: 1, desktop: { ...ok, file: "update.json" } },
    { schema: 1, desktop: { ...ok, sha256: "A".repeat(64) } },
    { schema: 1, desktop: { ...ok, size: 0 } },
    { schema: 1, desktop: { ...ok, size: 1.5 } },
    { schema: 1, desktop: { ...ok, version: "1.0.0-beta" } },
    { schema: 1, android: ok }, // no versionCode
  ];
  for (const m of bad) assert.throws(() => P.verifyManifest(JSON.stringify(m)), (e) => e.code === "manifest", JSON.stringify(m));
  for (const t of ["not json", "[]", "x".repeat(P.MAX_MANIFEST + 1)]) assert.throws(() => P.verifyManifest(t), (e) => e.code === "manifest");
  assert.equal(P.verifyManifest(JSON.stringify({ schema: 1, desktop: ok })).desktop.size, 10);
});

test("version compare", () => {
  assert.equal(P.compareVersions("0.3.0", "0.2.0"), 1);
  assert.equal(P.compareVersions("0.10.0", "0.9.9"), 1);
  assert.equal(P.compareVersions("1.2", "1.2.0"), 0);
  assert.equal(P.compareVersions("1.1.11", "1.2.0"), -1);
});

test("HTTP outcomes: offline, rate limit and no release are quiet kinds", () => {
  assert.equal(P.httpProblem(undefined), "offline");
  assert.equal(P.httpProblem(403), "ratelimit");
  assert.equal(P.httpProblem(429), "ratelimit");
  assert.equal(P.httpProblem(404), "norelease");
  assert.equal(P.httpProblem(500), "http");
  assert.equal(P.httpProblem(200), null);
});

test("release assets: https download URLs only, release page kept on github.com", () => {
  const r = P.releaseAssets({
    tag_name: "r1",
    html_url: "https://github.com/int11/opengrand/releases/tag/r1",
    assets: [{ name: "update.json", browser_download_url: "https://x/u", size: 9 }, { name: "evil", browser_download_url: "http://x/e" }],
  });
  assert.equal(r.assets.get("update.json").url, "https://x/u");
  assert.equal(r.assets.has("evil"), false);
  assert.equal(P.releaseAssets({ assets: [], html_url: "https://evil.example/" }).page, P.RELEASES_PAGE);
  assert.throws(() => P.releaseAssets({ message: "Not Found" }));
});

// ---------------------------------------------------------------- swap plan

test("update plan: unpacked build swaps folders and keeps the launch options", () => {
  const plan = P.updatePlan({
    platform: "win32",
    packaged: true,
    execPath: "D:\\Apps\\OpenGrand\\OpenGrand\\OpenGrand.exe",
    argv: ["D:\\Apps\\OpenGrand\\OpenGrand\\OpenGrand.exe", "--cdp=9222", "--automation", "--query=log"],
  });
  assert.equal(plan.kind, "dir");
  assert.equal(plan.installDir, "D:\\Apps\\OpenGrand\\OpenGrand");
  assert.equal(plan.newDir, "D:\\Apps\\OpenGrand\\OpenGrand.new");
  assert.equal(plan.oldDir, "D:\\Apps\\OpenGrand\\OpenGrand.old");
  assert.deepEqual(plan.args, ["--cdp=9222", "--automation", "--query=log"]);
  const env = P.helperEnv(plan, { pid: 42, log: "L", result: "R", relaunch: true });
  assert.equal(env.OG_PID, "42");
  assert.equal(env.OG_ARGC, "3");
  assert.equal(env.OG_ARG0, "--cdp=9222");
  assert.equal(env.OG_ARG2, "--query=log");
  assert.equal(env.OG_RELAUNCH, "1");
  assert.equal(env.OG_EXE, plan.exe);
  assert.equal(P.helperEnv(plan, { pid: 1, log: "", result: "", relaunch: false }).OG_RELAUNCH, "0");
});

test("update plan: portable exe, dev run, other platforms and a drive root only tell the user", () => {
  const base = { platform: "win32", packaged: true, execPath: "C:\\a\\OpenGrand\\OpenGrand.exe", argv: [] };
  assert.deepEqual(P.updatePlan({ ...base, portableFile: "C:\\d\\OpenGrand-portable.exe" }), { kind: "manual", reason: "portable" });
  assert.deepEqual(P.updatePlan({ ...base, packaged: false }), { kind: "manual", reason: "dev" });
  assert.deepEqual(P.updatePlan({ ...base, platform: "linux" }), { kind: "manual", reason: "platform" });
  assert.deepEqual(P.updatePlan({ ...base, execPath: "C:\\OpenGrand.exe" }), { kind: "manual", reason: "root" });
});

test("the helper script swaps by renaming, rolls back, relaunches, and takes everything from the environment", () => {
  const s = P.HELPER_PS1;
  for (const v of ["OG_PID", "OG_INSTALL", "OG_NEW", "OG_OLD", "OG_EXE", "OG_RESULT", "OG_RELAUNCH", "OG_ARGC"]) assert.ok(s.includes(`$env:${v}`), v);
  assert.match(s, /Wait-Process/);
  assert.match(s, /Rename-Item -LiteralPath \$inst/);
  assert.match(s, /Rename-Item -LiteralPath \$old -NewName \(Split-Path \$inst -Leaf\)/); // rollback
  assert.match(s, /Start-Process -FilePath \$exe/);
  assert.ok(!/[^\x09\x0a\x0d\x20-\x7e]/.test(s), "ASCII only (PowerShell 5.1 code page)");
});

// ---------------------------------------------------------------- unzip

test("zip: entry names that could escape the folder are refused", () => {
  for (const bad of ["../x", "a/../../x", "/abs", "C:/x", "a\\..\\x", "", "./x"]) assert.equal(P.safeEntryName(bad), null, bad);
  assert.equal(P.safeEntryName("resources/app/index.html"), "resources/app/index.html");
});

test("zip: extracts stored and deflated entries, checks CRC, refuses traversal", async () => {
  const d = tmp();
  const zip = makeZip([["OpenGrand.exe", "MZ exe", false], ["resources/", "", false], ["resources/app/index.html", "<html>".repeat(100), true]]);
  assert.equal(listZip(zip).length, 3);
  assert.equal(await extractZip(zip, path.join(d, "out")), 2);
  assert.equal(fs.readFileSync(path.join(d, "out", "resources", "app", "index.html"), "utf8"), "<html>".repeat(100));
  await assert.rejects(extractZip(makeZip([["../evil.txt", "x", false]]), path.join(d, "evil")), /unsafe/);
  assert.equal(fs.existsSync(path.join(d, "evil")), false);
  const broken = makeZip([["a.txt", "hello", false]]);
  broken[30 + 5] ^= 1; // flip a byte of the stored data
  await assert.rejects(extractZip(broken, path.join(d, "broken")), /CRC/);
  fs.rmSync(d, { recursive: true, force: true });
});

// ---------------------------------------------------------------- updater with a fake GitHub

function fakeGithub({ manifest, files = {}, latestStatus = 200, throwAll = false }) {
  const calls = [];
  const fetch = async (url) => {
    calls.push(url);
    if (throwAll) throw new TypeError("fetch failed");
    const respond = (status, body) => ({
      status,
      json: async () => JSON.parse(body),
      text: async () => body,
      body: { getReader: () => { let sent = false; return { read: async () => (sent ? { done: true } : ((sent = true), { done: false, value: new Uint8Array(Buffer.from(body)) })), cancel: async () => {} }; } },
    });
    if (url === P.LATEST_URL) {
      if (latestStatus !== 200) return respond(latestStatus, "{}");
      return respond(200, JSON.stringify({
        tag_name: "r1",
        html_url: "https://github.com/int11/opengrand/releases/tag/r1",
        assets: [{ name: "update.json", browser_download_url: "https://dl/update.json" }, ...Object.keys(files).map((n) => ({ name: n, browser_download_url: `https://dl/${n}` }))],
      }));
    }
    if (url === "https://dl/update.json") return respond(200, manifest);
    const n = url.replace("https://dl/", "");
    if (files[n] !== undefined) return respond(200, files[n]);
    return respond(404, "");
  };
  return { fetch, calls };
}

function setup(over = {}) {
  const root = tmp();
  const plan = { kind: "dir", installDir: path.join(root, "OpenGrand"), exe: path.join(root, "OpenGrand", "OpenGrand.exe"), newDir: path.join(root, "OpenGrand.new"), oldDir: path.join(root, "OpenGrand.old"), args: ["--cdp=9222"] };
  fs.mkdirSync(plan.installDir);
  const zip = makeZip([["OpenGrand.exe", "new exe", false], ["resources/app/index.html", "new app", true]]);
  const zipText = zip.toString("latin1");
  const entry = { version: "0.3.0", file: "OpenGrand-0.3.0-win-x64.zip", sha256: sha256(zip), size: zip.length, ...over.entry };
  const manifest = JSON.stringify({ schema: 1, desktop: entry });
  const spawned = [];
  let quits = 0;
  const gh = fakeGithub({ manifest, files: { [entry.file]: over.zipText ?? zipText }, ...over.gh });
  // the fake body sends a string; keep bytes intact by sending latin1 text back as bytes
  const fetch = async (url, init) => {
    const r = await gh.fetch(url, init);
    if (url.endsWith(".zip")) {
      const bytes = Buffer.from(over.zipText ?? zipText, "latin1");
      r.body = { getReader: () => { let sent = false; return { read: async () => (sent ? { done: true } : ((sent = true), { done: false, value: new Uint8Array(bytes) })), cancel: async () => {} }; } };
    }
    return r;
  };
  const mk = (extra = {}) => createUpdater({
    fetch,
    currentVersion: "0.2.0",
    plan,
    dir: path.join(root, "userData", "update"),
    spawnHelper: (script, env, cwd) => spawned.push({ script, env, cwd }),
    quit: () => quits++,
    ...extra,
  });
  return { root, plan, mk, spawned, quits: () => quits, calls: gh.calls };
}

test("updater: offline, rate limit and no release end quietly with their kind", async () => {
  for (const [gh, kind] of [[{ throwAll: true }, "offline"], [{ latestStatus: 403 }, "ratelimit"], [{ latestStatus: 404 }, "norelease"]]) {
    const s = setup({ gh });
    const st = await s.mk().check();
    assert.equal(st.phase, "error");
    assert.equal(st.error, kind);
    assert.equal(fs.existsSync(s.plan.newDir), false);
    fs.rmSync(s.root, { recursive: true, force: true });
  }
});

test("updater: newer release is downloaded, verified, staged; restart starts the helper with the same args", async () => {
  const s = setup();
  const u = s.mk();
  const phases = [];
  u.onChange((st) => phases.push(st.phase));
  const st = await u.check();
  assert.equal(st.phase, "ready", JSON.stringify(st));
  assert.equal(st.action, "restart");
  assert.equal(st.available, "0.3.0");
  assert.ok(phases.includes("downloading"));
  assert.equal(fs.readFileSync(path.join(s.plan.newDir, "OpenGrand.exe"), "utf8"), "new exe");
  assert.equal(fs.readdirSync(path.join(s.root, "userData", "update")).filter((f) => f.endsWith(".zip") || f.endsWith(".part")).length, 0);
  // a second check does not download again
  const n = s.calls.length;
  await u.check();
  assert.equal(s.calls.length, n);
  // a new start offers the staged copy at once
  assert.equal(s.mk().state().phase, "ready");
  assert.equal(await u.apply({ relaunch: true }), true);
  assert.equal(u.apply({ relaunch: false }), false); // once
  assert.equal(s.spawned.length, 1);
  assert.equal(s.spawned[0].env.OG_ARG0, "--cdp=9222");
  assert.equal(s.spawned[0].env.OG_NEW, s.plan.newDir);
  assert.equal(s.quits(), 1);
  assert.ok(fs.readFileSync(s.spawned[0].script, "utf8").includes("Rename-Item"));
  fs.rmSync(s.root, { recursive: true, force: true });
});

test("updater: restart quits only after spawnHelper's promise settles", async () => {
  const s = setup();
  let release;
  const u = s.mk({ spawnHelper: () => new Promise((r) => (release = r)) });
  assert.equal((await u.check()).phase, "ready");
  const p = u.apply({ relaunch: true });
  await new Promise((r) => setTimeout(r, 20));
  assert.equal(s.quits(), 0);
  release();
  assert.equal(await p, true);
  assert.equal(s.quits(), 1);
  fs.rmSync(s.root, { recursive: true, force: true });
});

test("updater: a quit while the restart's helper is still starting waits for it (no second helper)", async () => {
  const s = setup();
  let release;
  let starts = 0;
  const u = s.mk({ spawnHelper: () => { starts++; return new Promise((r) => (release = r)); } });
  assert.equal((await u.check()).phase, "ready");
  const p = u.apply({ relaunch: true });
  await new Promise((r) => setTimeout(r, 20));
  // the window is closed meanwhile: will-quit asks again and must get something to wait for
  const q = u.apply({ relaunch: false });
  assert.ok(q && typeof q.then === "function", "a quit during the start gets the running start to wait for");
  let settled = false;
  q.then(() => (settled = true));
  await new Promise((r) => setTimeout(r, 20));
  assert.equal(settled, false);
  release();
  assert.equal(await q, true);
  assert.equal(await p, true);
  assert.equal(starts, 1);
  assert.equal(s.quits(), 1);
  assert.equal(u.apply({ relaunch: false }), false); // started: nothing left to wait for
  fs.rmSync(s.root, { recursive: true, force: true });
});

test("updater: a download whose sha256 does not match is refused and nothing is staged", async () => {
  const bad = makeZip([["OpenGrand.exe", "evil exe", false]]);
  const s = setup({ zipText: Buffer.concat([bad, Buffer.alloc(0)]).toString("latin1"), entry: { size: bad.length } });
  const st = await s.mk().check();
  assert.equal(st.phase, "error");
  assert.equal(st.error, "sha256");
  assert.equal(fs.existsSync(s.plan.newDir), false);
  assert.equal(s.spawned.length, 0);
  fs.rmSync(s.root, { recursive: true, force: true });
});

test("updater: a download longer than update.json says is cut off", async () => {
  const s = setup({ entry: { size: 10 } });
  const st = await s.mk().check();
  assert.equal(st.error, "size");
  assert.equal(fs.existsSync(s.plan.newDir), false);
  fs.rmSync(s.root, { recursive: true, force: true });
});

test("updater: a malformed update.json is refused before any download", async () => {
  const s = setup({ gh: { manifest: '{"schema":2}' } });
  const st = await s.mk().check();
  assert.equal(st.error, "manifest");
  assert.ok(!s.calls.some((u) => u.endsWith(".zip")));
  fs.rmSync(s.root, { recursive: true, force: true });
});

test("updater: same or older version is up to date; portable / dev only tell the user", async () => {
  const s = setup();
  assert.equal((await s.mk({ currentVersion: "0.3.0" }).check()).phase, "uptodate");
  const m = await s.mk({ plan: { kind: "manual", reason: "portable" } }).check();
  assert.equal(m.phase, "ready");
  assert.equal(m.action, "manual");
  assert.ok(!s.calls.some((u) => u.endsWith(".zip")));
  fs.rmSync(s.root, { recursive: true, force: true });
});

test("updater: an install folder that cannot be written next to is told to the user, nothing downloaded", { skip: process.getuid && process.getuid() === 0 }, async () => {
  const s = setup();
  const parent = path.dirname(s.plan.newDir);
  fs.chmodSync(parent, 0o555);
  try {
    const st = await s.mk().check();
    assert.equal(st.phase, "ready");
    assert.equal(st.action, "manual");
    assert.equal(st.reason, "readonly");
    assert.ok(!s.calls.some((u) => u.endsWith(".zip")));
  } finally {
    fs.chmodSync(parent, 0o755);
    fs.rmSync(s.root, { recursive: true, force: true });
  }
});

test("updater: after the swap the new version reads the helper's result and drops the staged copy", async () => {
  const s = setup();
  await s.mk().check();
  fs.writeFileSync(path.join(s.root, "userData", "update", "result.json"), '\uFEFF{"state":"ok","error":""}');
  const u = s.mk({ currentVersion: "0.3.0" });
  assert.deepEqual(u.state().lastResult, { state: "ok", error: "" });
  assert.equal(u.state().phase, "idle");
  assert.equal(fs.existsSync(s.plan.newDir), false);
  assert.equal(fs.existsSync(path.join(s.root, "userData", "update", "staged.json")), false);
  fs.rmSync(s.root, { recursive: true, force: true });
});
