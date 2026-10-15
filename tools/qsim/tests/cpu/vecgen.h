//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/* Random binary32 operand generator biased toward hard cases. */
#ifndef VECGEN_H
#define VECGEN_H
#include <stdint.h>

static uint64_t vg_state = 0x9e3779b97f4a7c15ull;
static inline uint64_t vg_next(void)
{
    uint64_t z = (vg_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static inline uint32_t vg_u32(void) { return (uint32_t)vg_next(); }

static const uint32_t vg_special[] = {
    0x00000000, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000,
    0x7f800001, 0xff800001, 0x7fa00000, 0x7fffffff, 0x00000001, 0x80000001,
    0x000fffff, 0x007fffff, 0x807fffff, 0x00800000, 0x80800000, 0x00800001,
    0x7f7fffff, 0xff7fffff, 0x7f7ffffe, 0x3f800000, 0xbf800000, 0x40000000,
    0x3f000000, 0xbf000000, 0x3fc00000, 0x40200000, 0x41200000, 0x4effffff,
    0x4f000000, 0x4f000001, 0xcf000000, 0xcf000001, 0x4f7fffff, 0x4f800000,
    0x3e800000, 0x3ec00000, 0x3f400000, 0x3fa00000, 0x33800000, 0x34000000,
    0x16a09e66, 0x5fffffff, 0x20000000, 0x1fffffff, 0x0e000000, 0x00400000,
};

/* one operand: mixture of specials, random bits, near-integers, narrow
 * exponent bands (so add/sub cancel and fma ties occur) */
static inline uint32_t vg_float(void)
{
    uint32_t r = vg_u32();
    switch (r & 15) {
    case 0: case 1:
        return vg_special[vg_u32() % (sizeof vg_special / sizeof vg_special[0])];
    case 2: case 3: case 4:
        return vg_u32();
    case 5: { /* small integers and halves */
        int32_t v = (int32_t)(vg_u32() % 2001) - 1000;
        float f = (float)v / ((vg_u32() & 1) ? 1.0f : 2.0f);
        uint32_t b; __builtin_memcpy(&b, &f, 4); return b;
    }
    case 6: case 7: { /* exponent near 0 */
        uint32_t e = 127 + (vg_u32() % 9) - 4;
        return (vg_u32() & 0x807fffff) | (e << 23);
    }
    case 8: { /* subnormal / tiny */
        uint32_t e = vg_u32() % 3;
        return (vg_u32() & 0x807fffff) | (e << 23);
    }
    case 9: { /* huge */
        uint32_t e = 254 - vg_u32() % 3;
        return (vg_u32() & 0x807fffff) | (e << 23);
    }
    case 10: { /* few mantissa bits (exact / tie friendly) */
        uint32_t e = 100 + vg_u32() % 60;
        uint32_t m = vg_u32() & 0x7f0000;
        return (vg_u32() & 0x80000000) | (e << 23) | m;
    }
    case 11: { /* around 2^31 / 2^32 for conversions */
        uint32_t e = 157 + vg_u32() % 4;
        uint32_t m = (vg_u32() & 1) ? (vg_u32() & 0x7fffff) : ((vg_u32() & 1) ? 0x7fffff : 0);
        return (vg_u32() & 0x80000000) | (e << 23) | m;
    }
    case 12: { /* around 0.5..2 fractions for rounding */
        uint32_t e = 125 + vg_u32() % 5;
        return (vg_u32() & 0x807fffff) | (e << 23);
    }
    default: {
        uint32_t e = vg_u32() % 256;
        return (vg_u32() & 0x807fffff) | (e << 23);
    }
    }
}

/* second operand correlated with the first (same/close exponent) sometimes */
static inline uint32_t vg_float_near(uint32_t a)
{
    uint32_t r = vg_u32();
    if ((r & 3) == 0) {
        int32_t e = (int32_t)((a >> 23) & 0xff) + (int32_t)(vg_u32() % 5) - 2;
        if (e < 0) e = 0;
        if (e > 254) e = 254;
        return (vg_u32() & 0x807fffff) | ((uint32_t)e << 23);
    }
    if ((r & 3) == 1) return a ^ (vg_u32() & 0x8000000f);
    return vg_float();
}

static inline uint32_t vg_int(void)
{
    uint32_t r = vg_u32();
    switch (r & 7) {
    case 0: return 0;
    case 1: return 0x80000000u ^ (vg_u32() % 3);
    case 2: return 0x7fffffffu - (vg_u32() % 3);
    case 3: return 0xffffffffu - (vg_u32() % 3);
    case 4: return (1u << (vg_u32() % 32)) + (vg_u32() % 3) - 1;
    case 5: return (uint32_t)((int32_t)(vg_u32() % 2001) - 1000);
    default: return vg_u32();
    }
}
#endif
