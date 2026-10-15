#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------
#
# gate.sh <elfA> <elfB> <outdir> -- qsim rendering-equivalence gate.
#
# Runs both ELFs of one app over the full matrix (every workload x every
# GPU-schedule/memory mode), fast mode (no --profile), in parallel, then
# compares them with compare.py.  The app and its workloads come from
# apps/<app>/ (QSIM_APP, default quake; see qsim_app.sh).
#
#   modes    : eager-lint    --gpu-sched eager --mem lint    (infinitely fast GPU; flat memory + hazard lint)
#              lazy-lint     --gpu-sched lazy  --mem lint    (GPU as late as legally possible)
#              eager-strict  --gpu-sched eager --mem strict  (split L1 / committed SDRAM)
#              lazy-strict   --gpu-sched lazy  --mem strict  (both)
#              eager-seed    --gpu-sched eager --mem lint --seed-mem 0x5eed5eed12345678
#                            (uninitialized memory = PRNG pattern instead of zeros)
# Both ELFs run with the same caps.heap_size: 0x13A00000 - bss_end of ELF A (pinned with --heap-size), so a
# .bss change in B does not change the app's own heap / cache sizing of the workload.
# Output: <outdir>/A/<workload>.<mode>/ and <outdir>/B/<workload>.<mode>/ (frames.csv, summary.json,
# console.log, qsim.log), <outdir>/gate.txt (compare.py report), and one final line:
#   GATE: PASS   or   GATE: FAIL (<reasons>)
# Exit status: 0 PASS, 1 FAIL, 2 usage/setup error.
#
# FAIL when, in ANY run pair: hash_px, hash_z or hash_pal differ on an aligned frame; a frame exists in
# only one run; a run did not finish (or finished differently); the workload parity differs (the app
# profile's parity facts, caps.heap_size); or B introduces a memory-coherency hazard class A does not have.
#
# Environment:
#   QSIM_APP=<name|dir>                    app under test (default quake)
#   QSIM_DATA_DIR=<dir>                    host directory with the app's data files
#   QSIM_GATE_WORKLOADS="..."              default: the app's workload_default_set
#   QSIM_GATE_DEMOS=...                    older name for QSIM_GATE_WORKLOADS (used when that is unset)
#   QSIM_GATE_MODES="eager-lint lazy-lint eager-strict lazy-strict eager-seed"  (also: drain-lint, eager-flat)
#   QSIM_GATE_JOBS=N                       parallel qsim processes (default: nproc)
#   QSIM_GATE_HEAP=N                       heap size for both ELFs (default: from ELF A; "device" = each ELF's own)
#   QSIM_GATE_ALLOW_BOOT_FLIPS=1           frames present in one run only are allowed before the first host frame
#   QSIM_GATE_EXTRA="..."                  extra qsim options for every run
# A run directory whose gate.stamp matches (ELF sha256, qsim sha256, app profile, workload inputs, options)
# is reused, so a baseline can be gated against several candidates in the same <outdir> without re-running A.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ $# -eq 3 ] || { sed -n '8,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//;$d' >&2; exit 2; }
source "$HERE/qsim_app.sh"
qsim_app_select "${QSIM_APP:-quake}"
ELFA="$(realpath "$1")"; ELFB="$(realpath "$2")"; OUT="$3"
for f in "$ELFA" "$ELFB"; do [ -f "$f" ] || { echo "gate.sh: no such ELF: $f" >&2; exit 2; }; done
WORKLOADS="${QSIM_GATE_WORKLOADS:-${QSIM_GATE_DEMOS:-$(workload_default_set)}}"
MODES="${QSIM_GATE_MODES:-eager-lint lazy-lint eager-strict lazy-strict eager-seed}"
JOBS="${QSIM_GATE_JOBS:-$(nproc)}"
EXTRA="${QSIM_GATE_EXTRA:-}"
SEED=0x5eed5eed12345678
APROF="$QSIM_APP_PROFILE"
DATA_DIR="$QSIM_APP_DATA_DIR"
for w in $WORKLOADS; do workload_opts "$w" > /dev/null || exit 2; done

# heap parity: pin caps.heap_size to ELF A's device value for both ELFs
HEAP="${QSIM_GATE_HEAP:-}"
if [[ " $EXTRA " == *" --heap-size "* ]]; then
    HEAP=extra
elif [ -z "$HEAP" ]; then
    HEAP="$(python3 - "$ELFA" <<'EOF'
import sys
from elftools.elf.elffile import ELFFile
e = ELFFile(open(sys.argv[1], "rb"))
top = max(s["p_vaddr"] + s["p_memsz"] for s in e.iter_segments()
          if s["p_type"] == "PT_LOAD" and s["p_memsz"] and s["p_vaddr"] >= 0x10400000)
print(hex(0x13A00000 - ((top + 15) & ~15)))
EOF
)"
fi
HEAPOPT=""
case "$HEAP" in
    extra|device) ;;
    *) HEAPOPT="--heap-size $HEAP" ;;
