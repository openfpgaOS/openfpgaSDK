//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * Exact binary32 soft-float for qsim (integer-only; see fpu.h).
 * Structure follows Berkeley SoftFloat 3 (roundPackToF32 with tininess after
 * rounding) but is written from scratch around 128-bit accumulators so each
 * operation is a few obviously-exact integer steps.
 */
#include "fpu.h"

typedef unsigned __int128 u128;

#define RM_NE 0
#define RM_TZ 1
#define RM_DN 2
#define RM_UP 3
#define RM_MM 4

static inline uint32_t shr_jam32(uint32_t a, int dist)
{
    if (dist <= 0) return a;
    if (dist >= 32) return a != 0;
    return (a >> dist) | ((uint32_t)(a << (32 - dist)) != 0);
}

/* value = sig * 2^(exp - 156); sig has its leading one at bit 30 (or is a
 * jammed smaller value when called for subnormal paths).  exp is the biased
 * exponent minus one.  (SoftFloat convention.) */
static uint32_t round_pack(int sign, int exp, uint32_t sig, int rm, uint8_t *fl)
{
    int near_even = (rm == RM_NE);
    uint32_t inc = 0x40;
    if (rm != RM_NE && rm != RM_MM)
        inc = (rm == (sign ? RM_DN : RM_UP)) ? 0x7f : 0;
    uint32_t round_bits = sig & 0x7f;
    if ((unsigned)exp >= 0xfd) {
        if (exp < 0) {
            int tiny = (exp < -1) || (sig + inc < 0x80000000u);
            sig = shr_jam32(sig, -exp);
            exp = 0;
            round_bits = sig & 0x7f;
            if (tiny && round_bits) *fl |= QF_UF;
        } else if (exp > 0xfd || sig + inc >= 0x80000000u) {
            *fl |= QF_OF | QF_NX;
            return ((uint32_t)sign << 31) | (inc ? 0x7f800000u : 0x7f7fffffu);
        }
    }
    sig = (sig + inc) >> 7;
    if (round_bits) *fl |= QF_NX;
    if (near_even && round_bits == 0x40) sig &= ~1u;
    if (!sig) exp = 0;
    return ((uint32_t)sign << 31) + ((uint32_t)exp << 23) + sig;
}

static inline int clz128(u128 v)
{
    uint64_t hi = (uint64_t)(v >> 64);
    if (hi) return __builtin_clzll(hi);
    return 64 + __builtin_clzll((uint64_t)v);
}

/* value = sig * 2^e2 (sig != 0; low bits may carry a sticky jam) */
static uint32_t norm_round_pack(int sign, int e2, u128 sig, int rm, uint8_t *fl)
{
    int lz = clz128(sig);
    sig <<= lz;
    e2 -= lz;
    int sticky = (((uint64_t)(sig >> 64) & 0x1ffffffffull) != 0) || ((uint64_t)sig != 0);
    uint32_t s32 = (uint32_t)(sig >> 97) | (uint32_t)sticky;
    long exp = (long)e2 + 253;
    if (exp > 0x3ff) exp = 0x3ff;       /* keep in int range, still overflows */
    if (exp < -0x3ff) exp = -0x3ff;     /* shr_jam32 saturates at 32 anyway  */
    return round_pack(sign, (int)exp, s32, rm, fl);
}

/* finite nonzero unpack: value = sig * 2^e */
static inline void unpack(uint32_t a, uint32_t *sig, int *e)
{
    uint32_t ex = (a >> 23) & 0xff, fr = a & 0x7fffff;
    if (ex == 0) { *sig = fr; *e = -149; }
    else { *sig = fr | 0x800000u; *e = (int)ex - 150; }
}

static uint32_t nan_result(uint32_t a, uint32_t b, uint8_t *fl)
{
    if (qf_issnan(a) || qf_issnan(b)) *fl |= QF_NV;
    return QF_CANON_NAN;
}

