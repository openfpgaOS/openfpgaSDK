#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# gpuvec_check.sh <qsim-out-dir> -- replay frames captured with `qsim --gpuvec`
# through (1) the os25 gpu_core Verilator model (rtlsim/obj_os25/Vtb_gpu) and
# (2) the C model (gputest run), and check
#   * RTL output == C-model output (displayed FB dump, fence register, FNV-1a-64
#     of all 64 MB SDRAM after the frame), and
#   * FNV-1a-64 of the replayed FB == hash_px that qsim recorded for the frame
#     in frames.csv (the capture is complete: nothing the frame needs is missing).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?usage: gpuvec_check.sh <qsim-out-dir>}"
RTL="${RTL:-$HERE/rtlsim/obj_os25/Vtb_gpu}"
make -s -C "$HERE" -f Makefile.gpu gputest >/dev/null
shopt -s nullglob
vecs=("$OUT"/gpuvec/frame_*.vec)
[ ${#vecs[@]} -gt 0 ] || { echo "no vectors in $OUT/gpuvec (run qsim with --gpuvec)"; exit 2; }
printf '%s\n' "${vecs[@]}" | xargs -P "$(nproc)" -I{} sh -c '"$0" "{}" "{}.rtl" 2> "{}.rtl.log" || echo "RTL FAIL {}"' "$RTL"
fail=0
for v in "${vecs[@]}"; do
  "$HERE/gputest" run "$v" "$v.model" 2>/dev/null
  n=$(basename "$v" .vec); n=$((10#${n#frame_}))
  want=$(awk -F, -v f="$n" 'NR>1 && $1==f {print $14}' "$OUT/frames.csv")
  got=$(python3 - "$v.model" <<'EOF'
import sys
d = open(sys.argv[1], 'rb').read()
fb = d[:len(d) - 12]          # DUMP(fb) + FENC(4) + HASH(8)
h = 0xcbf29ce484222325
for b in fb:
    h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
print(f"{h:016x}")
EOF
)
  if cmp -s "$v.model" "$v.rtl"; then rtl="RTL==model"; else rtl="RTL!=model"; fail=1; fi
  if [ "$got" = "$want" ]; then px="hash_px match ($got)"; else px="hash_px MISMATCH (replay $got, qsim $want)"; fail=1; fi
  echo "frame $n: $rtl, $px"
done
[ $fail = 0 ] && echo "gpuvec_check: all ${#vecs[@]} frames OK" || { echo "gpuvec_check: FAILURES"; exit 1; }
