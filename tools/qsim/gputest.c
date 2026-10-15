//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * gputest.c -- randomized GPU command-stream generator + C-model replayer
 * for RTL equivalence testing of gpu.c against the os25 gpu_core.v.
 *
 *   gputest gen  SEED MODE OUT.vec     generate a vector (MODE: normal|stale|quake|garbage)
 *   gputest run  IN.vec OUT.bin        replay through gpu.c (same output format as RTL harness)
 *   gputest diff A.bin B.bin IN.vec    explain the first differences
 *
 * Address plan (26-bit GPU space, 64 MB, see gpuvec.h):
 *   0x0200000..0x21FFFFF  texture zone (RAND filled). Every texture address
 *                         field lies in centre +-4 MB and every command's
 *                         texel offset bound is <= 12 MB, so texel reads
 *                         never leave the zone.
 *   0x3000000..0x33FFFFF  framebuffer zone (RAND filled)
 *   0x3800000..0x3BFFFFF  z zone (RAND filled)
 *   0x3FC0000..0x3FFFFFF  default palookup slots (RAND filled)
 * All framebuffer/z address fields are drawn from zone centre +-500 KB with
 * bounded steps/counts, so any stale-field combination stays in its zone.
 * Texture/palookup reads never see GPU-written memory (the RTL texture cache
 * can only be compared against an architectural model under that rule).
 */
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gpu.h"
#include "gpuvec.h"

#define TEXZ_LO   0x0200000u
#define TEXZ_HI   0x2200000u
#define TEXZ_MID  0x1200000u
#define FBZ_LO    0x3000000u
#define FBZ_HI    0x3400000u
#define FBZ_MID   0x3200000u
#define ZZ_LO     0x3800000u
#define ZZ_HI     0x3C00000u
#define ZZ_MID    0x3A00000u
#define PAL_LO    0x3FC0000u

/* ------------------------------------------------------------------ */
static uint64_t rs;
static uint64_t r64(void)
{
    rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27;
    return rs * 2685821657736338717ull;
}
static uint32_t r32(void) { return (uint32_t)(r64() >> 32); }
static int rint_(int lo, int hi) { return lo + (int)(r64() % (uint64_t)(hi - lo + 1)); }
static int pct(int p) { return (int)(r64() % 100) < p; }
static double rf(double lo, double hi) { return lo + (hi - lo) * ((double)(r64() >> 11) / 9007199254740992.0); }

/* ------------------------------------------------------------------ */
typedef struct { uint32_t w[4096]; uint32_t n; } batch_t;
static FILE *vf;
static batch_t B;
static int mode_colormap = 1;
static int g_mode;
enum { M_NORMAL, M_STALE, M_QUAKE, M_GARBAGE, M_OVERLAP };

static void v32(uint32_t x) { fwrite(&x, 4, 1, vf); }
static void v_flush_batch(void)
{
    if (!B.n) return;
    v32(GV_TAG_CMDS); v32(B.n);
    fwrite(B.w, 4, B.n, vf);
    B.n = 0;
}
static void cmd_begin(uint32_t words_total)
{
    if (B.n + words_total > 4095) v_flush_batch();
}
static void put(uint32_t w) { B.w[B.n++] = w; }

/* ------------------------------------------------------------------ */
static uint32_t fb_addr_c(void)
{
    if (g_mode == M_OVERLAP) return FBZ_MID + (uint32_t)rint_(-32000, 32000);
    return FBZ_MID + (uint32_t)rint_(-500000, 500000);
}
static uint32_t z_addr_c(void)
{
    if (g_mode == M_OVERLAP) return (ZZ_MID + (uint32_t)rint_(-32000, 32000)) & ~(pct(95) ? 1u : 0u);
    return (ZZ_MID + (uint32_t)rint_(-500000, 500000)) & ~(pct(90) ? 1u : 0u);
}
static uint32_t hot_tex[4];
static int g_mode_is_stale;
static uint32_t tex_addr_c(void)
{
    if (g_mode_is_stale && pct(90))
        return hot_tex[rint_(0, 3)] + (uint32_t)rint_(0, 4096);
    return TEXZ_MID + (uint32_t)rint_(-4000000, 4000000);
}
static int32_t small_step(void)
{
    static const int32_t c[] = { 1, -1, 320, -320, 2, 640, -640, 4, 0 };
    if (pct(70)) return c[rint_(0, 8)];
    return rint_(-640, 640);
}

