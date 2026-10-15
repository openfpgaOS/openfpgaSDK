//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * Independent reference for RISC-V binary32 semantics, built on GMP exact
 * rationals.  Shares no code with qsim's fpu.c / host-SSE fast path:
 * results are rounded from the exact rational value with a direct reading of
 * IEEE-754 (tininess after rounding) and the RISC-V F spec (canonical NaN,
 * saturating conversions, fused NV rule).
 */
#include <gmp.h>
#include <stdint.h>
#include "ref_f32.h"

#define NX 1
#define UF 2
#define OF 4
#define DZ 8
#define NV 16
#define CNAN 0x7fc00000u

static int isnan_(uint32_t a) { return ((a >> 23) & 0xff) == 0xff && (a & 0x7fffff); }
static int issnan_(uint32_t a) { return isnan_(a) && !(a & 0x400000); }
static int isinf_(uint32_t a) { return ((a >> 23) & 0xff) == 0xff && !(a & 0x7fffff); }
static int iszero_(uint32_t a) { return (a & 0x7fffffff) == 0; }

/* exact value of a finite float */
static void to_q(mpq_t q, uint32_t a)
{
    int ex = (a >> 23) & 0xff;
    uint32_t fr = a & 0x7fffff;
    long e;
    mpz_t m;
    mpz_init_set_ui(m, ex ? (fr | 0x800000) : fr);
    e = ex ? ex - 150 : -149;
    mpq_set_z(q, m);
    if (e > 0) mpq_mul_2exp(q, q, e);
    else mpq_div_2exp(q, q, -e);
    if (a >> 31) mpq_neg(q, q);
    mpz_clear(m);
}

/* round-to-integer increment decision; frac in [0,1) */
static int round_inc(mpz_t n, mpq_t frac, int rm, int sign)
{
    int c = mpq_sgn(frac);
    if (c == 0) return 0;
    mpq_t half; mpq_init(half); mpq_set_ui(half, 1, 2);
    int cmp = mpq_cmp(frac, half);
    mpq_clear(half);
    switch (rm) {
    case 0: return cmp > 0 || (cmp == 0 && mpz_odd_p(n));
    case 1: return 0;
    case 2: return sign;
    case 3: return !sign;
    default: return cmp >= 0;
    }
}

/* split |q| * 2^sh into integer n and fraction */
static void scale_split(mpz_t n, mpq_t frac, mpq_t absq, long sh)
{
    mpq_t t; mpq_init(t);
    if (sh >= 0) mpq_mul_2exp(t, absq, sh); else mpq_div_2exp(t, absq, -sh);
    mpz_fdiv_q(n, mpq_numref(t), mpq_denref(t));
    mpq_t nq; mpq_init(nq); mpq_set_z(nq, n);
    mpq_sub(frac, t, nq);
    mpq_clear(nq); mpq_clear(t);
}

/* floor(log2(|q|)) for q != 0 */
static long ilog2q(mpq_t absq)
{
    long e = (long)mpz_sizeinbase(mpq_numref(absq), 2) - (long)mpz_sizeinbase(mpq_denref(absq), 2);
    mpq_t p; mpq_init(p);
    for (;;) {
        mpq_set_ui(p, 1, 1);
        if (e >= 0) mpq_mul_2exp(p, p, e); else mpq_div_2exp(p, p, -e);
        if (mpq_cmp(p, absq) > 0) { e--; continue; }
        mpq_mul_2exp(p, p, 1);
        if (mpq_cmp(p, absq) <= 0) { e++; continue; }
        break;
    }
    mpq_clear(p);
    return e;
}

uint32_t ref_round(mpq_t q, int rm, uint8_t *fl)
{
    int sign = mpq_sgn(q) < 0;
    mpq_t a; mpq_init(a); mpq_abs(a, q);
    long e = ilog2q(a);
    mpz_t n; mpz_init(n);
    mpq_t frac; mpq_init(frac);
    uint32_t res;

    /* unbounded-exponent rounding to 24 significant bits (tininess test) */
    scale_split(n, frac, a, 23 - e);
    int inexact_unb = mpq_sgn(frac) != 0;
    int inc = round_inc(n, frac, rm, sign);
    if (inc) mpz_add_ui(n, n, 1);
    long eu = e;
    if (mpz_cmp_ui(n, 1u << 24) == 0) { eu = e + 1; }
    int tiny = eu < -126;

    if (e >= -126) {
        (void)inexact_unb;
        if (eu > 127) {
            *fl |= OF | NX;
            int to_inf = (rm == 0 || rm == 4 || (rm == 2 && sign) || (rm == 3 && !sign));
            res = ((uint32_t)sign << 31) | (to_inf ? 0x7f800000u : 0x7f7fffffu);
        } else {
            if (inexact_unb) *fl |= NX;
            uint32_t m = (uint32_t)mpz_get_ui(n);
            if (eu != e) m = 1u << 23;
            res = ((uint32_t)sign << 31) | ((uint32_t)(eu + 127) << 23) | (m & 0x7fffff);
        }
    } else {
        scale_split(n, frac, a, 149);
        int inexact = mpq_sgn(frac) != 0;
        if (round_inc(n, frac, rm, sign)) mpz_add_ui(n, n, 1);
        uint32_t m = (uint32_t)mpz_get_ui(n);     /* <= 2^23 */
        if (inexact) *fl |= NX;
        if (tiny && inexact) *fl |= UF;
        res = ((uint32_t)sign << 31) | m;           /* m == 2^23 packs as min normal */
    }
    mpz_clear(n); mpq_clear(frac); mpq_clear(a);
    return res;
}

