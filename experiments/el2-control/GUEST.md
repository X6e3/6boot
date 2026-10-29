# EL1 guest / HVC stage — current contract

Explicit entry: `sixboot_el2_guest_entry(request, size, result, capacity)`.
The default boot path never calls it. It runs the BRK stage first; the guest
starts only after all 31 interrupted GPRs and SP passed their post-ERET checks.
No Stage-2 is enabled. This is a register-only guest, not memory isolation.

## Sequence

The guest entry marks preparation as its own stage while our EL2 vectors are
installed. Under the required VHE host state it saves PAR_EL1, then checks
VA == PA with AT S1E2R for every 4 KiB subpage covering .guest_text, including
both vectors and payload. This conservative stride also covers a 16 KiB host
granule. A failed AT/mismatching result refuses entry. PAR is restored on both
success and failure. This is code translation checking, not ownership proof.

SCTLR/VBAR/ELR/SPSR_EL12 are saved, then code is cleaned to PoC using CTR_EL0,
DSB SY, IC IALLU, DSB SY, ISB. SCTLR_EL12 is set to 0x30d00800, VBAR_EL12 to
the guest vectors and SP_EL1 to the separate guest stack top. HCR is changed
last: TGE/HCD/AMO/IMO/FMO/VM cleared, RW/E2H set. ISB and readback precede ERET
with SPSR_EL2=0x3c5 (masked EL1h) and ELR_EL2 at the payload.

HCR is marked written only after the MSR and ISB retired. A fault before that
marker does not retry the guest HCR write during cleanup. Guest preparation,
lower-EL execution and restore faults are recorded as guest-stage failures.
The original valid host SPSR is captured once and never replaced by a later
EL2 fault's PSTATE.

The payload seeds x0..x30=0x200+n and follows this order:

```asm
hvc #0x6e33
hvc #0x6e37
hvc #0x6e34
```

EL2 checks source vector 8, HVC64 EC/IL/immediate, exact following-instruction
ELR, masked EL1h, HCR/VBAR, register values and history. It replies in x0;
the payload checks the replies and preserved registers before DONE. HVC ELR
already points to the next instruction: the handler never adds four for HVC.
DONE returns to the EL2 cleanup target using the retained host PSTATE.
An unexpected event goes to the host abort path, never back to the guest.
EL1 vector entries issue HVCs which are rejected as unexpected. Their fallback
loops do not establish recovery if HVC delivery itself is unavailable.

## Restore and failure limits

While our vectors remain installed: restore HCR if written, then the saved
EL12 bank/SP_EL1 and PAR, all with readback. Old EL2 VBAR and explicit syndrome/
return state follow; DAIF is last. RESTORING makes re-entry skip guest/PAR
writes instead of retrying a trapping instruction. That path is a partial
restore failure, not a safe environment for kernel handoff. It may leave HCR,
SP_EL1, EL12 registers or PAR different. Never continue to Linux on such a result.
CONTEXT_LOST also forbids kernel handoff; the native adapter enforces this
(NATIVE.md). No universal recovery guarantee covers an inaccessible
first stack landing, private state failure or a trap to a higher/Apple-specific
execution environment. QEMU has no Apple GXF model.

## Result ABI

Request and preflight result remain v1. E2EX result is now **v2**, 2048 LE bytes;
the new trace/overflow semantics must not be read as the old reserved-zero ABI.
The initial 184 bytes retain their existing fields. Word at offset 8 describes
the EL2 session contract; frame 0 retains the raw BRK result. A restore failure
may make the session status 19 even when frame 0 says BRK passed.

| Byte offset | u64 field |
| --- | --- |
| 184 | guest_requested |
| 192 | guest status |
| 200 | guest verdict |
| 208 | HVC count |
| 216 | guest flags |
| 224 | guest HCR |
| 232 | pre-BRK SP |
| 240 | saved SCTLR_EL12 |
| 248 | signature_seen |

Flags: INSTALLED=1, RESTORED=2, EXPECTED=4, GUEST_DONE=8, SIGNATURE=16.
DONE requires the exact four-frame history, three accepted HVCs, verdict zero,
matching flags and complete restore. Normal guest return is 22, flags 0x1f,
frame count 4, restore mask 0x3ff. Guest status is separate from the raw BRK frame.

Four normal frames occupy 256..1791. On overflow the frames are preserved,
dropped saturates, and the last omitted syndrome is kept in a fixed summary.
At 1792: u32 E2OF magic 0x45324f46; u16 version 1 / u16 size 64; six u64 fields
at +8..+48: ESR, ELR, SPSR, FAR, vector, validity flags. Remaining bytes zero.
The summary is absent when dropped=0. A raw FAR is meaningful only with FAR_VALID.
It is not a complete fifth GPR frame.

Guest statuses: 21 probe observed; 22 DONE; 23 mismatch; 24 HCR refused;
25 not run; 26 unexpected; 27 nonidentity; 28 intermediate handshake.
Common failures 14..20 are retained, including TRACE_FULL and CONTEXT_LOST.
The native session is 336 bytes. Leaf dispatcher currently uses 32 stack bytes
inside its audited 1024-byte budget; zero stack use is no longer claimed.

## Evidence

34 ASan/UBSan host scenarios (15 foundation, 19 exception), ELF audit and 28
structural/mutation tests pass. QEMU 11.1.2 TCG executes the actual blob:
seven normal/fault modes under the inherited-HCR profile, plus normal execution
under the minimal HCR profile. The harness verifies caller RET/SP/x18..x29,
16 system/PSTATE values including PAN/PAR, actual frame syndromes/PCs/GPRs and
strict failure flags. Deliberate faults modify only temporary emulator blobs.
They do not emulate Apple GXF denial or prove arbitrary-fault recovery.

On 29 October 2026 the guest entry ran on J713 through the native adapter and
returned 22: the four-frame history above, verdict zero, three HVCs and
restore mask 0x3ff. That run had GXF off and Stage-2 translation off. See
J713.md.