/* texel offset bound for a (width, masks, clamp) combination */
static uint64_t tex_bound(uint32_t width, uint16_t wm_wire, uint16_t hm_wire,
                          int32_t cmin_t, int32_t cmax_t, int clamp_t,
                          int32_t cmin_s, int32_t cmax_s, int clamp_s)
{
    uint16_t wm = wm_wire ? wm_wire : 0xFFFF, hm = hm_wire ? hm_wire : 0xFFFF;
    uint64_t w = (width & 0xFFFF) ? (width & 0xFFFF) : 1;
    uint64_t tmax, smax;
    if (hm != 0xFFFF) tmax = hm;
    else if (clamp_t) {
        int32_t a = (int16_t)((uint32_t)cmin_t >> 16), b = (int16_t)((uint32_t)cmax_t >> 16);
        tmax = (uint64_t)(llabs(a) > llabs(b) ? llabs(a) : llabs(b));
    } else tmax = 32768;
    if (wm != 0xFFFF) smax = wm;
    else if (clamp_s) {
        int32_t a = (int16_t)((uint32_t)cmin_s >> 16), b = (int16_t)((uint32_t)cmax_s >> 16);
        smax = (uint64_t)(llabs(a) > llabs(b) ? llabs(a) : llabs(b));
    } else smax = 32768;
    return tmax * w + smax;
}

static void pick_tex_params(uint32_t *width, uint16_t *wm, uint16_t *hm, int no_clamp_bound)
{
    if (g_mode_is_stale && pct(80)) {
        static const uint32_t w2[] = { 16, 32, 64, 128 };
        static const uint16_t m2[] = { 0x0F, 0x1F, 0x3F, 0x7F };
        *width = w2[rint_(0, 3)]; *wm = m2[rint_(0, 3)]; *hm = m2[rint_(0, 3)];
        return;
    }
    static const uint32_t widths[] = { 1, 2, 3, 8, 15, 16, 32, 64, 100, 128, 256, 320, 1024, 4096, 0 };
    static const uint16_t masks[] = { 0, 0, 0, 0xFF, 0x3F, 0x7F, 0xFFF, 0x1F, 0x3FF, 0xFFFF };
    for (;;) {
        *width = widths[rint_(0, 14)];
        *wm = pct(15) ? (uint16_t)r32() : masks[rint_(0, 9)];
        *hm = pct(15) ? (uint16_t)r32() : masks[rint_(0, 9)];
        if (tex_bound(*width, *wm, *hm, 0, 0, 0, 0, 0, 0) <= 12000000u || !no_clamp_bound)
            return;
    }
}

/* ------------------------------------------------------------------ */
static uint8_t rand_flags(void)
{
    uint8_t f = 0;
    if (mode_colormap && pct(55)) f |= 0x01;
    if (pct(35)) f |= 0x04;
    if (pct(20)) f |= 0x20;
    if (pct(15)) f |= 0x40;
    if (pct(5)) f |= 0x02;
    if (pct(5)) f |= 0x08;
    if (pct(5)) f |= 0x10;
    if (pct(5)) f |= 0x80;
    return f;
}

static void gen_st(int32_t *s, int32_t *t, int32_t *ss, int32_t *ts)
{
    if (g_mode == M_GARBAGE || pct(25)) {
        *s = (int32_t)r32(); *t = (int32_t)r32();
        *ss = pct(50) ? (int32_t)r32() : rint_(-0x40000, 0x40000);
        *ts = pct(50) ? (int32_t)r32() : rint_(-0x40000, 0x40000);
    } else {
        *s = rint_(-0x100000, 0x1000000); *t = rint_(-0x100000, 0x1000000);
        *ss = pct(40) ? 0x10000 : rint_(-0x30000, 0x30000);
        *ts = pct(40) ? 0 : rint_(-0x30000, 0x30000);
    }
}

