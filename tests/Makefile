# Test harness for the defender project.
# Builds host-native test binaries that link libsimavr (or just exercise pure
# host-side models) and assert against expected behavior.

SIMAVR_DIR := $(abspath ../../tools/simavr)
SIMAVR_INC := $(SIMAVR_DIR)/simavr/sim
SIMAVR_TOP := $(SIMAVR_DIR)/simavr
SIMAVR_LIB := $(SIMAVR_DIR)/simavr/obj-x86_64-linux-gnu

CC      := gcc
CFLAGS  := -O0 -g -Wall -Wextra -I$(SIMAVR_INC) -I$(SIMAVR_TOP)
LDFLAGS := -L$(SIMAVR_LIB) -Wl,-rpath,$(SIMAVR_LIB)
LIBS    := -lsimavr -lelf

BUILD := build

.PHONY: all test profile clean

all: $(BUILD)/test_blinky $(BUILD)/test_ssd1306 $(BUILD)/test_oled $(BUILD)/test_defender \
     $(BUILD)/test_collision $(BUILD)/test_proj_motion $(BUILD)/test_input \
     $(BUILD)/test_lander_drift $(BUILD)/test_proj_spawn $(BUILD)/test_ship_collision \
     $(BUILD)/test_lander_spawn $(BUILD)/test_bug_repros $(BUILD)/test_frame_rate \
     $(BUILD)/test_audio $(BUILD)/test_humanoids $(BUILD)/test_abduction \
     $(BUILD)/test_freeing $(BUILD)/test_seek $(BUILD)/test_mutants

$(BUILD):
	mkdir -p $@

# libsimavr-linked tests that load device firmware
$(BUILD)/test_blinky: test_blinky.c | $(BUILD)
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_oled: test_oled.c ssd1306.c ssd1306.h | $(BUILD)
	$(CC) $(CFLAGS) test_oled.c ssd1306.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_defender: test_defender.c ssd1306.c ssd1306.h | $(BUILD)
	$(CC) $(CFLAGS) test_defender.c ssd1306.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_collision: test_collision.c | $(BUILD)
	$(CC) $(CFLAGS) test_collision.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_proj_motion: test_proj_motion.c sim_helpers.c sim_helpers.h | $(BUILD)
	$(CC) $(CFLAGS) test_proj_motion.c sim_helpers.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_input: test_input.c sim_helpers.c sim_helpers.h | $(BUILD)
	$(CC) $(CFLAGS) test_input.c sim_helpers.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_lander_drift: test_lander_drift.c sim_helpers.c sim_helpers.h | $(BUILD)
	$(CC) $(CFLAGS) test_lander_drift.c sim_helpers.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_proj_spawn: test_proj_spawn.c sim_helpers.c sim_helpers.h | $(BUILD)
	$(CC) $(CFLAGS) test_proj_spawn.c sim_helpers.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_ship_collision: test_ship_collision.c sim_helpers.c sim_helpers.h | $(BUILD)
	$(CC) $(CFLAGS) test_ship_collision.c sim_helpers.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_lander_spawn: test_lander_spawn.c sim_helpers.c sim_helpers.h | $(BUILD)
	$(CC) $(CFLAGS) test_lander_spawn.c sim_helpers.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_bug_repros: test_bug_repros.c sim_helpers.c sim_helpers.h | $(BUILD)
	$(CC) $(CFLAGS) test_bug_repros.c sim_helpers.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_frame_rate: test_frame_rate.c sim_helpers.c sim_helpers.h | $(BUILD)
	$(CC) $(CFLAGS) test_frame_rate.c sim_helpers.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_audio: test_audio.c sim_helpers.c sim_helpers.h | $(BUILD)
	$(CC) $(CFLAGS) test_audio.c sim_helpers.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_humanoids: test_humanoids.c sim_helpers.c sim_helpers.h | $(BUILD)
	$(CC) $(CFLAGS) test_humanoids.c sim_helpers.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_abduction: test_abduction.c sim_helpers.c sim_helpers.h | $(BUILD)
	$(CC) $(CFLAGS) test_abduction.c sim_helpers.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_freeing: test_freeing.c sim_helpers.c sim_helpers.h | $(BUILD)
	$(CC) $(CFLAGS) test_freeing.c sim_helpers.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_seek: test_seek.c sim_helpers.c sim_helpers.h | $(BUILD)
	$(CC) $(CFLAGS) test_seek.c sim_helpers.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/test_mutants: test_mutants.c sim_helpers.c sim_helpers.h | $(BUILD)
	$(CC) $(CFLAGS) test_mutants.c sim_helpers.c $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/profile: profile.c sim_helpers.c sim_helpers.h | $(BUILD)
	$(CC) $(CFLAGS) profile.c sim_helpers.c $(LDFLAGS) $(LIBS) -o $@

