#!/bin/sh
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# Stronger equivalence: also compare the per-pixel source trace
# (fb addr, full 32-bit s/t, z value, 24-bit light) between the traced RTL
# build (rtlsim/obj_os25_trace) and gputest_trace.
#   ./gpu_trace_equiv.sh FIRST_SEED COUNT [modes...]
set -e
cd "$(dirname "$0")"
FIRST=${1:-1}; COUNT=${2:-10}; shift 2 || true
MODES=${*:-"normal stale quake garbage overlap"}
OUT=gpuvec_out/trace_$FIRST
mkdir -p "$OUT"
list=""
for m in $MODES; do
  s=$FIRST
  while [ $s -lt $((FIRST + COUNT)) ]; do
    ./gputest gen $s $m "$OUT/$m.$s.vec"; list="$list $m.$s"; s=$((s + 1))
  done
done
echo $list | tr ' ' '\n' | grep -v '^$' | xargs -P "$(nproc)" -I{} sh -c \
  "./rtlsim/obj_os25_trace/Vtb_gpu $OUT/{}.vec $OUT/{}.rtl 2>/dev/null; ./gputest_trace run $OUT/{}.vec $OUT/{}.model"
pass=0; fail=0
for v in $list; do
  if cmp -s "$OUT/$v.model" "$OUT/$v.rtl" && cmp -s "$OUT/$v.model.trace" "$OUT/$v.rtl.trace"; then
    pass=$((pass + 1)); rm -f "$OUT/$v.model" "$OUT/$v.rtl" "$OUT/$v.model.trace" "$OUT/$v.rtl.trace"
  else
    fail=$((fail + 1)); echo "MISMATCH $v ($(wc -l < $OUT/$v.rtl.trace) rtl px, $(wc -l < $OUT/$v.model.trace) model px)"
    diff "$OUT/$v.rtl.trace" "$OUT/$v.model.trace" | head -4
  fi
done
echo "trace sweep $FIRST+$COUNT modes=[$MODES]: pass=$pass fail=$fail"