/* compact 0x48 (lanes 7 words) or 0x4C (5 words) */
static void gen_compact(int column)
{
    int lanes = rint_(1, 4);
    int lane_words = column ? 5 : 7;
    uint32_t nib = pct(88) ? (uint32_t)lanes : (uint32_t)rint_(0, 15);
    uint8_t flags = rand_flags();
    uint32_t width; uint16_t wm, hm;
    pick_tex_params(&width, &wm, &hm, 1);
    int32_t step = small_step();
    uint32_t n = 4 + (uint32_t)(lane_words * lanes);
    if (pct(6)) {
        int lo = column ? 9 : 11, hi = column ? 24 : 32;
        n = (uint32_t)rint_(lo, hi);
        if (pct(15)) n = (uint32_t)rint_(column ? 1 : 1, column ? 30 : 10);
    }
    cmd_begin(1 + n);
    put(((uint32_t)(column ? 0x4C : 0x48) << 24) | n);
    uint32_t words[40];
    uint32_t k = 0;
    words[k++] = (nib << 28) | ((uint32_t)flags << 20) | (pct(10) ? (r32() & 0xFFFFF) : 0);
    words[k++] = width | (pct(10) ? (r32() & 0xFFFF0000u) : 0);
    words[k++] = ((uint32_t)hm << 16) | wm;
    words[k++] = (uint32_t)step | (pct(5) ? 0xFC000000u : 0);
    for (int l = 0; l < 4; l++) {
        int32_t s, t, ss, ts;
        gen_st(&s, &t, &ss, &ts);
        uint32_t count = pct(5) ? 0 : (uint32_t)rint_(1, 700);
        if (pct(30)) count = (uint32_t)rint_(1, 20);
        words[k++] = fb_addr_c();
        words[k++] = tex_addr_c();
        words[k++] = ((uint32_t)rint_(0, 15) << 28) | ((uint32_t)rint_(0, 63) << 16) | count
                   | (pct(10) ? ((r32() & 0x3F) << 22) : 0);
        if (!column) words[k++] = (uint32_t)s;
        words[k++] = (uint32_t)t;
        if (!column) words[k++] = (uint32_t)ss;
        words[k++] = (uint32_t)ts;
    }
    for (uint32_t i = 0; i < n; i++)
        put(i < k ? words[i] : (uint32_t)(r32() % 100));
}

