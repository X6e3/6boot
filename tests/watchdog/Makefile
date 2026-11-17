# SPDX-License-Identifier: GPL-2.0-or-later
# The watchdog tests' heartbeat (heartbeat.c). Linux on aarch64 only (CNTVCT_EL0, CLOCK_BOOTTIME):
# the build VM, or the J713 itself.
#   make -C tests/watchdog          build it, run it for 2 s and check the log (check_heartbeat.py)
#   make -C tests/watchdog j713-9da check the log of the J713's 9D-a run (LOG=..., ARMED=...)
CC ?= cc
BUILD ?= ../../build/watchdog
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror
.PHONY: test j713-9da
test:
	@if [ "$$(uname -s)-$$(uname -m)" != Linux-aarch64 ]; then echo "heartbeat: skipped, needs Linux on aarch64"; exit 0; fi; \
	$(MAKE) -s $(BUILD)/heartbeat || exit 1; \
	mkdir -p $(BUILD)/run && rm -f $(BUILD)/run/*.log; \
	$(BUILD)/heartbeat $(BUILD)/run & p=$$!; sleep 2; kill -TERM $$p; wait $$p; \
	python3 -I check_heartbeat.py --stopped $(BUILD)/run/*.log
$(BUILD)/heartbeat: heartbeat.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@
$(BUILD):
	mkdir -p $@
j713-9da:
	python3 -I check_heartbeat.py $(LOG) --armed $(ARMED)