static uint32_t exact_zero_sign(int rm) { return rm == 2 ? 0x80000000u : 0; }

uint32_t ref_add(uint32_t a, uint32_t b, int rm, uint8_t *fl)
{
    if (isnan_(a) || isnan_(b)) { if (issnan_(a) || issnan_(b)) *fl |= NV; return CNAN; }
    if (isinf_(a) && isinf_(b)) {
        if ((a ^ b) >> 31) { *fl |= NV; return CNAN; }
        return a;
    }
    if (isinf_(a)) return a;
    if (isinf_(b)) return b;
    mpq_t x, y; mpq_init(x); mpq_init(y);
    to_q(x, a); to_q(y, b);
    mpq_add(x, x, y);
    uint32_t r;
    if (mpq_sgn(x) == 0) {
        if (iszero_(a) && iszero_(b) && !((a ^ b) >> 31)) r = a;
        else r = exact_zero_sign(rm);
    } else r = ref_round(x, rm, fl);
    mpq_clear(x); mpq_clear(y);
    return r;
}

uint32_t ref_sub(uint32_t a, uint32_t b, int rm, uint8_t *fl) { return ref_add(a, b ^ 0x80000000u, rm, fl); }

uint32_t ref_mul(uint32_t a, uint32_t b, int rm, uint8_t *fl)
{
    uint32_t s = (a ^ b) & 0x80000000u;
    if (isnan_(a) || isnan_(b)) { if (issnan_(a) || issnan_(b)) *fl |= NV; return CNAN; }
    if ((isinf_(a) && iszero_(b)) || (iszero_(a) && isinf_(b))) { *fl |= NV; return CNAN; }
    if (isinf_(a) || isinf_(b)) return s | 0x7f800000u;
    if (iszero_(a) || iszero_(b)) return s;
    mpq_t x, y; mpq_init(x); mpq_init(y);
    to_q(x, a); to_q(y, b); mpq_mul(x, x, y);
    uint32_t r = ref_round(x, rm, fl);
    mpq_clear(x); mpq_clear(y);
    return r;
}

uint32_t ref_div(uint32_t a, uint32_t b, int rm, uint8_t *fl)
{
    uint32_t s = (a ^ b) & 0x80000000u;
    if (isnan_(a) || isnan_(b)) { if (issnan_(a) || issnan_(b)) *fl |= NV; return CNAN; }
    if ((isinf_(a) && isinf_(b)) || (iszero_(a) && iszero_(b))) { *fl |= NV; return CNAN; }
    if (isinf_(a)) return s | 0x7f800000u;
    if (isinf_(b)) return s;
    if (iszero_(b)) { *fl |= DZ; return s | 0x7f800000u; }
    if (iszero_(a)) return s;
    mpq_t x, y; mpq_init(x); mpq_init(y);
    to_q(x, a); to_q(y, b); mpq_div(x, x, y);
    uint32_t r = ref_round(x, rm, fl);
    mpq_clear(x); mpq_clear(y);
    return r;
}

uint32_t ref_sqrt(uint32_t a, int rm, uint8_t *fl)
{
    if (isnan_(a)) { if (issnan_(a)) *fl |= NV; return CNAN; }
    if (iszero_(a)) return a;
    if (a >> 31) { *fl |= NV; return CNAN; }
    if (isinf_(a)) return a;
    /* a = m * 2^e ; sqrt via integer sqrt of m * 2^(2K + (e odd)) */
    int ex = (a >> 23) & 0xff;
    uint32_t fr = a & 0x7fffff;
    long e = ex ? ex - 150 : -149;
    mpz_t m, r, rem; mpz_init_set_ui(m, ex ? (fr | 0x800000) : fr); mpz_init(r); mpz_init(rem);
    if (e & 1) { mpz_mul_2exp(m, m, 1); e--; }
    const long K = 100;
    mpz_mul_2exp(m, m, 2 * K);
    mpz_sqrtrem(r, rem, m);
    /* sqrt = (r + eps) * 2^(e/2 - K); use 2r+1 when inexact */
    mpq_t q; mpq_init(q);
    long sh = e / 2 - K;
    if (mpz_sgn(rem)) { mpz_mul_2exp(r, r, 1); mpz_add_ui(r, r, 1); sh -= 1; }
    mpq_set_z(q, r);
    if (sh >= 0) mpq_mul_2exp(q, q, sh); else mpq_div_2exp(q, q, -sh);
    uint32_t res = ref_round(q, rm, fl);
    mpq_clear(q); mpz_clear(m); mpz_clear(r); mpz_clear(rem);
    return res;
}

