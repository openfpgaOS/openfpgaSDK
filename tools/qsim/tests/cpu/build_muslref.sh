#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# Build build/cpu/musl_ref.a: musl-1.2.5 float math compiled for the host with
# the same ISO-C flags musl uses (config.mak CFLAGS_C99FSE), symbols prefixed
# musl_.  Sources are copied from a read-only musl-1.2.5 tree (QSIM_MUSL_DIR,
# default the openfpgaOS firmware build).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QSIM="$(cd "$HERE/../.." && pwd)"
M="${QSIM_MUSL_DIR:-$HOME/Repos/openfpgaOS/src/firmware/musl/build/musl-1.2.5}"
S="$QSIM/build/cpu/muslref"
rm -rf "$S"; mkdir -p "$S/src" "$S/inc/bits" "$S/obj"
for f in sinf cosf tanf atan2f atanf fabsf powf powf_data floorf ceilf floor __tandf __sindf __cosdf \
         __rem_pio2f __rem_pio2_large __math_invalidf __math_oflowf __math_uflowf __math_xflowf \
         __math_divzerof exp2f_data log2f_data scalbnf scalbn sqrtf; do
    cp "$M/src/math/$f.c" "$S/src/"
done
cp "$M"/src/math/*.h "$S/src/"
sed -f "$M/tools/mkalltypes.sed" "$M/arch/x86_64/bits/alltypes.h.in" "$M/include/alltypes.h.in" > "$S/inc/bits/alltypes.h"
for f in "$S"/src/*.c; do
    o="$S/obj/$(basename "$f" .c).o"
    gcc -std=c99 -nostdinc -ffreestanding -fexcess-precision=standard -frounding-math -fno-strict-aliasing \
        -ffp-contract=off -fno-stack-protector -fno-builtin -O2 -D_XOPEN_SOURCE=700 \
        -I "$M/arch/x86_64" -I "$M/arch/generic" -I "$S/inc" -I "$M/src/include" -I "$M/src/internal" \
        -I "$M/include" -c "$f" -o "$o"
    objcopy --prefix-symbols=musl_ "$o"
done
rm -f "$QSIM/build/cpu/musl_ref.a"
ar rcs "$QSIM/build/cpu/musl_ref.a" "$S"/obj/*.o
echo "musl_ref.a: $(ls "$S"/obj/*.o | wc -l) objects"
