# Images (ARTIFACTS)

What `make image` builds and the hashes `make image-check` holds it to: the USB image `opengrand_install.SYS`
(installed once: OTA loader v4 stage-0 + loader slot A already holding stage-1 + main slot A already holding `ota_main`),
the stage-1 loader image `ota_stage1.c79` and the slot payload `ota_main.c79` (Bluetooth updates of the loader and of the
main), and `ota_loader_v4.SYS`, stage-0 alone with every slot empty - the build step `opengrand_install.SYS` is made from
and what the emulator round trips start from, not a user install path (alone it boots stock). Nothing here is a
download: all are built locally from the stock file plus the sources in this repository.

## Input (`target/`, not in the repository)

| name | path | sha256 | size (B) |
|---|---|---|---|
| stock | `target/CAx9_P06.SYS` | `09ad85703e44ad95d82475cfe2dae14c6edfa9f164a0b17b66b45f74c8d933c8` | 3,801,088 |

Kawai's official V2.19 system file for the CA79 / CA99 (MainProgram partition, raw). Get it from Kawai and put it
into `target/`; without it the image and emulator steps print `SKIP` (see [BUILDING.md](../../docs/BUILDING.md)).

## Built images (`result/stockfw/build/`, not in the repository)

The sha256 below is **key-independent**: every build uses its own OTA key (`result/stockfw/secrets/ota_key.bin`,
created by the first `make image`, never committed), which is patched into both `.SYS` images as stage-0's root key and
signs `ota_stage1.c79` and `ota_main.c79` (also the copies seeded into `opengrand_install.SYS`). The hash is taken over
- `.SYS`: the image with the OTA key region (the manifest's `secrets/ota_key.bin` blob patch, 32 B) put back to its
  erased fill `FF`, and in every seeded slot (the manifest's blob patches marked `"keyed": "c79@0x200"`) the embedded
  `.c79`'s key id and HMAC zeroed by the `.c79` rule below;
- `.c79`: the file with the key id (header bytes `0x24..0x27`) and the HMAC (header bytes `0x30..0x3F`) zeroed.

`make image-check` (`tools/build_images.py --check`) also checks that each `.SYS` reverts to the stock image, that the
key region holds the local key, that each `.c79` verifies with it, and that the seeds of `opengrand_install.SYS` are
the state areas (`C79L` seq 1 confirmed / `C79S` seq 1 unconfirmed, state version 1) followed by exactly
`build/ota_stage1.c79` / `build/ota_main.c79`. The seed blobs (`build/opengrand_install.seed_*.bin`) are key-dependent
and exist only under the git-ignored `build/`; `opengrand_install.SYS`'s hash changes whenever stage-1 or `ota_main`
changes. The build also refuses a stage-1 or slot payload ELF that references a stock NOR writer (only stage-0 may).
Feature bits and the full description of each image: `tools/images.py` (which also lists the test payloads `make image`
builds for the emulator round trips; they are not images and have no row here).

| name | file | sha256 (key-independent) | size (B) | description |
|---|---|---|---|---|
| opengrand_install | `opengrand_install.SYS` | `8934a5598c46b2ace3b5bf705053a027c1068eb288720e609c3e3d50e7b7f231` | 3,801,088 | The USB image to install (copied to the stick as `CAx9_P06.SYS`): `ota_loader_v4` + loader slot A (file 0x140000) pre-seeded with `ota_stage1.c79` (state `C79L`, seq 1, **confirmed** - there is no older loader) + main slot A (file 0x120000) pre-seeded with `ota_main.c79` (state `C79S`, seq 1, unconfirmed - confirmed after ~60 s on, 3 tries). Carries the root key. |
| ota_loader_v4 | `ota_loader_v4.SYS` | `74f3cf576a49aeee8088411ddde7182d68d9ecceea324cfd88fb77f7e6baa481` | 3,801,088 | Stage-0 alone (frozen, v2, 4,500 B of 9,676): the two hooks, escape keys E1 (pure stock) / E2 (SAFE), loader slot choice with try mark first, NOR -> 40 KB stage-1 window with CRC + HMAC (root key), RESELECT, the guarded NOR write service over one frozen range table (slot sectors and kvstore `0x604C0000-0x604FFFFF` always on; gates unused_2 + TouchCalib `0x60480000-0x604BFFFF`, `0x60600000-0x607FFFFF`, `0x60800000-0x60FFFFFF` off at every power-on, switched on for the boot only by stage-1 with `nor_enable` (refused as a mirror if a gate reads like BootProgram); never BootProgram or another stock partition, the voice-DB tail `0x602E0000` deliberately left out; scoped by caller: stage-1 every ON entry, the main the kvstore + gates granted main_writable) + cache sync + signature check for stage-1 and the main. Every slot empty (boots stock): the build step of `opengrand_install.SYS` and the round trips' base, not a user install path. Carries the root key. |
| ota_stage1 | `ota_stage1.c79` | `57b9b91269cd35484d50bc1d0f24cfb8a144b1f962b92c9ccecf7e82490c06ec` | 14,896 | Stage-1 loader image (format 2, type 2, version 2, load 0x40176000): main boot, OTA ops 0x0E-0x16 for main and loader slots, loader confirm, interlocks, apply now / RESELECT, status layout 3 (byte 111 = stage-0 NOR gates; accepts a stage-0 v1 boot block), runtime hook service (17 sites, `struct ota_svc` v3); fast upload (version 2): Begin reply offers Data v2 frames - CRC-32 each, up to 1536 B, LZ tokens, 4 in flight, cumulative ACK - next to the 200 B stop-and-wait frames. Uploaded over Bluetooth into the loader slot that is not running. |
| ota_main | `ota_main.c79` | `e739a902f61a9c25ddeef8133b2bcc4bedd4c321a5a4df58dd63526337be157d` | 106,404 | Every feature in one slot payload (power, SysEx front, session core, BT port, panel relay, startup settings, recorder control, settings store, executor gate, MIDI channel judge); its kvstore writes go through stage-0's NOR service (which gives the main the kvstore and, only if stage-1 grants one, a NOR gate - none today). The payload kept on the piano. |
