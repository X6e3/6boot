#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Audit a standalone build without executing it or accessing a device.

ELF checks are structural; instruction restrictions apply to the standalone
EL2/BRK increment. Neither proves hardware exception handling or register restore.
"""
import json
from pathlib import Path
import re
import struct
import sys


def require(condition, message):
    if not condition:
        raise SystemExit("ELF audit failed: " + message)


def dis_operands(op, args):
    # GNU/LLVM print load/store offsets in decimal/hex respectively.
    if op.startswith(("ld", "st")):
        args = re.sub(r"#(-?0x[0-9a-f]+)", lambda m: "#"+str(int(m[1],16)), args)
    if op in {"tbz", "tbnz"}:
        args = re.sub(r"#0x([0-9a-f]+)", lambda m: "#"+str(int(m[1],16)), args)
    if op.startswith("b") or op in {"cbz", "cbnz", "tbz", "tbnz"}:
        args = re.sub(r"(?<![#\w])0x([0-9a-f]+)(?=\s|$)", r"\1", args)
    return args


def audit(elf_path, binary_path, disassembly_path, stack_path):
    data = elf_path.read_bytes()

    def span(offset, size):
        require(0 <= offset <= len(data) and 0 <= size <= len(data) - offset,
                "truncated file span")
        return data[offset:offset + size]

    header = struct.unpack("<16sHHIQQQIHHHHHH", span(0, 64))
    ident, kind, machine, version, entry, phoff, shoff, _, ehsize, phsize, phnum, shsize, shnum, shstr = header
    require(ident[:7] == b"\x7fELF\x02\x01\x01", "not ELF64 little-endian")
    require((kind, machine, version, entry, ehsize, phsize, shsize) ==
            (2, 183, 1, 0, 64, 56, 64), "unexpected architecture/entry/header")
    require(0 < phnum < 16 and 0 < shnum < 128 and shstr < shnum, "table bounds")
    loads = []
    for i in range(phnum):
        p = struct.unpack("<IIQQQQQQ", span(phoff + i * phsize, phsize))
        ptype, flags, offset, va, pa, filesz, memsz, align = p
        if ptype == 1:
            require(pa == va and filesz <= memsz, "invalid load segment")
            require(align >= 0x4000 and align & (align - 1) == 0, "segment alignment")
            require(va % align == offset % align, "unaligned file/virtual segment")
            span(offset, filesz)
            loads.append(p)
        else:
            require(ptype not in (2, 3), "dynamic or interpreter segment")
    require(len(loads) == 2, "expected separate code/private segments")
    code, private = sorted(loads, key=lambda p: p[3])
    require(code[1] == 5 and code[3] == 0 and 0 < code[6] <= 0x4000,
            "code must be RX within the first page")
    require(private[1] == 6 and private[3] == 0x4000 and private[5] == 0 and
            private[6] == 0x1c000, "private RAM reservations must be RW/NOBITS")

    sections = [struct.unpack("<IIQQQQIIQQ", span(shoff + i * shsize, shsize))
                for i in range(shnum)]
    names = span(sections[shstr][4], sections[shstr][5])

    def string(table, offset):
        require(0 <= offset < len(table), "string offset")
        end = table.find(b"\0", offset)
        require(end >= 0, "unterminated string")
        return table[offset:end].decode("ascii")

    named = {}
    symbols = {}
    symbol_sizes = {}
    for section in sections:
        name, stype, flags, address, offset, size, link, _, _, entsize = section
        require(stype not in (4, 6, 9, 11), "relocations/dynamic state in standalone image")
        if stype != 8:
            span(offset, size)
        section_name = string(names, name)
        named[section_name] = section
        if flags & 2:  # SHF_ALLOC
            require(address + size <= 0x20000, "allocated section outside arena")
            require(not (flags & 1 and flags & 4), "writable executable section")
        if stype == 2:
            require(entsize == 24 and size % 24 == 0 and link < shnum, "symbol table")
            strings = span(sections[link][4], sections[link][5])
            for at in range(offset, offset + size, 24):
                sname, _, _, index, value, symbol_size = struct.unpack("<IBBHQQ", span(at, 24))
                label = string(strings, sname)
                require(not label or index != 0, "undefined symbol: " + label)
                if label:
                    symbols[label] = value
                    symbol_sizes[label] = symbol_size
    expected = {
        "sixboot_el2_entry": 0, "__e2_image_start": 0,
        "e2_entry_snapshot": 0x4000,
        "__e2_stack_start": 0x8000, "__e2_stack_end": 0xc000,
        "__e2_guest": 0xc000, "__e2_guest_stack": 0x10000,
        "__e2_trace": 0x14000, "__e2_image_end": 0x20000,
    }
    for name, value in expected.items():
        require(symbols.get(name) == value, "layout symbol " + name)
    require(symbols.get("e2_result_version") == 3, "result ABI version")
    require(0 < symbols.get("sixboot_el2_exception_entry", 0) < 0x4000,
            "separate exception entry")
    require(symbols.get("e2_exception_session") == 0x4060 and
            symbols.get("e2_exception_trace") == 0x14000, "exception state placement")
    vectors = named.get(".vectors")
    require(vectors is not None and vectors[3] % 0x800 == 0 and vectors[5] == 0x800,
            "vector table size/alignment")
    require(symbols.get("e2_vectors") == vectors[3] and
            symbols.get("e2_vectors_end") == vectors[3] + 0x800, "vector bounds")
    for i in range(16):
        require(symbols.get("e2_vector_" + str(i)) == vectors[3] + 128 * i,
                "vector offset " + str(i))
    require(symbols.get("e2_expected_resume") == symbols.get("e2_expected_brk", -8) + 4,
            "BRK resume address")
    for name, address, size in [(".state", 0x4000, 0x4000),
                                (".stack", 0x8000, 0x4000),
                                (".guest", 0xc000, 0x4000),
                                (".guest_stack", 0x10000, 0x4000),
                                (".trace", 0x14000, 0x4000),
                                (".reserved", 0x18000, 0x8000)]:
        s = named.get(name)
        require(s is not None and s[1] == 8 and (s[3], s[5]) == (address, size),
                "reservation " + name)

    blob = binary_path.read_bytes()
    require(0 < len(blob) <= 0x4000, "raw blob page budget")
    for name in (".text", ".rodata", ".vectors"):
        s = named.get(name)
        if s and s[5]:
            require(blob[s[3]:s[3] + s[5]] == span(s[4], s[5]), "blob differs from " + name)

    instructions = []
    for line in disassembly_path.read_text().splitlines():
        match = re.match(r"\s*[0-9a-f]+:\s+[0-9a-f]{8}\s+([a-z][a-z0-9_.]*)\s*(.*)", line)
        if match:
            address = int(line.split(":", 1)[0], 16)
            op={"b.lo":"b.cc", "b.hs":"b.cs"}.get(match[1].lower(),match[1].lower())
            instructions.append((address, op, dis_operands(op, match[2].lower())))
    require(len(instructions) > 30, "empty/unrecognized disassembly")
    # HVC, DC and IC exist only in the reviewed guest stage; checked by place below.
    forbidden = {"eretaa", "eretab", "smc", "svc",
                 "sys", "sysl", "tlbi", "wfi", "wfe", "sev", "sevl", "hlt"}
    writes = set()
    guest_lo, guest_hi = symbols["__e2_guest_text_start"], symbols["__e2_guest_text_end"]
    enter_lo, enter_hi = symbols["e2_guest_enter"], symbols["e2_guest_hcr_write"]
    for address, mnemonic, operands in instructions:
        require(mnemonic not in forbidden, "unexpected state mutation/transition: " + mnemonic)
        if guest_lo <= address < guest_hi:
            require(mnemonic in {"mov", "cmp", "b", "b.ne", "hvc", "nop", "msr"},
                    "unreviewed guest instruction: " + mnemonic)
        if mnemonic == "hvc":
            require(guest_lo <= address < guest_hi, "HVC outside the EL1 guest")
        if mnemonic == "at":
            require(address == symbols["e2_guest_identity_check"] and operands == "s1e2r, x10",
                    "address translation outside the guest identity check")
        if mnemonic in {"dc", "ic"}:
            require(enter_lo <= address < enter_hi and
                    (mnemonic, operands) in {("dc", "cvac, x12"), ("ic", "iallu")},
                    "cache maintenance outside the guest code clean")
        require(not (mnemonic == "mrs" and re.search(r"\bsp_el2\b", operands)),
                "SP_EL2 must not be MRS-read from EL2")
        # A branch address such as d44 is not a floating-point register.
        require(not re.search(r"(?<![0-9a-z_])[vsdq](?:3[01]|[12]?[0-9])(?=[,.\]}]|$)",
                              operands.split("//", 1)[0]), "FP/SIMD register use")
        if mnemonic == "msr":
            target = operands.split(",")[0].strip()
            require(target in {"vbar_el2", "daif", "daifset", "elr_el2", "spsr_el2", "esr_el2", "far_el2",
                               "hcr_el2", "sctlr_el12", "vbar_el12", "elr_el12", "spsr_el12", "sp_el1", "par_el1", "esr_el12", "far_el12", "esr_el1", "far_el1"},
                    "unreviewed system-register write: " + target)
            if target in {"esr_el1", "far_el1"}:
                expected = symbols["e2_guest_payload"] + (0 if target == "esr_el1" else 4)
                require(address == expected and operands == target + ", xzr", "EL1 fault-bank test outside guest prefix")
            if target in {"esr_el12", "far_el12"}:
                require(symbols["e2_exception_restore"] <= address < symbols["e2_exception_run"] + symbol_sizes["e2_exception_run"], "EL12 fault-bank write outside restore")
            if target == "hcr_el2":
                require(address in {symbols["e2_guest_hcr_write"], symbols["e2_restore_hcr_write"]},
                        "HCR_EL2 write outside the reviewed sites")
            if target == "par_el1":
                require(address == symbols["e2_restore_par_write"], "PAR write outside restore")
            writes.add(target)
    reads = {operands.split(",")[-1].strip() for _, mnemonic, operands in instructions if mnemonic == "mrs"}
    require(reads == {"currentel", "daif", "spsel", "sp_el0", "sp_el1", "vbar_el2", "hcr_el2",
                      "mpidr_el1", "elr_el2", "spsr_el2", "esr_el2", "far_el2",
                      "sctlr_el12", "vbar_el12", "elr_el12", "spsr_el12", "ctr_el0", "par_el1", "esr_el12", "far_el12",
                      "id_aa64mmfr0_el1", "id_aa64mmfr1_el1", "id_aa64mmfr2_el1", "id_aa64pfr0_el1"},
            "unexpected snapshot register set: " + repr(reads))
    require(writes == {"vbar_el2", "daif", "daifset", "elr_el2", "spsr_el2", "esr_el2", "far_el2",
                       "hcr_el2", "sctlr_el12", "vbar_el12", "elr_el12", "spsr_el12", "sp_el1", "par_el1", "esr_el12", "far_el12", "esr_el1", "far_el1"},
            "missing restore writes")

    def body(name):
        lo = symbols[name]
        hi = lo + symbol_sizes[name]
        return [(a, op, args) for a, op, args in instructions if lo <= a < hi]

    handler = body("e2_exception_common")
    calls = [(op, args) for _, op, args in handler if op in {"bl", "blr"}]
    require(len(calls) == 1 and calls[0][0] == "bl" and
            "<e2_exception_dispatch>" in calls[0][1], "handler must call only the bounded dispatcher")
    require(not any(op in {"bl", "blr"} for _, op, _ in body("e2_exception_dispatch")),
            "dispatcher must remain a leaf (no allocation/logging/library calls)")
    for reg in range(2, 30, 2):
        pattern = rf"x{reg},\s*x{reg + 1},\s*\[sp,\s*#{reg * 8}\]"
        require(any(op == "stp" and re.fullmatch(pattern, args) for _, op, args in handler),
                "missing GPR save pair " + str(reg))
        require(any(op == "ldp" and re.fullmatch(pattern, args) for _, op, args in handler),
                "missing GPR restore pair " + str(reg))
    require(any(op == "str" and re.fullmatch(r"x30,\s*\[sp,\s*#240\]", args)
                for _, op, args in handler), "missing LR save")
    require(any(op == "ldr" and re.fullmatch(r"x30,\s*\[sp,\s*#240\]", args)
                for _, op, args in handler), "missing LR restore")
    require(any(op == "ldp" and re.fullmatch(r"x0,\s*x1,\s*\[sp\]", args)
                for _, op, args in handler), "missing x0/x1 restore")
    require(handler[-2][1:] == ("add", "sp, sp, #0x180") and handler[-1][1] == "eret",
            "handler must unwind exactly its 384-byte frame before ERET")
    by_address = {a: (op, args) for a, op, args in instructions}
    # Guard the entire caller-frame check before any stack store. These are
    # structural checks, not a substitute for executing the entry on ARM.
    guard = symbols["e2_validate_caller_frame"]
    save = symbols["e2_save_caller_frame"]
    arg = symbols["e2_entry_argument_refusal"]
    env = symbols["e2_entry_environment_refusal"]
    expected_guard = [
        ("mov", "x12, sp"), ("tst", "x12, #0xf"), ("b.ne", f"{env:x}"),
        ("cmp", "x12, #0x70"), ("b.cc", f"{env:x}"), ("sub", "x13, x12, #0x70"),
        ("adds", "x14, x0, x1"), ("b.cs", f"{arg:x}"), ("cmp", "x0, x12"),
        ("b.cs", f"{guard + 48:x}"), ("cmp", "x14, x13"), ("b.hi", f"{arg:x}"),
        ("mov", "x15, #0xa0"), ("cbz", f"w17, {guard + 60:x}"), ("mov", "x15, #0x800"),
        ("adds", "x14, x2, x15"), ("b.cs", f"{arg:x}"), ("cmp", "x2, x12"),
        ("b.cs", f"{save:x}"), ("cmp", "x14, x13"), ("b.hi", f"{arg:x}"),
    ]
    require(save == guard + 4 * len(expected_guard), "caller-frame guard extent")
    for i, (wanted_op, wanted_args) in enumerate(expected_guard):
        op, args = by_address.get(guard + 4 * i, (None, ""))
        args = args.split("//", 1)[0].split("<", 1)[0].strip()
        require((op, args) == (wanted_op, wanted_args), "caller-frame guard instruction " + str(i))
    require(by_address.get(save) == ("sub", "sp, sp, #0x70"), "caller save-frame extent")
    require(not any(op in {"stp", "str", "stur", "bl", "blr"}
                    for a, op, _ in instructions if a < save), "write/call before caller-frame checks")
    # Nested stack failure must not replay the handler's own SPSR as host.
    bad = body("e2_exception_bad_stack")
    retained = symbols["e2_bad_stack_saved_host"]
    require(by_address.get(retained) == ("ldr", "x10, [x9, #72]"), "bad-stack retained host SPSR")
    select_host = symbols["e2_bad_stack_select_host"]
    direct_abort = symbols["e2_exception_direct_abort"]
    selection = [
        ("ldr", "x10, [x9, #136]"), ("cmp", "x10, #0x1"), ("b.eq", f"{retained:x}"),
        ("cbnz", f"x10, {direct_abort:x}"), ("ldr", "x10, [x9, #120]"),
        ("cbnz", f"x10, {direct_abort:x}"), ("mrs", "x10, spsr_el2"),
        ("b", f"{retained + 4:x}"),
    ]
    require(retained == select_host + 4 * len(selection), "bad-stack host selection extent")
    for i, wanted in enumerate(selection):
        op, args = by_address.get(select_host + 4 * i, (None, ""))
        args = args.split("//", 1)[0].split("<", 1)[0].strip()
        require((op, args) == wanted, "bad-stack host selection instruction " + str(i))
    for i in range(16):
        start = symbols["e2_vector_" + str(i)]
        require(by_address.get(start) == ("stp", "x0, x1, [sp, #-16]!"), "vector landing save")
        slot = [(a, op, args) for a, op, args in instructions if start <= a < start + 128]
        require(any(op == "stp" and args == "x0, x1, [sp]" for _, op, args in slot), "vector x0/x1 frame save")
        require(any(op == "sub" and args == "sp, sp, #0x170" for _, op, args in slot), "vector frame allocation")
        tags = [int(args.split("#")[1].split()[0], 0) for _, op, args in slot
                if op == "mov" and args.startswith("w0, #")]
        require(tags == [i, i], "vector source tags")
        require(any(op == "b" and "<e2_exception_common>" in args for _, op, args in slot), "vector handler target")
        require(any(op == "b" and "<e2_exception_bad_stack>" in args for _, op, args in slot), "vector stack failure target")
    brks = [(a, args) for a, op, args in instructions if op == "brk"]
    require(brks == [(symbols["e2_expected_brk"], "#0x6e32")], "unexpected test BRK")
    for reg in range(31):
        seed_at = symbols["e2_expected_brk"] - (31 - reg) * 4
        op, args = by_address.get(seed_at, (None, ""))
        require(op == "mov" and args.split("//")[0].strip() == f"x{reg}, #0x{0x100 + reg:x}",
                "missing runtime GPR seed " + str(reg))
        check_at = symbols["e2_expected_resume"] + reg * 8
        require(by_address.get(check_at) == ("cmp", f"x{reg}, #0x{0x100 + reg:x}") and
                by_address.get(check_at + 4, (None,))[0] == "b.ne",
                "missing runtime GPR check " + str(reg))
    require(sum(op == "eret" for _, op, _ in instructions) == 3, "exception return paths")
    # Guest stage: the third ERET enters EL1 only after HCR was written,
    # synchronized and read back, with ELR/SPSR set immediately before it.
    guest_eret = symbols["e2_guest_eret"]
    def at(address):
        op, args = by_address.get(address, (None, ""))
        return op, args.split("//", 1)[0].strip()
    capture = symbols["e2_host_capture_once"]
    require(at(capture) == ("ldr", "x11, [x1, #136]") and at(capture + 4)[0] == "cbnz" and
            "<e2_host_capture_done>" in at(capture + 4)[1], "host SPSR capture must be immutable")
    check_sp = symbols["e2_expected_sp_check"]
    require(at(check_sp + 8) == ("ldr", "x10, [x9, #288]") and
            at(check_sp + 12) == ("mov", "x11, sp") and at(check_sp + 16) == ("cmp", "x10, x11") and
            at(check_sp + 20)[0] == "b.ne", "BRK runtime SP comparison")
    require(at(guest_eret) == ("eret", ""), "guest entry ERET")
    payload_adr=re.match(r"x10, (?:0x)?([0-9a-f]+)\b",at(guest_eret - 16)[1])
    require(at(guest_eret - 16)[0] == "adr" and payload_adr is not None and
            int(payload_adr[1],16) == symbols["e2_guest_payload"] and
            at(guest_eret - 12) == ("msr", "elr_el2, x10") and
            at(guest_eret - 8) == ("mov", "x10, #0x3c5") and
            at(guest_eret - 4) == ("msr", "spsr_el2, x10"),
            "guest ELR/SPSR (payload, EL1h masked) set right before the ERET")
    for site in ("e2_guest_hcr_write", "e2_restore_hcr_write"):
        require(at(symbols[site] + 4)[0] == "isb", "HCR write needs ISB: " + site)
    ident = symbols["e2_guest_identity_check"]
    require(at(ident) == ("at", "s1e2r, x10") and at(ident + 4)[0] == "isb" and
            at(ident + 8) == ("mrs", "x11, par_el1") and at(ident + 12)[0] == "tbnz" and
            at(ident + 12)[1].startswith("w11, #0,") and at(ident + 24) == ("cmp", "x12, x11") and
            at(ident + 28)[0] == "b.ne" and ident < symbols["e2_guest_hcr_write"],
            "guest identity check")
    require(at(ident + 32) == ("add", "x10, x10, #0x1, lsl #12") and
            at(ident + 36) == ("cmp", "x10, x15") and at(ident + 40)[0] == "b.cc",
            "guest identity check must cover every 4 KiB subpage")
    par = symbols["e2_restore_par_write"]
    require(at(par - 4) == ("ldr", "x10, [x9, #320]") and at(par) == ("msr", "par_el1, x10") and
            at(par + 4)[0] == "isb" and at(par + 8) == ("mrs", "x11, par_el1") and
            at(par + 12) == ("cmp", "x10, x11"), "PAR restore/readback")
    hcr_set = symbols["e2_guest_hcr_write"]
    require(at(hcr_set + 16) == ("mrs", "x11, hcr_el2") and at(hcr_set + 20) == ("cmp", "x10, x11") and
            at(hcr_set + 24)[0] == "b.ne",
            "guest HCR read back before entering EL1")
    guest_entry_writes = [args.split(",")[0] for a, op, args in instructions
                          if enter_lo <= a < guest_eret and op == "msr"]
    require(guest_entry_writes == ["sctlr_el12", "vbar_el12", "sp_el1", "hcr_el2", "elr_el2", "spsr_el2"],
            "guest entry write order: " + repr(guest_entry_writes))
    require(symbols["e2_guest_after_probe"] == symbols["e2_guest_probe"] + 4 and
            by_address.get(symbols["e2_guest_probe"]) == ("hvc", f"#0x{0x6e33:x}"),
            "guest probe HVC")
    require(symbols["e2_guest_after_done"] == symbols["e2_guest_done"] + 4 and
            by_address.get(symbols["e2_guest_done"]) == ("hvc", f"#0x{0x6e34:x}"),
            "guest DONE HVC")
    require(symbols["e2_guest_after_signature"] == symbols["e2_guest_signature"] + 4 and
            by_address.get(symbols["e2_guest_signature"]) == ("hvc", "#0x6e37"), "guest handshake HVC")
    require(symbols["e2_guest_vectors"] == guest_lo and guest_lo % 0x800 == 0, "guest vector table")
    for i in range(16):
        start = symbols["e2_guest_vector_" + str(i)]
        require(start == guest_lo + 0x80 * i and
                by_address.get(start) == ("hvc", f"#0x{0x6e40 + i:x}"), "guest vector " + str(i))
    restore_hcr = symbols["e2_restore_hcr_write"]
    vbar_restores = [a for a, op, args in instructions
                     if op == "msr" and args == "vbar_el2, x10" and a > symbols["e2_exception_restore"]]
    require(symbols["e2_exception_restore"] < restore_hcr < min(vbar_restores),
            "HCR restored before the inherited vectors")
    for a, op, args in instructions:
        if op == "msr" and args.startswith("vbar_el2,"):
            require(by_address.get(a + 4, (None,))[0] == "isb", "VBAR write needs ISB")
    restore = [(a, op, args) for a, op, args in body("e2_exception_run")
               if a >= symbols["e2_exception_restore"]]
    for reg, offset in {"vbar_el2": 0, "elr_el2": 16, "spsr_el2": 24,
                        "esr_el2": 32, "far_el2": 40, "esr_el12": 336, "far_el12": 344, "daif": 8}.items():
        sites = [i for i, (_, op, args) in enumerate(restore)
                 if op == "msr" and args == reg + ", x10"]
        require(len(sites) == 1 and sites[0] > 0, "missing exact restore for " + reg)
        previous = restore[sites[0] - 1]
        wanted = "x10, [x9]" if offset == 0 else f"x10, [x9, #{offset}]"
        require(previous[1:] == ("ldr", wanted), "restore source offset for " + reg)
    system_writes = [(a, args.split(",")[0]) for a, op, args in restore
                     if op == "msr" and not args.startswith("daifset,")]
    require(system_writes[-1][1] == "daif", "interrupt mask must restore last")
    match = re.search(r"\be2_exception_dispatch\t(\d+)\tstatic", stack_path.read_text())
    require(match is not None and int(match[1]) <= 1024, "handler C stack budget")
    return {"status": "PASS", "machine": "AArch64", "blob_bytes": len(blob),
            "arena_bytes": 0x20000, "load_segments": len(loads),
            "vector_entries": 16, "frame_bytes": 384, "dispatcher_stack_bytes": int(match[1]),
            "system_register_write_targets": sorted(writes), "guest_transitions": 1,
            "scope": "structural/instruction audit only; ARM image not executed"}


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit("usage: audit_elf.py IMAGE.elf IMAGE.bin IMAGE.dis EXCEPTION.su")
    print(json.dumps(audit(*(Path(p) for p in sys.argv[1:])), sort_keys=True))