esac

make -s -C "$HERE" qsim >&2
QSIM="$HERE/qsim"
QSHA="$(sha256sum "$QSIM" | cut -d' ' -f1)"
ASHA="$(sha256sum "$APROF" | cut -d' ' -f1)"
mkdir -p "$OUT/A" "$OUT/B"

mode_opts() {
    case "$1" in
        eager-lint)   echo "--gpu-sched eager --mem lint" ;;
        lazy-lint)    echo "--gpu-sched lazy --mem lint" ;;
        eager-strict) echo "--gpu-sched eager --mem strict" ;;
        lazy-strict)  echo "--gpu-sched lazy --mem strict" ;;
        eager-seed)   echo "--gpu-sched eager --mem lint --seed-mem $SEED" ;;
        drain-lint)   echo "--gpu-sched drain --mem lint" ;;
        eager-flat)   echo "--gpu-sched eager --mem flat" ;;
        *) echo "gate.sh: unknown mode $1" >&2; return 1 ;;
    esac
}
for m in $MODES; do mode_opts "$m" > /dev/null || exit 2; done

JOBLIST="$OUT/jobs.txt"
: > "$JOBLIST"
NW=$(echo $WORKLOADS | wc -w); NM=$(echo $MODES | wc -w)
for side in A B; do
    elf="$ELFA"; [ "$side" = B ] && elf="$ELFB"
    esha="$(sha256sum "$elf" | cut -d' ' -f1)"
    for wl in $WORKLOADS; do
        wsha="$(workload_inputs_sha "$wl")"
        wdef="$(workload_opts "$wl" | tr '\n' ' ')"
        for mode in $MODES; do
            opts="$(mode_opts "$mode") $HEAPOPT"
            dir="$OUT/$side/$wl.$mode"
            stamp="elf=$esha qsim=$QSHA app=$QSIM_APP/$ASHA workload=$wl def=[$wdef] inputs=$wsha opts=$opts extra=$EXTRA data=$DATA_DIR"
            if [ -f "$dir/gate.stamp" ] && [ "$(cat "$dir/gate.stamp")" = "$stamp" ] && [ -f "$dir/summary.json" ]; then
                continue
            fi
            rm -rf "$dir"; mkdir -p "$dir"
            printf '%s\t%s\t%s\t%s\t%s\n' "$elf" "$dir" "$wl" "$opts" "$stamp" >> "$JOBLIST"
        done
    done
done

NJOBS=$(wc -l < "$JOBLIST")
echo "gate.sh: app $QSIM_APP: $NJOBS runs to do ($NW workloads x $NM modes x 2 ELFs, reused: $(( NW * NM * 2 - NJOBS ))), $JOBS parallel, heap ${HEAP}" >&2
T0=$(date +%s)
export QSIM DATA_DIR EXTRA HERE APROF QSIM_APP_DIR
# shellcheck disable=SC2016
tr '\n' '\0' < "$JOBLIST" | xargs -0 -r -P "$JOBS" -I{} bash -c '
    IFS=$'"'"'\t'"'"' read -r elf dir wl opts stamp <<< "$1"
    source "$QSIM_APP_DIR/workloads/workloads.sh"
    mapfile -t wopts < <(workload_opts "$wl")
    dataopt=(); [ -n "$DATA_DIR" ] && dataopt=(--data-dir "$DATA_DIR")
    # shellcheck disable=SC2086
    "$QSIM" "$elf" --app "$APROF" "${dataopt[@]}" --out "$dir" --quiet "${wopts[@]}" $opts $EXTRA > "$dir/qsim.log" 2>&1
    rc=$?
    if [ -f "$dir/summary.json" ]; then printf "%s" "$stamp" > "$dir/gate.stamp"; fi
    printf "gate.sh:   %-40s rc=%d %s\n" "${dir#$0/}" "$rc" "$(grep -m1 "stop:" "$dir/qsim.log" | sed "s/^qsim: //")" >&2
    exit 0
' "$OUT" {}
echo "gate.sh: runs finished in $(( $(date +%s) - T0 )) s" >&2

set +e
CMPOPTS=(--gate)
[ "${QSIM_GATE_ALLOW_BOOT_FLIPS:-0}" = 1 ] && CMPOPTS+=(--allow-boot-flips)
python3 "$HERE/compare.py" "${CMPOPTS[@]}" "$OUT/A" "$OUT/B" > "$OUT/gate.txt"
rc=$?
set -e
cat "$OUT/gate.txt"
line="$(grep -m1 '^GATE RESULT:' "$OUT/gate.txt" | sed 's/^GATE RESULT: //')"
if [ $rc -eq 0 ]; then
    echo "GATE: PASS"
    exit 0
elif [ $rc -eq 1 ]; then
    echo "GATE: FAIL (${line#FAIL: })"
    exit 1
else
    echo "GATE: FAIL (compare.py error $rc)"
    exit 2
fi
