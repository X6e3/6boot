# SPDX-License-Identifier: GPL-2.0-or-later
# Host tests for trial boots (installer level 2): the record and decision (src/xboot_bootstate.c),
# the read-only ESP reader (src/xboot_esp.c), SHA-256 (src/xboot_sha256.c) and the whole check
# (src/xboot_trial.c). Nothing here goes into stage 2 by itself.
#   make -C tests/bootstate           unit tests, the cross test with tools/bootstate.py, the SHA-256 and
#                                     UUID tests, and a freestanding compile with 6boot's own flags
#   make -C tests/bootstate fat       also FAT32 images made by macOS (needs hdiutil)
#   make -C tests/bootstate esp-fat   GPT disks with a FAT32 ESP made by macOS: lookup, hash, the whole check
#   make -C tests/bootstate mutate    every one-line mutant of the record module must fail the tests
#   make -C tests/bootstate mutate-esp  the same for the ESP reader, the trial check and SHA-256 (macOS)
CC ?= cc
R := ../..
BUILD ?= $(R)/build/bootstate
SRC := $(R)/src/xboot_bootstate.c
HDR := $(R)/src/xboot_bootstate.h
ESRC := $(R)/src/xboot_esp.c $(R)/src/xboot_sha256.c $(R)/src/xboot_trial.c $(SRC)
EHDR := $(R)/src/xboot_esp.h $(R)/src/xboot_sha256.h $(R)/src/xboot_trial.h $(HDR)
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all
# As in 6boot's Makefile: freestanding, no host headers but the compiler's and sysinc/.
FW := --target=aarch64-none-elf -std=gnu11 -O2 -Wall -Wextra -Werror -Wundef -Werror=strict-prototypes \
	-ffreestanding -fno-builtin -mgeneral-regs-only -nostdinc \
	-isystem $(shell $(CC) -print-file-name=include) -isystem $(R)/sysinc
.PHONY: all test cross freestanding fat mutate mutate-esp esp esp-fat
all: test cross esp freestanding
$(BUILD):
	mkdir -p $@
$(BUILD)/bootstate-test: bootstate_test.c $(SRC) $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) bootstate_test.c $(SRC) -o $@
$(BUILD)/esp-test: esp_test.c $(ESRC) $(EHDR) | $(BUILD)
	$(CC) $(CFLAGS) esp_test.c $(ESRC) -o $@
test: $(BUILD)/bootstate-test
	$<
cross: $(BUILD)/bootstate-test
	python3 -I cross_test.py $<
esp: $(BUILD)/esp-test
	$<
freestanding: | $(BUILD)
	$(CC) $(FW) -c $(R)/src/xboot_bootstate.c -o $(BUILD)/xboot_bootstate.o
	$(CC) $(FW) -c $(R)/src/xboot_esp.c -o $(BUILD)/xboot_esp.o
	$(CC) $(FW) -c $(R)/src/xboot_sha256.c -o $(BUILD)/xboot_sha256.o
	$(CC) $(FW) -c $(R)/src/xboot_trial.c -o $(BUILD)/xboot_trial.o
	@echo "freestanding: the four modules compile for aarch64-none-elf without host headers"
fat: $(BUILD)/bootstate-test
	python3 -I fat_test.py $<
esp-fat: $(BUILD)/esp-test
	python3 -I esp_fixture_test.py $<
mutate:
	./mutate.sh
mutate-esp:
	./mutate_esp.sh
