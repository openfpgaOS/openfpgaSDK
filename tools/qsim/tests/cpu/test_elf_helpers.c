//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * Run the production Quake ELF's libgcc soft-float helpers and musl libm
 * float functions inside qsim on random/special inputs and compare with host
 * arithmetic: double via SSE2, binary128 via GCC __float128, libm via the same
 * musl-1.2.5 sources compiled for the host (musl_ref.a, symbols prefixed
 * musl_).  Results must match bit-for-bit (NaN results: both NaN) and the
 * accrued fflags must match the host's IEEE exception flags.
 *
 *   test_elf_helpers app.elf [iterations]
 */
#define _GNU_SOURCE
#include <fenv.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "qelf.h"
#include "vecgen.h"

#define MAGIC 0x0ffffff0u
#define STACK 0x13A80000u
#define TMP   0x13000000u      /* scratch for binary128 operands */

static qsim_cpu *cpu;
static qsim_syms syms;
static uint32_t gp;
static uint64_t total_checks, total_fails, total_insns;

float musl_sinf(float); float musl_cosf(float); float musl_tanf(float);
float musl_atan2f(float, float); float musl_powf(float, float);
float musl_floorf(float); float musl_ceilf(float); double musl_floor(double);

static int on_trap(void *ctx, qsim_cpu *c, uint32_t cause, uint32_t tval)
{
    (void)ctx;
    if (cause == QSIM_CAUSE_FETCH_FAULT && tval == MAGIC) return QSIM_TRAP_STOP;
    fprintf(stderr, "unexpected trap cause=%u tval=%08x pc=%08x\n", cause, tval, c->pc);
    return QSIM_TRAP_STOP;
}

static uint32_t sym(const char *name)
{
    const qsim_sym *s = qsim_syms_lookup(&syms, name);
    if (!s) { fprintf(stderr, "missing symbol %s\n", name); exit(2); }
    return s->addr;
}

static void call(uint32_t fn)
{
    cpu->x[1] = MAGIC;
    cpu->x[2] = STACK;
    cpu->x[3] = gp;
    cpu->pc = fn;
    qsim_set_fflags(cpu, 0);
    uint64_t before = cpu->instret;
    int r = qsim_run(cpu, 100000000);
    total_insns += cpu->instret - before;
    if (r != QSIM_RUN_TRAP || cpu->trap_tval != MAGIC) {
        fprintf(stderr, "call %08x did not return (reason %d pc %08x)\n", fn, r, cpu->pc);
        exit(2);
    }
}

static uint8_t host_flags(void)
{
    int e = fetestexcept(FE_ALL_EXCEPT);
    return (uint8_t)(((e & FE_INVALID) ? 16 : 0) | ((e & FE_DIVBYZERO) ? 8 : 0) | ((e & FE_OVERFLOW) ? 4 : 0) |
                     ((e & FE_UNDERFLOW) ? 2 : 0) | ((e & FE_INEXACT) ? 1 : 0));
}

