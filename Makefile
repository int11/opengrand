# Repo-level entry points. Details per area: result/README.md.
#   make gen        regenerate every generated file from its source (result/protocol/spec/ca79.json -> gen.py)
#   make gen-check  gen, then fail if any generated file changed (= someone hand-edited or forgot to regen)
#   make check      fast check, every merge: generated files fresh, image-check, then firmware, app,
#                   desktop and the quick emulator round trips + the current payload's (STOCK_FAST), in parallel
#   make check-full everything (before an upload to the piano, before a big merge): check + every emulator
#                   round trip (STOCK_FAST + STOCK_SLOW), in parallel
#   make stock      every emulator round trip alone (image-check first); stock-fast: STOCK_FAST only
#   make image      build the stock-firmware images (opengrand_install.SYS, ota_stage1.c79, ota_main.c79; ota_loader_v4.SYS =
#                   stage-0 alone, its build step) from source into result/stockfw/build/
#                   (pinned toolchain check -> cave -> manifest -> patch; table: result/stockfw/tools/images.py)
#   make image-check  make image, then fail unless every image matches its ARTIFACTS.md hashes (key-independent:
#                   a slot payload / stage-1 / ota_loader built with another OTA key still matches)
# Stock firmware: target/ is not in the repository. Without target/CAx9_P06.SYS (Kawai's official V2.19 system
# file) image, image-check and every emulator round trip print SKIP and pass; with a file there that is not the
# stock image they fail.
# Parallel runs: scripts/run_checks.py, longest first (.check/timings.json), at most NPROC at a time (default: CPU
# count) and only as many as fit 80 % of the free memory by each job's measured peak RSS; NPROC=1 = one after the
# other. One log per command in .check/; only the failed ones are printed in full; a per-tool time table at the end.

PY    := python3 -I
STOCK := cd result/stockfw && uv run python -I
NPROC ?=
RUN   := $(PY) scripts/run_checks.py $(if $(NPROC),-j $(NPROC))

# Sources: result/protocol/spec/ca79.json (edited by hand) and result/firmware/src/midi/progmap_gen.c (committed table).
GENERATED := result/protocol/PARAMS.md \
             result/firmware/include/ca79ng/params_gen.h result/firmware/src/kproto/params_gen.c \
             result/app/src/protocol/params.gen.ts \
             result/firmware/include/ca79ng/ngver_gen.h result/protocol/NGVER.md \
             result/app/src/protocol/ngver.gen.ts

# Stock-image emulator tools (result/stockfw/tools), one per line: tool[:arg...] (":" stands for a space).
# STOCK_FAST: seconds each, plus the round trip of the payload being worked on. STOCK_SLOW: the rest.
# --shard:i/n = the i-th of n groups of a long round trip's independent scenarios (rtlib.Unit), one job each.
STOCK_FAST := \
	test_stockemu.py \
	test_normodel.py \
	roundtrip_ota_hooks.py:-q \
	roundtrip_install.py:-q \
	roundtrip_bt.py:-q \
	roundtrip_rec.py:-q \
	roundtrip_recctl.py:-q \
	roundtrip_power.py:ota_main:-q \
	roundtrip_state.py:-q \
	roundtrip_metro.py:-q \
	roundtrip_m4.py:ota_main \
	roundtrip_fanout.py:ota_main:--shard:1/3 \
	roundtrip_fanout.py:ota_main:--shard:2/3 \
	roundtrip_fanout.py:ota_main:--shard:3/3 \
	roundtrip_btsrc.py:ota_main \
	roundtrip_startup.py:ota_main:--shard:1/2 \
	roundtrip_startup.py:ota_main:--shard:2/2 \
	roundtrip_recplay.py:ota_main \
	roundtrip_gate.py:ota_main \
	roundtrip_recrec.py:-q \
	roundtrip_recsave.py:-q \
	roundtrip_recdel.py:ota_main \
	roundtrip_xgate.py:ota_main:-q \
	roundtrip_gpio.py:ota_main:-q \
	roundtrip_sysex.py:-q \
	stock_kso.py:-q \
	roundtrip_midich.py:ota_main \
	roundtrip_usbplay.py:ota_main \
	roundtrip_fread.py:ota_main \
	roundtrip_bootdefaults.py:ota_main \
	roundtrip_canary_t1.py:ota_main
