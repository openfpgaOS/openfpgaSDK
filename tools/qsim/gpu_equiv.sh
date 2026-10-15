#!/bin/sh
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# RTL-vs-model equivalence sweep for the qsim GPU raster model.
#   ./gpu_equiv.sh FIRST_SEED COUNT [modes...]
# Generates COUNT vectors per mode, replays each through the os25 gpu_core
# Verilator model (rtlsim/obj_os25/Vtb_gpu) and through gpu.c (gputest run),
# and compares framebuffer zone, z zone, fence register and a hash of all 64 MB.
set -e
cd "$(dirname "$0")"
FIRST=${1:-1}; COUNT=${2:-20}; shift 2 || true
MODES=${*:-"normal stale quake garbage"}
OUT=gpuvec_out/sweep_$FIRST
mkdir -p "$OUT"
JOBS=$(nproc)
list=""
for m in $MODES; do
  s=$FIRST
  while [ $s -lt $((FIRST + COUNT)) ]; do
    ./gputest gen $s $m "$OUT/$m.$s.vec"
    list="$list $m.$s"
    s=$((s + 1))
  done
done
# RTL in parallel (one vector per process), model sequential (fast)
echo $list | tr ' ' '\n' | grep -v '^$' | xargs -P "$JOBS" -I{} sh -c \
  "${RTL:-./rtlsim/obj_os25/Vtb_gpu} $OUT/{}.vec $OUT/{}.rtl 2> $OUT/{}.rtl.log || echo 'RTL FAIL {}'"
pass=0; fail=0
for v in $list; do
  ./gputest run "$OUT/$v.vec" "$OUT/$v.model"
  if cmp -s "$OUT/$v.model" "$OUT/$v.rtl"; then
    pass=$((pass + 1)); rm -f "$OUT/$v.model" "$OUT/$v.rtl"
  else
    fail=$((fail + 1)); echo "MISMATCH $v"; ./gputest diff "$OUT/$v.model" "$OUT/$v.rtl" | tail -5
  fi
done
echo "sweep $FIRST+$COUNT modes=[$MODES]: pass=$pass fail=$fail"
