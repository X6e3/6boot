#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Diskless TCG test. Executes the actual module blob, never a physical device.

QEMU is supplied by the caller. No download/install or network/disk backend.
Semihosting is used only by our harness for console output and process exit.
"""
import argparse
import json
import os
from pathlib import Path
import resource
import shlex
import struct
import subprocess

p=argparse.ArgumentParser()
p.add_argument("--qemu",required=True)
p.add_argument("--qemu-lib-dir",default="",help="optional macOS local bottle libraries")
p.add_argument("--build",type=Path,required=True,help="existing module build directory")
p.add_argument("--cc",default="clang")
p.add_argument("--target",default="--target=aarch64-none-elf")
p.add_argument("--ld",default="aarch64-elf-ld")
p.add_argument("--nm",default="aarch64-elf-nm")
p.add_argument("--debug",action="store_true")
p.add_argument("--native-gate",action="store_true",help="exercise the production native caller gate")
p.add_argument("--fault",choices=["none","setup","hcr","restore","identity","sp","sequence"],default="none")
p.add_argument("--profile",choices=["minimal","inherited-hcr"],default="inherited-hcr")
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
build=a.build.resolve(); test=build/"qemu"/("native" if a.native_gate else "direct")/a.profile/a.fault; test.mkdir(parents=True,exist_ok=True)
syms={}
for line in subprocess.check_output([a.nm,"-n",str(build/"el2-foundation.elf")],text=True).splitlines():
    parts=line.split()
    if len(parts)==3: syms[parts[2]]=int(parts[0],16)
blob=bytearray((build/"el2-foundation.bin").read_bytes())
if a.fault!="none":
    if a.fault=="identity":
        struct.pack_into("<I",blob,syms["e2_guest_identity_check"]+16,0xaa1f03eb)
    elif a.fault=="sequence":
        struct.pack_into("<I",blob,syms["e2_guest_signature"],0xd40dc682)
    elif a.fault=="sp":
        # Change only a temporary blob's 384-byte unwind to 400 bytes.
        region=blob[syms["e2_exception_common"]:syms["e2_exception_bad_stack"]]
        needle=struct.pack("<II",0x910603ff,0xd69f03e0)
        if region.count(needle)!=1: raise SystemExit("unwind injection site is ambiguous")
        pos=syms["e2_exception_common"]+region.index(needle)
        struct.pack_into("<I",blob,pos,0x910643ff)
    else:
        site={"setup":"e2_guest_identity_check","hcr":"e2_guest_hcr_write","restore":"e2_restore_hcr_write"}[a.fault]
        struct.pack_into("<I",blob,syms[site],0)  # UDF; only a temporary emulator copy.
binary=test/"module.bin"; binary.write_bytes(blob)
image=test/"image.S"
image.write_text('.section .module_image,"ax"\n.incbin '+json.dumps(str(binary))+'\n.section .note.GNU-stack,"",%progbits\n')
flags=[a.cc,*shlex.split(a.target),"-march=armv8.1-a","-mgeneral-regs-only","-ffreestanding",
       "-fno-builtin","-fno-stack-protector","-fno-pic","-O2","-Wall","-Wextra","-Werror","-I"+str(root/"include")]
defs=["-DE2_TEST_BRK_ENTRY="+str(syms["sixboot_el2_exception_entry"]),
      "-DE2_TEST_HCR="+str(0x488000000 if a.profile=="minimal" else 0x32488000038),
      "-DE2_TEST_RESULT_VERSION="+str(syms.get("e2_result_version",1)),
      "-DE2_TEST_GUEST_ENTRY="+str(syms["sixboot_el2_guest_entry"]),
      "-DE2_TEST_BRK_EXPECTED="+str(19 if a.fault=="sp" else 13),
      "-DE2_TEST_GUEST_EXPECTED="+str(22 if a.fault=="none" else 19 if a.fault in {"restore","sp"} else 27 if a.fault=="identity" else 26),
      "-DE2_TEST_PARTIAL="+str(int(a.fault=="restore")),
      "-DE2_TEST_HAS_SIGNATURE="+str(int("e2_guest_signature" in syms))]
for name,symbol in {"BRK_PC":"e2_expected_brk","PROBE_PC":"e2_guest_after_probe",
                    "SIGNATURE_PC":"e2_guest_after_signature","DONE_PC":"e2_guest_after_done",
                    "RETURN_PC":"e2_guest_return","RESTORE_PC":"e2_restore_hcr_write"}.items():
    defs.append("-DE2_TEST_"+name+"="+str(syms.get(symbol,0)))
objects=[]
sources=[root/"tests/qemu/start.S",root/"tests/qemu/harness.c",image]
if a.native_gate:
    defs.append("-DE2_TEST_NATIVE=1")
    flags += ["-I"+str(root/"native"),"-I"+str(root.parent.parent/"src")]
    sources += [root/"native/gate.S",root/"native/policy.c",root/"native/mapping.c"]
for src in sources:
    obj=test/(src.stem+".o")
    subprocess.run(flags+defs+["-c",str(src),"-o",str(obj)],check=True)
    objects.append(str(obj))
elf=test/"harness.elf"
subprocess.run([a.ld,"--no-undefined","-T",str(root/"tests/qemu/harness.ld"),*objects,"-o",str(elf)],check=True)
cmd=[a.qemu,"-machine","virt,virtualization=on,secure=off,gic-version=3","-cpu","max","-accel","tcg",
     "-smp","1","-m","128M","-nodefaults","-display","none","-serial","none","-monitor","none",
     "-net","none","-semihosting-config","enable=on,target=native","-device",f"loader,file={elf},cpu-num=0"]
if a.debug: cmd += ["-d","int","-D",str(test/"exceptions.log")]
(test/"command.json").write_text(json.dumps(cmd,indent=2)+"\n")
def limit_output():
    resource.setrlimit(resource.RLIMIT_FSIZE,(2*1024*1024,2*1024*1024))
env=os.environ.copy()
if a.qemu_lib_dir: env["DYLD_LIBRARY_PATH"]=str(Path(a.qemu_lib_dir).resolve())
with (test/"serial.log").open("wb") as log:
    try:
        run=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,timeout=10,preexec_fn=limit_output,env=env)
    except subprocess.TimeoutExpired:
        raise SystemExit("QEMU timed out (not a pass); see "+str(test/"serial.log"))
text=(test/"serial.log").read_text(errors="replace")
evidence=[]; case=""; native=""
for line in text.splitlines():
    if line.startswith("CASE "): case=line.split()[1]
    if line.startswith("WIRE_NATIVE="): native=line.split("=",1)[1]
    if line.startswith("WIRE_RESULT="):
        evidence.append({"mode":case,"native":native,"result":line.split("=",1)[1]})
if evidence: (test/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n")
visible="\n".join(line for line in text.splitlines() if not line.startswith("WIRE_"))+"\n"
print(visible if len(visible)<65536 else visible[-32768:],end="")
if run.returncode or "QEMU_TCG_PASS" not in text or "CASE_FAIL" in text:
    raise SystemExit("QEMU failed, exit="+str(run.returncode))
