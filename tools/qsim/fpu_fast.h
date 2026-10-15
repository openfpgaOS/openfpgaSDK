//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * Round-to-nearest-even fast path on host SSE/FMA3.
 *
 * x86 SSE scalar single-precision arithmetic is IEEE-754 exact, detects
 * tininess after rounding, and (with exceptions masked) raises UE only for
 * tiny-and-inexact results -- the same rules RISC-V uses.  Exception bits
 * accumulate in MXCSR and are folded into fcsr lazily (cpu.c).  Differences
 * handled here: RISC-V returns the canonical NaN, and fused multiply-add must
 * raise NV for inf*0 even when the addend is a quiet NaN.
 *
 * Requirements: MXCSR = 0x1f80 (all masked, nearest, no FZ/DAZ) while these
 * run; compile with -ffp-contract=off and -mfma (or -march=native on an FMA
 * host) so fmaf() becomes vfmadd.
 */
#ifndef QSIM_FPU_FAST_H
#define QSIM_FPU_FAST_H
#include <string.h>
#include <math.h>
#include <xmmintrin.h>
#include "fpu.h"

#define QSIM_MXCSR_DEFAULT 0x1f80u

static inline float qff_f(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline uint32_t qff_b(float f)
{
    uint32_t b; memcpy(&b, &f, 4);
    return qf_isnan(b) ? QF_CANON_NAN : b;
}

static inline uint32_t qff_add(uint32_t a, uint32_t b) { return qff_b(qff_f(a) + qff_f(b)); }
static inline uint32_t qff_sub(uint32_t a, uint32_t b) { return qff_b(qff_f(a) - qff_f(b)); }
static inline uint32_t qff_mul(uint32_t a, uint32_t b) { return qff_b(qff_f(a) * qff_f(b)); }
static inline uint32_t qff_div(uint32_t a, uint32_t b) { return qff_b(qff_f(a) / qff_f(b)); }
static inline uint32_t qff_sqrt(uint32_t a)
{
    return qff_b(_mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(qff_f(a)))));
}

/* a*b + c, fused.  Special operands go to the exact soft path. */
static inline uint32_t qff_fma(uint32_t a, uint32_t b, uint32_t c, uint8_t *fl)
{
    /* exponent field 0xff (inf/NaN) on any operand -> soft path */
    if (__builtin_expect(((a >> 23) & 0xff) == 0xff || ((b >> 23) & 0xff) == 0xff ||
                         ((c >> 23) & 0xff) == 0xff, 0))
        return qf_fma(a, b, c, 0, fl);
    return qff_b(fmaf(qff_f(a), qff_f(b), qff_f(c)));
}

static inline uint8_t qff_mxcsr_to_rv(uint32_t m)
{
    return (uint8_t)(((m & 0x01) ? QF_NV : 0) | ((m & 0x04) ? QF_DZ : 0) |
                     ((m & 0x08) ? QF_OF : 0) | ((m & 0x10) ? QF_UF : 0) |
                     ((m & 0x20) ? QF_NX : 0));
}

#endif
