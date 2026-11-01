# SPDX-License-Identifier: GPL-2.0-or-later
# Standalone QEMU ELF only. No production/native boot integration.
BUILD ?= build
RUN ?= $(BUILD)/qemu
DEPTH ?= 32
CC := clang
HOST_CC ?= cc
LD := aarch64-elf-ld
OBJDUMP := aarch64-elf-objdump
CFLAGS := --target=aarch64-none-elf -march=armv8.1-a -O2 -g -std=gnu11 \
 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -mgeneral-regs-only \
 -Wall -Wextra -Werror -Wconversion -Wshadow -fstack-usage -I. -I../include
CFLAGS += -fdebug-prefix-map=$(abspath $(BUILD))=/e0-build -fdebug-prefix-map=$(CURDIR)=/6boot/e0
HEADERS := e0.h ../include/el2_control.h ../include/el2_exception.h
OBJS := $(BUILD)/entry.o $(BUILD)/runtime.o $(BUILD)/stage2.o $(BUILD)/vectors.o $(BUILD)/start.o $(BUILD)/harness.o
ifeq ($(WORLDS),1)
HEADERS += worlds.h $(BUILD)/worlds_build.h
CFLAGS += -DE0_WORLDS -I$(BUILD)
OBJS += $(BUILD)/worlds.o $(BUILD)/worlds_policy.o $(BUILD)/worlds_payload.o
HARNESS := worlds_harness.c
ifeq ($(VISUAL),1)
CFLAGS += -DE0_VISUAL -DEW_PIXEL_DEPTH=$(DEPTH) -I../../../src
ifeq ($(filter $(DEPTH),30 32),)
$(error visual DEPTH must be 30 or 32)
endif
ifeq ($(SMOKE),1)
CFLAGS += -DE0_VISUAL_SMOKE
endif
OBJS += $(BUILD)/visual.o $(BUILD)/visual_check.o $(BUILD)/fb.o $(BUILD)/fb_memory.o $(BUILD)/font.o
E0_LDFLAGS := --gc-sections
VISUAL_INPUTS := visual.c visual_check.c visual_check.h visual_smoke.h ../../../src/fb.c ../../../src/fb.h ../../../src/fb_e0.h ../../../src/memory.c ../../../font/font.bin
endif
else
HARNESS := harness.c
endif
.PHONY: all test firmware qemu
all: test firmware
$(BUILD):
	mkdir -p $@
$(BUILD)/host-test: stage2.c host_test.c $(HEADERS) | $(BUILD)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror -Wconversion -I. -I../include -fsanitize=address,undefined stage2.c host_test.c -o $@
test: $(BUILD)/host-test
	$(BUILD)/host-test
ifeq ($(WORLDS),1)
	$(BUILD)/worlds-host-test
test: $(BUILD)/worlds-host-test
$(BUILD)/worlds-host-test: stage2.c worlds_policy.c worlds_test.c $(HEADERS) | $(BUILD)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror -Wconversion -Wshadow -I. -I../include -fsanitize=address,undefined stage2.c worlds_policy.c worlds_test.c -o $@
$(BUILD)/worlds_build.h: entry.S runtime.c stage2.c worlds.c worlds.h worlds_policy.c worlds.S worlds_harness.c e0.h ../arch/vectors.S ../tests/qemu/start.S ../include/el2_control.h ../include/el2_exception.h module.ld Makefile $(VISUAL_INPUTS) | $(BUILD)
	python3 -I -B -c 'import hashlib,pathlib,sys; h=hashlib.sha256(b"WORLDS=1;VISUAL=$(if $(filter 1,$(VISUAL)),1,0);SMOKE=$(if $(filter 1,$(SMOKE)),1,0);DEPTH=$(DEPTH)\0"); [(h.update(p.encode()+b"\0"),h.update(pathlib.Path(p).read_bytes())) for p in sorted(sys.argv[2:])]; pathlib.Path(sys.argv[1]).write_text("\043define EW_BUILD_ID UINT64_C(0x"+h.hexdigest()[:16]+")\n")' $@ $^
$(BUILD)/worlds_payload.o: worlds.S $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
endif
ifeq ($(VISUAL),1)
$(BUILD)/visual.o: visual.c $(HEADERS) visual_check.h | $(BUILD)
	$(CC) $(filter-out -Wconversion,$(CFLAGS)) -c $< -o $@
