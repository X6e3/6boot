# SPDX-License-Identifier: GPL-2.0-or-later
# Host tests for src/xboot_bootstate.c (trial boots, installer level 2). Nothing here goes into stage 2.
#   make -C tests/bootstate           unit tests, the cross test with tools/bootstate.py, and a
#                                     freestanding compile with 6boot's own flags
#   make -C tests/bootstate fat       also FAT32 images made by macOS (needs hdiutil)
#   make -C tests/bootstate mutate    every one-line mutant of the module must fail the tests
CC ?= cc
R := ../..
BUILD ?= $(R)/build/bootstate
SRC := $(R)/src/xboot_bootstate.c
HDR := $(R)/src/xboot_bootstate.h
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all
# As in 6boot's Makefile: freestanding, no host headers but the compiler's and sysinc/.
FW := --target=aarch64-none-elf -std=gnu11 -O2 -Wall -Wextra -Werror -Wundef -Werror=strict-prototypes \
	-ffreestanding -fno-builtin -mgeneral-regs-only -nostdinc \
	-isystem $(shell $(CC) -print-file-name=include) -isystem $(R)/sysinc
.PHONY: all test cross freestanding fat mutate
all: test cross freestanding
$(BUILD):
	mkdir -p $@
$(BUILD)/bootstate-test: bootstate_test.c $(SRC) $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) bootstate_test.c $(SRC) -o $@
test: $(BUILD)/bootstate-test
	$<
cross: $(BUILD)/bootstate-test
	python3 -I cross_test.py $<
freestanding: | $(BUILD)
	$(CC) $(FW) -c $(SRC) -o $(BUILD)/xboot_bootstate.o
	@echo "freestanding: $(SRC) compiles for aarch64-none-elf without host headers"
fat: $(BUILD)/bootstate-test
	python3 -I fat_test.py $<
mutate:
	./mutate.sh