/* sum of two finite nonzero values x*2^ex (+/-) y*2^ey, x,y < 2^49 */
static uint32_t add_finite(int sx, int ex, uint64_t x, int sy, int ey, uint64_t y, int rm, uint8_t *fl)
{
    /* place leading ones at bit 100 */
    int lx = 63 - __builtin_clzll(x), ly = 63 - __builtin_clzll(y);
    u128 X = (u128)x << (100 - lx); ex -= 100 - lx;
    u128 Y = (u128)y << (100 - ly); ey -= 100 - ly;
    int e;
    if (ex >= ey) {
        int d = ex - ey;
        if (d >= 128) Y = (Y != 0);
        else if (d) Y = (Y >> d) | ((Y << (128 - d)) != 0);
        e = ex;
    } else {
        int d = ey - ex;
        if (d >= 128) X = (X != 0);
        else X = (X >> d) | ((X << (128 - d)) != 0);
        e = ey;
    }
    int sign;
    u128 S;
    if (sx == sy) { S = X + Y; sign = sx; }
    else if (X > Y) { S = X - Y; sign = sx; }
    else if (Y > X) { S = Y - X; sign = sy; }
    else { return (rm == RM_DN) ? 0x80000000u : 0; }
    return norm_round_pack(sign, e, S, rm, fl);
}

uint32_t qf_add(uint32_t a, uint32_t b, int rm, uint8_t *fl)
{
    int sa = a >> 31, sb = b >> 31;
    if (qf_isnan(a) || qf_isnan(b)) return nan_result(a, b, fl);
    if (qf_isinf(a)) {
        if (qf_isinf(b) && sa != sb) { *fl |= QF_NV; return QF_CANON_NAN; }
        return a;
    }
    if (qf_isinf(b)) return b;
    if (qf_iszero(a)) {
        if (qf_iszero(b)) {
            if (sa == sb) return a;
            return (rm == RM_DN) ? 0x80000000u : 0;
        }
        return b;
    }
    if (qf_iszero(b)) return a;
    uint32_t ma, mb; int ea, eb;
    unpack(a, &ma, &ea); unpack(b, &mb, &eb);
    return add_finite(sa, ea, ma, sb, eb, mb, rm, fl);
}

uint32_t qf_sub(uint32_t a, uint32_t b, int rm, uint8_t *fl)
{
    return qf_add(a, b ^ 0x80000000u, rm, fl);
}

uint32_t qf_mul(uint32_t a, uint32_t b, int rm, uint8_t *fl)
{
    int s = (a ^ b) >> 31;
    if (qf_isnan(a) || qf_isnan(b)) return nan_result(a, b, fl);
    if (qf_isinf(a) || qf_isinf(b)) {
        if (qf_iszero(a) || qf_iszero(b)) { *fl |= QF_NV; return QF_CANON_NAN; }
        return ((uint32_t)s << 31) | 0x7f800000u;
    }
    if (qf_iszero(a) || qf_iszero(b)) return (uint32_t)s << 31;
    uint32_t ma, mb; int ea, eb;
    unpack(a, &ma, &ea); unpack(b, &mb, &eb);
    return norm_round_pack(s, ea + eb, (u128)((uint64_t)ma * mb), rm, fl);
}

static inline void normalize24(uint32_t *m, int *e)
{
    int l = 31 - __builtin_clz(*m);
    if (l < 23) { *m <<= 23 - l; *e -= 23 - l; }
}

uint32_t qf_div(uint32_t a, uint32_t b, int rm, uint8_t *fl)
{
    int s = (a ^ b) >> 31;
    if (qf_isnan(a) || qf_isnan(b)) return nan_result(a, b, fl);
    if (qf_isinf(a)) {
        if (qf_isinf(b)) { *fl |= QF_NV; return QF_CANON_NAN; }
        return ((uint32_t)s << 31) | 0x7f800000u;
    }
    if (qf_isinf(b)) return (uint32_t)s << 31;
    if (qf_iszero(b)) {
        if (qf_iszero(a)) { *fl |= QF_NV; return QF_CANON_NAN; }
        *fl |= QF_DZ;
        return ((uint32_t)s << 31) | 0x7f800000u;
    }
    if (qf_iszero(a)) return (uint32_t)s << 31;
    uint32_t ma, mb; int ea, eb;
    unpack(a, &ma, &ea); unpack(b, &mb, &eb);
    normalize24(&ma, &ea); normalize24(&mb, &eb);
    uint64_t n = (uint64_t)ma << 40;
    uint64_t q = n / mb, r = n % mb;
    q = (q << 1) | (r != 0);           /* jam bit below the quotient LSB */
    return norm_round_pack(s, ea - eb - 41, (u128)q, rm, fl);
}

