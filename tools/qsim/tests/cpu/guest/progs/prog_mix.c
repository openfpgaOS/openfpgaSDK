//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/* Mixed compiled-C workload: integer, 64-bit libgcc, float (F ext), double
 * (soft-float), recursion, indirect calls.  Output must match the host build. */
#include "progio.h"

static uint32_t crc_tab[256];
static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xedb88320u : c >> 1;
        crc_tab[i] = c;
    }
}
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) c = crc_tab[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static int32_t arr[20000];
static void qsort_i(int32_t *a, int lo, int hi)
{
    while (lo < hi) {
        int32_t p = a[(lo + hi) >> 1];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) { int32_t t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { qsort_i(a, lo, j); lo = i; } else { qsort_i(a, i, hi); hi = j; }
    }
}

static uint8_t buf[65536];

typedef int32_t (*binop)(int32_t, int32_t);
static int32_t f_add(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static int32_t f_mul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static int32_t f_div(int32_t a, int32_t b) { return (b && !(a == INT32_MIN && b == -1)) ? a / b : 0; }
static int32_t f_rem(int32_t a, int32_t b) { return (b && !(a == INT32_MIN && b == -1)) ? a % b : 0; }
static int32_t f_shr(int32_t a, int32_t b) { return a >> (b & 31); }

static uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int main(void)
{
    uint64_t h = 0;
    crc_init();
    for (uint32_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)prng();
    out_kv("crc", crc32(buf, sizeof buf));

    for (int i = 0; i < 20000; i++) arr[i] = (int32_t)prng();
    qsort_i(arr, 0, 19999);
    for (int i = 0; i < 20000; i++) h = mix64(h, (uint32_t)arr[i]);
    out_kv("sort", h);

    h = 0;
    for (int i = 0; i < 20000; i++) {
        uint64_t a = (uint64_t)prng() << 32;
        a |= prng();
        uint64_t b = (uint64_t)prng() << 16;
        b |= prng();
        int64_t sa = (int64_t)a, sb = (int64_t)(b | 1);
        if (i % 7 == 0) b >>= 30;
        h = mix64(h, a * b);
        h = mix64(h, b ? a / b : 0);
        h = mix64(h, b ? a % b : 0);
        h = mix64(h, (uint64_t)(sa / sb));
        h = mix64(h, (uint64_t)(sa % sb));
        h = mix64(h, a >> (i & 63));
        h = mix64(h, (uint64_t)(sa >> (i & 63)));
    }
    out_kv("i64", h);

    binop ops[5] = {f_add, f_mul, f_div, f_rem, f_shr};
    h = 0;
    for (int i = 0; i < 50000; i++) {
        int32_t x = (int32_t)prng();          /* argument evaluation order is unspecified: sequence it */
        int32_t y = (int32_t)(prng() >> (i & 15));
        h = mix64(h, (uint32_t)ops[i % 5](x, y));
    }
    out_kv("ind", h);
    out_kv("fib", fib(24));

    /* float: n-body (F extension) */
    float px[5], py[5], pz[5], vx[5], vy[5], vz[5], m[5];
    for (int i = 0; i < 5; i++) {
        px[i] = (float)(int32_t)(prng() % 2000) * 0.01f - 10.0f; py[i] = (float)(prng() % 1000) * 0.013f;
        pz[i] = (float)(prng() % 777) * 0.02f; vx[i] = vy[i] = vz[i] = 0.0f; m[i] = 1.0f + (float)(prng() % 10);
    }
    for (int step = 0; step < 3000; step++) {
        for (int i = 0; i < 5; i++)
            for (int j = i + 1; j < 5; j++) {
                float dx = px[i] - px[j], dy = py[i] - py[j], dz = pz[i] - pz[j];
                float d2 = dx * dx + dy * dy + dz * dz + 0.01f;
                float d = __builtin_sqrtf(d2);
                float mag = 0.001f / (d2 * d);
                vx[i] -= dx * m[j] * mag; vy[i] -= dy * m[j] * mag; vz[i] -= dz * m[j] * mag;
                vx[j] += dx * m[i] * mag; vy[j] += dy * m[i] * mag; vz[j] += dz * m[i] * mag;
            }
        for (int i = 0; i < 5; i++) { px[i] += vx[i]; py[i] += vy[i]; pz[i] += vz[i]; }
    }
    h = 0;
    for (int i = 0; i < 5; i++) { h = mix64(h, fbits(px[i])); h = mix64(h, fbits(vy[i])); h = mix64(h, fbits(pz[i])); }
    out_kv("nbody", h);

    /* float conversions and mandelbrot */
    h = 0;
    for (int y = 0; y < 120; y++)
        for (int x = 0; x < 160; x++) {
            float cr = (float)x / 40.0f - 2.5f, ci = (float)y / 40.0f - 1.5f, zr = 0, zi = 0;
            int it = 0;
            while (it < 64 && zr * zr + zi * zi < 4.0f) { float t = zr * zr - zi * zi + cr; zi = 2.0f * zr * zi + ci; zr = t; it++; }
            h = mix64(h, (uint64_t)it);
            h = mix64(h, (uint64_t)(int32_t)(zr * 1000.0f));
            h = mix64(h, (uint64_t)(uint32_t)(zi * zi * 100.0f));
        }
    out_kv("mandel", h);

    /* double (soft-float on the guest) */
    double pi = 0, s = 1, sum = 0;
    for (int k = 0; k < 20000; k++) { pi += s / (2 * k + 1); s = -s; }
    for (int k = 1; k < 5000; k++) sum += 1.0 / ((double)k * (double)k) + (double)(float)(1.0 / k);
    double t = 0.7, sinv = 0, term = t;
    for (int k = 1; k < 20; k++) { sinv += term; term *= -t * t / ((2 * k) * (2 * k + 1)); }
    out_kv("dbl", mix64(mix64(dbits(pi), dbits(sum)), dbits(sinv)));

    /* float <-> int edge conversions (in range) */
    h = 0;
    for (int i = 0; i < 100000; i++) {
        int32_t v = (int32_t)prng();
        float f = (float)v;
        double d = (double)v * 0.5;
        h = mix64(h, fbits(f));
        h = mix64(h, (uint64_t)(int32_t)(f * 0.25f));
        h = mix64(h, (uint64_t)(int32_t)d);
        h = mix64(h, fbits((float)d));
        h = mix64(h, (uint64_t)(uint32_t)((float)((uint32_t)v >> 1)));
    }
    out_kv("conv", h);
    return 0;
}
