# OpenGrand code cave - one framework, images chosen by feature bits.
#
#   make STAGE=0 V=s0                         -> build/cave_s0.bin (+ .elf)  (ota_loader_v4 stage-0, the .SYS cave)
#   make STAGE=1 V=s1                         -> build/loader_s1.bin (+ .elf)  (ota_loader_v4 stage-1, image ota_stage1)
#   make PAYLOAD=1 V=<variant> FEAT0=.. ...  -> build/payload_<variant>.bin (+ .elf)  (ota_main: V=pmain)
#   make toolchain-check                      -> fail unless the pinned toolchain is present
#   make clean
#
# Normally driven by `make image` at the repo root (result/stockfw/tools/build_images.py),
# which holds the image table (variant -> feature bytes).  IDENTIFY reports FEAT0/FEAT1
# verbatim, so the bits passed here are also what the piano tells the app.
#
# STAGE=0: the cave of image ota_loader_v4 = stage-0 (src/s0.c, s0.ld): a raw binary placed at file offset 0x113A14
# of the MainProgram image (SDRAM 0x40173A14) by result/stockfw/tools/patcher.py - the only code in the .SYS.
# STAGE=1: stage-1 (src/s1.c, s1.ld), linked into the window 0x40176000; tools/ota_pack.py pack_loader signs it.
# PAYLOAD=1 builds a slot payload instead: ca79ng.c (+ core) + src/ota_payload.c linked to run
# at OTA_RUN_BASE (ota_payload.ld) -> build/payload_<V>.{bin,elf}; tools/ota_pack.py signs it.
# PAYLOAD=1 V=phook3 is the runtime-hook demo test payload instead (src/hook_demo.c + hook_demo3.ld).
# FEAT1 0x10 (POWER) = the slot payload ota_main: one fixed source / define set (below, POWER_SRC); the other bits of
# feat1 0x20 / 0x40, FEAT2 and FEAT3 are only reported by IDENTIFY.
#
# CORE feature bit (feat1 0x02): the OpenGrand core (result/firmware: kproto, param store,
# power timer, util) and its cave port (result/firmware/ports/cave) are compiled with the
# same flags and linked in (RW state NOLOAD in R1).  Those builds also
# write per-function stack usage and call graphs (build/core_<V>/*.su, *.ci) for
# tools/stack_budget.py.
TC_DIR := ../.toolchain/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi
TC   := $(TC_DIR)/bin/arm-none-eabi-
CC   := $(TC)gcc
LD   := $(TC)ld
OC   := $(TC)objcopy
OD   := $(TC)objdump

# Pinned toolchain: `--version` first line and the sha256 of every binary that touches
# the cave bytes (toolchain.sha256).  A different compiler may emit different bytes, and
# the image sha256 in ARTIFACTS.md would no longer reproduce.
TC_VERSION := arm-none-eabi-gcc (Arm GNU Toolchain 13.3.Rel1 (Build arm-13.24)) 13.3.1 20240614

# arm926ej-s = the ARM core in the OMAP-L13x class SoC this firmware runs on.
# -fno-zero-initialized-in-bss: nothing zeroes .bss for us (the cave is loaded from
# flash as-is), so zero-initialised state must live in .data.
CFLAGS := -mcpu=arm926ej-s -marm -mno-unaligned-access \
          -Os -ffreestanding -fno-pic -fno-builtin -fno-common \
          -fno-zero-initialized-in-bss -fno-stack-protector \
          -ffunction-sections -fdata-sections \
          -Wall -Wextra -Werror -std=c11 -Iinclude

SRC := src/ca79ng.c
OUTDIR := build

FW       := ../../firmware
CORE_SRC := $(FW)/src/kproto/kproto.c $(FW)/src/kproto/store.c $(FW)/src/kproto/params_gen.c \
            $(FW)/src/system/system.c $(FW)/src/util/util.c \
            $(FW)/ports/cave/hal_cave.c $(FW)/ports/cave/core_cave.c $(FW)/ports/cave/cave_rt.c
