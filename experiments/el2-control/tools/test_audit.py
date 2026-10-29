#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Mutate temporary ARM ELF copies; never execute them or access a device."""
import importlib.util
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest

build, objcopy, objdump, nm = sys.argv[1:]
build = Path(build).resolve()
spec = importlib.util.spec_from_file_location("el2_audit", Path(__file__).with_name("audit_elf.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class InstructionGuards(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.elf = (build / "el2-foundation.elf").read_bytes()
        cls.dis = (build / "el2-foundation.dis").read_text()
        cls.symbols = {}
        result = subprocess.check_output([nm, "-S", str(build / "el2-foundation.elf")], text=True)
        for line in result.splitlines():
            parts = line.split()
            if len(parts) >= 3:
                cls.symbols[parts[-1]] = int(parts[0], 16)

    def find(self, operation, prefix, lo=0, hi=1 << 64):
        for line in self.dis.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+([a-z][a-z0-9_.]*)\s*(.*)", line)
            if m and lo <= int(m[1], 16) < hi and m[2] == operation and m[3].startswith(prefix):
                return int(m[1], 16)
        self.fail("instruction not found: " + operation + " " + prefix)

    def reject_mutation(self, address, word, reason):
        data = bytearray(self.elf)
        phoff = struct.unpack_from("<Q", data, 32)[0]
        phnum = struct.unpack_from("<H", data, 56)[0]
        for i in range(phnum):
            kind, _, offset, va, _, size, _, _ = struct.unpack_from("<IIQQQQQQ", data, phoff + 56 * i)
            if kind == 1 and va <= address < va + size:
                struct.pack_into("<I", data, offset + address - va, word)
                break
        else:
            self.fail("mutation address outside file-backed segment")
        with tempfile.TemporaryDirectory(prefix="audit-mutation-", dir=build) as d:
            p = Path(d); elf = p / "bad.elf"; binary = p / "bad.bin"; dis = p / "bad.dis"
            elf.write_bytes(data)
            subprocess.run([objcopy, "-O", "binary", "--only-section=.text", "--only-section=.rodata",
                            "--only-section=.vectors", "--only-section=.guest_text",
                            str(elf), str(binary)], check=True)
            dis.write_bytes(subprocess.check_output([objdump, "-d", str(elf)]))
            with self.assertRaisesRegex(SystemExit, reason):
                module.audit(elf, binary, dis, build / "exception.su")

    def test_baseline(self):
        self.assertEqual(module.audit(build / "el2-foundation.elf", build / "el2-foundation.bin",
                                      build / "el2-foundation.dis", build / "exception.su")["status"], "PASS")

    def test_missing_x18_restore(self):
        at = self.find("ldp", "x18, x19", self.symbols["e2_exception_common"], self.symbols["e2_exception_bad_stack"])
        self.reject_mutation(at, 0xd503201f, "missing GPR restore pair 18")

    def test_missing_vbar_barrier(self):
        self.reject_mutation(self.symbols["e2_install_vectors"] + 8, 0xd503201f, "VBAR write needs ISB")

    def test_wrong_frame_unwind(self):
        at = self.find("eret", "", self.symbols["e2_exception_common"], self.symbols["e2_exception_bad_stack"])
        self.reject_mutation(at - 4, 0xd503201f, "unwind exactly")

    def test_wrong_vector_source(self):
        at = self.find("mov", "w0, #0x4", self.symbols["e2_vector_4"], self.symbols["e2_vector_5"])
        self.reject_mutation(at, 0x52800000, "vector source tags")

    def test_hcr_write(self):
        # MRS HCR_EL2 -> corresponding MSR, used only in a non-executed copy.
        at = self.find("mrs", "x10, hcr_el2", self.symbols["e2_exception_run"], self.symbols["e2_install_vectors"])
        phoff = struct.unpack_from("<Q", self.elf, 32)[0]
        code_offset = struct.unpack_from("<Q", self.elf, phoff + 8)[0]
        word = struct.unpack_from("<I", self.elf, code_offset + at)[0] & ~(1 << 21)
        self.reject_mutation(at, word, "HCR_EL2 write outside the reviewed sites")

    def test_guest_transition(self):
        self.reject_mutation(self.symbols["e2_expected_brk"], 0xd4000002, "HVC outside the EL1 guest")

    def test_guest_hcr_barrier(self):
        self.reject_mutation(self.symbols["e2_guest_hcr_write"] + 4, 0xd503201f,
                             "HCR write needs ISB: e2_guest_hcr_write")

    def test_restore_hcr_barrier(self):
        self.reject_mutation(self.symbols["e2_restore_hcr_write"] + 4, 0xd503201f,
                             "HCR write needs ISB: e2_restore_hcr_write")

    def test_guest_hcr_not_read_back(self):
        self.reject_mutation(self.symbols["e2_guest_hcr_write"] + 16, 0xd503201f,
                             "guest HCR read back before entering EL1")

    def test_guest_unmasked_entry(self):
        # MOV x10,#0x5 (EL1h with D/A/I/F clear) in place of #0x3c5.
        self.reject_mutation(self.symbols["e2_guest_eret"] - 8, 0xd28000aa,
                             "guest ELR/SPSR")

    def test_guest_identity_check_skipped(self):
        # TBNZ on the PAR fault bit removed: a faulting translation would pass.
        self.reject_mutation(self.symbols["e2_guest_identity_check"] + 12, 0xd503201f,
                             "guest identity check")

    def test_guest_probe_immediate(self):
        self.reject_mutation(self.symbols["e2_guest_probe"], 0xd40dc682, "guest probe HVC")

    def test_guest_vector_slot(self):
        self.reject_mutation(self.symbols["e2_guest_vector_5"], 0xd503201f, "guest vector 5")

    def test_handshake_immediate(self):
        self.reject_mutation(self.symbols["e2_guest_signature"], 0xd40dc682, "guest handshake HVC")

    def test_guest_memory_access(self):
        self.reject_mutation(self.symbols["e2_guest_payload"], 0xf9400000, "guest must remain register-only")

    def test_semihosting_in_module(self):
        self.reject_mutation(self.symbols["e2_expected_brk"], 0xd45e0000,
                             "unexpected state mutation/transition: hlt")

    def test_missing_par_restore(self):
        self.reject_mutation(self.symbols["e2_restore_par_write"], 0xd503201f, "missing restore writes")

    def test_missing_host_capture_guard(self):
        self.reject_mutation(self.symbols["e2_host_capture_once"] + 4, 0xd503201f,
                             "host SPSR capture must be immutable")

    def test_missing_runtime_sp_check(self):
        self.reject_mutation(self.symbols["e2_expected_sp_check"] + 16, 0xd503201f,
                             "BRK runtime SP comparison")

    def test_identity_first_subpage_only(self):
        self.reject_mutation(self.symbols["e2_guest_identity_check"] + 40, 0xd503201f,
                             "cover every 4 KiB subpage")

    def test_cache_maintenance_elsewhere(self):
        # IC IALLU dropped into the BRK-stage seeds (non-executed copy).
        self.reject_mutation(self.symbols["e2_expected_brk"] - 4, 0xd508751f,
                             "cache maintenance outside the guest code clean")

    def test_handler_library_call(self):
        self.reject_mutation(self.symbols["e2_exception_dispatch"], 0x94000000, "dispatcher must remain a leaf")

    def test_missing_runtime_gpr_check(self):
        self.reject_mutation(self.symbols["e2_expected_resume"], 0xd503201f, "missing runtime GPR check 0")

    def test_missing_caller_frame_alias_guard(self):
        self.reject_mutation(self.symbols["e2_validate_caller_frame"] + 44,
                             0xd503201f, "caller-frame guard instruction 11")

    def test_small_exception_output_guard(self):
        self.reject_mutation(self.symbols["e2_validate_caller_frame"] + 56,
                             0xd280100f, "caller-frame guard instruction 14")

    def test_bad_stack_replays_nested_spsr(self):
        # MRS x10,SPSR_EL2 in place of the retained original host SPSR load.
        self.reject_mutation(self.symbols["e2_bad_stack_saved_host"],
                             0xd53c400a, "bad-stack retained host SPSR")

    def test_bad_stack_ignores_retained_host(self):
        self.reject_mutation(self.symbols["e2_bad_stack_select_host"] + 8,
                             0xd503201f, "bad-stack host selection instruction 2")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