/* long-form 0x48 */
static void gen_long(void)
{
    int nrec = pct(80) ? rint_(1, 24) : rint_(1, 512);
    if (pct(10)) nrec = 4 * rint_(1, 6);
    uint8_t flags = rand_flags();
    int attr_mode;
    int r = rint_(0, 99);
    if (g_mode == M_QUAKE) attr_mode = r < 20 ? 0 : r < 55 ? 1 : 3;
    else attr_mode = r < 22 ? 0 : r < 52 ? 1 : r < 90 ? 3 : r < 95 ? 2 : rint_(4, 15);
    int axis = pct(80) ? 0 : pct(95) ? 1 : rint_(0, 15);
    int zr = rint_(0, 99);
    int z_mode = zr < 40 ? 0 : zr < 60 ? 1 : zr < 75 ? 2 : zr < 95 ? 3 : rint_(4, 15);
    int fmt = pct(97) ? 0 : rint_(1, 15);
    if (g_mode == M_QUAKE) {
        fmt = 0; axis = pct(90) ? 0 : 1;
        if (z_mode > 3) z_mode = 3;
        if (z_mode == 1) flags &= (uint8_t)~0x04;
        flags &= (uint8_t)~0x40;
    }
    uint32_t control = flags | ((uint32_t)rint_(0, 15) << 8) | ((uint32_t)attr_mode << 12)
                     | ((uint32_t)axis << 16) | ((uint32_t)fmt << 20) | ((uint32_t)z_mode << 24);
    if (pct(20)) control |= (r32() & 3u) << 28;   /* mirror s/t */
    if (pct(4)) control |= (r32() & 3u) << 30;
    if (pct(3)) control |= (r32() & 0x3u) << 26;

    /* clamps */
    int32_t cmin[3] = {0, 0, 0}, cmax[3] = {0, 0, 0};
    int clr = rint_(0, 99);
    for (int i = 0; i < 3; i++) {
        if (clr < 30) { cmin[i] = 0; cmax[i] = 0; }
        else if (clr < 80) {
            int32_t a = rint_(-0x400000, 0x2000000), b = rint_(-0x400000, 0x2000000);
            cmin[i] = a < b ? a : b; cmax[i] = a < b ? b : a;
            if (pct(20)) cmin[i] = 0;
        } else { cmin[i] = (int32_t)r32(); cmax[i] = (int32_t)r32(); if (pct(50)) cmin[i] = 0; }
    }
    int clamp_s = cmin[0] || cmax[0], clamp_t = cmin[1] || cmax[1];
    uint32_t width; uint16_t wm, hm;
    for (int tries = 0;; tries++) {
        pick_tex_params(&width, &wm, &hm, 0);
        if (tex_bound(width, wm, hm, cmin[1], cmax[1], clamp_t, cmin[0], cmax[0], clamp_s) <= 12000000u)
            break;
        if (tries > 50) { wm = hm = 0xFF; width = 64; break; }
    }

    int32_t org[3], du[3], dv[3];
    uint32_t shift_word = 0;
    int quakeish = (g_mode == M_QUAKE) || (g_mode != M_GARBAGE && pct(70));
    if (!quakeish) {
        for (int i = 0; i < 3; i++) { org[i] = (int32_t)r32(); du[i] = pct(50) ? (int32_t)r32() : rint_(-0x20000, 0x20000); dv[i] = (int32_t)r32(); }
        if (attr_mode == 3) shift_word = pct(80) ? (uint32_t)rint_(0, 31) : r32();
        else shift_word = pct(85) ? 0 : (pct(50) ? (uint32_t)rint_(1, 31) : r32());
    } else if (attr_mode == 0) {
        org[0] = rint_(-0x100000, 0x2000000); org[1] = rint_(-0x100000, 0x2000000);
        du[0] = rint_(-0x30000, 0x30000); dv[0] = rint_(-0x30000, 0x30000);
        du[1] = rint_(-0x30000, 0x30000); dv[1] = rint_(-0x30000, 0x30000);
        org[2] = (int32_t)r32(); du[2] = rint_(-100, 100); dv[2] = rint_(-100, 100);
    } else {
        /* screen-space planes of a textured plane: zi = 1/z linear, s*zi, t*zi */
        double z0 = rf(6.0, 2000.0);
        double zi0 = 1.0 / z0;
        double gzu = rf(-1.0, 1.0) * zi0 / rf(40.0, 400.0);
        double gzv = rf(-1.0, 1.0) * zi0 / rf(40.0, 400.0);
        if (pct(8)) gzu = -zi0 / rf(4.0, 60.0);       /* singular: zi crosses 0 */
        if (pct(10)) { gzu = 0; if (pct(50)) gzv = 0; } /* constant-z */
        double s0 = rf(-64, 512), t0 = rf(-64, 512);
        double dsu = rf(-3, 3), dsv = rf(-3, 3), dtu = rf(-3, 3), dtv = rf(-3, 3);
        double fo[3], fu[3], fv[3];
        fo[2] = zi0; fu[2] = gzu; fv[2] = gzv;
        fo[0] = s0 * zi0; fu[0] = dsu * zi0 + s0 * gzu; fv[0] = dsv * zi0 + s0 * gzv;
        fo[1] = t0 * zi0; fu[1] = dtu * zi0 + t0 * gzu; fv[1] = dtv * zi0 + t0 * gzv;
        if (attr_mode == 1) {
            for (int i = 0; i < 3; i++) {
                org[i] = (int32_t)llround(fo[i] * 65536.0 * 65536.0 / 65536.0 * 1.0);
                du[i]  = (int32_t)llround(fu[i] * 65536.0);
                dv[i]  = (int32_t)llround(fv[i] * 65536.0);
                org[i] = (int32_t)llround(fo[i] * 65536.0);
            }
            if (pct(50)) { for (int i = 0; i < 3; i++) { org[i] *= 16; du[i] *= 16; dv[i] *= 16; } }
            shift_word = pct(95) ? 0 : (uint32_t)rint_(1, 3);
        } else {
            /* D_PARAM_FLOAT_SETUP: scale by 2^(29-sh), sh = floor(log2(fmax)) */
            double fmax = 0;
            for (int i = 0; i < 3; i++) {
                double o = fo[i], a = fu[i] * 255, b = fv[i] * 255;
                double cands[6] = { o, fu[i], fv[i], o + a, o + b, o + a + b };
                for (int j = 0; j < 6; j++) if (fabs(cands[j]) > fmax) fmax = fabs(cands[j]);
            }
            int e; frexp(fmax, &e);
            int sh = e - 1;
            if (sh < 0) sh = 0;
            if (sh > 31) sh = 31;
            if (pct(15)) sh = rint_(0, 31);
            double scale = ldexp(1.0, 29 - sh);
            for (int i = 0; i < 3; i++) {
                double a = fo[i] * scale, b = fu[i] * scale, c = fv[i] * scale;
                org[i] = (int32_t)(a > 2147483647.0 ? 2147483647 : a < -2147483648.0 ? INT32_MIN : (int64_t)a);
                du[i] = (int32_t)(b > 2147483647.0 ? 2147483647 : b < -2147483648.0 ? INT32_MIN : (int64_t)b);
                dv[i] = (int32_t)(c > 2147483647.0 ? 2147483647 : c < -2147483648.0 ? INT32_MIN : (int64_t)c);
            }
            shift_word = (uint32_t)sh;
            if (pct(3)) shift_word |= r32() << 5;
        }
    }
    int32_t lorg = pct(50) ? rint_(0, 63) << 16 : (int32_t)r32();
    int32_t ldu = pct(60) ? rint_(-0x8000, 0x8000) : (int32_t)r32();
    int32_t ldv = pct(60) ? rint_(-0x8000, 0x8000) : (int32_t)r32();

    uint32_t recw = (uint32_t)((nrec + 1) / 2) * 3;
    uint32_t n = 31 + recw;
    int extra = 0;
    if (pct(6)) { extra = rint_(-(int)recw + 2, 12); if ((int)n + extra < 33) extra = 33 - (int)n; }
    n = (uint32_t)((int)n + extra);
    uint32_t count_word = (uint32_t)nrec;
    if (pct(6)) count_word = (uint32_t)rint_(0, nrec + 10);
    if (pct(3)) count_word |= r32() << 16;

    cmd_begin(1 + n);
    put((0x48u << 24) | n | (pct(3) ? ((r32() & 0x7FF) << 13) : 0));
    put(fb_addr_c());
    put((uint32_t)small_step() | (pct(3) ? 0xFC000000u : 0));
    put((uint32_t)small_step());
    put(tex_addr_c());
    put(width | (pct(10) ? (r32() & 0xFFFF0000u) : 0));
    put(wm | (pct(10) ? (r32() & 0xFFFF0000u) : 0));
    put(hm | (pct(10) ? (r32() & 0xFFFF0000u) : 0));
    put(control);
    for (int i = 0; i < 3; i++) { put((uint32_t)org[i]); put((uint32_t)du[i]); put((uint32_t)dv[i]); }
    put((uint32_t)lorg); put((uint32_t)ldu); put((uint32_t)ldv);
    for (int i = 0; i < 3; i++) { put((uint32_t)cmin[i]); put((uint32_t)cmax[i]); }
    put(z_addr_c());
    put((uint32_t)(pct(80) ? 640 : small_step()));
    put((uint32_t)(pct(85) ? 2 : small_step()));
    put(count_word);
    put(shift_word);
    uint32_t emitted = 31;
    while (emitted < n) {
        uint32_t u = (uint32_t)(pct(95) ? rint_(0, 255) : rint_(-8, -1)) & 0xFFFF;
        uint32_t v = (uint32_t)(pct(95) ? rint_(0, 255) : rint_(-8, -1)) & 0xFFFF;
        uint32_t c = pct(5) ? 0 : (uint32_t)(pct(40) ? rint_(1, 16) : rint_(1, 400));
        uint32_t u2 = (uint32_t)rint_(0, 255), v2 = (uint32_t)rint_(0, 255);
        uint32_t c2 = pct(5) ? 0 : (uint32_t)(pct(40) ? rint_(1, 16) : rint_(1, 400));
        uint32_t pw[3] = { (v << 16) | u, (u2 << 16) | c, (c2 << 16) | v2 };
        for (int i = 0; i < 3 && emitted < n; i++, emitted++) put(pw[i]);
    }
}

