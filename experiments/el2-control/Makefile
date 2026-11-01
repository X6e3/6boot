# SPDX-License-Identifier: GPL-2.0-or-later
# Explicit standalone build only; never invoked by the production Makefile.
BUILD ?= build
HOST_CC ?= cc
FW_CC ?= clang
FW_TARGET ?= --target=aarch64-none-elf
CROSS_COMPILE ?= aarch64-elf-
FW_LD ?= $(CROSS_COMPILE)ld
OBJCOPY ?= $(CROSS_COMPILE)objcopy
OBJDUMP ?= $(CROSS_COMPILE)objdump
NM ?= $(CROSS_COMPILE)nm
PYTHON ?= python3
QEMU ?= qemu-system-aarch64
QEMU_LIB_DIR ?=
WARN := -Wall -Wextra -Werror -Wconversion -Wshadow
COMMON := -std=c11 -O2 $(WARN) -Iinclude
FWFLAGS := $(COMMON) $(FW_TARGET) -ffreestanding -fno-builtin \
           -fno-stack-protector -fno-pic -mgeneral-regs-only \
           -fno-unwind-tables -fno-asynchronous-unwind-tables \
           -ffunction-sections -fdata-sections -fstack-usage
HOSTFLAGS := $(COMMON) -g -fsanitize=address,undefined -fno-omit-frame-pointer
HEADERS := include/el2_control.h include/el2_trace.h include/el2_exception.h
OBJECTS := $(BUILD)/entry.o $(BUILD)/session.o $(BUILD)/vectors.o \
           $(BUILD)/guest.o $(BUILD)/control.o $(BUILD)/trace.o $(BUILD)/exception.o
CODE_SECTIONS := --only-section=.text --only-section=.rodata --only-section=.vectors \
                 --only-section=.guest_text
