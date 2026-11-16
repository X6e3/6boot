#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Each mutant changes one line of src/xboot_esp.c, src/xboot_trial.c or src/xboot_sha256.c; the unit
# tests or the disk fixtures (esp_fixture_test.py, built once) must notice every one. macOS only.
#   tests/bootstate/mutate_esp.sh            (from the 6boot tree; never touches the real files)
set -uo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/src" "$T/tests/bootstate" "$T/tools" "$T/fixture"
cp "$R"/src/xboot_{bootstate,esp,sha256,trial}.[ch] "$T/src/"
cp "$R"/tests/bootstate/{esp_test.c,esp_fixture_test.py,Makefile} "$T/tests/bootstate/"
cp "$R/tools/bootstate.py" "$T/tools/"
make -s -C "$T/tests/bootstate" BUILD="$T/build" "$T/build/esp-test" >/dev/null || exit 1
python3 -I "$T/tests/bootstate/esp_fixture_test.py" "$T/build/esp-test" --build "$T/fixture" || exit 1
python3 -I "$T/tests/bootstate/esp_fixture_test.py" "$T/build/esp-test" --check "$T/fixture" >/dev/null ||
  { echo "the unmutated code fails the fixtures"; exit 1; }
mutants=(
  'xboot_sha256.c ::: 0x428a2f98, 0x71374491 ::: 0x428a2f99, 0x71374491'
  'xboot_sha256.c ::: (ror(e, 6) ^ ror(e, 11) ::: (ror(e, 7) ^ ror(e, 11)'
  'xboot_sha256.c ::: uint64_t bits = s->bytes \* 8; ::: uint64_t bits = s->bytes;'
  'xboot_esp.c ::: {3, 2, 1, 0, 5, 4, 7, 6, ::: {2, 3, 1, 0, 5, 4, 7, 6,'
  'xboot_esp.c ::: if (memcmp(h, "EFI PART", 8) || hsize ::: if (0 || hsize'
  'xboot_esp.c ::: if (crc32(0, hc, hsize) != le32(h + 16)) ::: if (0)'
  'xboot_esp.c ::: if (ecrc != le32(h + 88)) ::: if (0)'
  'xboot_esp.c ::: \*count = hi - lo + 1; ::: *count = hi - lo;'
  'xboot_esp.c :::         \*next = 0; :::         *next = c;'
  'xboot_esp.c ::: return (f->data_start + (uint64_t)(c - 2) \* f->spc) ::: return (f->data_start + (uint64_t)(c - 1) * f->spc)'
  'xboot_esp.c ::: if (up(lfn\[i\]) != up(name\[i\])) ::: if (lfn[i] != name[i])'
  'xboot_esp.c ::: uint32_t n = left < sizeof(buf) ? left : sizeof(buf); ::: uint32_t n = sizeof(buf);'
  'xboot_esp.c ::: if (c == l->mark)\n        return 1; ::: if (0)\n        return 1;'
  'xboot_trial.c ::: if (r->trial_size != bs.trial_size) { ::: if (0) {'
  'xboot_trial.c ::: if (!xb_bs_wants_trial(r->record, in.record_len) || xb_bs_parse(r->record, in.record_len, &bs)) ::: if (xb_bs_parse(r->record, in.record_len, &bs))'
  'xboot_trial.c :::         if (e != XB_ESP_E_NOENT)\n            fail(r, XB_T_RECORD, e); :::         fail(r, XB_T_RECORD, e);'
  'xboot_trial.c ::: in.trial = memcmp(r->trial_sha256, bs.trial_sha256, 32) ? XB_BS_TRIAL_HASH : XB_BS_TRIAL_OK; ::: in.trial = XB_BS_TRIAL_OK;'
)
killed=0
for m in "${mutants[@]}"; do
  file=${m%% ::: *}; rest=${m#* ::: }; from=${rest%% ::: *}; to=${rest#* ::: }
  python3 -I - "$R/src/$file" "$T/src/$file" "$from" "$to" <<'PY'
import re, sys
src, dst, a, b = sys.argv[1:5]
s = open(src).read()
a = a.replace('\\n', '\n')
pat = re.sub(r'\\(.)', r'\1', a)          # the shell list escapes regex characters; match literally
n = s.count(pat)
open(dst, 'w').write(s.replace(pat, b.replace('\\n', '\n'), 1) if n == 1 else s)
sys.exit(0 if n == 1 else 3)
PY
  if [ $? -ne 0 ]; then echo "MUTANT DID NOT APPLY: $file: $from"; cp "$R/src/$file" "$T/src/$file"; continue; fi
  rm -f "$T/build/esp-test"
  if make -s -C "$T/tests/bootstate" BUILD="$T/build" "$T/build/esp-test" >/dev/null 2>&1 &&
     "$T/build/esp-test" >/dev/null 2>&1 &&
     python3 -I "$T/tests/bootstate/esp_fixture_test.py" "$T/build/esp-test" --check "$T/fixture" >/dev/null 2>&1; then
    echo "SURVIVED: $file: $from"
  else
    killed=$((killed + 1))
  fi
  cp "$R/src/$file" "$T/src/$file"
done
echo "esp mutate: $killed of ${#mutants[@]} mutants killed"
[ "$killed" -eq "${#mutants[@]}" ]
