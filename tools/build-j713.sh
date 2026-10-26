#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build 6boot's stage 2 for J713 from this tree, on Linux aarch64 with gcc, binutils, rustc and cargo
# (the build VM). Writes <out>/m1n1-6boot-user.bin, the die alone for normal use, and
# <out>/m1n1-6boot-dev.bin, with the console and the boot trace. The tree itself is not changed.
#
#   tools/build-j713.sh [out]        default: ./out
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "${1:-$R/out}"
O=$(cd "${1:-$R/out}" && pwd)
TAG=${TAG:-6boot-p32}
{ clang --version | head -1; ld.lld --version; gcc --version | head -1; ld --version | head -1
  rustc --version; cargo --version; } > "$O/toolchain.txt"   # m1n1 builds with clang; the entry probe with gcc

# The entry probe: host tests, then the bare-metal blob that stage 2 embeds.
make -C "$R/entry-probe" BUILD="$O/entry-probe" all
objcopy -O binary --only-section=.text --only-section=.rodata "$O/entry-probe/xboot-entry-probe.elf" \
    "$O/entry-probe.bin"

variant() {   # variant <name> [make CFG]
  local d=$O/$1 args=(-j8)
  [ -n "${2:-}" ] && args+=(CFG="$2")
  rm -rf "$d"
  rsync -a --exclude .git --exclude /out --exclude /build "$R/" "$d/"
  mkdir -p "$d/build"
  python3 -I "$R/tools/embed_entry_probe.py" "$O/entry-probe.bin" "$d/build/entry_probe_image.h"
  cp "$R/data/die_blocks.h" "$d/build/"
  (cd "$d" && M1N1_VERSION_TAG="$TAG-$1-j713" make "${args[@]}")
  cp "$d/build/m1n1.bin" "$O/m1n1-6boot-$1.bin"
}
variant dev
variant user "FB_SILENT_MODE SIXBOOT_USER"
grep -q '^#define SIXBOOT_USER' "$O/user/build/build_cfg.h"
(cd "$O" && sha256sum entry-probe.bin m1n1-6boot-dev.bin m1n1-6boot-user.bin)