static void gen_clear(void)
{
    uint32_t n = pct(95) ? 3 : (uint32_t)rint_(0, 5);
    int zone_z = pct(25);
    uint32_t addr = zone_z ? z_addr_c() : fb_addr_c();
    uint32_t w = (uint32_t)(pct(10) ? rint_(0, 3) : rint_(1, 640));
    uint32_t h = (uint32_t)(pct(10) ? rint_(0, 2) : rint_(1, 200));
    uint32_t stride = pct(30) ? 0 : (uint32_t)rint_(1, 800);
    if (n < 3) h = (uint32_t)rint_(0, 3);
    cmd_begin(1 + n);
    put((0x11u << 24) | n);
    uint32_t pw[5] = { addr, (w << 16) | h, (stride << 16) | (r32() & 0xFFFF), r32(), r32() };
    for (uint32_t i = 0; i < n; i++) put(pw[i]);
}

static uint32_t token_ctr = 100;
static void gen_misc(void)
{
    int r = rint_(0, 99);
    if (r < 20) {                                  /* fence */
        uint32_t n = pct(95) ? 1 : (uint32_t)rint_(0, 3);
        cmd_begin(1 + n); put((0x02u << 24) | n);
        for (uint32_t i = 0; i < n; i++) put(token_ctr++);
    } else if (r < 30) {                           /* flip */
        uint32_t n = pct(95) ? 2 : (uint32_t)rint_(0, 4);
        cmd_begin(1 + n); put((0x42u << 24) | n);
        for (uint32_t i = 0; i < n; i++) put(i == 0 ? r32() : token_ctr++);
    } else if (r < 45) {                           /* set fb */
        uint32_t n = pct(90) ? 2 : (uint32_t)rint_(0, 4);
        cmd_begin(1 + n); put((0x23u << 24) | n);
        for (uint32_t i = 0; i < n; i++) put(i == 1 ? (uint32_t)rint_(1, 800) | (pct(10) ? r32() << 16 : 0) : r32());
    } else if (r < 55) {                           /* set texture */
        cmd_begin(3); put((0x20u << 24) | 2); put(tex_addr_c()); put(r32());
    } else if (r < 65) {                           /* 0x4A sticky state (writes shared staging) */
        uint32_t n = pct(80) ? (pct(50) ? 16 : 17) : (uint32_t)rint_(10, 20);
        cmd_begin(1 + n); put((0x4Au << 24) | n);
        uint32_t width; uint16_t wm, hm;
        pick_tex_params(&width, &wm, &hm, 1);
        uint32_t pw[20] = { fb_addr_c(), (uint32_t)small_step(), (uint32_t)small_step(), tex_addr_c(),
                            width, ((uint32_t)hm << 16) | wm, r32() & 0xFF0F3FFFu,
                            0, 0, 0, 0, z_addr_c(), 640, 2, r32(), r32(), r32(), r32(), r32(), r32() };
        for (uint32_t i = 0; i < n; i++) put(pw[i]);
    } else if (r < 75) {                           /* disabled triangle opcodes */
        static const uint8_t ops[] = { 0x49, 0x4B, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57 };
        static const uint8_t sizes[] = { 36, 14, 16, 17, 18, 26, 16, 18, 7, 1, 6, 8, 9 };
        int i = rint_(0, 12);
        uint32_t n = pct(80) ? sizes[i] : (uint32_t)rint_(0, 40);
        cmd_begin(1 + n); put(((uint32_t)ops[i] << 24) | n);
        for (uint32_t k = 0; k < n; k++) put(r32());
    } else if (r < 85) {                           /* 0x58 continuation (drained on os25) */
        uint32_t n = (uint32_t)rint_(0, 40);
        cmd_begin(1 + n); put((0x58u << 24) | n);
        for (uint32_t k = 0; k < n; k++) put(r32());
    } else {                                       /* unknown opcode */
        uint8_t op = (uint8_t)r32();
        static const uint8_t known[] = { 0x02, 0x11, 0x20, 0x23, 0x42, 0x48, 0x4A, 0x4C };
        for (unsigned k = 0; k < sizeof(known); k++) if (op == known[k]) op = 0x7F;
        uint32_t n = (uint32_t)rint_(0, 60);
        cmd_begin(1 + n); put(((uint32_t)op << 24) | n);
        for (uint32_t k = 0; k < n; k++) put(r32());
    }
}