/* ---- operand generators ---------------------------------------------- */
static const uint64_t dspecial[] = {
    0, 0x8000000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull,
    0x7ff0000000000001ull, 0xfff8000000000001ull, 1, 0x000fffffffffffffull, 0x0010000000000000ull,
    0x7fefffffffffffffull, 0xffefffffffffffffull, 0x3ff0000000000000ull, 0xbff0000000000000ull,
    0x4340000000000000ull, 0x41dfffffffc00000ull, 0x41e0000000000000ull, 0xc1e0000000000000ull,
    0x41efffffffe00000ull, 0x3fe0000000000000ull, 0x3ca0000000000000ull, 0x36a0000000000000ull,
    0x47efffffe0000000ull, 0x47effffff0000000ull, 0x3690000000000000ull, 0x36a0000000000001ull,
};
static uint64_t vg_double(void)
{
    uint64_t r = vg_next();
    switch (r & 15) {
    case 0: case 1: return dspecial[vg_u32() % (sizeof dspecial / sizeof dspecial[0])];
    case 2: case 3: case 4: return vg_next();
    case 5: { double d = (double)(int32_t)vg_u32() / (double)(1 << (vg_u32() % 8)); uint64_t b; memcpy(&b, &d, 8); return b; }
    case 6: case 7: { uint64_t e = 1023 + vg_u32() % 17 - 8; return (vg_next() & 0x800fffffffffffffull) | (e << 52); }
    case 8: { uint64_t e = vg_u32() % 3; return (vg_next() & 0x800fffffffffffffull) | (e << 52); }
    case 9: { uint64_t e = 2046 - vg_u32() % 3; return (vg_next() & 0x800fffffffffffffull) | (e << 52); }
    case 10: { uint64_t e = 1023 + 20 + vg_u32() % 16; return (vg_next() & 0x800fffffffffffffull) | (e << 52); }
    case 11: { uint64_t e = 1023 - 1074 + vg_u32() % 80; return (vg_next() & 0x800fffffffffffffull) | (e << 52); }
    case 12: { uint64_t e = 896 + vg_u32() % 16; return (vg_next() & 0x800fffffffffffffull) | (e << 52); }
    default: { uint64_t e = vg_u32() % 2048; return (vg_next() & 0x800fffffffffffffull) | (e << 52); }
    }
}
static uint64_t vg_double_near(uint64_t a)
{
    uint32_t r = vg_u32();
    if ((r & 3) == 0) {
        int64_t e = (int64_t)((a >> 52) & 0x7ff) + (int64_t)(vg_u32() % 5) - 2;
        if (e < 0) e = 0;
        if (e > 2046) e = 2046;
        return (vg_next() & 0x800fffffffffffffull) | ((uint64_t)e << 52);
    }
    if ((r & 3) == 1) return a ^ (vg_next() & 0x800000000000003full);
    return vg_double();
}

static int dnan(uint64_t b) { return ((b >> 52) & 0x7ff) == 0x7ff && (b & 0xfffffffffffffull); }
static int fnan(uint32_t b) { return ((b >> 23) & 0xff) == 0xff && (b & 0x7fffff); }

typedef struct { const char *name; uint64_t checks, fails; } tstat;