CORE_HDR := $(wildcard $(FW)/include/ca79ng/*.h $(FW)/hal/*.h $(FW)/ports/cave/*.h)
# -fno-tree-loop-distribute-patterns: no libc to call, so GCC must not turn a byte loop
# (cave_rt.c's own memset/memcpy among them) into a call to memset/memcpy.
# -DBTS_NEVER_SUSPEND / -DBTS_NO_NEWEST / -DBTS_NO_TEST_STATS (read by src/bt/btsrc.c only): the source manager's legacy
# paths kept for the host model tests - the sink's STREAMING STOP, the policy mode NEWEST and the counters only the
# tests read - are not in the slot payload.
CORE_CFLAGS := -I$(FW)/include -I$(FW)/src -I$(FW)/hal -I$(FW)/ports/cave \
               -fno-tree-loop-distribute-patterns -fstack-usage -fcallgraph-info=su -DBTS_NEVER_SUSPEND -DBTS_NO_NEWEST \
               -DBTS_NO_TEST_STATS

V     ?=
FEAT0 ?=
FEAT1 ?= 0x00
CORE  := $(shell echo $$(( ($(FEAT1)) & 0x02 )))
OTA   := $(shell echo $$(( ($(FEAT1)) & 0x04 )))
POWER := $(shell echo $$(( ($(FEAT1)) & 0x10 )))
PAYLOAD ?= 0
OTA_HDR := include/ota.h include/s0.h src/ota_payload.c ota_payload.ld stock_ota.ld
OTA_CFLAGS := -fno-tree-loop-distribute-patterns -fstack-usage
OTA_CORE_SRC := $(if $(filter-out 0,$(CORE)),$(CORE_SRC))
# POWER feature bit (feat1 0x10) = the power-family slot payload, and from source that is only ota_main (every feature at
# once: feat1 0x77, feat2 0x7F, feat3 0x3F - src/ca79ng.c refuses any other combination).  Its sources and defines are
# FIXED: the per-feature bits of feat1 0x20 / 0x40, feat2 and feat3 no longer pick sources or -D switches (folded after
# their field tests); they are only what IDENTIFY
# reports (-DCA79NG_FEAT2 / -DCA79NG_FEAT3 to src/ca79ng.c).
#   power port + standby / APO rule / kvstore (06_power_cave), TV wake (07_tv_wake), remote off + quiet polls (09_p3_power),
#   GPIO diagnostic op 0x17; the runtime hooks src/power_hooks.c
POWER_SRC := $(FW)/src/system/standby.c $(FW)/src/system/power.c $(FW)/src/params/kvstore.c \
             $(FW)/ports/cave/power_cave.c $(FW)/src/system/gpiodiag.c $(FW)/ports/cave/gpio_cave.c
#   Phase 3-1 SysEx front
POWER_SRC += $(FW)/src/kproto/kpfront.c $(FW)/ports/cave/sysex_cave.c
#   BT port + source manager, op 0x1D
POWER_SRC += $(FW)/ports/cave/bt_cave.c $(FW)/src/bt/btsrc.c
#   session core + S1 settings state + metronome stage 1 and the fn 0x24 Read job with 7-bit
#   DataPacket counters (ports/cave/fread_cave.c, stock_fread.ld, the allow-list site 0x40143ACC)
POWER_SRC += $(FW)/src/kproto/kpsess.c $(FW)/src/kproto/kpstate.c $(FW)/src/kproto/kpmetro.c $(FW)/ports/cave/fread_cave.c
#   panel-UI relay 0x7D ops 0x30-0x3F <-> port 3
POWER_SRC += $(FW)/ports/cave/relay_cave.c
#   startup settings, op 0x1C
POWER_SRC += $(FW)/src/kproto/kpstart.c $(FW)/ports/cave/startup_cave.c
#   playback / record control R1-R2, InternalSongDelete, R3 save decision, op 0x1E
POWER_SRC += $(FW)/src/recorder/recview.c $(FW)/src/recorder/recctl.c $(FW)/src/recorder/recdel.c \
             $(FW)/ports/cave/recctl_cave.c $(FW)/src/recorder/recsave.c
#   intake gate, op 1B 06
POWER_SRC += $(FW)/src/kproto/kpgate.c
#   executor gate, op 0x1F, off at boot until 1F 01 01
POWER_SRC += $(FW)/src/system/xgate.c $(FW)/ports/cave/xgate_cave.c
#   MIDI channel messages / delayed apply, op 1B 07, off at boot until 1B 07 01
POWER_SRC += $(FW)/src/kproto/kpchan.c
#   RAM canary over the unreferenced SDRAM blocks, op 0x20 (ngver 4), nothing written before an arm
POWER_SRC += $(FW)/src/system/ramcanary.c $(FW)/ports/cave/canary_cave.c
OTA_POWER_SRC := $(if $(filter-out 0,$(POWER)),$(POWER_SRC))
# the two build switches left in the ports, both "cave vs host test", not features:
#   CA79NG_SESSION=1  sysex_cave.c / power_hooks.c: the full front (the host test builds the bare Phase 3-1 front)
#   PWC_STARTUP=1     power_cave.c / bt_cave.c: startup settings and the BT policy through the power port's kvstore
#                     (the host tests bind their own stand-ins)
POWER_CFLAGS := -DCA79NG_SESSION=1 -DPWC_STARTUP=1
FEAT2 ?= 0x00
FEAT3 ?= 0x00
FEAT2_CFLAGS := $(if $(filter-out 0,$(shell echo $$(( ($(FEAT2)) )))),-DCA79NG_FEAT2=$(FEAT2))
FEAT3_CFLAGS := $(if $(filter-out 0,$(shell echo $$(( ($(FEAT3)) )))),-DCA79NG_FEAT3=$(FEAT3))
PAYLOAD_LD := $(if $(filter-out 0,$(POWER)),main_payload.ld,ota_payload.ld)
# THUMB (default 1 for the power-family slot payloads = ota_main; 0 for everything else -
# the loader cave and the test payloads keep their ARM bytes): the C of the payload is
# compiled as Thumb (ARMv5TE Thumb-1, -26 % payload)
#   -fno-jump-tables  Thumb-1 switch tables would call libgcc's __gnu_thumb1_case_* (not linked; smaller without)
#   --use-blx         ARM <-> Thumb calls become BLX instead of ld veneers (ARMv5TE has BLX)
# What stays ARM: every hand-written stub (src/power_hooks.c `.arm` blocks - hook_set needs a 4-aligned ARM target and
# encodes a `bl`), and the entry table's four entry points (ARM trampolines in src/ota_payload.c - the loader
# accepts only 4-aligned entry pointers, in_run(..., 4)).  THUMB=0 builds the ARM payload as before.
THUMB ?= $(if $(filter 1,$(PAYLOAD)),$(if $(filter-out 0,$(POWER)),1,0),0)
ifeq ($(THUMB),1)
CFLAGS += -mthumb -fno-jump-tables
PAYLOAD_LDFLAGS := --use-blx
endif

STAGE ?=
ifeq ($(V),)
all:
	@echo "pick an image: make V=<variant> FEAT0=0x.. FEAT1=0x.. (or 'make image' at the repo root)"; false
else ifeq ($(STAGE),0)
all: $(OUTDIR)/cave_$(V).bin
else ifeq ($(STAGE),1)
all: $(OUTDIR)/loader_$(V).bin
else
ifeq ($(FEAT0),)
$(error FEAT0 is required with V=$(V))
endif
ifeq ($(PAYLOAD),1)
all: $(OUTDIR)/payload_$(V).bin
else
all: $(OUTDIR)/cave_$(V).bin
endif
endif

toolchain-check:
	@test "$$($(CC) --version | head -n 1)" = "$(TC_VERSION)" || \
	  { echo "toolchain: '$$($(CC) --version | head -n 1)' is not the pinned '$(TC_VERSION)'"; exit 1; }
	@cd .. && grep -v '^#' cave/toolchain.sha256 | sha256sum --check --quiet --strict - || \
	  { echo "toolchain: binaries differ from cave/toolchain.sha256"; exit 1; }

# Always rebuilt (FORCE): the feature bits are command-line input, not a file make can track.
OTA_OUT := $(OUTDIR)/core_$(V)
S0_DEPS := src/s0.c include/s0.h include/stock.h s0.ld stock.ld stock_ota.ld stock_norw.ld
S1_DEPS := src/s1.c include/s0.h include/ota.h include/stock.h s1.ld stock.ld stock_core.ld stock_ota.ld
ifeq ($(STAGE),0)
# ota_loader_v4 STAGE-0 (frozen, USB): src/s0.c alone, s0.ld (the only image that links the stock NOR writers)
$(OUTDIR)/cave_$(V).elf: $(S0_DEPS) toolchain-check FORCE | $(OUTDIR)
	rm -rf $(OTA_OUT) && mkdir -p $(OTA_OUT)
	$(CC) $(CFLAGS) $(OTA_CFLAGS) -mthumb -fno-jump-tables -c src/s0.c -o $(OTA_OUT)/s0.o
	$(LD) --use-blx -T s0.ld -L. --no-undefined -static --gc-sections --entry s0_h1 --no-warn-rwx-segments \
	  $(OTA_OUT)/s0.o -Map $(OTA_OUT)/cave.map -o $@
else ifeq ($(STAGE),1)
# ota_loader_v4 STAGE-1 (loader slots, Bluetooth-updatable): src/s1.c alone, s1.ld (window 0x40176000, 40 KB)
$(OUTDIR)/loader_$(V).elf: $(S1_DEPS) toolchain-check FORCE | $(OUTDIR)
	rm -rf $(OTA_OUT) && mkdir -p $(OTA_OUT)
	$(CC) $(CFLAGS) $(OTA_CFLAGS) -c src/s1.c -o $(OTA_OUT)/s1.o
	$(LD) -T s1.ld -L. --no-undefined -static --gc-sections --entry s1_entry --no-warn-rwx-segments \
	  $(OTA_OUT)/s1.o -Map $(OTA_OUT)/cave.map -o $@
else ifeq ($(PAYLOAD)$(filter phook3,$(V)),1$(V))
# runtime-hook demo test payload ota_hookdemo3: src/hook_demo.c alone (-DHOOKDEMO_V3=1), no ca79ng.c / core,
# hook_demo3.ld.
$(OUTDIR)/payload_$(V).elf: src/hook_demo.c hook_demo.ld hook_demo3.ld $(OTA_HDR) stock.ld stock_core.ld include/stock.h \
                            toolchain-check FORCE | $(OUTDIR)
	rm -rf $(OTA_OUT) && mkdir -p $(OTA_OUT)
	$(CC) $(CFLAGS) $(OTA_CFLAGS) -DCA79NG_FEAT0=$(FEAT0) -DCA79NG_FEAT1=$(FEAT1) -DHOOKDEMO_V3=1 \
	  -c src/hook_demo.c -o $(OTA_OUT)/hook_demo.o
	$(LD) -T hook_demo3.ld -L. --no-undefined -static --gc-sections --entry ota_entry --no-warn-rwx-segments \
	  $(OTA_OUT)/hook_demo.o -Map $(OTA_OUT)/cave.map -o $@
else ifeq ($(PAYLOAD),1)
$(OUTDIR)/payload_$(V).elf: $(SRC) $(CORE_SRC) $(CORE_HDR) $(OTA_HDR) stock.ld stock_core.ld include/stock.h \
                            $(OTA_POWER_SRC) $(if $(filter-out 0,$(POWER)),src/power_hooks.c main_payload.ld power_payload.ld stock_power.ld \
                              stock_sysex.ld stock_bt.ld stock_session.ld stock_relay.ld stock_rec.ld stock_xgate.ld stock_chan.ld stock_fread.ld) \
                            toolchain-check FORCE | $(OUTDIR)
	rm -rf $(OTA_OUT) && mkdir -p $(OTA_OUT)
	$(CC) $(CFLAGS) $(CORE_CFLAGS) -DCA79NG_FEAT0=$(FEAT0) -DCA79NG_FEAT1=$(FEAT1) $(FEAT2_CFLAGS) $(FEAT3_CFLAGS) -c src/ca79ng.c -o $(OTA_OUT)/ca79ng.o
	$(CC) $(CFLAGS) $(OTA_CFLAGS) -DCA79NG_FEAT0=$(FEAT0) -DCA79NG_FEAT1=$(FEAT1) -c src/ota_payload.c -o $(OTA_OUT)/ota_payload.o
	set -e; for f in $(OTA_CORE_SRC) $(OTA_POWER_SRC); do \
	  $(CC) $(CFLAGS) $(CORE_CFLAGS) $(if $(filter-out 0,$(POWER)),$(POWER_CFLAGS)) -c $$f -o $(OTA_OUT)/$$(basename $$f .c).o; done
	$(if $(filter-out 0,$(POWER)),$(CC) $(CFLAGS) $(CORE_CFLAGS) $(POWER_CFLAGS) -c src/power_hooks.c -o $(OTA_OUT)/power_hooks.o)
	$(LD) $(PAYLOAD_LDFLAGS) -T $(PAYLOAD_LD) -L. --no-undefined -static --gc-sections --entry ota_entry --no-warn-rwx-segments \
	  $(OTA_OUT)/ota_payload.o $(OTA_OUT)/ca79ng.o $(patsubst %.c,$(OTA_OUT)/%.o,$(notdir $(OTA_CORE_SRC) $(OTA_POWER_SRC))) \
	  $(if $(filter-out 0,$(POWER)),$(OTA_OUT)/power_hooks.o) -Map $(OTA_OUT)/cave.map -o $@
else
$(OUTDIR)/cave_$(V).elf: FORCE
	@echo "V=$(V): the only cave image is ota_loader_v4's stage-0 (make STAGE=0 V=s0)"; false
endif

$(OUTDIR)/cave_$(V).bin: $(OUTDIR)/cave_$(V).elf
	$(OC) -O binary $< $@
	@echo "$@: $$(stat -c%s $@) bytes (feat0 $(FEAT0) feat1 $(FEAT1))"

$(OUTDIR)/loader_$(V).bin: $(OUTDIR)/loader_$(V).elf
	$(OC) -O binary -j .cave $< $@
	@echo "$@: $$(stat -c%s $@) bytes (stage-1 loader image)"

$(OUTDIR)/payload_$(V).bin: $(OUTDIR)/payload_$(V).elf
	$(OC) -O binary -j .cave $< $@
	@echo "$@: $$(stat -c%s $@) bytes (slot payload, feat0 $(FEAT0) feat1 $(FEAT1))"

$(OUTDIR):
	mkdir -p $(OUTDIR)

clean:
	rm -rf $(OUTDIR)

FORCE:

.PHONY: all toolchain-check clean FORCE
