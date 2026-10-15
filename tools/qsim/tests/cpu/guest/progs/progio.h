//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/* I/O shim so the same program builds as a qsim guest and as a host binary */
#ifndef PROGIO_H
#define PROGIO_H
#include <stdint.h>
#ifdef GUEST
static inline void out_ch(int c) { *(volatile uint32_t *)0x4F000000u = (uint32_t)c; }
#else
#include <stdio.h>
static inline void out_ch(int c) { putchar(c); }
#endif
static inline void out_str(const char *s) { while (*s) out_ch(*s++); }
static inline void out_hex(uint64_t v, int digits)
{
    for (int i = digits - 1; i >= 0; i--) out_ch("0123456789abcdef"[(v >> (4 * i)) & 15]);
}
static inline void out_kv(const char *k, uint64_t v)
{
    out_str(k); out_ch('='); out_hex(v, 16); out_ch('\n');
}
static inline uint64_t mix64(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}
static uint64_t prng_s = 88172645463325252ull;
static inline uint32_t prng(void)
{
    prng_s ^= prng_s << 13; prng_s ^= prng_s >> 7; prng_s ^= prng_s << 17;
    return (uint32_t)(prng_s >> 16);
}
static inline uint64_t fbits(float f) { uint32_t b; __builtin_memcpy(&b, &f, 4); return b; }
static inline uint64_t dbits(double d) { uint64_t b; __builtin_memcpy(&b, &d, 8); return b; }
#endif