profile: $(BUILD)/profile
	@$(MAKE) -C .. all >/dev/null
	$(BUILD)/profile ../build/defender.elf
	@echo
	./profile_report.py ../build/defender.elf

# Host-side unit tests (pull in ssd1306.c; ssd1306.c includes simavr headers
# for the attach() callbacks, so we still link libsimavr)
$(BUILD)/test_ssd1306: test_ssd1306.c ssd1306.c ssd1306.h | $(BUILD)
	$(CC) $(CFLAGS) test_ssd1306.c ssd1306.c $(LDFLAGS) $(LIBS) -o $@

test: all
	@$(MAKE) -C .. all >/dev/null
	@fail=0; \
	echo ">>> $(BUILD)/test_blinky"; \
	$(BUILD)/test_blinky ../build/blinky.elf || fail=1; \
	echo ">>> $(BUILD)/test_ssd1306"; \
	$(BUILD)/test_ssd1306 || fail=1; \
	echo ">>> $(BUILD)/test_oled"; \
	$(BUILD)/test_oled ../build/oled.elf || fail=1; \
	echo ">>> $(BUILD)/test_defender"; \
	$(BUILD)/test_defender ../build/defender.elf || fail=1; \
	echo ">>> $(BUILD)/test_collision"; \
	$(BUILD)/test_collision ../build/defender.elf || fail=1; \
	echo ">>> $(BUILD)/test_proj_motion"; \
	$(BUILD)/test_proj_motion ../build/defender.elf || fail=1; \
	echo ">>> $(BUILD)/test_input"; \
	$(BUILD)/test_input ../build/defender.elf || fail=1; \
	echo ">>> $(BUILD)/test_lander_drift"; \
	$(BUILD)/test_lander_drift ../build/defender.elf || fail=1; \
	echo ">>> $(BUILD)/test_proj_spawn"; \
	$(BUILD)/test_proj_spawn ../build/defender.elf || fail=1; \
	echo ">>> $(BUILD)/test_ship_collision"; \
	$(BUILD)/test_ship_collision ../build/defender.elf || fail=1; \
	echo ">>> $(BUILD)/test_lander_spawn"; \
	$(BUILD)/test_lander_spawn ../build/defender.elf || fail=1; \
	echo ">>> $(BUILD)/test_bug_repros"; \
	$(BUILD)/test_bug_repros ../build/defender.elf || fail=1; \
	echo ">>> $(BUILD)/test_frame_rate"; \
	$(BUILD)/test_frame_rate ../build/defender.elf || fail=1; \
	echo ">>> $(BUILD)/test_audio"; \
	$(BUILD)/test_audio ../build/defender.elf || fail=1; \
	echo ">>> $(BUILD)/test_humanoids"; \
	$(BUILD)/test_humanoids ../build/defender.elf || fail=1; \
	echo ">>> $(BUILD)/test_abduction"; \
	$(BUILD)/test_abduction ../build/defender.elf || fail=1; \
	echo ">>> $(BUILD)/test_freeing"; \
	$(BUILD)/test_freeing ../build/defender.elf || fail=1; \
	echo ">>> $(BUILD)/test_seek"; \
	$(BUILD)/test_seek ../build/defender.elf || fail=1; \
	echo ">>> $(BUILD)/test_mutants"; \
	$(BUILD)/test_mutants ../build/defender.elf || fail=1; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "ALL TESTS PASSED"

clean:
	rm -rf $(BUILD)
