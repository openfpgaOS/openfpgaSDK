//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * Host-level check of qsim's F implementation (fpu.c soft path for all five
 * rounding modes, fpu_fast.h SSE path for round-to-nearest) against the GMP
 * exact-rational reference.  Usage: test_fpu_host [vectors_per_op]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>
#include "../../fpu.h"
#include "../../fpu_fast.h"
#include "ref_f32.h"
#include "vecgen.h"

static const char *opn[] = {"add","sub","mul","div","sqrt","min","max","sgnj","sgnjn","sgnjx",
    "eq","lt","le","class","cvt_w","cvt_wu","cvt_sw","cvt_swu","mv_xw","mv_wx",
    "madd","msub","nmsub","nmadd"};

static uint32_t soft(int op, uint32_t a, uint32_t b, uint32_t c, int rm, uint8_t *fl)
{
    switch (op) {
    case ROP_ADD: return qf_add(a, b, rm, fl);
    case ROP_SUB: return qf_sub(a, b, rm, fl);
    case ROP_MUL: return qf_mul(a, b, rm, fl);
    case ROP_DIV: return qf_div(a, b, rm, fl);
    case ROP_SQRT: return qf_sqrt(a, rm, fl);
    case ROP_MIN: return qf_min(a, b, fl);
    case ROP_MAX: return qf_max(a, b, fl);
    case ROP_EQ: return qf_eq(a, b, fl);
    case ROP_LT: return qf_lt(a, b, fl);
    case ROP_LE: return qf_le(a, b, fl);
    case ROP_CLASS: return qf_class(a);
    case ROP_CVT_W: return qf_to_i32(a, rm, fl);
    case ROP_CVT_WU: return qf_to_u32(a, rm, fl);
    case ROP_CVT_SW: return qf_from_i32(a, rm, fl);
    case ROP_CVT_SWU: return qf_from_u32(a, rm, fl);
    case ROP_MADD: return qf_fma(a, b, c, rm, fl);
    case ROP_MSUB: return qf_fma(a, b, c ^ 0x80000000u, rm, fl);
    case ROP_NMSUB: return qf_fma(a ^ 0x80000000u, b, c, rm, fl);
    case ROP_NMADD: return qf_fma(a ^ 0x80000000u, b, c ^ 0x80000000u, rm, fl);
    default: return ref_op(op, a, b, c, rm, fl);
    }
}

static int fast_ok(int op) { return op <= ROP_SQRT || (op >= ROP_MADD && op <= ROP_NMADD); }

static uint32_t fast(int op, uint32_t a, uint32_t b, uint32_t c, uint8_t *fl)
{
    _mm_setcsr(QSIM_MXCSR_DEFAULT);
    uint32_t r = 0;
    uint8_t sfl = 0;
    switch (op) {
    case ROP_ADD: r = qff_add(a, b); break;
    case ROP_SUB: r = qff_sub(a, b); break;
    case ROP_MUL: r = qff_mul(a, b); break;
    case ROP_DIV: r = qff_div(a, b); break;
    case ROP_SQRT: r = qff_sqrt(a); break;
    case ROP_MADD: r = qff_fma(a, b, c, &sfl); break;
    case ROP_MSUB: r = qff_fma(a, b, c ^ 0x80000000u, &sfl); break;
    case ROP_NMSUB: r = qff_fma(a ^ 0x80000000u, b, c, &sfl); break;
    case ROP_NMADD: r = qff_fma(a ^ 0x80000000u, b, c ^ 0x80000000u, &sfl); break;
    }
    *fl = qff_mxcsr_to_rv(_mm_getcsr()) | sfl;
    _mm_setcsr(QSIM_MXCSR_DEFAULT);
    return r;
}

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 200000;
    long total = 0, fails = 0;
    int ops[] = {ROP_ADD, ROP_SUB, ROP_MUL, ROP_DIV, ROP_SQRT, ROP_MIN, ROP_MAX, ROP_EQ, ROP_LT,
                 ROP_LE, ROP_CLASS, ROP_CVT_W, ROP_CVT_WU, ROP_CVT_SW, ROP_CVT_SWU,
                 ROP_MADD, ROP_MSUB, ROP_NMSUB, ROP_NMADD};
    for (unsigned oi = 0; oi < sizeof ops / sizeof ops[0]; oi++) {
        int op = ops[oi];
        long opfail = 0, opn_ = 0;
        for (long i = 0; i < n; i++) {
            uint32_t a, b, c;
            if (op == ROP_CVT_SW || op == ROP_CVT_SWU) { a = vg_int(); b = c = 0; }
            else {
                a = vg_float(); b = vg_float_near(a);
                c = vg_float();
                if (op >= ROP_MADD && (vg_u32() & 3) == 0) {
                    /* make the addend cancel the product approximately */
                    uint8_t t = 0; uint32_t p = qf_mul(a, b, 0, &t);
                    c = p ^ 0x80000000u ^ (vg_u32() & 7);
                }
            }
            for (int rm = 0; rm < 5; rm++) {
                uint8_t rf = 0, sf = 0;
                uint32_t rr = ref_op(op, a, b, c, rm, &rf);
                uint32_t sr = soft(op, a, b, c, rm, &sf);
                total++; opn_++;
                if (rr != sr || rf != sf) {
                    if (opfail < 8)
                        printf("SOFT %s rm=%d a=%08x b=%08x c=%08x: ref %08x/%02x got %08x/%02x\n",
                               opn[op], rm, a, b, c, rr, rf, sr, sf);
                    opfail++;
                }
                if (rm == 0 && fast_ok(op)) {
                    uint8_t ff = 0;
                    uint32_t fr = fast(op, a, b, c, &ff);
                    total++; opn_++;
                    if (rr != fr || rf != ff) {
                        if (opfail < 8)
                            printf("FAST %s a=%08x b=%08x c=%08x: ref %08x/%02x got %08x/%02x\n",
                                   opn[op], a, b, c, rr, rf, fr, ff);
                        opfail++;
                    }
                }
            }
        }
        printf("%-8s %9ld checks %6ld failures\n", opn[op], opn_, opfail);
        fails += opfail;
    }
    printf("fpu-host: %ld checks, %ld failures\n", total, fails);
    return fails != 0;
}