static void v_rand(uint32_t addr, uint32_t len, uint32_t seed)
{
    v32(GV_TAG_RAND); v32(addr); v32(len); v32(seed | 1);
}

static int gen_vector(uint64_t seed, int mode, const char *path)
{
    vf = fopen(path, "wb");
    if (!vf) { perror(path); return 1; }
    rs = seed * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    for (int i = 0; i < 8; i++) r64();
    g_mode = mode;
    mode_colormap = (mode != M_STALE);
    g_mode_is_stale = (mode == M_STALE);
    for (int i = 0; i < 4; i++) hot_tex[i] = TEXZ_MID + (uint32_t)rint_(-3000000, 3000000);
    B.n = 0;

    v_rand(TEXZ_LO, TEXZ_HI - TEXZ_LO, r32());
    /* sprinkle 0xFF (transparent) bytes: overwrite random chunks */
    for (int i = 0; i < 64; i++) {
        uint32_t a = TEXZ_MID + (uint32_t)rint_(-15000000, 15000000);
        uint32_t len = (uint32_t)rint_(16, 2048);
        v32(GV_TAG_MEM); v32(a); v32(len);
        for (uint32_t k = 0; k < ((len + 3) & ~3u); k++) { uint8_t b = pct(60) ? 0xFF : (uint8_t)r32(); fwrite(&b, 1, 1, vf); }
    }
    v_rand(FBZ_LO, FBZ_HI - FBZ_LO, r32());
    v_rand(ZZ_LO, ZZ_HI - ZZ_LO, r32());
    v_rand(PAL_LO, 0x4000000u - PAL_LO, r32());
    v_rand(0x2400000u, 0x0400000u, r32());          /* alternate palookup area */
    if (mode != M_QUAKE || pct(50)) {
        v32(GV_TAG_TLUT);
        for (int i = 0; i < 32768; i++) { uint8_t b = (uint8_t)r32(); fwrite(&b, 1, 1, vf); }
    }

    int nbatches = rint_(2, mode == M_QUAKE ? 6 : 10);
    for (int bi = 0; bi < nbatches; bi++) {
        if (pct(30)) {
            v_flush_batch();
            uint32_t pal = pct(50) ? PAL_LO : (0x2400000u + ((uint32_t)rint_(0, 200) << 14) + (pct(20) ? r32() & 0x3FFF : 0));
            if (pct(15)) pal = 0x3FF8000u + ((uint32_t)rint_(0, 1) << 14);   /* page + cmap_id wraps 12 bits -> low memory */
            v32(GV_TAG_PALB); v32(pal);
        }
        int ncmd = rint_(1, 60);
        for (int c = 0; c < ncmd; c++) {
            int r = rint_(0, 99);
            if (mode == M_QUAKE) {
                if (r < 35) gen_long();
                else if (r < 70) gen_compact(0);
                else if (r < 80) gen_compact(1);
                else if (r < 90) gen_clear();
                else gen_misc();
            } else {
                if (r < 35) gen_long();
                else if (r < 55) gen_compact(0);
                else if (r < 65) gen_compact(1);
                else if (r < 78) gen_clear();
                else gen_misc();
            }
            if (pct(8)) v_flush_batch();           /* split batches at command boundaries */
        }
        v_flush_batch();
        if (mode == M_STALE) {
            /* mutate cached texture lines, flush only sometimes */
            for (int k = 0; k < 20; k++)
                v_rand(TEXZ_MID + (uint32_t)rint_(-12000000, 12000000), (uint32_t)rint_(1, 65536), r32());
            for (int k = 0; k < 12; k++)
                v_rand(hot_tex[rint_(0, 3)] + (uint32_t)rint_(-8192, 70000), (uint32_t)rint_(1, 8192), r32());
            if (pct(40)) v32(GV_TAG_FLSH);
        } else if (pct(20)) {
            v32(GV_TAG_FLSH);
        }
        if (pct(30)) { v32(GV_TAG_FENC); }
    }
    v32(GV_TAG_DUMP); v32(FBZ_LO); v32(FBZ_HI - FBZ_LO);
    v32(GV_TAG_DUMP); v32(ZZ_LO); v32(ZZ_HI - ZZ_LO);
    v32(GV_TAG_FENC);
    v32(GV_TAG_HASH);
    v32(GV_TAG_END);
    fclose(vf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* C-model replayer                                                     */
/* ------------------------------------------------------------------ */
static int run_vector(const char *vec, const char *outp, int stats)
{
    FILE *f = fopen(vec, "rb");
    if (!f) { perror(vec); return 2; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)len + 8);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { fclose(f); return 2; }
    fclose(f);
    FILE *out = fopen(outp, "wb");
    if (!out) { perror(outp); return 2; }

    uint8_t *mem = (uint8_t *)calloc(1, GV_SDRAM_BYTES);
    gpu_mem_if_t mif = { 0 };
    mif.base = mem;
    mif.mask = GV_SDRAM_MASK;
    gpu_config_t cfg;
    gpu_default_config(&cfg);
    if (getenv("GPU_NO_TEX_CACHE")) cfg.tex_cache = 0;
    gpu_t *g = gpu_create(&cfg, &mif);
#ifdef GPU_TRACE
    {
        extern FILE *gpu_trace_file;
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.trace", outp);
        gpu_trace_file = fopen(tp, "w");
    }
#endif
    gpu_mmio_write(g, 0x00, 4);
    gpu_mmio_write(g, 0x00, 1);
    gpu_mmio_write(g, 0x30, GV_DEFAULT_PALOOKUP_BASE);
    gpu_mmio_write(g, 0x00, 16);
    gpu_mmio_write(g, 0x28, 1);

    size_t pos = 0;
    uint64_t exec_ns = 0;
#define RD32() (pos += 4, (uint32_t)buf[pos-4] | ((uint32_t)buf[pos-3] << 8) | ((uint32_t)buf[pos-2] << 16) | ((uint32_t)buf[pos-1] << 24))
    int rc = 0;
    while (pos + 4 <= (size_t)len) {
        uint32_t tag = RD32();
        if (tag == GV_TAG_MEM) {
            uint32_t a = RD32(), l = RD32();
            for (uint32_t i = 0; i < l; i++) mem[(a + i) & GV_SDRAM_MASK] = buf[pos + i];
            pos += (l + 3) & ~3u;
        } else if (tag == GV_TAG_TLUT) {
            gpu_mmio_write(g, 0x20, 0);
            for (uint32_t i = 0; i < 32768; i += 4) {
                uint32_t w = (uint32_t)buf[pos+i] | ((uint32_t)buf[pos+i+1] << 8)
                           | ((uint32_t)buf[pos+i+2] << 16) | ((uint32_t)buf[pos+i+3] << 24);
                gpu_mmio_write(g, 0x24, w);
            }
            pos += 32768;
        } else if (tag == GV_TAG_PALB) {
            gpu_mmio_write(g, 0x30, RD32());
        } else if (tag == GV_TAG_FLSH) {
            gpu_mmio_write(g, 0x28, 1);
        } else if (tag == GV_TAG_CMDS) {
            uint32_t n = RD32();
            for (uint32_t i = 0; i < n; i++) gpu_mmio_write(g, 0x04, RD32());
            if ((gpu_mmio_read(g, 0x14) & 0x300) != 0x100) { fprintf(stderr, "model ring overflow\n"); rc = 3; break; }
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            gpu_mmio_write(g, 0x00, 8);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            exec_ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
        } else if (tag == GV_TAG_DUMP) {
            uint32_t a = RD32(), l = RD32();
            for (uint32_t i = 0; i < l; i++) fputc(mem[(a + i) & GV_SDRAM_MASK], out);
        } else if (tag == GV_TAG_FENC) {
            uint32_t v = gpu_fence_reached(g);
            fwrite(&v, 4, 1, out);
        } else if (tag == GV_TAG_RAND) {
            uint32_t a = RD32(), l = RD32(); unsigned st = RD32();
            for (uint32_t i = 0; i < l; i++) mem[(a + i) & GV_SDRAM_MASK] = (uint8_t)gv_rand_next(&st);
        } else if (tag == GV_TAG_HASH) {
            uint64_t h = 0xcbf29ce484222325ull;
            for (uint32_t i = 0; i < GV_SDRAM_BYTES; i++) { h ^= mem[i]; h *= 0x100000001b3ull; }
            fwrite(&h, 8, 1, out);
        } else if (tag == GV_TAG_END) {
            break;
        } else {
            fprintf(stderr, "bad tag %08x\n", tag); rc = 2; break;
        }
    }
    if (stats) {
        gpu_print_stats(g, stderr);
        fprintf(stderr, "model execute time %.3f ms (%.1f Mpixels/s)\n", exec_ns / 1e6,
                exec_ns ? gpu_get_stats(g)->pixels / (exec_ns / 1e9) / 1e6 : 0.0);
    }
#ifdef GPU_TRACE
    { extern FILE *gpu_trace_file; if (gpu_trace_file) fclose(gpu_trace_file); gpu_trace_file = NULL; }
#endif
    fclose(out);
    gpu_destroy(g);
    free(mem);
    free(buf);
    return rc;
}

static int diff_outputs(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    if (!fa || !fb) { fprintf(stderr, "open failed\n"); return 2; }
    uint32_t zone_len = FBZ_HI - FBZ_LO;
    long off = 0; int ca, cb; int shown = 0; long ndiff = 0;
    while (1) {
        ca = fgetc(fa); cb = fgetc(fb);
        if (ca == EOF || cb == EOF) break;
        if (ca != cb) {
            ndiff++;
            if (shown < 20) {
                const char *zone; uint32_t addr;
                if ((uint32_t)off < zone_len) { zone = "fb"; addr = FBZ_LO + (uint32_t)off; }
                else if ((uint32_t)off < 2 * zone_len) { zone = "z"; addr = ZZ_LO + (uint32_t)off - zone_len; }
                else { zone = "tail"; addr = (uint32_t)off; }
                printf("diff %s @%07x: A=%02x B=%02x\n", zone, addr, ca, cb);
                shown++;
            }
        }
        off++;
    }
    if (ca != cb) printf("length differs\n");
    printf("%ld differing bytes\n", ndiff);
    fclose(fa); fclose(fb);
    return ndiff ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 5 && !strcmp(argv[1], "gen")) {
        int mode = !strcmp(argv[3], "stale") ? M_STALE : !strcmp(argv[3], "quake") ? M_QUAKE
                 : !strcmp(argv[3], "garbage") ? M_GARBAGE : !strcmp(argv[3], "overlap") ? M_OVERLAP : M_NORMAL;
        return gen_vector(strtoull(argv[2], NULL, 0), mode, argv[4]);
    }
    if (argc >= 4 && !strcmp(argv[1], "run"))
        return run_vector(argv[2], argv[3], argc > 4);
    if (argc >= 4 && !strcmp(argv[1], "diff"))
        return diff_outputs(argv[2], argv[3]);
    fprintf(stderr, "usage: gputest gen SEED MODE OUT | run VEC OUT [stats] | diff A B\n");
    return 2;
}
