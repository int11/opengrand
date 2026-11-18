#!/usr/bin/env python3
"""Run independent check commands in parallel (root Makefile `check` / `check-full`).

  python3 -I scripts/run_checks.py [-j N] [--logs DIR] [--name STAGE] 'cmd 1' 'label::cmd 2' ...

Each command runs through the shell from the repo root, its output goes to DIR/<name>.log (default .check/);
<name> is the label before "::", else derived from the command (tools/roundtrip_power.py ota_main -> roundtrip_power.ota_main,
`--shard 2/4` -> .2of4). One line per finished command: ok/FAIL, seconds, peak RSS, the [PASS]/[FAIL] lines it printed,
its name. At the end a time table (longest first, shards of one tool summed per tool), the full log of every failed
command, and exit status 1 if any failed.

Scheduling: DIR/timings.json keeps each job's last time and peak RSS (the largest process of its tree, from wait4).
Longest first (LPT: unknown jobs first, then by last time); a job starts when fewer than N run and its RSS fits the
memory budget next to the running ones (MemAvailable at start x 0.8; one job always runs). N defaults to the CPU
count; NPROC=1 = one after the other.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
PASS_RE = re.compile(r"^\s*(\[PASS\]|PASS\b)", re.M)
FAIL_RE = re.compile(r"^\s*(\[FAIL\]|FAIL\b)", re.M)
SKIP_RE = re.compile(r"^\s*SKIP\b.*$", re.M)      # a job (or part of one) skipped: shown after its result line
SHARD_RE = re.compile(r"\.(\d+)of(\d+)$")
DEFAULT_RSS_MB = 1024          # a job never measured: assume this much until it has run once
MEM_SHARE = 0.8


def job_name(cmd):
    """'cd result/stockfw && uv run python -I tools/roundtrip_power.py ota_main -q' -> 'roundtrip_power.ota_main';
    '... tools/roundtrip_session.py -q --shard 2/4' -> 'roundtrip_session.2of4'."""
    m = re.search(r"tools/(\w+)\.py((?:\s+[^-\s][^\s]*)*)", cmd)
    if m:
        args = [re.sub(r"[^\w.-]", "", os.path.basename(a)) for a in m.group(2).split()]
        name = ".".join([m.group(1)] + [x for x in args if x])
        sh = re.search(r"--shard\s+(\d+)/(\d+)", cmd)
        return name + (f".{sh[1]}of{sh[2]}" if sh else "")
    return re.sub(r"[^\w.-]+", "_", cmd.strip())[:60].strip("_")


def mem_available_mb():
    try:
        with open("/proc/meminfo") as fh:
            for line in fh:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) // 1024
    except OSError:
        pass
    return None


def load_timings(path):
    try:
        t = json.load(open(path))
        return {k: v for k, v in t.items() if isinstance(v, dict)}
    except (OSError, ValueError):
        return {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-j", type=int, default=0, help="jobs at a time (default: CPU count, within the memory budget)")
    ap.add_argument("--logs", default=os.path.join(ROOT, ".check"))
    ap.add_argument("--name", default="checks")
    ap.add_argument("cmds", nargs="+")
    a = ap.parse_args()
    os.makedirs(a.logs, exist_ok=True)
    tpath = os.path.join(a.logs, "timings.json")
    timings = load_timings(tpath)
    jobs, seen = [], set()
    for c in a.cmds:
        label, sep, rest = c.partition("::")
        if sep and re.fullmatch(r"[\w.-]+", label):
            n, c = label, rest
        else:
            n = job_name(c)
        while n in seen:
            n += "_"
        seen.add(n)
        jobs.append((n, c))

    known_rss = [v["rss_mb"] for v in timings.values() if v.get("rss_mb")]
    guess = max(known_rss) if known_rss else DEFAULT_RSS_MB
    rss_of = {n: timings.get(n, {}).get("rss_mb") or guess for n, _c in jobs}
    sec_of = {n: timings.get(n, {}).get("s") for n, _c in jobs}
    order = sorted(jobs, key=lambda j: -(sec_of[j[0]] if sec_of[j[0]] is not None else 1e9))   # LPT, unknown first
    cpus = os.cpu_count() or 1
    width = max(1, a.j or cpus)
    avail = mem_available_mb()
    budget = int(avail * MEM_SHARE) if avail else None
    print(f"{a.name}: {len(jobs)} commands, up to {min(width, len(jobs))} at a time"
          + (f", memory budget {budget} MB" if budget else "") + f", logs {os.path.relpath(a.logs, ROOT)}/", flush=True)

    pending = list(order)
    running = {}                                   # pid -> (name, Popen, t0, log file)
    done, failed = {}, []
    t_start = time.monotonic()
    peak_jobs = peak_mem = 0
    while pending or running:
        used = sum(rss_of[v[0]] for v in running.values())
        while pending and len(running) < width:
            k = next((i for i, (n, _c) in enumerate(pending)
                      if not running or budget is None or used + rss_of[n] <= budget), None)
            if k is None:
                break
            n, c = pending.pop(k)
            fh = open(os.path.join(a.logs, n + ".log"), "wb")
            p = subprocess.Popen(c, shell=True, cwd=ROOT, stdout=fh, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
            running[p.pid] = (n, p, time.monotonic(), fh)
            used += rss_of[n]
            peak_jobs, peak_mem = max(peak_jobs, len(running)), max(peak_mem, used)
        pid, status, ru = os.wait4(-1, 0)
        if pid not in running:
            continue
        n, p, t0, fh = running.pop(pid)
        fh.close()
        dt = time.monotonic() - t0
        rc = p.returncode = os.waitstatus_to_exitcode(status)     # reaped here: Popen must not wait for it again
        rss = ru.ru_maxrss // 1024                 # KB -> MB (the largest process of the job's tree)
        text = open(os.path.join(a.logs, n + ".log"), encoding="utf-8", errors="replace").read()
        np_, nf = len(PASS_RE.findall(text)), len(FAIL_RE.findall(text))
        cpu = ru.ru_utime + ru.ru_stime            # CPU of the job's whole tree (every reaped descendant)
        done[n] = (rc, dt, np_, nf, rss, cpu)
        if rc == 0:                               # a failed run's time / size says little about the next one
            timings[n] = {"s": round(dt, 1), "rss_mb": rss, "cpu": round(cpu, 1)}
            rss_of[n] = rss
        else:
            failed.append(n)
        print(f"  {'ok  ' if rc == 0 else 'FAIL'} {dt:6.1f}s {cpu:6.1f}s cpu {rss:5d} MB  pass {np_:3d}  fail {nf:2d}  {n}",
              flush=True)
        for sk in sorted(set(m.strip() for m in SKIP_RE.findall(text)))[:3]:
            print(f"         {sk}", flush=True)
    wall = time.monotonic() - t_start
    try:
        with open(tpath, "w") as fh:
            json.dump(timings, fh, indent=1, sort_keys=True)
    except OSError:
        pass

    # per-tool table: the shards of one tool (name.<i>of<n>) summed - PASS lines added, time = slowest shard
    tools = {}
    for n, (rc, dt, np_, nf, rss, cpu) in done.items():
        base = SHARD_RE.sub("", n)
        t = tools.setdefault(base, [0, 0.0, 0.0, 0, 0, 0, 0.0])
        t[0] += 1
        t[1] = max(t[1], dt)
        t[2] += dt
        t[3] += np_
        t[4] = max(t[4], rss)
        t[5] |= rc != 0
        t[6] += cpu
    print(f"\n  {'time':>6}  {'summed':>6}  {'cpu':>6}  {'MB':>5}  {'pass':>4}  job (shards)")
    for base, (k, mx, tot, np_, rss, bad, cpu) in sorted(tools.items(), key=lambda x: -x[1][1]):
        print(f"  {mx:5.1f}s  {tot:5.1f}s  {cpu:5.1f}s  {rss:5d}  {np_:4d}  {base}" + (f" ({k})" if k > 1 else "")
              + ("  FAIL" if bad else ""))
    for n in failed:
        print(f"\n===== FAILED: {n} (exit {done[n][0]}) =====")
        sys.stdout.write(open(os.path.join(a.logs, n + ".log"), encoding="utf-8", errors="replace").read())
    total = sum(v[2] for v in done.values())
    summed = sum(v[1] for v in done.values())
    cpu = sum(v[5] for v in done.values())
    print(f"{a.name}: {len(jobs) - len(failed)}/{len(jobs)} ok, {total} PASS lines, {wall:.1f}s wall "
          f"({summed:.0f}s summed, {cpu:.0f}s cpu, at most {peak_jobs} at once, ~{peak_mem} MB planned)"
          + (f" - FAILED: {' '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