static uint64_t isqrt64(uint64_t n, uint64_t *rem)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    *rem = n;
    return r;
}

uint32_t qf_sqrt(uint32_t a, int rm, uint8_t *fl)
{
    if (qf_isnan(a)) return nan_result(a, a, fl);
    if (qf_iszero(a)) return a;
    if (a >> 31) { *fl |= QF_NV; return QF_CANON_NAN; }
    if (qf_isinf(a)) return a;
    uint32_t m; int e;
    unpack(a, &m, &e);
    normalize24(&m, &e);
    uint64_t n = (uint64_t)m;
    if (e & 1) { n <<= 1; e -= 1; }
    n <<= 38;                           /* n < 2^63 */
    uint64_t rem, r = isqrt64(n, &rem);
    r = (r << 1) | (rem != 0);
    return norm_round_pack(0, (e - 38) / 2 - 1, (u128)r, rm, fl);
}

uint32_t qf_fma(uint32_t a, uint32_t b, uint32_t c, int rm, uint8_t *fl)
{
    int sp = (a ^ b) >> 31, sc = c >> 31;
    int inf_times_zero = (qf_isinf(a) && qf_iszero(b)) || (qf_iszero(a) && qf_isinf(b));
    if (qf_isnan(a) || qf_isnan(b) || qf_isnan(c)) {
        if (qf_issnan(a) || qf_issnan(b) || qf_issnan(c) || inf_times_zero) *fl |= QF_NV;
        return QF_CANON_NAN;
    }
    if (inf_times_zero) { *fl |= QF_NV; return QF_CANON_NAN; }
    if (qf_isinf(a) || qf_isinf(b)) {
        if (qf_isinf(c) && sc != sp) { *fl |= QF_NV; return QF_CANON_NAN; }
        return ((uint32_t)sp << 31) | 0x7f800000u;
    }
    if (qf_isinf(c)) return c;
    if (qf_iszero(a) || qf_iszero(b)) {
        if (qf_iszero(c)) {
            if (sp == sc) return c;
            return (rm == RM_DN) ? 0x80000000u : 0;
        }
        return c;
    }
    uint32_t ma, mb, mc; int ea, eb, ec;
    unpack(a, &ma, &ea); unpack(b, &mb, &eb);
    uint64_t p = (uint64_t)ma * mb;
    if (qf_iszero(c)) return norm_round_pack(sp, ea + eb, (u128)p, rm, fl);
    unpack(c, &mc, &ec);
    return add_finite(sp, ea + eb, p, sc, ec, mc, rm, fl);
}

/* ---- conversions ------------------------------------------------------ */

static uint32_t to_int(uint32_t a, int rm, uint8_t *fl, int is_signed)
{
    int sign = a >> 31;
    if (qf_isnan(a)) { *fl |= QF_NV; return is_signed ? 0x7fffffffu : 0xffffffffu; }
    if (qf_isinf(a)) {
        *fl |= QF_NV;
        if (is_signed) return sign ? 0x80000000u : 0x7fffffffu;
        return sign ? 0 : 0xffffffffu;
    }
    if (qf_iszero(a)) return 0;
    uint32_t m; int e;
    unpack(a, &m, &e);
    uint64_t mag; int inexact = 0; int big = 0;
    if (e >= 0) {
        if (e > 9) big = 1;             /* >= 2^33 */
        mag = (uint64_t)m << (e > 9 ? 0 : e);
    } else {
        int sh = -e;
        uint64_t ip, rem, half;
        if (sh >= 40) { ip = 0; rem = 1; half = 2; /* < 0.5, nonzero */ }
        else {
            ip = (uint64_t)m >> sh;
            rem = (uint64_t)m & ((1ull << sh) - 1);
            half = 1ull << (sh - 1);
        }
        inexact = rem != 0;
        int inc = 0;
        switch (rm) {
        case RM_NE: inc = rem > half || (rem == half && (ip & 1)); break;
        case RM_TZ: inc = 0; break;
        case RM_DN: inc = sign && rem; break;
        case RM_UP: inc = !sign && rem; break;
        default:    inc = rem >= half; break;   /* MM */
        }
        mag = ip + inc;
    }
    if (is_signed) {
        if (big || (sign ? mag > 0x80000000ull : mag > 0x7fffffffull)) {
            *fl |= QF_NV;
            return sign ? 0x80000000u : 0x7fffffffu;
        }
    } else {
        if (sign ? (mag != 0) : (big || mag > 0xffffffffull)) {
            *fl |= QF_NV;
            return sign ? 0 : 0xffffffffu;
        }
    }
    if (inexact) *fl |= QF_NX;
    return sign ? (uint32_t)(0 - (uint32_t)mag) : (uint32_t)mag;
}

