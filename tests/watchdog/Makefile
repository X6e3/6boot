# SPDX-License-Identifier: GPL-2.0-or-later
# The watchdog tests' heartbeat (heartbeat.c) and its log check (check_heartbeat.py).
#   make -C tests/watchdog unit     the check on written logs (test_check_heartbeat.py): any host, Python 3
#   make -C tests/watchdog          unit, then on Linux aarch64 only (the build VM; CNTVCT_EL0,
#                                   CLOCK_BOOTTIME): build the heartbeat, run it for 2 s and check the log:
#                                   a file under a test path stands in for /dev/watchdog, held open by sleep
#                                   for the first second; fake_wdt.py stands in for the watchdog's registers
#                                   (a file as /dev/mem; the read is on through a command line file with
#                                   x6e3.hbwdt=1); as root the kernel log marks run too. Then every fake
#                                   watchdog scenario (wdt_scenarios.py).
#   make -C tests/watchdog j713-9da check the log of the J713's 9D-a run (LOG=..., ARMED=...)
CC ?= cc
BUILD ?= ../../build/watchdog
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror
.PHONY: test unit j713-9da
test: unit
	@if [ "$$(uname -s)-$$(uname -m)" != Linux-aarch64 ]; then echo "heartbeat: skipped, needs Linux on aarch64"; exit 0; fi; \
	$(MAKE) -s $(BUILD)/heartbeat || exit 1; \
	mkdir -p $(BUILD)/run && rm -f $(BUILD)/run/*.log && : > $(BUILD)/run/fake-watchdog0; \
	pre="$$(cd $(BUILD)/run && pwd -P)/fake-watchdog"; \
	echo "console=tty0 x6e3.hb=1 x6e3.hbwdt=1" > $(BUILD)/run/cmdline; \
	rm -rf $(BUILD)/run/dt $(BUILD)/run/fake-mem $(BUILD)/run/fake-wdt.*; python3 -I fake_wdt.py $(BUILD)/run 3 & f=$$!; \
	while [ ! -e $(BUILD)/run/fake-wdt.ready ]; do sleep 0.05; done; \
	sleep 1 < $(BUILD)/run/fake-watchdog0 & s=$$!; \
	$(BUILD)/heartbeat $(BUILD)/run "$$pre" $(BUILD)/run/dt/soc $(BUILD)/run/fake-mem $(BUILD)/run/cmdline & p=$$!; \
	sleep 2; kill -TERM $$p; wait $$p; wait $$s; wait $$f; \
	python3 -I check_heartbeat.py --stopped --opened-by sleep --expect-feed --wdt-tol 0.05 --cur-tol-ms 5 \
		$(BUILD)/run/*.log || exit 1; \
	python3 -I wdt_scenarios.py $(BUILD)/heartbeat $(BUILD)/scenarios
unit:
	python3 -I test_check_heartbeat.py
$(BUILD)/heartbeat: heartbeat.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@
$(BUILD):
	mkdir -p $@
j713-9da:
	python3 -I check_heartbeat.py $(LOG) --armed $(ARMED)
