#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# Build the qsim CPU guest test ELFs with the openfpgaOS SDK container.
# The container mounts one repository root, so sources are staged inside it
# (QSIM_TEST_STAGE, default <SDK>/build/qsim-tests/cpu) and results copied back.
#   QSIM_SDK_CONTAINER   the container wrapper to build with (default <SDK>/tools/sdk-container.sh)
#   QSIM_TEST_APP_ELF    optional production app ELF: also disassembled for the decoder cross-check
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QSIM="$(cd "$HERE/../.." && pwd)"
OUT="$QSIM/build/cpu/guest"
SDK="$(cd "$QSIM/../.." && pwd)"
STAGE="${QSIM_TEST_STAGE:-$SDK/build/qsim-tests/cpu}"
CONTAINER="${QSIM_SDK_CONTAINER:-$SDK/tools/sdk-container.sh}"
mkdir -p "$OUT" "$STAGE"
python3 "$HERE/guest/gen_stubs.py" "$STAGE"
python3 "$HERE/decode_xcheck.py" gen "$STAGE/xcheck"
cp "$HERE"/guest/*.S "$HERE"/guest/*.c "$HERE"/guest/*.ld "$STAGE"/
cp "$HERE"/guest/progs/*.c "$HERE"/guest/progs/*.h "$STAGE"/ 2>/dev/null || true
printf 'QSIM_TEST_APP_ELF=%q\n' "${QSIM_TEST_APP_ELF:-}" > "$STAGE/build.sh"
cat >> "$STAGE/build.sh" <<'EOF'
set -e
CC=riscv64-unknown-elf-gcc
ARCH="-march=rv32imafc_zicsr_zifencei_zicbom -mabi=ilp32f"
COMMON="$ARCH -nostdlib -nostartfiles -ffreestanding -fno-builtin -T link.ld"
$CC $COMMON -O2 -o t_ops.elf crt0.S stubs.S t_ops.c -lgcc
riscv64-unknown-elf-objdump -d -M no-aliases t_ops.elf > t_ops.dis
PFLAGS="-O2 -ffp-contract=off -fno-tree-loop-distribute-patterns -DGUEST"
for p in prog_*.c; do
    [ -f "$p" ] || continue
    b=${p%.c}
    $CC $COMMON $PFLAGS -o $b.elf crt0.S $p -lgcc
done
$CC $COMMON $PFLAGS -DPROG_FRAMES=1500 -o prog_bench_long.elf crt0.S prog_bench.c -lgcc
riscv64-unknown-elf-objdump -d prog_bench.elf > prog_bench.dis
APP="${QSIM_TEST_APP_ELF:-}"
[ -n "$APP" ] && [ -f "$APP" ] && riscv64-unknown-elf-objdump -d -M no-aliases $APP > app_noalias.dis
cd xcheck
$CC $ARCH -c words.S -o words.o
riscv64-unknown-elf-objdump -d -M no-aliases words.o > words.dis
$CC $ARCH -c csrs.S -o csrs.o
riscv64-unknown-elf-objdump -d -M no-aliases csrs.o > csrs.dis
EOF
bash "$CONTAINER" -C "$STAGE" bash build.sh
cp "$STAGE"/*.elf "$STAGE"/t_ops.dis "$STAGE"/stubs_meta.h "$STAGE"/stubs_keys.txt "$OUT"/
mkdir -p "$QSIM/build/cpu/xcheck" "$QSIM/build/cpu/elfdec"
[ -f "$STAGE/app_noalias.dis" ] && cp "$STAGE/app_noalias.dis" "$QSIM/build/cpu/elfdec/"
cp "$STAGE"/xcheck/words.bin "$STAGE"/xcheck/words.dis "$STAGE"/xcheck/csrs.dis "$QSIM/build/cpu/xcheck/"
python3 "$HERE/check_keys.py" "$OUT/t_ops.dis" "$OUT/stubs_keys.txt"