uint32_t ref_fma(uint32_t a, uint32_t b, uint32_t c, int rm, uint8_t *fl)
{
    int itz = (isinf_(a) && iszero_(b)) || (iszero_(a) && isinf_(b));
    if (isnan_(a) || isnan_(b) || isnan_(c)) {
        if (issnan_(a) || issnan_(b) || issnan_(c) || itz) *fl |= NV;
        return CNAN;
    }
    if (itz) { *fl |= NV; return CNAN; }
    uint32_t sp = (a ^ b) & 0x80000000u;
    if (isinf_(a) || isinf_(b)) {
        if (isinf_(c) && (c & 0x80000000u) != sp) { *fl |= NV; return CNAN; }
        return sp | 0x7f800000u;
    }
    if (isinf_(c)) return c;
    mpq_t x, y, z; mpq_init(x); mpq_init(y); mpq_init(z);
    to_q(x, a); to_q(y, b); to_q(z, c);
    mpq_mul(x, x, y); mpq_add(x, x, z);
    uint32_t r;
    if (mpq_sgn(x) == 0) {
        int pzero = iszero_(a) || iszero_(b);
        if (pzero && iszero_(c) && sp == (c & 0x80000000u)) r = c;
        else r = exact_zero_sign(rm);
    } else r = ref_round(x, rm, fl);
    mpq_clear(x); mpq_clear(y); mpq_clear(z);
    return r;
}

static uint32_t ref_to_int(uint32_t a, int rm, uint8_t *fl, int sgn)
{
    int sign = a >> 31;
    uint32_t max = sgn ? 0x7fffffffu : 0xffffffffu;
    uint32_t min = sgn ? 0x80000000u : 0;
    if (isnan_(a)) { *fl |= NV; return max; }
    if (isinf_(a)) { *fl |= NV; return sign ? min : max; }
    mpq_t q, frac; mpq_init(q); mpq_init(frac);
    to_q(q, a);
    mpq_t aq; mpq_init(aq); mpq_abs(aq, q);
    mpz_t n; mpz_init(n);
    scale_split(n, frac, aq, 0);
    int inexact = mpq_sgn(frac) != 0;
    if (round_inc(n, frac, rm, sign)) mpz_add_ui(n, n, 1);
    if (sign) mpz_neg(n, n);
    uint32_t r;
    mpz_t lo, hi; mpz_init(lo); mpz_init(hi);
    if (sgn) { mpz_set_si(lo, -2147483648L); mpz_set_si(hi, 2147483647L); }
    else { mpz_set_ui(lo, 0); mpz_set_ui(hi, 0xffffffffUL); }
    if (mpz_cmp(n, lo) < 0) { *fl |= NV; r = min; }
    else if (mpz_cmp(n, hi) > 0) { *fl |= NV; r = max; }
    else {
        if (inexact) *fl |= NX;
        r = (uint32_t)(int64_t)mpz_get_si(n);
    }
    mpz_clear(lo); mpz_clear(hi); mpz_clear(n); mpq_clear(q); mpq_clear(frac); mpq_clear(aq);
    return r;
}

uint32_t ref_to_i32(uint32_t a, int rm, uint8_t *fl) { return ref_to_int(a, rm, fl, 1); }
uint32_t ref_to_u32(uint32_t a, int rm, uint8_t *fl) { return ref_to_int(a, rm, fl, 0); }

uint32_t ref_from_i32(uint32_t v, int rm, uint8_t *fl)
{
    if (!v) return 0;
    mpq_t q; mpq_init(q); mpq_set_si(q, (long)(int32_t)v, 1);
    uint32_t r = ref_round(q, rm, fl);
    mpq_clear(q);
    return r;
}

uint32_t ref_from_u32(uint32_t v, int rm, uint8_t *fl)
{
    if (!v) return 0;
    mpq_t q; mpq_init(q); mpq_set_ui(q, v, 1);
    uint32_t r = ref_round(q, rm, fl);
    mpq_clear(q);
    return r;
}