uint32_t qf_to_i32(uint32_t a, int rm, uint8_t *fl) { return to_int(a, rm, fl, 1); }
uint32_t qf_to_u32(uint32_t a, int rm, uint8_t *fl) { return to_int(a, rm, fl, 0); }

uint32_t qf_from_i32(uint32_t v, int rm, uint8_t *fl)
{
    int sign = (int32_t)v < 0;
    uint32_t mag = sign ? 0 - v : v;
    if (!mag) return 0;
    return norm_round_pack(sign, 0, (u128)mag, rm, fl);
}

uint32_t qf_from_u32(uint32_t v, int rm, uint8_t *fl)
{
    if (!v) return 0;
    return norm_round_pack(0, 0, (u128)v, rm, fl);
}

/* ---- compare / min / max / class -------------------------------------- */

/* a < b for non-NaN inputs (sign-magnitude compare, -0 == +0) */
static inline int lt_bits(uint32_t a, uint32_t b)
{
    int sa = a >> 31, sb = b >> 31;
    if (sa != sb) return sa && (uint32_t)((a | b) << 1) != 0;
    return a != b && (sa ^ (a < b));
}

uint32_t qf_eq(uint32_t a, uint32_t b, uint8_t *fl)
{
    if (qf_isnan(a) || qf_isnan(b)) {
        if (qf_issnan(a) || qf_issnan(b)) *fl |= QF_NV;
        return 0;
    }
    return a == b || (uint32_t)((a | b) << 1) == 0;
}

uint32_t qf_lt(uint32_t a, uint32_t b, uint8_t *fl)
{
    if (qf_isnan(a) || qf_isnan(b)) { *fl |= QF_NV; return 0; }
    return lt_bits(a, b);
}

uint32_t qf_le(uint32_t a, uint32_t b, uint8_t *fl)
{
    if (qf_isnan(a) || qf_isnan(b)) { *fl |= QF_NV; return 0; }
    return a == b || (uint32_t)((a | b) << 1) == 0 || lt_bits(a, b);
}

uint32_t qf_min(uint32_t a, uint32_t b, uint8_t *fl)
{
    if (qf_isnan(a) || qf_isnan(b)) return nan_result(a, b, fl);
    if ((uint32_t)((a | b) << 1) == 0) return a | b;         /* -0 wins */
    return lt_bits(b, a) ? b : a;
}

uint32_t qf_max(uint32_t a, uint32_t b, uint8_t *fl)
{
    if (qf_isnan(a) || qf_isnan(b)) return nan_result(a, b, fl);
    if ((uint32_t)((a | b) << 1) == 0) return a & b;         /* +0 wins */
    return lt_bits(a, b) ? b : a;
}

uint32_t qf_class(uint32_t a)
{
    int s = a >> 31;
    uint32_t ex = (a >> 23) & 0xff, fr = a & 0x7fffff;
    if (ex == 0xff) {
        if (fr == 0) return s ? 1u << 0 : 1u << 7;
        return (fr & 0x400000) ? 1u << 9 : 1u << 8;
    }
    if (ex == 0) {
        if (fr == 0) return s ? 1u << 3 : 1u << 4;
        return s ? 1u << 2 : 1u << 5;
    }
    return s ? 1u << 1 : 1u << 6;
}