$(BUILD)/fb-source/src/fb.c: ../../../src/fb.c ../../../src/memory.c ../../../data/die_blocks.h | $(BUILD)
	python3 -I -B -c 'from pathlib import Path; import shutil,sys; p=Path(sys.argv[1]); (p/"src").mkdir(parents=True,exist_ok=True); (p/"build").mkdir(exist_ok=True); shutil.copy2("../../../src/fb.c",p/"src/fb.c"); shutil.copy2("../../../data/die_blocks.h",p/"build/die_blocks.h"); (p/"build/build_cfg.h").write_text("\043define SIXBOOT_E0_VISUAL 1\n")' $(BUILD)/fb-source
$(BUILD)/fb.o: $(BUILD)/fb-source/src/fb.c ../../../src/fb.h
	$(CC) $(filter-out -Wconversion -Wshadow,$(CFLAGS)) -ffunction-sections -fdata-sections -I../../../src -I../../../src/libfdt -I../../../sysinc -c $< -o $@
$(BUILD)/fb-source/src/memory.c: $(BUILD)/fb-source/src/fb.c
	cp ../../../src/memory.c $@
$(BUILD)/fb_memory.o: $(BUILD)/fb-source/src/memory.c ../../../src/memory.h | $(BUILD)
	$(CC) $(filter-out -Wconversion -Wshadow,$(CFLAGS)) -ffunction-sections -fdata-sections -I../../../src -I../../../src/libfdt -I../../../sysinc -c $< -o $@
$(BUILD)/font.o: ../../../font/font.bin | $(BUILD)
	python3 -I -B -c 'from pathlib import Path; import sys; Path(sys.argv[1]).write_text(".section .rodata.font,\"a\"\n.global _binary_build_font_bin_start\n_binary_build_font_bin_start:\n.incbin \""+str(Path(sys.argv[2]).resolve())+"\"\n")' $(BUILD)/font.S $<
	$(CC) $(CFLAGS) -c $(BUILD)/font.S -o $@
$(BUILD)/visual-check: visual_check.c visual_check_cli.c visual_check.h | $(BUILD)
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror -Wconversion -fsanitize=address,undefined visual_check.c visual_check_cli.c -o $@
endif
$(BUILD)/harness.o: $(HARNESS) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: %.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/entry.o: entry.S $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/vectors.o: ../arch/vectors.S $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/start.o: ../tests/qemu/start.S | $(BUILD)
	$(CC) $(CFLAGS) -DE2_TEST_HCR=0x32488000038 -c $< -o $@
$(BUILD)/e0.elf: $(OBJS) module.ld
	$(LD) $(E0_LDFLAGS) --no-undefined --fatal-warnings -z max-page-size=0x4000 -T module.ld $(OBJS) -o $@
$(BUILD)/e0.dis: $(BUILD)/e0.elf
	$(OBJDUMP) -d $< > $@
firmware: $(BUILD)/e0.elf $(BUILD)/e0.dis
	python3 -I -B audit.py --build $(BUILD)
qemu: all
ifeq ($(WORLDS),1)
ifeq ($(VISUAL),1)
ifeq ($(SMOKE),1)
	python3 -I -B test_restore.py --visual-smoke --build $(BUILD) --out $(RUN) --qemu "$(QEMU)" --lib "$(QEMU_LIB_DIR)"
else
	python3 -I -B test_restore.py --visual --build $(BUILD) --out $(RUN) --qemu "$(QEMU)" --lib "$(QEMU_LIB_DIR)"
	python3 -I -B test_restore.py --visual --bad-trace --build $(BUILD) --out $(RUN)-bad --qemu "$(QEMU)" --lib "$(QEMU_LIB_DIR)"
	$(MAKE) WORLDS=1 VISUAL=1 BUILD=$(BUILD) $(BUILD)/visual-check
	python3 -I -B worlds_trace_test.py --records $(RUN) --c-validator $(BUILD)/visual-check
endif
else
	python3 -I -B worlds_run.py --build $(BUILD) --qemu "$(QEMU)" --lib "$(QEMU_LIB_DIR)" --out $(RUN)
	python3 -I -B worlds_trace_test.py --records $(RUN)
	python3 -I -B test_restore.py --worlds --build $(BUILD) --out $(RUN)/negative --qemu "$(QEMU)" --lib "$(QEMU_LIB_DIR)"
endif
else
	python3 -I -B run_qemu.py --build $(BUILD) --qemu "$(QEMU)" --lib "$(QEMU_LIB_DIR)" --out $(RUN)
	python3 -I -B test_restore.py --build $(BUILD) --out $(RUN)/restore-trap --qemu "$(QEMU)" --lib "$(QEMU_LIB_DIR)"
endif
