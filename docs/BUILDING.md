# Building OpenGrand

Everything builds on **Linux x86_64** (a WSL2 distribution on Windows works). The app parts also build on
macOS/Windows, but the firmware image build expects Linux.

> Building is safe - nothing touches your piano until you flash, and flashing is a separate, deliberate
> step ([FLASHING.md](FLASHING.md)). Read [SAFETY.md](SAFETY.md) first anyway.

## 1. Prerequisites

| Tool | Needed for | Notes |
|---|---|---|
| `git`, `make`, host `gcc`, `python3` (3.10+) | everything | the firmware host tests and simulator use the host compiler |
| [uv](https://docs.astral.sh/uv/) | image build, emulator tests | Python dependencies are pinned in `result/stockfw/uv.lock`; never `pip install` them globally |
| **Arm GNU Toolchain 13.3.Rel1** (`x86_64` host, `arm-none-eabi`) | firmware images | exact version is enforced, see below |
| Node.js 22 (or newer) + npm | app, desktop, tests | |
| JDK 17 + Android SDK (platform 35) | Android app only | Gradle wrapper is included |

### Arm toolchain (pinned)

The extension code must compile to exactly the same bytes on every machine, so the build refuses any
other compiler. Download **Arm GNU Toolchain 13.3.Rel1**, archive
`arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi.tar.xz`, from Arm's
[GNU toolchain downloads page](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) and unpack it
under `result/stockfw/.toolchain/` (git-ignored):

```bash
mkdir -p result/stockfw/.toolchain
tar -xf arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi.tar.xz -C result/stockfw/.toolchain
make -C result/stockfw/cave toolchain-check
```

`toolchain-check` compares `arm-none-eabi-gcc --version` and the SHA-256 of every binary that touches the
output (`result/stockfw/cave/toolchain.sha256`). It runs automatically before every image build.

### Python environment

```bash
cd result/stockfw && uv sync && cd ../..
```

## 2. Your copy of Kawai's system update

OpenGrand needs Kawai's official **CA99/CA79 system software update V2.19**. Download it yourself from
Kawai's official website (the support/download page for the CA99/CA79 in your region), unzip it, and put
the system file in `target/` at the repository root:

```bash
mkdir -p target
cp /path/to/unzipped/CAx9_P06.SYS target/
sha256sum target/CAx9_P06.SYS
# 09ad85703e44ad95d82475cfe2dae14c6edfa9f164a0b17b66b45f74c8d933c8  target/CAx9_P06.SYS   (3,801,088 bytes)
```

Every tool checks this digest and refuses any other file. `target/` is git-ignored and
`scripts/leakcheck.py` refuses to let it (or any `.SYS` file) be committed. Never share this file or anything
built from it.

## 3. Keys

Two keys are created on your machine the first time they are needed. They are yours alone; keep a backup
somewhere safe outside the repository, and never commit or share them.

| Key | Created by | Used for |
|---|---|---|
| `result/stockfw/secrets/ota_key.bin` (32 random bytes) | the first `make image` | built into your USB image; signs every Bluetooth update for **your** piano |
| panel UI signing key | `cd result/app && npm run panelui:key` | only for the advanced replacement panel UI |

Show the Bluetooth update key id at any time:

```bash
cd result/stockfw && uv run python -I tools/ota_pack.py key
```

If you lose `ota_key.bin`, your piano will refuse Bluetooth updates signed with a new key; you would have to
install a new USB image built with the new key ([FLASHING.md](FLASHING.md)). Anyone who has your key and is in
Bluetooth range could upload code to your piano.

## 4. Build and check

From the repository root:

| Command | What it does |
|---|---|
| `make check` | fast checks: generated files fresh, image build + check, firmware host tests, app typecheck + tests, desktop tests, quick emulator round trips (in parallel) |
| `make check-full` | everything, including the slow emulator round trips - run this before flashing anything |
| `make image` | builds the images `opengrand_install.SYS`, `ota_stage1.c79` and `ota_main.c79` (plus `ota_loader_v4.SYS`, stage-0 alone, which the install image is built from, and the emulator test payloads) into `result/stockfw/build/` (toolchain check, extension code, patch, verify) |
| `make image-check` | `make image`, then compares every image with `result/stockfw/ARTIFACTS.md` |
| `make gen` / `make gen-check` | regenerate the parameter tables from `result/protocol/spec/ca79.json` / fail if they are stale |
| `make fw`, `make app`, `make desktop` | one area only |
| `python3 scripts/leakcheck.py` | publishing safety check (see [CONTRIBUTING.md](CONTRIBUTING.md)) |

**Without `target/CAx9_P06.SYS`** (for example in CI, or when you only work on the app), every step that
needs Kawai's file - `make image`, `make image-check` and the emulator round trips - prints `SKIP ...` and
does not fail (the leak check notes that it skipped its stock-overlap scan). The firmware host tests, app,
desktop and generator checks run normally.

Outputs (git-ignored, **never distribute**):

- `result/stockfw/build/opengrand_install.SYS` - the USB image for the one-time install, the only file you put on
  the stick: the fixed loader part (stage-0) with loader slot A already holding stage-1 and main slot A already holding
  `ota_main`, both signed with your key
- `result/stockfw/build/ota_main.c79` - the signed slot payload for later Bluetooth updates (the other `.c79` files
  there are test payloads for the emulator round trips)
- `result/stockfw/build/ota_stage1.c79` - the signed loader image (stage-1) for Bluetooth updates of the loader
- `result/stockfw/build/ota_loader_v4.SYS` - stage-0 alone (every slot empty): an internal build step of
  `opengrand_install.SYS`, used by the emulator round trips; not something to install
- `result/stockfw/build/opengrand_install.seed_*.bin` - the slot contents seeded into the install image (contain
  signatures made with your key)

`result/stockfw/ARTIFACTS.md` lists the images and their hashes.

## 5. Remote app (web / PWA)

```bash
cd result/app
npm ci
npm run typecheck && npm test      # includes end-to-end tests against the firmware simulator
npm run build                      # -> result/app/dist/ (installable PWA)
npm run dev                        # http://localhost:8079 ; http://localhost:8079/?sim uses the simulator
```

Web MIDI and Web Bluetooth only work in a secure context: `https://...` or `http://localhost`. To install
on a phone, either serve `dist/` from any HTTPS static host, or forward the dev server over USB debugging
(`adb reverse tcp:8079 tcp:8079`) and open `http://localhost:8079` in Chrome on the phone.

## 6. Windows desktop app

```bash
cd result/desktop
npm ci
npm test                 # shell policy tests (plain Node)
npm run desktop          # builds the web app and starts it in Electron
npm run dist:win         # portable .exe in result/desktop/dist-electron/ (cross-builds from Linux)
```

## 7. Android app

```bash
cd result/app && npm run build          # the Android shell packages result/app/dist
cd android
./gradlew assemblePhoneDebug            # phone app  -> app/build/outputs/apk/phone/debug/
./gradlew assemblePanelDebug            # built-in panel flavour (advanced)
./gradlew testPhoneDebugUnitTest
```

Point Gradle at your SDK with `ANDROID_HOME` or `android/local.properties` (`sdk.dir=...`, git-ignored).

## 8. Releases and the in-app update (maintainers)

The phone app and the Windows desktop app check the latest GitHub release of this repository (at start, then every
24 h while online; on/off and "Check now" under Information › Automatic updates). Each release carries `update.json`
(format in `result/desktop/src/updatepolicy.cjs`) plus the files it names. There is no extra signing key: the apps
trust HTTPS to GitHub and this repository's releases, and check size and SHA-256 of the download. Android also refuses
an APK signed with another certificate than the installed app (the release APK uses the debug key of the build
machine, `~/.android/debug.keystore`; a build from another machine needs an uninstall first).

```bash
node scripts/release_build.mjs            # builds the APK + Windows zip, runs the leak checks, writes update.json
                                          # into dist-release/<tag>/ and prints the gh release command
```

Apps up to Android 1.2.0 / desktop 0.3.0 still require a signature in `update.json`; the script adds it while the old
key file (`result/stockfw/secrets/update_ed25519.pem`, never in the repository) exists.
