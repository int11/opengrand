# Installing and updating OpenGrand

> **You do this at your own risk.** Flashing modified system firmware can permanently damage your
> instrument and will likely void your warranty. Read [SAFETY.md](SAFETY.md) first. If any step does not
> behave exactly as described, **stop**, do not improvise, and go to [Full recovery](#full-recovery).

This page describes the procedure in general terms. Image and file names below are the ones current at
the time of writing; `result/stockfw/ARTIFACTS.md` is the authoritative list.

## How it fits together

```
 USB, once: opengrand_install.SYS          Bluetooth, from then on
 ────────────────────────────────          ───────────────────────
 Kawai V2.19 system firmware               loader slot A ─┐  the OpenGrand loader (stage-1): receives
 + stage-0: the small fixed part of the                   ├─ updates, starts the main features; the
   OpenGrand loader + your update key      loader slot B ─┘  newest confirmed one runs
 + loader slot A: stage-1, already in       main slot A ──┐  the main features (ota_main): the newest
   the file                                               ├─ working one runs, the other is kept as
 + main slot A: ota_main, already in it     main slot B ──┘  a fallback
```

1. **First install (USB, one file):** you build one USB image yourself, `opengrand_install.SYS` (Kawai's V2.19
   system file + the small fixed part of the OpenGrand loader ("stage-0") + your Bluetooth update key + the rest of
   the loader ("stage-1") and the main features already placed in their first slots, signed with that key) and
   install it with **Kawai's official USB update procedure**. After the first power-on every feature is there; no
   Bluetooth upload is needed. The image is built locally from your own copy of Kawai's file and must never be
   distributed.
2. **Updates (Bluetooth):** later versions of the main features (`ota_main.c79`) **and of the loader itself**
   (`ota_stage1.c79`) are signed files that the app uploads over Bluetooth into the slot that is not running. No USB
   stick, no special key combination. Only stage-0 stays as the USB image put it.
3. **Recovery:** two escape-key combinations (an older loader without the main features, or pure stock); Kawai's
   official update restores the piano completely.

## Before you start

- [ ] `make check-full` passes on your machine ([BUILDING.md](BUILDING.md)).
- [ ] Your `target/CAx9_P06.SYS` is Kawai's V2.19 file and matches its SHA-256 (the build checks this).
- [ ] The piano shows system version **V2.19** (panel menu → system information).
- [ ] You have a **recovery USB stick**: FAT32, containing only Kawai's **unmodified** `CAx9_P06.SYS`.
      Label it. Keep it next to the piano.
- [ ] A copy of Kawai's original file and of `result/stockfw/secrets/ota_key.bin` exist **outside** your PC.
- [ ] Recorder songs and anything else you care about are exported to USB.
- [ ] Stable power (no switching power strips, ideally a UPS), no children or pets around, and time to
      recover if something goes wrong.
- [ ] A phone or camera to film the screen during the install - it is the only record if something fails.

## First install (USB)

The first install is **one file**: `opengrand_install.SYS`. (`build/` also holds `ota_loader_v4.SYS`, stage-0 alone
with every slot empty; it is an internal build step - do not install it: alone it starts nothing but Kawai's firmware.)

1. Build: `make image`. The USB image is `result/stockfw/build/opengrand_install.SYS`: stage-0, your update key, loader
   slot A already holding stage-1 (marked confirmed) and main slot A already holding the main features (`ota_main`),
   both signed with your key.
2. Prepare a **separate** stick (not your recovery stick): FAT32, and in its root **only one file** -
   `opengrand_install.SYS`, **renamed to `CAx9_P06.SYS`**. No other `.SYS` files and no other firmware files of any
   name on the stick. Verify the copy with `sha256sum` against the file in `build/`.
3. Switch the piano off. Insert the stick into the **USB to Device** port (next to the headphone jacks under
   the keyboard), not any other port.
4. Start Kawai's update procedure as described in Kawai's update instructions for the CA99/CA79: hold the
   four highest keys (**E7, A7, B7, C8**) - and no other key - and press **POWER**.
5. The panel shows the firmware update progress. **Do not remove the stick or cut power.** It takes about one
   to two minutes and ends with a completion message; the piano then waits.
6. Switch off, remove the stick, switch on. The piano must start normally: home screen, all keys and
   pedals sound, volume normal. If not, go to [Full recovery](#full-recovery).
7. **Leave the piano on for at least 60 seconds** after this first start. The main features in main slot A are
   then marked **confirmed** and kept. Until then they are on trial: if each of the first three starts is
   switched off before 60 s, the loader gives main slot A up and runs without the main features (the piano
   still answers the app, so the features come back with a Bluetooth upload or by installing the file again).
   The loader in loader slot A is confirmed already - there is nothing older to fall back to.
8. Connect the OpenGrand app (Bluetooth MIDI or USB). Under **Information → custom firmware → Check** it
   should report loader A (seq 1, confirmed) running, the main features running from main slot A (confirmed after
   the first minute), and both B slots empty.
9. In the app's Bluetooth update section, import your update key (`ota_key.bin`, or its 64 hex digits) - it
   is needed for later updates. The key id shown must match `uv run python -I tools/ota_pack.py key` (run in
   `result/stockfw`).

At this point the piano runs Kawai's firmware, the OpenGrand loader and the main features; nothing else has to
be uploaded. A USB install always erases all four Bluetooth slots and writes them fresh from the file; your piano
settings and recorder songs are stored elsewhere in flash and are kept.

## Updates over Bluetooth

Every later version is a signed `.c79` file uploaded over Bluetooth - no USB stick. `make image` writes two:
`result/stockfw/build/ota_main.c79` (the main features) and `result/stockfw/build/ota_stage1.c79` (the loader). The
other `.c79` files in `build/` are test payloads for the emulator round trips; do not upload them.

**The main features (`ota_main.c79`):**

1. In the app: **Information → Bluetooth update**, choose the file, **Upload and commit**. Do not play or operate
   the piano meanwhile; it can pause for a second or two while flash is erased. The file is written to the main slot
   that is not running (the first update after the USB install goes to main slot B), verified and only then marked
   valid.
2. Start it: switch the piano off and on (or use **Apply now**, which switches without a power cycle).
3. Leave the piano on for **about a minute**. A main slot started at power-on is marked **confirmed** automatically
   after ~60 s of normal running. Check the slot state in the app.

**The loader (`ota_stage1.c79`)** - rarely needed; only when a new loader version is published in this repository:

1. The running loader must be confirmed (the app refuses otherwise). Upload the file the same way: the app reads
   the file's header, sees a loader and writes it to the loader slot that is not running, with the next loader
   sequence number.
2. Switch the piano off and on (or press **Switch loader now**, which restarts the loader without a power cycle).
   The new loader starts **on trial**.
3. Reconnect the app. Once Bluetooth, the status and one signed exchange work, the app **confirms** the new loader
   automatically (it remembers the upload; **Confirm this loader** on the firmware card does the same by hand). A
   loader is never confirmed by time alone: one that cannot hear Bluetooth stays unconfirmed.
4. If the new loader does not come up or the app cannot reach it: switch the piano off and on **three times**. After
   three unconfirmed starts the previous (confirmed) loader runs again - no USB needed.

Safety net:

- Every file is checked (CRC and an HMAC-SHA256 signature made with **your** key) before it is written and again at
  every boot. A file signed with another key is refused. A loader file is checked with exactly the rules stage-0 uses
  to start it, so a loader that stage-0 would refuse never reaches the flash.
- A power cut during an upload leaves that slot invalid; the other slot runs.
- A new slot that has not been confirmed is only tried three times; then it is skipped and the previous one runs.
- **Roll back** (Information → Developer) rejects the running main slot; after the next power cycle the previous one
  runs (or no main features until you upload again). **Roll back loader** does the same for the loader, but the app
  never lets you reject or overwrite the last confirmed loader. The same card shows both loader slots, stage-0's boot
  reason and SAFE mode.
- Only stage-0 can write to the flash, and only inside its fixed table: the four update slots and the settings store,
  plus three spare areas that stay locked at every power-on until a future loader unlocks them for that power cycle
  after checking them on your piano (today's loader never does). Never Kawai's boot program, firmware, your user
  memory or recordings.

## Escape keys

Hold the keys while switching the piano on, and keep holding them until the panel has started:

- **A0 + A#0** (the two lowest keys): **safe mode** - the older of the two loaders runs (the only one, if there is
  just one) and the main features are **not** started. The app can still connect, so you can reject or re-upload a
  main slot that misbehaves, or let a bad newer loader be replaced.
- **A0 + A#0 + B0** (the three lowest keys): **pure stock** - no OpenGrand code runs at all for this power-on; the
  piano behaves exactly like Kawai's firmware (the app finds no OpenGrand extension).

The next normal power-on uses the slots again.

## Full recovery

Kawai's official update always works as a complete reset of the system firmware, because Kawai's updater
runs before any OpenGrand code:

1. Switch off. Insert your **recovery stick** (unmodified `CAx9_P06.SYS` only) into **USB to Device**.
2. Hold **E7, A7, B7, C8** and press **POWER**; wait for the completion message; switch off, remove the
   stick, switch on.
3. Check that the system information shows **V2.19**.

The piano is then 100 % stock. Any USB system update - Kawai's or a newer `opengrand_install.SYS` - **erases
all four Bluetooth slots** (settings and recorder songs are stored elsewhere and kept). A new `opengrand_install.SYS`
brings loader slot A and main slot A back with the versions it was built with; leave the piano on for 60 s again
after it. Your key stays the same as long as you keep `ota_key.bin`.

If the piano does not respond at all and the update screen cannot be reached with the recovery stick,
contact Kawai service. (Tell them the truth about what you installed.)

## Replacement panel UI (advanced)

The built-in touch panel runs its own software, separate from the system firmware above. OpenGrand can
replace the screen it shows through the panel's own UI update mechanism, which requires a key you must
supply yourself; nothing in this repository provides it. Signed UI packages can then be updated over
Bluetooth through the piano. Reverting means reinstalling Kawai's panel UI with Kawai's own procedure.
Only attempt this if you understand the risks and the code in `result/app/panelui` and
`result/app/scripts`.