STOCK_SLOW := \
	roundtrip_ota.py:-q \
	roundtrip_ota_apply.py:-q \
	roundtrip_ota_window.py:-q \
	roundtrip_v4_boot.py:-q:--shard:1/3 \
	roundtrip_v4_boot.py:-q:--shard:2/3 \
	roundtrip_v4_boot.py:-q:--shard:3/3 \
	roundtrip_session.py:-q:--shard:1/4 \
	roundtrip_session.py:-q:--shard:2/4 \
	roundtrip_session.py:-q:--shard:3/4 \
	roundtrip_session.py:-q:--shard:4/4 \
	roundtrip_relay.py:ota_main:-q

# target/ is not tracked; every emulator round trip is skipped when the stock image is absent (one SKIP job instead of
# the list; the tools also skip on their own when run by hand).
STOCK_SHA256 := 09ad85703e44ad95d82475cfe2dae14c6edfa9f164a0b17b66b45f74c8d933c8
NO_STOCK := $(if $(wildcard target/CAx9_P06.SYS),,1)
STOCK_SKIP := stock::echo 'SKIP emulator round trips: target/CAx9_P06.SYS absent - put the official Kawai V2.19 system \
              file CAx9_P06.SYS (sha256 $(STOCK_SHA256)) into target/'
stock_cmds = $(if $(NO_STOCK),"$(STOCK_SKIP)",$(foreach t,$(1),'$(STOCK) tools/$(subst :, ,$(t))'))

.PHONY: gen gen-check gen-fresh check check-full check-prep fw app desktop stock stock-fast image image-check

gen:
	$(PY) result/protocol/tools/gen.py

gen-check: gen
	git diff --exit-code -- $(GENERATED)

# gen, then fail if gen rewrote anything - the gen-check question without needing a clean git tree
gen-fresh:
	@tmp=$$(mktemp -d) && for f in $(GENERATED); do mkdir -p $$tmp/$$(dirname $$f) && cp $$f $$tmp/$$f; done; \
	$(MAKE) -s --no-print-directory gen > /dev/null; rc=$$?; \
	for f in $(GENERATED); do cmp -s $$f $$tmp/$$f || { echo "stale generated file (make gen rewrote it): $$f"; rc=1; }; done; \
	rm -rf $$tmp; exit $$rc

# firmware, app and the emulator round trips in one parallel run (longest first, so the app's tests overlap
# the round trips instead of waiting for them)
BASE_CMDS := 'fw::$(MAKE) -C result/firmware check' \
             'app::cd result/app && npm run typecheck && npm test' 'desktop::cd result/desktop && npm test'

check: check-prep
	@$(RUN) --name check $(BASE_CMDS) $(call stock_cmds,$(STOCK_FAST))

check-full: check-prep
	@$(RUN) --name check-full $(BASE_CMDS) $(call stock_cmds,$(STOCK_FAST) $(STOCK_SLOW))

# before the parallel run: gen-fresh (gen rewrites files the app reads); the firmware simulator and host tools
# (the app's e2e tests run the simulator, round trips load cave_selftest / libkp*.so: built here, `fw` and the round
# trips' own make calls then only find them fresh - no relink under a running test); the images (image-check)
check-prep: gen-fresh
	@$(MAKE) -s -C result/firmware sim tools
	@$(MAKE) -s image-check

fw:
	$(MAKE) -C result/firmware check

app:
	cd result/app && npm run typecheck && npm test

# desktop shell policy tests (plain Node, no Electron); the Electron smoke test is `cd result/desktop && npm run smoke`
desktop:
	cd result/desktop && npm test

# Reads target/CAx9_P06.SYS (stock, never written; absent = SKIP); writes result/stockfw/build/ and
# regenerates the tracked cave/build/*.{bin,elf} and patches/*.json (byte-identical when
# nothing changed - git shows no diff). The OTA key result/stockfw/secrets/ota_key.bin is created on first use.
image:
	$(STOCK) tools/build_images.py

image-check: image
	$(STOCK) tools/build_images.py --check

# The emulator round trips alone: they need the images (image-check) and the host firmware build (roundtrip_ota,
# roundtrip_state, roundtrip_metro run make -C result/firmware themselves).
stock-fast: image-check
	@$(RUN) --name stock-fast $(call stock_cmds,$(STOCK_FAST))

stock: image-check
	@$(RUN) --name stock $(call stock_cmds,$(STOCK_FAST) $(STOCK_SLOW))
