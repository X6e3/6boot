#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Each mutant changes one line of src/xboot_bootstate.c; the tests must notice every one.
#   tests/bootstate/mutate.sh            (from the 6boot tree; never touches the real file)
set -uo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/src" "$T/tests/bootstate" "$T/tools"
cp "$R/src/xboot_bootstate.h" "$T/src/"
cp "$R/tests/bootstate/"{bootstate_test.c,cross_test.py,Makefile} "$T/tests/bootstate/"
cp "$R/tools/bootstate.py" "$T/tools/"
mutants=(
  's/    bs.tries_left--;/    ;/'                                             # 6boot does not count the try
  's/    bs.seq++;/    ;/'                                                     # seq not advanced
  's/    if (!in->sector_ok) {/    if (0) {/'                                  # writes without the raw check
  's/    if (!bs.tries_left) {/    if (0) {/'                                  # boots a trial with no tries
  's/    d->watchdog = 1;/    d->watchdog = 0;/'                              # trial without the watchdog
  's/        case XB_BS_TRIAL_HASH:/        case XB_BS_TRIAL_OK:/'           # trusts a hash mismatch
  's/0xedb88320U/0xedb88321U/'                                               # wrong CRC
  's/    if (get_le32(p + 508) != xb_bs_crc32(p, 508))/    if (0)/'            # CRC not checked
  's/        return !bs->tries_left \&\& /        return /'                    # idle with tries
  's/               bs->tries_left <= bs->tries_set/               bs->tries_left <= bs->tries_set + 1/'
  's/    if (!fields_ok(\&bs) || !all_zero(p + 52, 508 - 52))/    if (!fields_ok(\&bs) || !all_zero(p + 53, 508 - 53))/'
  's/    if (first_cluster < 2 || first_cluster - 2ULL >= clusters)/    if (first_cluster < 2 || first_cluster - 2ULL > clusters)/'
  's/    if (cluster_bytes < block_bytes || offset % block_bytes)/    if (cluster_bytes < block_bytes)/'
  's/    uint64_t root_sectors = ((uint64_t)root_entries \* 32 + bps - 1) \/ bps;/    uint64_t root_sectors = 0;/'
  's/        memcmp(block, old_rec, XB_BS_BYTES))/        0)/'                 # patches a block it did not check
  's/    if (bpb\[510\] != 0x55 || bpb\[511\] != 0xaa)/    if (0)/'               # any sector as a boot sector
)
killed=0
for m in "${mutants[@]}"; do
  sed "$m" "$R/src/xboot_bootstate.c" > "$T/src/xboot_bootstate.c"
  if cmp -s "$R/src/xboot_bootstate.c" "$T/src/xboot_bootstate.c"; then
    echo "MUTANT DID NOT APPLY: $m"; continue
  fi
  rm -rf "$T/build"
  if make -s -C "$T/tests/bootstate" BUILD="$T/build" test cross >/dev/null 2>&1; then
    echo "SURVIVED: $m"
  else
    killed=$((killed + 1))
  fi
done
echo "bootstate mutate: $killed of ${#mutants[@]} mutants killed"
[ "$killed" -eq "${#mutants[@]}" ]
