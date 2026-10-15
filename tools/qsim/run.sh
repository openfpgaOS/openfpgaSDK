#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------
#
# run.sh <elf> <outdir> [workload] -- one qsim run of an openfpgaOS app ELF.
#
# The app and its workloads come from apps/<app>/ (QSIM_APP, default quake):
#   apps/<app>/<app>.app            the app profile (data slots, .ini, argv[0])
#   apps/<app>/workloads/workloads.sh   the workload table (workload_opts <name>)
# Run `./run.sh` with no arguments to list the workloads of the selected app.
#
# Environment:
#   QSIM_APP=<name|dir>    app under test (default quake; a name resolves to apps/<name>)
#   QSIM_DATA_DIR=<dir>    host directory holding the app's data files (--data-dir).
#                          Required by apps that declare data: slots.  QSIM_PAK_DIR
#                          is accepted as the older name.
#   QSIM_FAST=1            skip --profile (about 2-2.5x faster; model columns are 0)
#   QSIM_PPM_EVERY=K       screenshot period (0 = none; default 100)
#   QSIM_GPU_SCHED=S       --gpu-sched eager|lazy|drain (default eager)
#   QSIM_MEM=M             --mem flat|lint|strict (default flat)
#   QSIM_SEED_MEM=S        --seed-mem S (default 0: zero-filled memory)
#   QSIM_HEAP_SIZE=N       --heap-size N (default: device-exact 0x13A00000 - bss_end)
#   QSIM_EXTRA="..."       extra qsim options (e.g. "--frames 300")
#
# One run checks one workload, GPU schedule and memory mode.  The rendering-
# equivalence gate is gate.sh <elfA> <elfB> <outdir>.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=qsim_app.sh
source "$HERE/qsim_app.sh"
qsim_app_select "${QSIM_APP:-quake}"

[ $# -ge 2 ] || { echo "usage: run.sh <elf> <outdir> [$(workload_names | tr ' ' '|')]" >&2; exit 2; }
ELF="$1"; OUT="$2"; WL="${3:-$(workload_names | awk '{print $1}')}"
PPM_EVERY="${QSIM_PPM_EVERY:-100}"
mapfile -t WOPTS < <(workload_opts "$WL") || true
[ ${#WOPTS[@]} -gt 0 ] || exit 2

make -s -C "$HERE" qsim >&2

OPTS=(--app "$QSIM_APP_PROFILE" --out "$OUT" "${WOPTS[@]}")
[ -n "$QSIM_APP_DATA_DIR" ] && OPTS+=(--data-dir "$QSIM_APP_DATA_DIR")
[ "${QSIM_FAST:-0}" = 1 ] || OPTS+=(--profile)
[ -n "${QSIM_GPU_SCHED:-}" ] && OPTS+=(--gpu-sched "$QSIM_GPU_SCHED")
[ -n "${QSIM_MEM:-}" ] && OPTS+=(--mem "$QSIM_MEM")
[ -n "${QSIM_SEED_MEM:-}" ] && OPTS+=(--seed-mem "$QSIM_SEED_MEM")
[ -n "${QSIM_HEAP_SIZE:-}" ] && OPTS+=(--heap-size "$QSIM_HEAP_SIZE")
[ "$PPM_EVERY" -gt 0 ] && OPTS+=(--ppm-every "$PPM_EVERY")
# shellcheck disable=SC2206
[ -n "${QSIM_EXTRA:-}" ] && OPTS+=(${QSIM_EXTRA})

mkdir -p "$OUT"
set +e
"$HERE/qsim" "$ELF" "${OPTS[@]}" 2> >(tee "$OUT/qsim.log" >&2)
rc=$?
set -e
python3 - "$OUT" <<'EOF'
import json, sys
s = json.load(open(sys.argv[1] + "/summary.json"))
w = s["per_frame_timedemo_window"]
mc = s.get("memory_coherency")
print(f"app: {s.get('app') or '?'}  workload: {s.get('workload') or '?'}  "
      f"mode: gpu_sched={s.get('gpu_sched', 'eager')} mem={s.get('mem_mode', 'flat')}"
      + (f" seed_mem={s['seed_mem']}" if s.get("seed_mem") else "")
      + (f", memory-coherency hazards={mc['hazards']} (worst case {mc['hazards_worst_case']})" if mc else ""))
print(f"status={s['status']} frames={s['frames']} result='{s['timedemo_result']}' "
      f"insns={s['totals']['insns']} est_cycles={s['totals']['est_cycles']} "
      f"wall={s['wall_seconds']}s mips={s['guest_mips']}")
kind = (s.get("window") or {}).get("kind", "timedemo")
if w["frames"]:
    print(f"{kind} window: {w['frames']} frames from frame {w['first_frame']}, insns/frame p50={w['insns']['p50']} "
          f"p90={w['insns']['p90']} max={w['insns']['max']}"
          + (f", est_cycles/frame p50={w['est_cycles']['p50']} p90={w['est_cycles']['p90']} "
             f"max={w['est_cycles']['max']}" if "est_cycles" in w else ""))
EOF
exit $rc
