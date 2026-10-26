CC := gcc
LD := ld
BUILD ?= /tmp/j713-xboot-p3-entry-20261010
COMMON := -std=c11 -O2 -Wall -Wextra -Werror -Iinclude
FW := $(COMMON) -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -mgeneral-regs-only -fno-asynchronous-unwind-tables
.PHONY: all test firmware
all: test firmware
$(BUILD):
	mkdir -p $@
$(BUILD)/probe-test: core/probe.c tests/probe_test.c include/xboot_probe.h | $(BUILD)
	$(CC) $(COMMON) -fsanitize=address,undefined -g core/probe.c tests/probe_test.c -o $@
test: $(BUILD)/probe-test
	$<
$(BUILD)/probe.o: core/probe.c include/xboot_probe.h | $(BUILD)
	$(CC) $(FW) -c $< -o $@
$(BUILD)/entry.o: arch/entry.S | $(BUILD)
	$(CC) -c $< -o $@
$(BUILD)/xboot-entry-probe.elf: $(BUILD)/entry.o $(BUILD)/probe.o probe.ld
	$(LD) --no-undefined -T probe.ld $(BUILD)/entry.o $(BUILD)/probe.o -o $@
firmware: $(BUILD)/xboot-entry-probe.elf
