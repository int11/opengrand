// Online self-update of the desktop app: check the latest GitHub release, verify its signed update.json, download and
// verify the Windows zip, extract it next to the install folder, and on restart (or quit) let a detached helper swap the
// folders. Format, key and swap rules: ./updatepolicy.cjs. Electron objects come in as arguments (fetch, spawn, quit),
// so tests/updater.test.cjs drives this module under plain Node with fakes.
"use strict";

const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const P = require("./updatepolicy.cjs");
const { extractZip } = require("./unzip.cjs");

const HEADERS = { accept: "application/vnd.github+json", "user-agent": "OpenGrand-desktop-updater", "x-github-api-version": "2022-11-28" };

/**
 * @param {object} o
 *   fetch(url, init)        WHATWG fetch (Electron net.fetch in the app)
 *   currentVersion          app.getVersion()
 *   plan                    P.updatePlan(...)
 *   dir                     folder for downloads, markers and the helper (userData/update)
 *   spawnHelper(script, env, cwd)   start the swap helper detached
 *   quit()                  quit the app (after the helper is started)
 *   pubKey                  base64 raw Ed25519 key (default P.UPDATE_PUBKEY; tests pass their own)
 *   log(...)
 */
function createUpdater(o) {
  const log = o.log || (() => {});
  const dir = o.dir;
  const stagedFile = path.join(dir, "staged.json");
  const resultFile = path.join(dir, "result.json");
  const listeners = new Set();
  let st = { phase: "idle", current: o.currentVersion, kind: o.plan.kind, reason: o.plan.reason };
  let running = null;

  const set = (patch) => {
    st = { ...st, ...patch };
    for (const l of listeners) {
      try {
        l(st);
      } catch {
        /* a listener's problem is not the updater's */
      }
    }
  };
  const fail = (code, message) => Object.assign(new Error(message || code), { code });

  async function get(url, accept) {
    let res;
    try {
      res = await o.fetch(url, { headers: accept ? { ...HEADERS, accept } : HEADERS, redirect: "follow" });
    } catch (e) {
      throw fail("offline", e && e.message);
    }
    const problem = P.httpProblem(res.status);
    if (problem) throw fail(problem, `HTTP ${res.status}`);
    return res;
  }

  function readStaged() {
    try {
      const s = JSON.parse(fs.readFileSync(stagedFile, "utf8"));
      return s && typeof s.version === "string" && typeof s.newDir === "string" ? s : null;
    } catch {
      return null;
    }
  }

  function stagedReady(version) {
    const s = readStaged();
    return !!s && s.version === version && o.plan.kind === "dir" && s.newDir === o.plan.newDir && fs.existsSync(path.join(s.newDir, path.win32.basename(o.plan.exe)));
  }

  /** On start: what the last swap did, leftovers of an applied update cleaned, a staged update offered again. */
  function init() {
    try {
      const r = JSON.parse(fs.readFileSync(resultFile, "utf8").replace(/^﻿/, ""));
      fs.rmSync(resultFile, { force: true });
      if (r && (r.state === "ok" || r.state === "failed")) st.lastResult = { state: r.state, error: typeof r.error === "string" ? r.error : "" };
    } catch {
      /* no swap since the last start */
    }
    const s = readStaged();
    if (!s) return;
    if (P.compareVersions(s.version, o.currentVersion) <= 0) {
      // applied (or older than what runs): the staged copy and its marker are spent
      if (o.plan.kind === "dir" && s.newDir === o.plan.newDir) fs.rmSync(s.newDir, { recursive: true, force: true });
      fs.rmSync(stagedFile, { force: true });
    } else if (stagedReady(s.version)) {
      st = { ...st, phase: "ready", available: s.version, action: "restart" };
    }
  }

  async function download(asset, entry) {
    fs.mkdirSync(dir, { recursive: true });
    const part = path.join(dir, `${entry.file}.part`);
    const res = await get(asset.url, "application/octet-stream");
    const hash = crypto.createHash("sha256");
    const out = fs.openSync(part, "w");
    let got = 0;
    try {
      const reader = res.body.getReader();
      let lastPct = -1;
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        got += value.length;
        if (got > entry.size) {
          await reader.cancel().catch(() => {});
          throw fail("size", "download larger than update.json says");
        }
        hash.update(value);
        fs.writeSync(out, value);
        const pct = Math.floor((got * 100) / entry.size);
        if (pct !== lastPct) set({ progress: (lastPct = pct) });
      }
    } catch (e) {
      fs.closeSync(out);
      fs.rmSync(part, { force: true });
      throw e.code ? e : fail("offline", e.message);
    }
    fs.closeSync(out);
    if (got !== entry.size) {
      fs.rmSync(part, { force: true });
      throw fail("size", `got ${got} of ${entry.size} bytes`);
    }
    if (hash.digest("hex") !== entry.sha256) {
      fs.rmSync(part, { force: true });
      throw fail("sha256", "download does not match update.json");
    }
    return part;
  }

  async function stage(zipFile, entry) {
    const plan = o.plan;
    fs.rmSync(plan.newDir, { recursive: true, force: true });
    try {
      await extractZip(fs.readFileSync(zipFile), plan.newDir);
    } catch (e) {
      fs.rmSync(plan.newDir, { recursive: true, force: true });
      throw fail(e.code === "EACCES" || e.code === "EPERM" ? "write" : "extract", e.message);
    } finally {
      fs.rmSync(zipFile, { force: true });
    }
    if (!fs.existsSync(path.join(plan.newDir, path.win32.basename(plan.exe)))) {
      fs.rmSync(plan.newDir, { recursive: true, force: true });
      throw fail("extract", `${path.win32.basename(plan.exe)} is not at the top of the zip`);
    }
    fs.writeFileSync(stagedFile, JSON.stringify({ version: entry.version, sha256: entry.sha256, newDir: plan.newDir }));
  }

  async function run() {
    set({ phase: "checking", error: undefined, progress: undefined });
    const rel = await (await get(P.LATEST_URL)).json();
    const { assets, page } = P.releaseAssets(rel);
    const mf = assets.get(P.MANIFEST_NAME);
    if (!mf) throw fail("norelease", "the latest release has no update.json");
    const text = await (await get(mf.url, "application/octet-stream")).text();
    const m = P.verifyManifest(text, o.pubKey || P.UPDATE_PUBKEY);
    const d = m.desktop;
    if (!d || P.compareVersions(d.version, o.currentVersion) <= 0) return set({ phase: "uptodate", available: undefined, action: undefined, page, checkedAt: Date.now() });
    set({ available: d.version, page });
    if (o.plan.kind !== "dir") return set({ phase: "ready", action: "manual", checkedAt: Date.now() });
    if (stagedReady(d.version)) return set({ phase: "ready", action: "restart", checkedAt: Date.now() });
    const asset = assets.get(d.file);
    if (!asset) throw fail("manifest", `the release has no ${d.file}`);
    set({ phase: "downloading", progress: 0 });
    const zip = await download(asset, d);
    await stage(zip, d);
    log(`update ${d.version} staged in ${o.plan.newDir}`);
    set({ phase: "ready", action: "restart", progress: undefined, checkedAt: Date.now() });
  }

  /** Check (and download when newer). Never throws; the outcome is in the state. */
  function check() {
    if (st.phase === "ready" && st.action === "restart") return Promise.resolve(st);
    if (!running) {
      running = run()
        .catch((e) => {
          const code = e.code || "error";
          log(`update check: ${code} ${e.message || ""}`);
          set({ phase: "error", error: code, message: e.message, progress: undefined, checkedAt: Date.now() });
        })
        .finally(() => (running = null))
        .then(() => st);
    }
    return running;
  }

  let applied = false;
  /** Start the swap helper; with `relaunch` it starts the new version and the app quits now. Returns false when nothing is staged. */
  function apply({ relaunch }) {
    if (applied || st.phase !== "ready" || st.action !== "restart" || o.plan.kind !== "dir") return false;
    applied = true;
    const script = path.join(dir, "apply.ps1");
    fs.writeFileSync(script, "﻿" + P.HELPER_PS1); // BOM: Windows PowerShell 5.1 reads a BOM-less script as ANSI
    const env = P.helperEnv(o.plan, { pid: process.pid, log: path.join(dir, "apply.log"), result: resultFile, relaunch });
    o.spawnHelper(script, env, dir);
    log(`update helper started (${relaunch ? "restart" : "on quit"})`);
    if (relaunch) o.quit();
    return true;
  }

  init();
  return {
    state: () => st,
    check,
    apply,
    onChange(l) {
      listeners.add(l);
      return () => listeners.delete(l);
    },
  };
}

module.exports = { createUpdater };