/* comparisons through exact rationals */
static int cmpq(uint32_t a, uint32_t b)
{
    if (isinf_(a) || isinf_(b)) {
        int va = isinf_(a) ? ((a >> 31) ? -1 : 1) : 0;
        int vb = isinf_(b) ? ((b >> 31) ? -1 : 1) : 0;
        if (isinf_(a) && isinf_(b)) return va - vb;
        if (isinf_(a)) return va;
        return -vb;
    }
    mpq_t x, y; mpq_init(x); mpq_init(y);
    to_q(x, a); to_q(y, b);
    int c = mpq_cmp(x, y);
    mpq_clear(x); mpq_clear(y);
    return c;
}

uint32_t ref_eq(uint32_t a, uint32_t b, uint8_t *fl)
{
    if (isnan_(a) || isnan_(b)) { if (issnan_(a) || issnan_(b)) *fl |= NV; return 0; }
    return cmpq(a, b) == 0;
}
uint32_t ref_lt(uint32_t a, uint32_t b, uint8_t *fl)
{
    if (isnan_(a) || isnan_(b)) { *fl |= NV; return 0; }
    return cmpq(a, b) < 0;
}
uint32_t ref_le(uint32_t a, uint32_t b, uint8_t *fl)
{
    if (isnan_(a) || isnan_(b)) { *fl |= NV; return 0; }
    return cmpq(a, b) <= 0;
}
uint32_t ref_min(uint32_t a, uint32_t b, uint8_t *fl)
{
    if (isnan_(a) || isnan_(b)) { if (issnan_(a) || issnan_(b)) *fl |= NV; return CNAN; }
    int c = cmpq(a, b);
    if (c == 0) return (a >> 31) ? a : b;   /* -0 preferred */
    return c < 0 ? a : b;
}
uint32_t ref_max(uint32_t a, uint32_t b, uint8_t *fl)
{
    if (isnan_(a) || isnan_(b)) { if (issnan_(a) || issnan_(b)) *fl |= NV; return CNAN; }
    int c = cmpq(a, b);
    if (c == 0) return (a >> 31) ? b : a;   /* +0 preferred */
    return c > 0 ? a : b;
}
uint32_t ref_class(uint32_t a)
{
    int s = a >> 31;
    if (isinf_(a)) return s ? 1 : 1 << 7;
    if (isnan_(a)) return issnan_(a) ? 1 << 8 : 1 << 9;
    if (iszero_(a)) return s ? 1 << 3 : 1 << 4;
    if (((a >> 23) & 0xff) == 0) return s ? 1 << 2 : 1 << 5;
    return s ? 1 << 1 : 1 << 6;
}

/* dispatcher used by the tests: op ids shared with the guest harness */
uint32_t ref_op(int op, uint32_t a, uint32_t b, uint32_t c, int rm, uint8_t *fl)
{
    switch (op) {
    case ROP_ADD: return ref_add(a, b, rm, fl);
    case ROP_SUB: return ref_sub(a, b, rm, fl);
    case ROP_MUL: return ref_mul(a, b, rm, fl);
    case ROP_DIV: return ref_div(a, b, rm, fl);
    case ROP_SQRT: return ref_sqrt(a, rm, fl);
    case ROP_MIN: return ref_min(a, b, fl);
    case ROP_MAX: return ref_max(a, b, fl);
    case ROP_SGNJ: return (a & 0x7fffffff) | (b & 0x80000000u);
    case ROP_SGNJN: return (a & 0x7fffffff) | (~b & 0x80000000u);
    case ROP_SGNJX: return a ^ (b & 0x80000000u);
    case ROP_EQ: return ref_eq(a, b, fl);
    case ROP_LT: return ref_lt(a, b, fl);
    case ROP_LE: return ref_le(a, b, fl);
    case ROP_CLASS: return ref_class(a);
    case ROP_CVT_W: return ref_to_i32(a, rm, fl);
    case ROP_CVT_WU: return ref_to_u32(a, rm, fl);
    case ROP_CVT_SW: return ref_from_i32(a, rm, fl);
    case ROP_CVT_SWU: return ref_from_u32(a, rm, fl);
    case ROP_MV_XW: return a;
    case ROP_MV_WX: return a;
    case ROP_MADD: return ref_fma(a, b, c, rm, fl);
    case ROP_MSUB: return ref_fma(a, b, c ^ 0x80000000u, rm, fl);
    case ROP_NMSUB: return ref_fma(a ^ 0x80000000u, b, c, rm, fl);
    case ROP_NMADD: return ref_fma(a ^ 0x80000000u, b, c ^ 0x80000000u, rm, fl);
    }
    return 0;
}
