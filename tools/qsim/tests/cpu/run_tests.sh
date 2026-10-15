#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# qsim CPU self-test suite.  Run from anywhere:  bash tests/cpu/run_tests.sh [--quick]
# Prerequisites: make -f Makefile.cpu all tests ; make -f Makefile.cpu guests (container)
# Optional: QSIM_TEST_APP_ELF=<a production app ELF> adds two checks that run the
# real thing -- a decoder cross-check over every instruction of its functions, and
# its soft-float/libm helpers against the host.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QSIM="$(cd "$HERE/../.." && pwd)"
B="$QSIM/build/cpu"
APP="${QSIM_TEST_APP_ELF:-}"
QUICK=0; [ "${1:-}" = "--quick" ] && QUICK=1
FPU_N=500000; STUB_V=3000; ELF_N=100000
[ $QUICK = 1 ] && { FPU_N=20000; STUB_V=200; ELF_N=5000; }
fail=0
run() {
    local name="$1"; shift
    echo "== $name"
    if "$@"; then echo "   PASS"; else echo "   FAIL ($name)"; fail=1; fi
}

run "fpu-host (qsim F soft path + SSE path vs GMP exact reference)" \
    bash -c "'$B/test_fpu_host' $FPU_N | tail -1"
run "api (budget, SMC, hooks, alias, misaligned, interrupts, faults, fflags, profiling)" "$B/test_api"
run "instruction stubs (every opcode class, 32-bit + compressed, random vectors)" \
    bash -c "'$B/cpurun' --stubs --vecs $STUB_V --quiet '$B/guest/t_ops.elf' 2>&1 | tail -15"
run "instruction stubs under the profiling interpreter" \
    bash -c "'$B/cpurun' --stubs --vecs $((STUB_V / 4 + 1)) --prof --seed 99 --quiet '$B/guest/t_ops.elf' 2>&1 | grep -E '^stubs:|failures$' | tail -1"
run "decoder cross-check vs GNU objdump" \
    bash -c "'$B/decode_dump' '$B/xcheck/words.bin' 0x10000000 > '$B/xcheck/qsim.txt' && python3 '$HERE/decode_xcheck.py' cmp '$B/xcheck' | tail -2"
if [ -n "$APP" ] && [ -f "$APP" ] && [ -f "$B/elfdec/app_noalias.dis" ]; then
    run "decoder cross-check on every instruction of the production ELF's functions" \
        bash -c "python3 '$HERE/decode_xcheck.py' elf '$B/elfdec' '$B/xcheck' '$APP' '$B/decode_dump' | tail -2"
fi
for p in prog_mix prog_bench; do
    run "compiled C program $p: qsim (fast + profiling) vs host-native output" bash -c "
        '$B/host/$p' > '$B/host/$p.out' &&
        '$B/cpurun' '$B/guest/$p.elf' 2>/dev/null > '$B/host/$p.qsim' &&
        '$B/cpurun' --prof '$B/guest/$p.elf' 2>/dev/null > '$B/host/$p.qsimprof' &&
        cmp '$B/host/$p.out' '$B/host/$p.qsim' && cmp '$B/host/$p.out' '$B/host/$p.qsimprof' && cat '$B/host/$p.out'"
done
if [ -n "$APP" ] && [ -f "$APP" ]; then
    run "production ELF soft-float helpers + libm vs host" bash -c "'$B/test_elf_helpers' '$APP' $ELF_N"
else
    echo "== production ELF helpers: SKIPPED (set QSIM_TEST_APP_ELF to a production app ELF)"
fi
if [ $fail = 0 ]; then echo "ALL CPU TESTS PASSED"; else echo "SOME CPU TESTS FAILED"; fi
exit $fail