static void report(tstat *t, int ok, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static void report(tstat *t, int ok, const char *fmt, ...)
{
    t->checks++; total_checks++;
    if (ok) return;
    t->fails++; total_fails++;
    if (t->fails <= 5) {
        va_list ap;
        va_start(ap, fmt);
        fprintf(stderr, "FAIL %s: ", t->name);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        va_end(ap);
    }
}

static void set_d(int reg, uint64_t v) { cpu->x[reg] = (uint32_t)v; cpu->x[reg + 1] = (uint32_t)(v >> 32); }
static uint64_t get_d(int reg) { return (uint64_t)cpu->x[reg + 1] << 32 | cpu->x[reg]; }

/* ---- double arithmetic ---------------------------------------------------- */
static void test_df_arith(const char *name, int op, long n)
{
    tstat t = {name, 0, 0};
    uint32_t fn = sym(name);
    for (long i = 0; i < n; i++) {
        uint64_t a = vg_double(), b = vg_double_near(a);
        set_d(10, a); set_d(12, b);
        call(fn);
        uint64_t g = get_d(10);
        uint8_t gf = (uint8_t)qsim_fflags(cpu);
        volatile double x, y; double z = 0;
        memcpy((double *)&x, &a, 8); memcpy((double *)&y, &b, 8);
        feclearexcept(FE_ALL_EXCEPT);
        switch (op) { case 0: z = x + y; break; case 1: z = x - y; break; case 2: z = x * y; break; default: z = x / y; }
        uint8_t hf = host_flags();
        uint64_t e; memcpy(&e, &z, 8);
        int ok = (dnan(e) ? dnan(g) : g == e) && gf == hf;
        report(&t, ok, "a=%016" PRIx64 " b=%016" PRIx64 " got %016" PRIx64 "/%02x expected %016" PRIx64 "/%02x",
               a, b, g, gf, e, hf);
    }
    printf("  %-16s %9" PRIu64 " checks %5" PRIu64 " failures\n", name, t.checks, t.fails);
}

static void test_df_cmp(const char *name, int kind, long n)
{
    tstat t = {name, 0, 0};
    uint32_t fn = sym(name);
    for (long i = 0; i < n; i++) {
        uint64_t a = vg_double(), b = (vg_u32() & 7) == 0 ? a : vg_double_near(a);
        set_d(10, a); set_d(12, b);
        call(fn);
        int32_t g = (int32_t)cpu->x[10];
        uint8_t gf = (uint8_t)qsim_fflags(cpu);
        double x, y;
        memcpy(&x, &a, 8); memcpy(&y, &b, 8);
        int un = isnan(x) || isnan(y);
        int snan = (dnan(a) && !(a & 0x8000000000000ull)) || (dnan(b) && !(b & 0x8000000000000ull));
        uint8_t ef = kind == 0 ? (snan ? 16 : 0) : (un ? 16 : 0);
        int ok;
        if (kind == 0) ok = un ? g != 0 : ((x == y) ? g == 0 : g != 0);          /* __eqdf2 */
        else if (kind == 1) ok = un ? g < 0 : ((x >= y) ? g >= 0 : g < 0);     /* __gedf2 */
        else ok = un ? g > 0 : ((x <= y) ? g <= 0 : g > 0);                    /* __ledf2 */
        ok = ok && gf == ef;
        report(&t, ok, "a=%016" PRIx64 " b=%016" PRIx64 " got %d/%02x flags expected %02x", a, b, g, gf, ef);
    }
    printf("  %-16s %9" PRIu64 " checks %5" PRIu64 " failures\n", name, t.checks, t.fails);
}

static void test_df_conv(long n)
{
    tstat t1 = {"__floatsidf", 0, 0}, t2 = {"__floatunsidf", 0, 0}, t3 = {"__extendsfdf2", 0, 0},
          t4 = {"__truncdfsf2", 0, 0}, t5 = {"__fixdfsi", 0, 0};
    uint32_t f1 = sym("__floatsidf"), f2 = sym("__floatunsidf"), f3 = sym("__extendsfdf2"),
             f4 = sym("__truncdfsf2"), f5 = sym("__fixdfsi");
    for (long i = 0; i < n; i++) {
        uint32_t v = vg_int();
        cpu->x[10] = v; call(f1);
        double e = (double)(int32_t)v; uint64_t eb; memcpy(&eb, &e, 8);
        report(&t1, get_d(10) == eb && qsim_fflags(cpu) == 0, "v=%08x got %016" PRIx64, v, get_d(10));
        cpu->x[10] = v; call(f2);
        e = (double)v; memcpy(&eb, &e, 8);
        report(&t2, get_d(10) == eb && qsim_fflags(cpu) == 0, "v=%08x got %016" PRIx64, v, get_d(10));

        uint32_t fb = vg_float();
        cpu->f[10] = fb; call(f3);
        volatile float ff; memcpy((float *)&ff, &fb, 4);
        feclearexcept(FE_ALL_EXCEPT);
        e = (double)ff;
        uint8_t hf = host_flags();
        memcpy(&eb, &e, 8);
        uint64_t g = get_d(10);
        report(&t3, (dnan(eb) ? dnan(g) : g == eb) && qsim_fflags(cpu) == hf,
               "f=%08x got %016" PRIx64 "/%02x expected %016" PRIx64 "/%02x", fb, g, qsim_fflags(cpu), eb, hf);

        uint64_t db = vg_double();
        set_d(10, db); call(f4);
        volatile double dd; memcpy((double *)&dd, &db, 8);
        feclearexcept(FE_ALL_EXCEPT);
        float r = (float)dd;
        hf = host_flags();
        uint32_t rb; memcpy(&rb, &r, 4);
        report(&t4, (fnan(rb) ? fnan(cpu->f[10]) : cpu->f[10] == rb) && qsim_fflags(cpu) == hf,
               "d=%016" PRIx64 " got %08x/%02x expected %08x/%02x", db, cpu->f[10], qsim_fflags(cpu), rb, hf);

        /* __fixdfsi: skip the (2^31, 2^31+1) magnitude sliver whose flag rule is libgcc-specific */
        double d; memcpy(&d, &db, 8);
        if (!isnan(d) && fabs(d) >= 2147483648.0 && fabs(d) < 2147483649.0) continue;
        set_d(10, db); call(f5);
        int32_t ei; uint8_t ef;
        if (isnan(d)) { ei = (db >> 63) ? INT32_MIN : INT32_MAX; ef = 16; }
        else if (d >= 2147483648.0 || d < -2147483648.0) { ei = d < 0 ? INT32_MIN : INT32_MAX; ef = 16; }
        else { ei = (int32_t)d; ef = ((double)ei != d) ? 1 : 0; }
        int gi = (int32_t)cpu->x[10];
        int ok = qsim_fflags(cpu) == ef && (ef == 16 ? 1 : gi == ei);
        if (ef == 16 && !isnan(d)) ok = ok && gi == ei;
        report(&t5, ok, "d=%016" PRIx64 " got %d/%02x expected %d/%02x", db, gi, qsim_fflags(cpu), ei, ef);
    }
    tstat *ts[] = {&t1, &t2, &t3, &t4, &t5};
    for (int i = 0; i < 5; i++)
        printf("  %-16s %9" PRIu64 " checks %5" PRIu64 " failures\n", ts[i]->name, ts[i]->checks, ts[i]->fails);
}

/* ---- binary128 -------------------------------------------------------------- */
typedef __float128 f128;
static void put128(uint32_t addr, f128 v) { uint8_t b[16]; memcpy(b, &v, 16); qsim_write(cpu, addr, b, 16); }
static int q_nan(const uint8_t *b) { uint16_t e; memcpy(&e, b + 14, 2); e &= 0x7fff; if (e != 0x7fff) return 0;
    for (int i = 0; i < 14; i++) if (b[i]) return 1; return (b[14] | (b[15] & 0x7f)) != 0xff || 0; }
static f128 vg_quad(void)
{
    uint64_t d = vg_double();
    double x; memcpy(&x, &d, 8);
    f128 q = (f128)x;
    uint32_t r = vg_u32();
    if ((r & 3) == 1) { uint8_t b[16]; memcpy(b, &q, 16); for (int i = 0; i < 7; i++) b[i] = (uint8_t)vg_u32(); memcpy(&q, b, 16); }
    if ((r & 15) == 2) { uint8_t b[16]; for (int i = 0; i < 16; i++) b[i] = (uint8_t)vg_u32(); memcpy(&q, b, 16); }
    return q;
}

static void test_tf(long n)
{
    const char *names[] = {"__addtf3", "__subtf3", "__multf3", "__divtf3"};
    for (int op = 0; op < 4; op++) {
        tstat t = {names[op], 0, 0};
        uint32_t fn = sym(names[op]);
        for (long i = 0; i < n; i++) {
            f128 a = vg_quad(), b = vg_quad();
            put128(TMP + 16, a); put128(TMP + 32, b);
            cpu->x[10] = TMP; cpu->x[11] = TMP + 16; cpu->x[12] = TMP + 32;
            call(fn);
            uint8_t g[16]; qsim_read(cpu, TMP, g, 16);
            volatile f128 x = a, y = b; f128 z;
            feclearexcept(FE_ALL_EXCEPT);
            switch (op) { case 0: z = x + y; break; case 1: z = x - y; break; case 2: z = x * y; break; default: z = x / y; }
            uint8_t hf = host_flags();
            uint8_t e[16]; memcpy(e, &z, 16);
            int en = z != z, gn = 0;
            { f128 gq; memcpy(&gq, g, 16); gn = gq != gq; }
            int ok = (en ? gn : !memcmp(g, e, 16)) && (uint8_t)qsim_fflags(cpu) == hf;
            (void)q_nan;
            report(&t, ok, "op%d got flags %02x expected %02x", op, qsim_fflags(cpu), hf);
        }
        printf("  %-16s %9" PRIu64 " checks %5" PRIu64 " failures\n", names[op], t.checks, t.fails);
    }
    tstat t1 = {"__extenddftf2", 0, 0}, t2 = {"__trunctfdf2", 0, 0}, t3 = {"__trunctfsf2", 0, 0},
          t4 = {"__extendsftf2", 0, 0};
    uint32_t f1 = sym("__extenddftf2"), f2 = sym("__trunctfdf2"), f3 = sym("__trunctfsf2"), f4 = sym("__extendsftf2");
    for (long i = 0; i < n; i++) {
        uint64_t db = vg_double();
        double d; memcpy(&d, &db, 8);
        cpu->x[10] = TMP; set_d(11, db);
        call(f1);
        uint8_t g[16]; qsim_read(cpu, TMP, g, 16);
        volatile double vd = d;
        feclearexcept(FE_ALL_EXCEPT);
        f128 q = (f128)vd;
        uint8_t hf = host_flags();
        uint8_t e[16]; memcpy(e, &q, 16);
        { f128 gq; memcpy(&gq, g, 16);
          report(&t1, ((q != q) ? gq != gq : !memcmp(g, e, 16)) && qsim_fflags(cpu) == hf, "d=%016" PRIx64, db); }

        f128 a = vg_quad();
        put128(TMP + 16, a);
        cpu->x[10] = TMP + 16;
        call(f2);
        volatile f128 va = a;
        feclearexcept(FE_ALL_EXCEPT);
        double r = (double)va;
        hf = host_flags();
        uint64_t rb; memcpy(&rb, &r, 8);
        report(&t2, (dnan(rb) ? dnan(get_d(10)) : get_d(10) == rb) && qsim_fflags(cpu) == hf,
               "got %016" PRIx64 "/%02x expected %016" PRIx64 "/%02x", get_d(10), qsim_fflags(cpu), rb, hf);

        cpu->x[10] = TMP + 16;
        call(f3);
        feclearexcept(FE_ALL_EXCEPT);
        float rf = (float)va;
        hf = host_flags();
        uint32_t rfb; memcpy(&rfb, &rf, 4);
        report(&t3, (fnan(rfb) ? fnan(cpu->f[10]) : cpu->f[10] == rfb) && qsim_fflags(cpu) == hf,
               "got %08x/%02x expected %08x/%02x", cpu->f[10], qsim_fflags(cpu), rfb, hf);

        uint32_t fb = vg_float();
        cpu->x[10] = TMP; cpu->f[10] = fb;
        call(f4);
        qsim_read(cpu, TMP, g, 16);
        volatile float vf; memcpy((float *)&vf, &fb, 4);
        feclearexcept(FE_ALL_EXCEPT);
        q = (f128)vf;
        hf = host_flags();
        memcpy(e, &q, 16);
        { f128 gq; memcpy(&gq, g, 16);
          report(&t4, ((q != q) ? gq != gq : !memcmp(g, e, 16)) && qsim_fflags(cpu) == hf, "f=%08x", fb); }
    }
    tstat *ts[] = {&t1, &t2, &t3, &t4};
    for (int i = 0; i < 4; i++)
        printf("  %-16s %9" PRIu64 " checks %5" PRIu64 " failures\n", ts[i]->name, ts[i]->checks, ts[i]->fails);
}

/* ---- libm -------------------------------------------------------------------- */
static uint32_t vg_angle(void)
{
    uint32_t r = vg_u32();
    float f;
    switch (r & 7) {
    case 0: case 1: f = (float)((int32_t)vg_u32() % 100000) / 1000.0f; break;
    case 2: f = (float)((int32_t)vg_u32() % 1000) * 0.78539816f; break;
    case 3: f = (float)(vg_u32() % 2000) / 64.0f; break;
    default: { uint32_t b = vg_float(); return b; }
    }
    uint32_t b; memcpy(&b, &f, 4);
    return b;
}

static void test_libm(long n)
{
    struct { const char *name; float (*h1)(float); float (*h2)(float, float); } fns[] = {
        {"sinf", musl_sinf, NULL}, {"cosf", musl_cosf, NULL}, {"tanf", musl_tanf, NULL},
        {"floorf", musl_floorf, NULL}, {"ceilf", musl_ceilf, NULL},
        {"atan2f", NULL, musl_atan2f}, {"powf", NULL, musl_powf},
    };
    for (unsigned k = 0; k < sizeof fns / sizeof fns[0]; k++) {
        tstat t = {fns[k].name, 0, 0};
        uint32_t fn = sym(fns[k].name);
        for (long i = 0; i < n; i++) {
            uint32_t a = vg_angle(), b = vg_angle();
            if (fns[k].h2 && !strcmp(fns[k].name, "powf") && (vg_u32() & 1)) {
                float x = (float)(vg_u32() % 400) / 37.0f, y = (float)((int32_t)vg_u32() % 200) / 13.0f;
                memcpy(&a, &x, 4); memcpy(&b, &y, 4);
            }
            cpu->f[10] = a; cpu->f[11] = b;
            call(fn);
            uint32_t g = cpu->f[10];
            uint8_t gf = (uint8_t)qsim_fflags(cpu);
            float x, y, z;
            memcpy(&x, &a, 4); memcpy(&y, &b, 4);
            feclearexcept(FE_ALL_EXCEPT);
            z = fns[k].h1 ? fns[k].h1(x) : fns[k].h2(x, y);
            uint8_t hf = host_flags();
            uint32_t e; memcpy(&e, &z, 4);
            int ok = (fnan(e) ? fnan(g) : g == e) && gf == hf;
            report(&t, ok, "a=%08x b=%08x got %08x/%02x expected %08x/%02x", a, b, g, gf, e, hf);
        }
        printf("  %-16s %9" PRIu64 " checks %5" PRIu64 " failures\n", fns[k].name, t.checks, t.fails);
    }
    tstat t = {"floor", 0, 0};
    uint32_t fn = sym("floor");
    for (long i = 0; i < n; i++) {
        uint64_t a = vg_double();
        set_d(10, a);
        call(fn);
        double x; memcpy(&x, &a, 8);
        feclearexcept(FE_ALL_EXCEPT);
        double z = musl_floor(x);
        uint8_t hf = host_flags();
        uint64_t e; memcpy(&e, &z, 8);
        uint64_t g = get_d(10);
        report(&t, (dnan(e) ? dnan(g) : g == e) && qsim_fflags(cpu) == hf,
               "a=%016" PRIx64 " got %016" PRIx64 "/%02x expected %016" PRIx64 "/%02x", a, g, qsim_fflags(cpu), e, hf);
    }
    printf("  %-16s %9" PRIu64 " checks %5" PRIu64 " failures\n", "floor", t.checks, t.fails);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: test_elf_helpers app.elf [n]\n"); return 2; }
    long n = argc > 2 ? atol(argv[2]) : 20000;
    cpu = qsim_cpu_new(NULL);
    uint32_t entry;
    if (qsim_elf_load(cpu, argv[1], &entry) || qsim_elf_symbols(argv[1], &syms)) {
        fprintf(stderr, "cannot load %s\n", argv[1]);
        return 2;
    }
    gp = sym("__global_pointer$");
    cpu->env.trap = on_trap;
    printf("production ELF helpers (%s), %ld vectors each:\n", argv[1], n);
    test_df_arith("__adddf3", 0, n);
    test_df_arith("__subdf3", 1, n);
    test_df_arith("__muldf3", 2, n);
    test_df_arith("__divdf3", 3, n);
    test_df_cmp("__eqdf2", 0, n);
    test_df_cmp("__gedf2", 1, n);
    test_df_cmp("__ledf2", 2, n);
    test_df_conv(n);
    test_tf(n / 4);
    test_libm(n);
    printf("elf-helpers: %" PRIu64 " checks, %" PRIu64 " failures, %" PRIu64 " guest instructions\n",
           total_checks, total_fails, total_insns);
    qsim_syms_free(&syms);
    qsim_cpu_free(cpu);
    return total_fails != 0;
}