FDT_DIR := ../../src/libfdt
FDT_SRC := $(wildcard $(FDT_DIR)/*.c)

.PHONY: all test firmware audit audit-tests qemu-test qemu-matrix
all: test firmware audit-tests
$(BUILD):
	mkdir -p $@
$(BUILD)/host-test: core/control.c core/trace.c tests/host_test.c $(HEADERS) | $(BUILD)
	$(HOST_CC) $(HOSTFLAGS) core/control.c core/trace.c tests/host_test.c -o $@
$(BUILD)/exception-test: core/control.c core/exception.c tests/exception_test.c $(HEADERS) | $(BUILD)
	$(HOST_CC) $(HOSTFLAGS) core/control.c core/exception.c tests/exception_test.c -o $@
$(BUILD)/native-test: native/policy.c native/fdt_memory.c native/native.h tests/native_test.c $(HEADERS) $(FDT_SRC) | $(BUILD)
	$(HOST_CC) $(filter-out -Wconversion -Wshadow,$(HOSTFLAGS)) -D_POSIX_C_SOURCE=200809L -include stdlib.h -I$(FDT_DIR) native/policy.c native/fdt_memory.c tests/native_test.c $(FDT_SRC) -o $@
test: $(BUILD)/host-test $(BUILD)/exception-test $(BUILD)/native-test
	$(BUILD)/host-test
	$(BUILD)/exception-test
	$(BUILD)/native-test
$(BUILD)/entry.o: arch/entry.S $(HEADERS) | $(BUILD)
	$(FW_CC) $(FW_TARGET) -Iinclude -c $< -o $@
$(BUILD)/session.o: arch/exception.S $(HEADERS) | $(BUILD)
	$(FW_CC) $(FW_TARGET) -Iinclude -c $< -o $@
$(BUILD)/vectors.o: arch/vectors.S $(HEADERS) | $(BUILD)
	$(FW_CC) $(FW_TARGET) -Iinclude -c $< -o $@
$(BUILD)/guest.o: arch/guest.S $(HEADERS) | $(BUILD)
	$(FW_CC) $(FW_TARGET) -Iinclude -c $< -o $@
$(BUILD)/control.o: core/control.c $(HEADERS) | $(BUILD)
	$(FW_CC) $(FWFLAGS) -c $< -o $@
$(BUILD)/trace.o: core/trace.c $(HEADERS) | $(BUILD)
	$(FW_CC) $(FWFLAGS) -c $< -o $@
$(BUILD)/exception.o: core/exception.c $(HEADERS) | $(BUILD)
	$(FW_CC) $(FWFLAGS) -c $< -o $@
$(BUILD)/el2-foundation.elf: $(OBJECTS) module.ld
	$(FW_LD) --no-relax --no-undefined --fatal-warnings -z max-page-size=0x4000 \
	  -T module.ld $(OBJECTS) -o $@
$(BUILD)/el2-foundation.bin: $(BUILD)/el2-foundation.elf
	$(OBJCOPY) -O binary $(CODE_SECTIONS) $< $@
$(BUILD)/el2-foundation.dis: $(BUILD)/el2-foundation.elf
	$(OBJDUMP) -d $< > $@
audit: $(BUILD)/el2-foundation.elf $(BUILD)/el2-foundation.bin $(BUILD)/el2-foundation.dis
	$(PYTHON) -I -B tools/audit_elf.py $(BUILD)/el2-foundation.elf \
	  $(BUILD)/el2-foundation.bin $(BUILD)/el2-foundation.dis $(BUILD)/exception.su
firmware: audit
audit-tests: audit
	$(PYTHON) -I -B tools/test_audit.py $(BUILD) $(OBJCOPY) $(OBJDUMP) $(NM)
qemu-test: firmware
	$(PYTHON) -I -B tools/run_qemu.py --qemu "$(QEMU)" --qemu-lib-dir="$(QEMU_LIB_DIR)" --build "$(BUILD)" \
	  --cc "$(FW_CC)" --target="$(FW_TARGET)" --ld "$(FW_LD)" --nm "$(NM)"
qemu-matrix: qemu-test
	$(PYTHON) -I -B tools/run_qemu.py --qemu "$(QEMU)" --qemu-lib-dir="$(QEMU_LIB_DIR)" --build "$(BUILD)" --fault setup \
	  --cc "$(FW_CC)" --target="$(FW_TARGET)" --ld "$(FW_LD)" --nm "$(NM)"
	$(PYTHON) -I -B tools/run_qemu.py --qemu "$(QEMU)" --qemu-lib-dir="$(QEMU_LIB_DIR)" --build "$(BUILD)" --fault hcr \
	  --cc "$(FW_CC)" --target="$(FW_TARGET)" --ld "$(FW_LD)" --nm "$(NM)"
	$(PYTHON) -I -B tools/run_qemu.py --qemu "$(QEMU)" --qemu-lib-dir="$(QEMU_LIB_DIR)" --build "$(BUILD)" --fault restore \
	  --cc "$(FW_CC)" --target="$(FW_TARGET)" --ld "$(FW_LD)" --nm "$(NM)"
	$(PYTHON) -I -B tools/run_qemu.py --qemu "$(QEMU)" --qemu-lib-dir="$(QEMU_LIB_DIR)" --build "$(BUILD)" --fault identity \
	  --cc "$(FW_CC)" --target="$(FW_TARGET)" --ld "$(FW_LD)" --nm "$(NM)"
	$(PYTHON) -I -B tools/run_qemu.py --qemu "$(QEMU)" --qemu-lib-dir="$(QEMU_LIB_DIR)" --build "$(BUILD)" --fault sp \
	  --cc "$(FW_CC)" --target="$(FW_TARGET)" --ld "$(FW_LD)" --nm "$(NM)"
	$(PYTHON) -I -B tools/run_qemu.py --qemu "$(QEMU)" --qemu-lib-dir="$(QEMU_LIB_DIR)" --build "$(BUILD)" --fault sequence \
	  --cc "$(FW_CC)" --target="$(FW_TARGET)" --ld "$(FW_LD)" --nm "$(NM)"

# The production native caller gate, exercised around the actual module blob.
.PHONY: qemu-native-matrix
qemu-native-matrix: firmware
	@set -e; for fault in none setup hcr restore identity sp sequence; do \
	  $(PYTHON) -I -B tools/run_qemu.py --native-gate --qemu "$(QEMU)" --qemu-lib-dir="$(QEMU_LIB_DIR)" --build "$(BUILD)" --fault $$fault --cc "$(FW_CC)" --target="$(FW_TARGET)" --ld "$(FW_LD)" --nm "$(NM)"; \
	done
