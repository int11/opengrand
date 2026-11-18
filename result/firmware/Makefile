# OpenGrand firmware — host build (simulator + tests).
#   make test     build and run unit/integration tests
#   make sim      build the simulator (ports/sim)
#   make clean
# The cave port (ports/cave) is also built for ARM, inside the stock image:
# result/stockfw/cave/Makefile (CORE feature bit, `make image` at the repo root).
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wstrict-prototypes -Werror=implicit-function-declaration
CFLAGS  += -Iinclude -Isrc
LDLIBS  += -lm
BUILD   := build

CORE_SRC := $(wildcard src/*/*.c)
TEST_SRC := $(wildcard tests/*.c)
SIM_SRC  := $(wildcard ports/sim/*.c)
# cave port on the host: the stock routines are stand-ins (stock_host.c), and the cave's
# C runtime gets a cave_rt_ prefix so it can be tested next to the host libc
CAVE_SRC := ports/cave/hal_cave.c ports/cave/core_cave.c ports/cave/stock_host.c

CORE_OBJ := $(CORE_SRC:%.c=$(BUILD)/%.o)
TEST_OBJ := $(TEST_SRC:%.c=$(BUILD)/%.o)
SIM_OBJ  := $(SIM_SRC:%.c=$(BUILD)/%.o)
CAVE_OBJ := $(CAVE_SRC:%.c=$(BUILD)/%.o) $(BUILD)/ports/cave/cave_rt_host.o
# the cave power port (slot payload ota_power): tests only - its stock routines are the test's
# own stand-ins (tests/test_power_cave.c), so it stays out of tools/cave_selftest
POWER_CAVE_OBJ := $(BUILD)/ports/cave/power_cave.o
# the Phase 3-1 SysEx port (slot payload ota_sysex): same - its stock routine / globals are
# tests/test_sysex_cave.c's stand-ins
SYSEX_CAVE_OBJ := $(BUILD)/ports/cave/sysex_cave.o
# the BT port (slot payload ota_bt): same - tests/test_bt_cave.c's stand-ins (+ the power test's UART1 ones)
BT_CAVE_OBJ := $(BUILD)/ports/cave/bt_cave.o
# the panel-UI relay (ota_main feat2 0x04): same - tests/test_relay_cave.c's ring-count stand-ins
RELAY_CAVE_OBJ := $(BUILD)/ports/cave/relay_cave.o
# the playback control (ota_main feat2 0x10): its stock routines, stock RAM reads and the session core's TX / bound
# mask are tests/test_recctl_cave.c's stand-ins (RCC_HOST)
REC_CAVE_OBJ := $(BUILD)/ports/cave/recctl_cave.o

.PHONY: all test sim tools check clean
all: test sim tools

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/ports/cave/%.o: CFLAGS += -Ihal -Iports/cave
$(BUILD)/tests/test_cave.o $(BUILD)/tests/test_power_cave.o $(BUILD)/tests/test_sysex_cave.o $(BUILD)/tests/test_bt_cave.o $(BUILD)/tests/test_relay_cave.o $(BUILD)/tests/test_recctl_cave.o: CFLAGS += -Ihal -Iports/cave
$(BUILD)/ports/cave/recctl_cave.o: CFLAGS += -DRCC_HOST=1
# the BT port keeps its policy's NOR part through the power port's kvstore (ota_main: PWC_STARTUP); here through
# tests/test_bt_cave.c's RAM-flash stand-ins of pwc_kv / pwc_kv_known / pwc_flash_idle (BTC_HOST: btc_host_pin, the tests written before the power-on pin run without it)
$(BUILD)/ports/cave/bt_cave.o: CFLAGS += -DBTC_KV=1 -DBTC_HOST=1
# the power port is tested as its TV-wake build (slot payload ota_power2; ota_sysex builds it without -
# the emulator round trip result/stockfw/tools/roundtrip_power.py covers that build)
$(BUILD)/ports/cave/power_cave.o $(BUILD)/tests/test_power_cave.o: CFLAGS += -DPWC_HOST=1
# host stubs for the standby / BT audio part of hal.h (cave binding at Phase 3-3)
$(BUILD)/ports/sim/hal_power_sim.o $(BUILD)/tests/test_power.o: CFLAGS += -Ihal -Iports/sim
$(BUILD)/ports/cave/cave_rt_host.o: ports/cave/cave_rt.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DCAVE_RT_HOST -MMD -MP -c $< -o $@

# the feature tests also exercise the simulator reference synth
$(BUILD)/ca79ng_tests: $(CORE_OBJ) $(TEST_OBJ) $(BUILD)/ports/sim/synth.o $(BUILD)/ports/sim/hal_power_sim.o $(CAVE_OBJ) $(POWER_CAVE_OBJ) $(SYSEX_CAVE_OBJ) $(BT_CAVE_OBJ) $(RELAY_CAVE_OBJ) $(REC_CAVE_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/ca79ng_sim: $(CORE_OBJ) $(SIM_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

test: $(BUILD)/ca79ng_tests
	./$(BUILD)/ca79ng_tests

sim: $(BUILD)/ca79ng_sim

$(BUILD)/updcheck: tools/updcheck.c $(CORE_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

# host run of the cave self-test (cross-checked against the ARM build by result/stockfw/tools/roundtrip_ota.py check 4)
$(BUILD)/cave_selftest: tools/cave_selftest.c $(CORE_OBJ) $(filter-out %cave_rt_host.o,$(CAVE_OBJ))
	$(CC) $(CFLAGS) -Ihal -Iports/cave $^ -o $@ $(LDLIBS)

# the settings-state module as a shared library for the emulator round trip result/stockfw/tools/roundtrip_state.py
# (ctypes; compiled straight from the sources into its own file - no shared objects with the other targets, so
# the round trip can build it while other checks build the firmware)
KST_SO_SRC := tools/kpstate_host.c src/kproto/kpstate.c src/kproto/store.c src/kproto/params_gen.c src/util/util.c
$(BUILD)/libkpstate.so: $(KST_SO_SRC) include/ca79ng/kpstate.h include/ca79ng/kproto.h include/ca79ng/params_gen.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -shared -fPIC $(KST_SO_SRC) -o $@.tmp$$$$ $(LDLIBS) && mv -f $@.tmp$$$$ $@

# the metronome module (Phase 3-5 stage 1) the same way, for result/stockfw/tools/roundtrip_metro.py
KPM_SO_SRC := tools/kpmetro_host.c src/kproto/kpmetro.c src/kproto/store.c src/kproto/params_gen.c src/util/util.c
$(BUILD)/libkpmetro.so: $(KPM_SO_SRC) include/ca79ng/kpmetro.h include/ca79ng/kproto.h include/ca79ng/params_gen.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -shared -fPIC $(KPM_SO_SRC) -o $@.tmp$$$$ $(LDLIBS) && mv -f $@.tmp$$$$ $@

tools: $(BUILD)/updcheck $(BUILD)/cave_selftest $(BUILD)/libkpstate.so $(BUILD)/libkpmetro.so

# end-to-end checks: unit/integration tests, update container cross-check, simulator smoke run
check: test tools sim
	head -c 300000 /dev/urandom > $(BUILD)/main.bin
	python3 -I tools/mkupd.py pack -o $(BUILD)/CA79NG.UPD --version 1.0.0 main=$(BUILD)/main.bin
	./$(BUILD)/updcheck $(BUILD)/CA79NG.UPD
	./$(BUILD)/cave_selftest > /dev/null
	printf 'midi panel F0 40 7F 7E 08 02 01 01 F7\nkey 60 100 200\nwait 300\nstatus\nquit\n' | ./$(BUILD)/ca79ng_sim | grep -q 'tx panel F0 40 7F 7E 04 1E 02 00 40 F7'
	# power policy (0x7D op 0x18) survives a simulator restart on the same settings flash
	rm -f $(BUILD)/policy_flash.bin
	printf 'midi panel F0 40 7F 7E 08 02 01 01 F7\nmidi panel F0 7D 43 41 37 39 18 01 10 01 02 3C 00 70 00 00 00 00 F7\nquit\n' | ./$(BUILD)/ca79ng_sim --flash $(BUILD)/policy_flash.bin | grep -q 'tx panel F0 7D 43 41 37 39 58 01 00 '
	printf 'midi usb F0 7D 43 41 37 39 18 00 F7\nquit\n' | ./$(BUILD)/ca79ng_sim --flash $(BUILD)/policy_flash.bin | grep -q 'tx usb F0 7D 43 41 37 39 58 00 00 10 01 02 3C 00 70 00 00 00 00 F7'
	@echo "check: all passed"

clean:
	rm -rf $(BUILD)

-include $(CORE_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(SIM_OBJ:.o=.d) $(CAVE_OBJ:.o=.d) $(POWER_CAVE_OBJ:.o=.d) $(SYSEX_CAVE_OBJ:.o=.d) $(BT_CAVE_OBJ:.o=.d) $(RELAY_CAVE_OBJ:.o=.d)
