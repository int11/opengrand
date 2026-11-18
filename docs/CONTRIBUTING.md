# Contributing to OpenGrand

Thanks for helping. Bug reports, test reports from other models (CA99, NV5S, NV10S), translations, app and
firmware changes are all welcome.

## Contributor License Agreement (required)

Before a pull request can be merged you must sign the [OpenGrand Individual CLA](CLA.md). **CLA Assistant**
posts a comment on your first pull request; signing takes one click with your GitHub account and covers all
your future contributions.

Why a CLA and not just a DCO sign-off: OpenGrand is dual licensed (GPLv3 for everyone, plus commercial
licenses - see [COMMERCIAL.md](COMMERCIAL.md)). A `Signed-off-by` line (Developer Certificate of Origin) only
certifies that you may submit the code under the project's open-source license; it does not give the
project the right to also license your code under other terms. The CLA does that, while **you keep your
copyright**, and it guarantees that your contribution stays available under the GPL.

## Never commit

This repository is public and must stay free of third-party and personal material. **Never commit:**

- Kawai firmware or anything derived from it: `*.SYS`, slot payloads (`*.c79`, `*.c79u`), NOR dumps
  (`CA79NOR*`), extracted resources, the panel app or its UI bundles, decompiled or disassembled Kawai code,
  or long byte sequences copied from Kawai's files;
- keys: `result/stockfw/secrets/`, `ota_key.bin`, panel UI keys, any private key, or anything that derives
  or recovers a Kawai key;
- personal data: Bluetooth/MAC addresses of real devices, serial numbers, IP addresses, home directory paths,
  user names, logs or captures from your own devices (use synthetic data, and locally administered
  addresses such as `02:00:00:00:00:01` in tests);
- `target/`, the maintainer's private research notes (`docs/` holds only the public guides; `scripts/leakcheck.py` refuses the notes layout), editor/agent folders (`.claude/`, `CLAUDE.md`).

### Leak check and pre-commit hook

`scripts/leakcheck.py` (Python standard library only) enforces the list above. It scans every tracked file
for forbidden paths, real-looking MAC addresses, private IP addresses, home paths, PEM/private key blocks,
unexplained 64-digit hex strings, vendor crypto names and links to the maintainer's private notes (which are not
part of this repository); and, if you have `target/CAx9_P06.SYS` locally, for content shared with Kawai's file:
any binary run of 16 bytes or more that is not allowlisted byte for byte, or a text string of 48 bytes or more that
is not an allowlisted interface fact (Bluetooth module commands and the like). Enable the hook once per clone:

```bash
git config core.hooksPath scripts/hooks
```

The hook runs `python3 scripts/leakcheck.py --cached` on what you are about to commit. CI runs the same check.
If a finding is a false positive (a public test vector, a documented constant), add it to
`scripts/leakcheck.allow` with a comment explaining why it is public - reviewers will check that entry.
Do not bypass the hook with `--no-verify`.

A binary run shared with Kawai's file can only be allowed one run at a time, byte for byte: a
`stock-bin:<path> <offset> <length> <sha256>` line names the file, the offset and length of the run as the leak
check reports it, and the sha256 of exactly those bytes. If the bytes at that offset change, the run grows or
moves, or the same bytes turn up in another file, the entry no longer matches and the check fails again. Such an
entry needs a comment saying what the bytes are (generic instruction sequences, not Kawai code) and why they cannot
change; there are none today (prefer changing the code over adding one).

## Development

Setup is described in [BUILDING.md](BUILDING.md). Useful commands from the repository root:

| Command | When |
|---|---|
| `make check` | before every pull request |
| `make check-full` | before anything is flashed, and for changes to `result/stockfw` or the extension code |
| `make -C result/firmware check` | firmware core: host tests, update container cross-check, simulator smoke test |
| `cd result/app && npm run typecheck && npm test` | remote app |
| `cd result/desktop && npm test` | desktop shell |
| `make gen` then `make gen-check` | after editing `result/protocol/spec/ca79.json` - never hand-edit generated files |
| `python3 scripts/leakcheck.py` | before pushing |

Without `target/CAx9_P06.SYS`, stock-dependent steps print `SKIP` - that is fine for app and core work.
Changes that affect the code running on the piano must pass the emulator round trips (`make check-full`,
which needs your own copy of Kawai's file).

### Guidelines

- Keep the "no Kawai content" rule: implement behaviour from your own understanding of interfaces and
  protocols; do not copy vendor code, strings, tables or UI assets.
- Code on the piano runs inside Kawai's firmware: no blocking, no new writes to stock state without a
  documented analysis, and every change covered by host tests and an emulator round trip.
- Firmware: C11, keep the host build free of warnings (`-Wall -Wextra -Wshadow`). App: TypeScript, no UI framework, tests with
  `node --test`.
- One logical change per pull request, with tests. Describe what you tested and on which model/system version.
- Hardware test reports are very valuable - please include the model, system version (V2.19), OpenGrand
  commit and app version, but no Bluetooth addresses or serial numbers.

## Reporting issues

Use GitHub issues. Mark anything that could damage an instrument or lose data with **[safety]**
(see [SAFETY.md](SAFETY.md)).
