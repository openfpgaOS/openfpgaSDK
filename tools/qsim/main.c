//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * qsim -- boot a production openfpgaOS app ELF on the qsim CPU core and run
 * a deterministic workload.  What is app-specific (data slots, the app .ini,
 * argv[0], console conventions) comes from an app profile (app.h).
 *
 *   qsim <elf> --out <dir> [--app <profile>] [--data-dir <dir>] [--args "<guest argv>"]
 *        [--frames N] [--profile] [--ppm-every K] [--no-gpu-model] [options]
 *
 * Outputs in --out: frames.csv, summary.json, console.log, and with --profile
 * profile.txt + profile.tsv; ppm/frame_NNNNN.ppm with --ppm-every.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "cache.h"
#include "env.h"
#include "prof.h"
#include "lint.h"

/* ------------------------------------------------------------------------ */
/* sha256 (for summary.json)                                                */

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha256_t;
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(sha256_t *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static int sha256_file(const char *path, char out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    sha256_t s = { {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}, 0, {0}, 0 };
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            s.buf[s.n++] = buf[i];
            if (s.n == 64) { sha_block(&s, s.buf); s.n = 0; }
        }
        s.len += n;
    }
    fclose(f);
    uint64_t bits = s.len * 8;
    s.buf[s.n++] = 0x80;
    if (s.n > 56) { while (s.n < 64) s.buf[s.n++] = 0; sha_block(&s, s.buf); s.n = 0; }
    while (s.n < 56) s.buf[s.n++] = 0;
    for (int i = 7; i >= 0; i--) s.buf[s.n++] = (uint8_t)(bits >> (8 * i));
    sha_block(&s, s.buf);
    for (int i = 0; i < 8; i++) sprintf(out + 8 * i, "%08x", s.h[i]);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* symbol classification                                                    */

static int is_softfloat(const char *n)
{
    static const char *pre[] = {"__add", "__sub", "__mul", "__div", "__neg", "__eq", "__ne", "__lt",
        "__le", "__gt", "__ge", "__unord", "__fix", "__float", "__extend", "__trunc", "__cmp", "__pow", 0};
    if (strncmp(n, "__", 2)) return 0;
    size_t L = strlen(n);
    if (L < 7 || L > 18) return 0;
    const char *t = n + L - 5;
    if (!strstr(t, "df") && !strstr(t, "sf") && !strstr(t, "tf")) return 0;
    for (int i = 0; pre[i]; i++) if (!strncmp(n, pre[i], strlen(pre[i]))) return 1;
    return 0;
}

static int is_libm(const char *n)
{
    static const char *names[] = {"sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sqrt", "floor",
        "ceil", "pow", "exp", "log", "log10", "fmod", "round", "rint", "lrint", "lround", "fabs", "trunc",
        "hypot", "exp2", "log2", "cbrt", "modf", "frexp", "ldexp", "scalbn", 0};
    size_t L = strlen(n);
    for (int i = 0; names[i]; i++) {
        size_t k = strlen(names[i]);
        if (!strncmp(n, names[i], k) && (L == k || (L == k + 1 && (n[k] == 'f' || n[k] == 'l'))))
            return 1;
    }
    return !strncmp(n, "__sin", 5) || !strncmp(n, "__cos", 5) || !strncmp(n, "__tan", 5) ||
           !strncmp(n, "__rem_pio2", 10) || !strncmp(n, "__math_", 7) || !strncmp(n, "__expo2", 7);
}

/* ------------------------------------------------------------------------ */
/* profile aggregation                                                      */

typedef struct {
    const char *name;
    uint32_t addr, size;
    uint64_t insns, cycles, ic, dc, bp, calls, tails;
    int kind;        /* 0 function, 1 soft-float helper, 2 libm, 3 OS service, 4 unknown */
} frow;

typedef struct { const char *caller, *callee; uint64_t n; int kind; } edge;

typedef struct {
    frow *rows; size_t nrows;
    edge *edges; size_t nedges;
    uint64_t tot_insns, tot_cycles, unknown_insns;
} profile_t;

static size_t arena_index(qsim_cpu *cpu, uint32_t pc, int *ok)
{
    uint32_t o = pc - cpu->sdram_base;
    *ok = 1;
    if (o < cpu->sdram_size) return (size_t)(cpu->sd_dc - cpu->dc_base) + (o >> 1);
    if (pc < cpu->bram_size) return (size_t)(cpu->br_dc - cpu->dc_base) + (pc >> 1);
    *ok = 0;
    return 0;
}

static uint32_t arena_pc(qsim_cpu *cpu, size_t i)
{
    size_t sd_first = (size_t)(cpu->sd_dc - cpu->dc_base);
    if (i >= sd_first) return cpu->sdram_base + (uint32_t)((i - sd_first) << 1);
    return (uint32_t)(i << 1);
}

static char svc_label[ENV_NSVC][40];

static const qsim_sym *func_of(qenv *E, uint32_t pc, const char **name, int *kind, uint32_t *addr)
{
    if (pc - ENV_SVC_BASE < 4u * ENV_NSVC) {
        int i = (int)((pc - ENV_SVC_BASE) / 4);
        *name = svc_label[i]; *kind = 3; *addr = ENV_SVC_BASE + 4u * (uint32_t)i;
        return NULL;
    }
    const qsim_sym *s = qsim_syms_func(&E->syms, pc);
    if (!s) { *name = "<unknown>"; *kind = 4; *addr = 0; return NULL; }
    *name = s->name; *addr = s->addr;
    *kind = is_softfloat(s->name) ? 1 : is_libm(s->name) ? 2 : 0;
    return s;
}

static int cmp_name_ptr(const void *a, const void *b)
{
    const frow *x = a, *y = b;
    return x->name < y->name ? -1 : x->name > y->name;
}

static void build_profile(qenv *E, profile_t *P)
{
    qsim_cpu *cpu = E->cpu;
    qsim_prof *pr = cpu->prof;
    memset(P, 0, sizeof *P);
    for (int i = 0; i < ENV_NSVC; i++) snprintf(svc_label[i], sizeof svc_label[i], "svc:%s", env_svc_name(i));
    size_t cap = 1024, ecap = 256;
    P->rows = calloc(cap, sizeof *P->rows);
    P->edges = calloc(ecap, sizeof *P->edges);
    /* regions: BRAM decoded code + SDRAM decoded code (hooks included) */
    size_t sd_first = (size_t)(cpu->sd_dc - cpu->dc_base);
    size_t ranges[2][2] = {
        { 0, cpu->br_code_hi / 2u + 2 },
        { sd_first + (cpu->code_lo >> 1), sd_first + (cpu->code_hi >> 1) + 32 },
    };
    if (cpu->code_hi <= cpu->code_lo) ranges[1][1] = ranges[1][0];
    const char *last_name = NULL;
    frow *last = NULL;
    for (int r = 0; r < 2; r++) {
        for (size_t i = ranges[r][0]; i < ranges[r][1] && i < pr->n; i++) {
            uint64_t c = pr->cnt[i], pn = pr->pen[i], cl = pr->calls[i], tl = pr->tails[i];
            if (!c && !pn && !cl && !tl && !pr->site_n[i]) continue;
            uint32_t pc = arena_pc(cpu, i);
            const char *name; int kind; uint32_t fa;
            const qsim_sym *s = func_of(E, pc, &name, &kind, &fa);
            if (c || pn || cl || tl) {
                frow *row = NULL;
                if (last && last_name == name) row = last;
                else {
                    for (size_t k = P->nrows; k-- > 0;) if (P->rows[k].name == name) { row = &P->rows[k]; break; }
                    if (!row) {
                        if (P->nrows == cap) { cap *= 2; P->rows = realloc(P->rows, cap * sizeof *P->rows); }
                        row = &P->rows[P->nrows++];
                        memset(row, 0, sizeof *row);
                        row->name = name; row->addr = fa; row->size = s ? s->size : 0; row->kind = kind;
                    }
                }
                last = row; last_name = name;
                row->insns += c;
                row->cycles += c + pn;
                row->ic += pr->ev_ic[i]; row->dc += pr->ev_dc[i]; row->bp += pr->ev_bp[i];
                if (pc == fa) { row->calls += cl; row->tails += tl; }
                P->tot_insns += c;
                P->tot_cycles += c + pn;
                if (kind == 4) P->unknown_insns += c;
            }
            if (pr->site_n[i]) {
                uint32_t tpc = arena_pc(cpu, pr->site_tgt[i]);
                const char *tn; int tk; uint32_t ta;
                func_of(E, tpc, &tn, &tk, &ta);
                if ((tk == 1 || tk == 2 || tk == 3) && tpc == ta) {
                    size_t k;
                    for (k = 0; k < P->nedges; k++)
                        if (P->edges[k].caller == name && P->edges[k].callee == tn) break;
                    if (k == P->nedges) {
                        if (P->nedges == ecap) { ecap *= 2; P->edges = realloc(P->edges, ecap * sizeof *P->edges); }
                        P->edges[k].caller = name; P->edges[k].callee = tn; P->edges[k].n = 0; P->edges[k].kind = tk;
                        P->nedges++;
                    }
                    P->edges[k].n += pr->site_n[i];
                }
            }
        }
    }
    /* rows with the same name (e.g. a function split into regions) are
     * already merged above because names are symbol-table pointers */
    (void)cmp_name_ptr;
}

static int by_cycles(const void *a, const void *b)
{
    const frow *x = a, *y = b;
    if (x->cycles != y->cycles) return x->cycles < y->cycles ? 1 : -1;
    return strcmp(x->name, y->name);
}
static int by_edge(const void *a, const void *b)
{
    const edge *x = a, *y = b;
    if (x->n != y->n) return x->n < y->n ? 1 : -1;
    int c = strcmp(x->callee, y->callee);
    return c ? c : strcmp(x->caller, y->caller);
}

static void write_profile(qenv *E, profile_t *P, const char *txt, const char *tsv, size_t top,
                          const char *sha)
{
    qsort(P->rows, P->nrows, sizeof *P->rows, by_cycles);
    qsort(P->edges, P->nedges, sizeof *P->edges, by_edge);
    FILE *o = fopen(txt, "w");
    if (!o) return;
    qsim_model *m = E->cpu->model;
    fprintf(o, "qsim profile\nelf: %s\nelf_sha256: %s\nargv:", E->elf_path, sha);
    for (int i = 0; i < E->argc; i++) fprintf(o, " %s", E->argv[i]);
    fprintf(o, "\ninstructions: %" PRIu64 "  est_cycles: %" PRIu64 " (%.3f cycles/insn)  unattributed insns: %" PRIu64 "\n\n",
            P->tot_insns, P->tot_cycles, P->tot_insns ? (double)P->tot_cycles / (double)P->tot_insns : 0.0,
            P->unknown_insns);
    qsim_model_report(m, E->cpu->instret - E->svc_total, o);
    fprintf(o, "\n== functions by self est_cycles (top %zu of %zu) ==\n", top, P->nrows);
    fprintf(o, "%-34s %13s %6s %14s %6s %6s %10s %10s %10s %11s %9s\n", "function", "self_insns", "%ins",
            "self_cycles", "%cyc", "cyc/i", "icache_ms", "dcache_ms", "mispred", "calls", "tailcalls");
    for (size_t i = 0; i < P->nrows && i < top; i++) {
        frow *r = &P->rows[i];
        fprintf(o, "%-34.34s %13" PRIu64 " %6.2f %14" PRIu64 " %6.2f %6.2f %10" PRIu64 " %10" PRIu64 " %10" PRIu64
                   " %11" PRIu64 " %9" PRIu64 "\n",
                r->name, r->insns, P->tot_insns ? 100.0 * (double)r->insns / (double)P->tot_insns : 0.0,
                r->cycles, P->tot_cycles ? 100.0 * (double)r->cycles / (double)P->tot_cycles : 0.0,
                r->insns ? (double)r->cycles / (double)r->insns : 0.0, r->ic, r->dc, r->bp, r->calls, r->tails);
    }
    fprintf(o, "\n== soft-float helpers (libgcc) ==\n%-24s %11s %9s %14s %14s\n", "helper", "calls", "tailcalls",
            "self_insns", "self_cycles");
    uint64_t sf_calls = 0, sf_cyc = 0;
    for (size_t i = 0; i < P->nrows; i++) {
        frow *r = &P->rows[i];
        if (r->kind != 1) continue;
        fprintf(o, "%-24s %11" PRIu64 " %9" PRIu64 " %14" PRIu64 " %14" PRIu64 "\n", r->name, r->calls, r->tails,
                r->insns, r->cycles);
        sf_calls += r->calls + r->tails; sf_cyc += r->cycles;
    }
    fprintf(o, "%-24s %11" PRIu64 " %9s %14s %14" PRIu64 " (%.2f%% of cycles)\n", "TOTAL", sf_calls, "", "", sf_cyc,
            P->tot_cycles ? 100.0 * (double)sf_cyc / (double)P->tot_cycles : 0.0);
    fprintf(o, "\n== soft-float calls by caller ==\n%-34s %-20s %11s\n", "caller", "helper", "calls");
    for (size_t i = 0; i < P->nedges; i++)
        if (P->edges[i].kind == 1)
            fprintf(o, "%-34.34s %-20s %11" PRIu64 "\n", P->edges[i].caller, P->edges[i].callee, P->edges[i].n);
    fprintf(o, "\n== libm functions ==\n%-24s %11s %9s %14s %14s\n", "function", "calls", "tailcalls", "self_insns",
            "self_cycles");
    for (size_t i = 0; i < P->nrows; i++) {
        frow *r = &P->rows[i];
        if (r->kind == 2)
            fprintf(o, "%-24s %11" PRIu64 " %9" PRIu64 " %14" PRIu64 " %14" PRIu64 "\n", r->name, r->calls, r->tails,
                    r->insns, r->cycles);
    }
    fprintf(o, "\n== libm calls by caller ==\n%-34s %-20s %11s\n", "caller", "function", "calls");
    for (size_t i = 0; i < P->nedges; i++)
        if (P->edges[i].kind == 2)
            fprintf(o, "%-34.34s %-20s %11" PRIu64 "\n", P->edges[i].caller, P->edges[i].callee, P->edges[i].n);
    fprintf(o, "\n== OS service calls (kernel cost not modelled) ==\n");
    for (int i = 0; i < ENV_NSVC; i++)
        if (E->svc_calls[i]) fprintf(o, "%-28s %12" PRIu64 "\n", env_svc_name(i), E->svc_calls[i]);
    fprintf(o, "\n== OS service calls by caller ==\n");
    for (size_t i = 0; i < P->nedges; i++)
        if (P->edges[i].kind == 3)
            fprintf(o, "%-34.34s %-28s %11" PRIu64 "\n", P->edges[i].caller, P->edges[i].callee, P->edges[i].n);
    fprintf(o, "\n== ecalls (kernel cost not modelled) ==\n");
    for (int i = 0; i < 512; i++)
        if (E->ecalls_linux[i]) fprintf(o, "linux %-4d %12" PRIu64 "\n", i, E->ecalls_linux[i]);
    for (int e = 0; e < 16; e++)
        for (int f = 0; f < 16; f++)
            if (E->ecalls_vendor[e][f]) fprintf(o, "vendor 0xC0DE%04X/%d %12" PRIu64 "\n", e, f, E->ecalls_vendor[e][f]);
    if (E->gpu) { fprintf(o, "\n== GPU model ==\n"); gpu_print_stats(E->gpu, o); }
    fclose(o);

    FILE *t = fopen(tsv, "w");
    if (!t) return;
    fprintf(t, "function\tkind\taddr\tsize\tself_insns\tself_cycles\ticache_miss\tdcache_miss\tmispredict\tcalls\ttailcalls\n");
    static const char *kn[] = {"func", "softfloat", "libm", "service", "unknown"};
    for (size_t i = 0; i < P->nrows; i++) {
        frow *r = &P->rows[i];
        fprintf(t, "%s\t%s\t0x%08x\t%u\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\n",
                r->name, kn[r->kind], r->addr, r->size, r->insns, r->cycles, r->ic, r->dc, r->bp, r->calls, r->tails);
    }
    fclose(t);
    FILE *c = NULL;
    {
        char p[4096];
        snprintf(p, sizeof p, "%s", tsv);
        char *dot = strrchr(p, '.');
        if (dot) strcpy(dot, "_calls.tsv");
        c = fopen(p, "w");
    }
    if (c) {
        fprintf(c, "caller\tcallee\tkind\tcalls\n");
        for (size_t i = 0; i < P->nedges; i++)
            fprintf(c, "%s\t%s\t%s\t%" PRIu64 "\n", P->edges[i].caller, P->edges[i].callee, kn[P->edges[i].kind],
                    P->edges[i].n);
        fclose(c);
    }
}

/* --coverage: every executed instruction address with its execution count
 * (profiling interpreter counts), for coverage.py.  Hook addresses excluded. */
static void write_coverage(qenv *E, const char *path, const char *sha)
{
    qsim_cpu *cpu = E->cpu;
    qsim_prof *pr = cpu->prof;
    FILE *o = fopen(path, "w");
    if (!o || !pr) { if (o) fclose(o); return; }
    fprintf(o, "# qsim coverage elf_sha256=%s\npc\tcount\n", sha);
    size_t sd_first = (size_t)(cpu->sd_dc - cpu->dc_base);
    size_t ranges[2][2] = {
        { 0, cpu->br_code_hi / 2u + 2 },
        { sd_first + (cpu->code_lo >> 1), sd_first + (cpu->code_hi >> 1) + 32 },
    };
    if (cpu->code_hi <= cpu->code_lo) ranges[1][1] = ranges[1][0];
    for (int r = 0; r < 2; r++)
        for (size_t i = ranges[r][0]; i < ranges[r][1] && i < pr->n; i++) {
            if (!pr->cnt[i]) continue;
            uint32_t pc = arena_pc(cpu, i);
            if (pc - ENV_SVC_BASE < 4u * ENV_NSVC) continue;
            fprintf(o, "%08x\t%" PRIu64 "\n", pc, pr->cnt[i]);
        }
    fclose(o);
}

/* per-frame counters                                                       */

typedef struct {
    uint64_t insns, cycles, ic_sd, ic_br, dc, bp, sf, words, gpx;
    uint64_t hazards, barriers, obs_px;
} counters;

static size_t *sf_idx;
static size_t n_sf;

static void read_counters(qenv *E, counters *c)
{
    memset(c, 0, sizeof *c);
    c->insns = E->cpu->instret - E->svc_total;   /* guest instructions (service hooks excluded) */
    c->words = E->cmd_words_total;
    if (E->gpu) {
        const gpu_stats_t *gs = gpu_get_stats(E->gpu);
        c->gpx = gs->pixels;
        for (int k = 0; k < GPU_OBS_N; k++) c->barriers += gs->barriers[k];
        c->obs_px = gs->obs_px;
    }
    if (E->lint) c->hazards = lint_total_aware(E->lint);
    qsim_model *m = E->cpu->model;
    if (m) {
        c->cycles = m->cyc;
        c->ic_sd = m->ic_miss_sdram;
        c->ic_br = m->ic_miss_bram;
        c->dc = m->dc_load_miss + m->dc_store_miss;
        c->bp = m->br_mispredict + m->jump_mispredict;
        qsim_prof *p = E->cpu->prof;
        for (size_t i = 0; i < n_sf; i++) c->sf += p->calls[sf_idx[i]] + p->tails[sf_idx[i]];
    }
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* nearest-rank percentile on a sorted array */
static uint64_t pct(const uint64_t *s, size_t n, double p)
{
    if (!n) return 0;
    size_t k = (size_t)(p * (double)n + 0.999999);
    if (k < 1) k = 1;
    if (k > n) k = n;
    return s[k - 1];
}

static void stats_json(FILE *o, const char *ind, const char *name, const uint64_t *v, size_t n, int last)
{
    uint64_t *s = malloc((n ? n : 1) * sizeof *s);
    if (n) memcpy(s, v, n * sizeof *s);
    qsort(s, n, sizeof *s, cmp_u64);
    unsigned long long sum = 0;
    for (size_t i = 0; i < n; i++) sum += s[i];
    fprintf(o, "%s\"%s\": {\"mean\": %.1f, \"p50\": %" PRIu64 ", \"p90\": %" PRIu64 ", \"p99\": %" PRIu64
               ", \"max\": %" PRIu64 ", \"min\": %" PRIu64 ", \"sum\": %llu}%s\n",
            ind, name, n ? (double)sum / (double)n : 0.0, pct(s, n, 0.50), pct(s, n, 0.90), pct(s, n, 0.99),
            n ? s[n - 1] : 0, n ? s[0] : 0, sum, last ? "" : ",");
    free(s);
}

enum { M_INSNS, M_CYC, M_IC, M_DC, M_BP, M_SF, M_WORDS, M_GPX, M_HAZ, M_BAR, M_WPX, M_N };
static const char *const metric_names[M_N] = {
    "insns", "est_cycles", "icache_miss", "dcache_miss", "mispredict", "softfloat_calls", "gpu_cmd_words", "gpu_pixels",
    "mem_hazards", "gpu_barriers", "gpu_wait_px"
};

static void window_json(FILE *o, const char *name, uint64_t **cols, size_t first, size_t n, int have_model, int last)
{
    fprintf(o, "  \"%s\": {\n    \"first_frame\": %zu,\n    \"frames\": %zu,\n", name, first, n);
    int k = 0;
    for (int mi = 0; mi < M_N; mi++) {
        if (!have_model && (mi == M_CYC || mi == M_IC || mi == M_DC || mi == M_BP || mi == M_SF)) continue;
        k++;
    }
    for (int mi = 0; mi < M_N; mi++) {
        if (!have_model && (mi == M_CYC || mi == M_IC || mi == M_DC || mi == M_BP || mi == M_SF)) continue;
        stats_json(o, "    ", metric_names[mi], cols[mi] + first, n, --k == 0);
    }
    fprintf(o, "  }%s\n", last ? "" : ",");
}

static void json_str(FILE *o, const char *s)
{
    fputc('"', o);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { fputc('\\', o); fputc(*s, o); }
        else if ((unsigned char)*s < 0x20) fprintf(o, "\\u%04x", (unsigned char)*s);
        else fputc(*s, o);
    }
    fputc('"', o);
}

/* ------------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
        "usage: qsim <elf> --out <dir> [--app <profile>] [--data-dir <dir>] [options]\n"
        "  --app FILE             app profile: data slots, .ini, argv[0], console conventions (app.h;\n"
        "                         default: a bare app with no data files -- see apps/<app>/<app>.app)\n"
        "  --data-dir DIR         host directory holding the app's data files (the app profile's data: slots)\n"
        "  --args \"<argv>\"        guest arguments after argv[0] (default: the profile's default_args)\n"
        "  --frames N             stop after N presented frames (default: until the profile's result line)\n"
        "  --profile              profiling interpreter + os25 timing model: est_cycles/cache/branch columns,\n"
        "                         profile.txt, profile.tsv, profile_calls.tsv\n"
        "  --ppm-every K          write out/ppm/frame_NNNNN.ppm every K presented frames\n"
        "  --ppm-start S          first frame eligible for --ppm-every (default 0)\n"
        "  --no-gpu-model         do not rasterize GPU commands (flip/fence only); hash_px then covers CPU writes only\n"
        "  --gpuvec F1,F2,...     capture the GPU input of these presented frames as out/gpuvec/frame_N.vec\n"
        "                         (gpuvec.h format; replay with gpuvec_check.sh against the RTL)\n"
        "  --gpu-sched S          GPU schedule: eager (default; infinitely fast GPU, commands run at publish),\n"
        "                         lazy (as late as legally possible: only when the CPU observes progress, and only\n"
        "                         as far as that observation needs), drain (every observation runs everything)\n"
        "  --mem M                memory: flat (default), lint (L1 D-cache coherency hazards counted, execution\n"
        "                         unchanged), strict (split L1/committed SDRAM: GPU, uncached alias and scanout\n"
        "                         read committed SDRAM, so coherency hazards change pixels; also counts hazards)\n"
        "  --check-strict-invariant  (debug, slow) with --mem strict: verify at every flip that the CPU view and\n"
        "                         committed SDRAM differ only on dirty or stale L1 lines\n"
        "  --no-normalize-build-stamp  keep the ELF's \"<build_stamp> <__TIME__> <__DATE__>\" string as built\n"
        "  --max-insns N          instruction limit (default 400e9)\n"
        "  --heap-size N          override caps.heap_size (default 0x13A00000 - bss_end, device-exact)\n"
        "  --file NAME=PATH       serve host file PATH as read-only data slot NAME (ids 20+, repeatable; e.g.\n"
        "                         a console script for \"+exec NAME\")\n"
        "  --end-on TEXT          stop (status workload_complete) when a guest console line contains TEXT\n"
        "  --window-on TEXT       measurement window = frames published from the host frame that prints a\n"
        "                         console line containing TEXT (default window: the profile's result frames)\n"
        "  --window-end-on TEXT   ... up to (not including) the host frame that prints TEXT\n"
        "  --seed-mem SEED        fill memory the device does not initialize (stack, unallocated SDRAM/BRAM,\n"
        "                         CRAM0) with a PRNG pattern from SEED instead of zeros (default 0 = zeros)\n"
        "  --coverage             (implies --profile) write coverage.tsv: every executed pc and its count\n"
        "  --label NAME           workload name recorded in summary.json (\"workload\"; run.sh/gate.sh set it)\n"
        "  --top N                functions listed in profile.txt (default 80)\n"
        "  --echo                 echo guest console to stderr\n"
        "  --quiet                no progress line\n"
        "  --verbose              log file opens and config lookups\n");
}

int main(int argc, char **argv)
{
    const char *elf = NULL, *data_dir = NULL, *out = NULL, *args = NULL, *app_file = NULL;
    uint64_t max_frames = 0, max_insns = 400000000000ull, ppm_start = 0;
    int profile = 0, ppm_every = 0, gpu_model = 1, echo = 0, verbose = 0, quiet = 0, normalize_date = 1;
    int gpu_sched = GPU_SCHED_EAGER, mem_mode = ENV_MEM_FLAT, check_invariant = 0;
    uint32_t heap_override = 0;
    size_t top = 80;
    const char *gpuvec = NULL;
    int coverage = 0;
    const char *label = "";
    env_opts eo;
    memset(&eo, 0, sizeof eo);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEXTARG() (i + 1 < argc ? argv[++i] : (usage(), exit(2), (char *)0))
        if (!strcmp(a, "--data-dir") || !strcmp(a, "--pak-dir")) data_dir = NEXTARG();
        else if (!strcmp(a, "--app")) app_file = NEXTARG();
        else if (!strcmp(a, "--out")) out = NEXTARG();
        else if (!strcmp(a, "--args")) args = NEXTARG();
        else if (!strcmp(a, "--frames")) max_frames = strtoull(NEXTARG(), 0, 0);
        else if (!strcmp(a, "--profile")) profile = 1;
        else if (!strcmp(a, "--ppm-every")) ppm_every = atoi(NEXTARG());
        else if (!strcmp(a, "--ppm-start")) ppm_start = strtoull(NEXTARG(), 0, 0);
        else if (!strcmp(a, "--no-gpu-model")) gpu_model = 0;
        else if (!strcmp(a, "--gpuvec")) gpuvec = NEXTARG();
        else if (!strcmp(a, "--no-normalize-build-stamp") || !strcmp(a, "--no-normalize-exe-date")) normalize_date = 0;
        else if (!strcmp(a, "--gpu-sched")) {
            const char *v = NEXTARG();
            if (!strcmp(v, "eager")) gpu_sched = GPU_SCHED_EAGER;
            else if (!strcmp(v, "lazy")) gpu_sched = GPU_SCHED_LAZY;
            else if (!strcmp(v, "drain")) gpu_sched = GPU_SCHED_DRAIN;
            else { fprintf(stderr, "unknown --gpu-sched %s\n", v); usage(); return 2; }
        }
        else if (!strcmp(a, "--check-strict-invariant")) check_invariant = 1;
        else if (!strcmp(a, "--mem")) {
            const char *v = NEXTARG();
            if (!strcmp(v, "flat")) mem_mode = ENV_MEM_FLAT;
            else if (!strcmp(v, "lint")) mem_mode = ENV_MEM_LINT;
            else if (!strcmp(v, "strict")) mem_mode = ENV_MEM_STRICT;
            else { fprintf(stderr, "unknown --mem %s\n", v); usage(); return 2; }
        }
        else if (!strcmp(a, "--max-insns")) max_insns = (uint64_t)strtod(NEXTARG(), 0);
        else if (!strcmp(a, "--heap-size")) heap_override = (uint32_t)strtoul(NEXTARG(), 0, 0);
        else if (!strcmp(a, "--file")) {
            char *v = NEXTARG(), *eq = strchr(v, '=');
            if (!eq || eq == v || !eq[1] || eo.n_extra_files == ENV_MAX_EXTRA_FILES) {
                fprintf(stderr, "qsim: bad or too many --file %s (NAME=PATH, at most %d)\n", v, ENV_MAX_EXTRA_FILES);
                return 2;
            }
            *eq = 0;
            eo.extra_name[eo.n_extra_files] = v;
            eo.extra_path[eo.n_extra_files++] = eq + 1;
        }
        else if (!strcmp(a, "--end-on")) eo.end_marker = NEXTARG();
        else if (!strcmp(a, "--window-on")) eo.window_marker = NEXTARG();
        else if (!strcmp(a, "--window-end-on")) eo.window_end_marker = NEXTARG();
        else if (!strcmp(a, "--seed-mem")) eo.seed_mem = strtoull(NEXTARG(), 0, 0);
        else if (!strcmp(a, "--coverage")) { coverage = 1; profile = 1; }
        else if (!strcmp(a, "--label")) label = NEXTARG();
        else if (!strcmp(a, "--top")) top = (size_t)atoi(NEXTARG());
        else if (!strcmp(a, "--echo")) echo = 1;
        else if (!strcmp(a, "--quiet")) quiet = 1;
        else if (!strcmp(a, "--verbose")) verbose = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
        else elf = a;
    }
    if (!elf || !out) { usage(); return 2; }

    static app_profile app;
    if (app_file) { if (app_load(&app, app_file)) return 2; }
    else app_default(&app);
    if (!args) args = app.default_args ? app.default_args : "";
    if (gpuvec && (gpu_sched != GPU_SCHED_EAGER || mem_mode == ENV_MEM_STRICT)) {
        fprintf(stderr, "qsim: --gpuvec needs --gpu-sched eager and flat or lint memory\n");
        return 2;
    }
    if (mkdir(out, 0777) && errno != EEXIST) { perror(out); return 2; }

    /* argv: the profile's elf_name + whitespace-split --args (ENV_SPEC 7.1) */
    char *argbuf = strdup(args);
    char *gargv[33];
    int gargc = 0;
    gargv[gargc++] = app.elf_name;
    for (char *t = strtok(argbuf, " \t"); t && gargc < 32; t = strtok(NULL, " \t")) gargv[gargc++] = t;
    gargv[gargc] = NULL;

    char path[4096];
    snprintf(path, sizeof path, "%s/console.log", out);
    FILE *console = fopen(path, "w");
    static qenv E;
    E.normalize_exe_date = normalize_date;
    E.gpu_sched = gpu_sched;
    E.mem_mode = mem_mode;
    eo.normalize_exe_date = normalize_date;
    eo.gpu_sched = gpu_sched;
    eo.mem_mode = mem_mode;
    E.opt = eo;
    if (gpuvec && gpu_model) {
        static char vecdir[4096];
        snprintf(vecdir, sizeof vecdir, "%s/gpuvec", out);
        mkdir(vecdir, 0777);
        E.vec_dir = vecdir;
        char *list = strdup(gpuvec);
        for (char *t = strtok(list, ","); t && E.vec_nframes < 256; t = strtok(NULL, ","))
            E.vec_frames[E.vec_nframes++] = strtoull(t, 0, 0);
        free(list);
    }
    if (env_init(&E, elf, &app, data_dir, gargc, gargv, gpu_model, heap_override, console)) {
        fprintf(stderr, "qsim: environment setup failed\n");
        return 1;
    }
    E.echo = echo;
    E.verbose = verbose;
    E.check_invariant = check_invariant;
    if (ppm_every > 0) {
        static char ppmdir[4096];
        snprintf(ppmdir, sizeof ppmdir, "%s/ppm", out);
        mkdir(ppmdir, 0777);
        E.ppm_dir = ppmdir;
        E.ppm_every = ppm_every;
        E.ppm_start = ppm_start;
    }
    if (profile) {
        if (qsim_enable_profiling(E.cpu, NULL)) { fprintf(stderr, "qsim: profiling init failed\n"); return 1; }
        n_sf = 0;
        sf_idx = calloc(E.syms.n + 1, sizeof *sf_idx);
        for (size_t i = 0; i < E.syms.n; i++) {
            const qsim_sym *s = &E.syms.s[i];
            if (s->type != 2 || !is_softfloat(s->name)) continue;
            int ok; size_t ix = arena_index(E.cpu, s->addr, &ok);
            if (!ok) continue;
            /* libgcc aliases share one entry point (__ltdf2/__ledf2, __gtdf2/__gedf2,
             * ...): count each address once (profile anomaly A1) */
            size_t k = 0;
            while (k < n_sf && sf_idx[k] != ix) k++;
            if (k == n_sf) sf_idx[n_sf++] = ix;
        }
    }
    char sha[65] = "?";
    sha256_file(elf, sha);

    snprintf(path, sizeof path, "%s/frames.csv", out);
    FILE *csv = fopen(path, "w");
    if (!csv) { perror(path); return 1; }
    fprintf(csv, "frame,host_framecount,buffer,insns,est_cycles,icache_miss_sdram,icache_miss_bram,dcache_miss,"
                 "mispredict,softfloat_calls,gpu_cmd_words,gpu_pixels,hash_cmd,hash_px,hash_z,hash_pal,"
                 "mem_hazards,gpu_barriers,gpu_wait_px\n");

    size_t cap_rows = 4096, nrows = 0;
    uint64_t *cols[M_N];
    for (int mi = 0; mi < M_N; mi++) cols[mi] = malloc(cap_rows * sizeof(uint64_t));
    uint32_t *hfc_rows = malloc(cap_rows * sizeof(uint32_t));
    uint64_t oracle_px = FNV64_INIT, oracle_z = FNV64_INIT, oracle_pal = FNV64_INIT, oracle_cmd = FNV64_INIT;
    counters prev, now, first_frame = {0};
    read_counters(&E, &prev);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const char *stop_reason = "?";
    uint64_t last_frame_insn = 0;

    for (;;) {
        uint64_t left = max_insns > E.cpu->instret ? max_insns - E.cpu->instret : 0;
        if (!left) { stop_reason = "instruction limit"; break; }
        int r = qsim_run(E.cpu, left < 200000000ull ? left : 200000000ull);
        /* Run ends (timedemo result line, guest exit): a lazy schedule may still
         * hold the last FLIPs; let the GPU finish so every schedule reports the
         * same frames. */
        if ((E.done == ENV_DONE_TIMEDEMO || E.done == ENV_DONE_EXIT || E.done == ENV_DONE_MARKER) && E.gpu)
            gpu_drain(E.gpu, -1);
        if (E.npend) {
            read_counters(&E, &now);
            for (int k = 0; k < E.npend; k++) {
                env_frame_rec *fr = &E.pend[k];
                counters d = {0};
                if (k == 0) {
                    d.insns = now.insns - prev.insns; d.cycles = now.cycles - prev.cycles;
                    d.ic_sd = now.ic_sd - prev.ic_sd; d.ic_br = now.ic_br - prev.ic_br;
                    d.dc = now.dc - prev.dc; d.bp = now.bp - prev.bp; d.sf = now.sf - prev.sf;
                    d.gpx = now.gpx - prev.gpx;
                    d.hazards = now.hazards - prev.hazards; d.barriers = now.barriers - prev.barriers;
                    d.obs_px = now.obs_px - prev.obs_px;
                }
                if (nrows == 0) first_frame = now;
                fprintf(csv, "%" PRIu64 ",%u,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                             ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64
                             ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                        fr->seq, fr->host_framecount, fr->idx, d.insns, d.cycles, d.ic_sd, d.ic_br, d.dc, d.bp, d.sf,
                        fr->cmd_words, d.gpx, fr->hash_cmd, fr->hash_px, fr->hash_z, fr->hash_pal,
                        d.hazards, d.barriers, d.obs_px);
                uint8_t hb[8];
                memcpy(hb, &fr->hash_px, 8); oracle_px = fnv1a64(oracle_px, hb, 8);
                memcpy(hb, &fr->hash_z, 8); oracle_z = fnv1a64(oracle_z, hb, 8);
                memcpy(hb, &fr->hash_pal, 8); oracle_pal = fnv1a64(oracle_pal, hb, 8);
                memcpy(hb, &fr->hash_cmd, 8); oracle_cmd = fnv1a64(oracle_cmd, hb, 8);
                if (nrows == cap_rows) {
                    cap_rows *= 2;
                    for (int mi = 0; mi < M_N; mi++) cols[mi] = realloc(cols[mi], cap_rows * sizeof(uint64_t));
                    hfc_rows = realloc(hfc_rows, cap_rows * sizeof(uint32_t));
                }
                hfc_rows[nrows] = fr->host_framecount;
                cols[M_INSNS][nrows] = d.insns; cols[M_CYC][nrows] = d.cycles;
                cols[M_IC][nrows] = d.ic_sd + d.ic_br; cols[M_DC][nrows] = d.dc; cols[M_BP][nrows] = d.bp;
                cols[M_SF][nrows] = d.sf; cols[M_WORDS][nrows] = fr->cmd_words; cols[M_GPX][nrows] = d.gpx;
                cols[M_HAZ][nrows] = d.hazards; cols[M_BAR][nrows] = d.barriers; cols[M_WPX][nrows] = d.obs_px;
                nrows++;
            }
            E.npend = 0;
            prev = now;
            last_frame_insn = E.cpu->instret;
            if (!quiet && (nrows % 100 == 0 || E.done)) {
                clock_gettime(CLOCK_MONOTONIC, &t1);
                double el = (double)(t1.tv_sec - t0.tv_sec) + 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec);
                fprintf(stderr, "qsim: frame %zu  %.2fG insns  %.0f MIPS\n", nrows,
                        (double)E.cpu->instret / 1e9, (double)E.cpu->instret / 1e6 / (el > 0 ? el : 1));
            }
        }
        if (E.done) { stop_reason = E.done_msg; break; }
        if (r == QSIM_RUN_TRAP) { stop_reason = E.done_msg[0] ? E.done_msg : "trap"; if (!E.done) E.done = 2; break; }
        if (max_frames && nrows >= max_frames) { stop_reason = "frame limit"; break; }
        if (E.cpu->instret - last_frame_insn > 20000000000ull) {
            stop_reason = "watchdog: no presented frame for 20e9 instructions";
            E.done = 2;
            break;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double wall = (double)(t1.tv_sec - t0.tv_sec) + 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec);
    fprintf(stderr, "qsim: stop: %s\n", stop_reason);
    fclose(csv);
    if (console) fclose(console);
    read_counters(&E, &now);

    /* Result-line window: the app profile's result_line reports N host
     * frames (Quake: CL_FinishTimeDemo counts the host frames after
     * td_startframe+1).  In steady-state playback there is exactly one flip
     * per host frame and none after the result line, so these are the last N
     * rows.  summary.json keeps the historical key names. */
    uint64_t td_frames = E.result_frames;
    if (td_frames > nrows) td_frames = nrows;
    size_t win_first = nrows - td_frames, win_n = td_frames;
    const char *win_kind = td_frames ? "timedemo" : "none";
    /* --window-on: frames published in or after the host frame that printed the
     * marker (host_framecount at publish, so every GPU schedule agrees) */
    if (E.opt.window_marker) {
        win_kind = "marker";
        win_first = nrows;
        if (E.window_seen)
            for (size_t i = 0; i < nrows; i++) if (hfc_rows[i] >= E.window_hfc) { win_first = i; break; }
        size_t win_end = nrows;
        if (E.window_end_seen)
            for (size_t i = win_first; i < nrows; i++) if (hfc_rows[i] >= E.window_end_hfc) { win_end = i; break; }
        win_n = win_end - win_first;
    }

    snprintf(path, sizeof path, "%s/summary.json", out);
    FILE *js = fopen(path, "w");
    fprintf(js, "{\n  \"workload\": ");
    json_str(js, label);
    fprintf(js, ",\n  \"elf\": ");
    json_str(js, elf);
    {
        char *rp = realpath(elf, NULL);
        fprintf(js, ",\n  \"elf_abspath\": ");
        json_str(js, rp ? rp : elf);
        free(rp);
    }
    fprintf(js, ",\n  \"elf_sha256\": \"%s\",\n  \"argv\": [", sha);
    for (int i = 0; i < gargc; i++) { if (i) fputs(", ", js); json_str(js, gargv[i]); }
    fprintf(js, "],\n  \"app\": ");
    json_str(js, app.name);
    fprintf(js, ",\n  \"app_profile\": ");
    json_str(js, app.path);
    fprintf(js, ",\n  \"data_dir\": ");
    json_str(js, data_dir ? data_dir : "");
    fprintf(js, ",\n  \"profile\": %s,\n  \"gpu_model\": %s,\n  \"gpu_sched\": \"%s\",\n  \"mem_mode\": \"%s\","
                "\n  \"status\": \"%s\",\n  \"stop_reason\": ",
            profile ? "true" : "false", gpu_model ? "true" : "false", gpu_sched_name(gpu_sched),
            mem_mode == ENV_MEM_STRICT ? "strict" : mem_mode == ENV_MEM_LINT ? "lint" : "flat",
            E.done == ENV_DONE_ERROR ? "error" : E.done == ENV_DONE_TIMEDEMO ? "timedemo_complete" :
            E.done == ENV_DONE_EXIT ? "exit" : E.done == ENV_DONE_MARKER ? "workload_complete" : "limit");
    json_str(js, stop_reason);
    fprintf(js, ",\n  \"timedemo_result\": ");       /* historical key: the profile's result line */
    json_str(js, E.timedemo_line);
    fprintf(js, ",\n  \"parity\": {");
    for (int i = 0; i < app.nparity; i++) {
        if (i) fputs(", ", js);
        json_str(js, app.parity[i].name);
        fputs(": ", js);
        if (E.parity_seen[i]) json_str(js, E.parity_val[i]); else fputs("null", js);
    }
    fputs("}", js);
    fprintf(js, ",\n  \"frames\": %zu,\n  \"kernel_flips\": %" PRIu64 ",\n  \"palette_sets\": %" PRIu64 ",\n",
            nrows, E.kernel_flips, E.palette_sets);
    fprintf(js, "  \"heap_size\": %u,\n  \"heap_size_pinned\": %s,\n  \"bss_end\": \"0x%08x\",\n  \"exe_dates_normalized\": %d,\n",
            heap_override ? heap_override : ENV_MMAP_TOP - E.bss_end, heap_override ? "true" : "false", E.bss_end,
            E.exe_dates_normalized);
    fprintf(js, "  \"seed_mem\": %" PRIu64 ",\n  \"window\": {\"kind\": \"%s\", \"first_frame\": %zu, \"frames\": %zu",
            E.opt.seed_mem, win_kind, win_first, win_n);
    if (E.opt.window_marker) {
        fprintf(js, ", \"marker\": ");
        json_str(js, E.opt.window_marker);
        fprintf(js, ", \"marker_seen\": %s, \"marker_host_framecount\": %u", E.window_seen ? "true" : "false",
                E.window_hfc);
        if (E.opt.window_end_marker) {
            fprintf(js, ", \"end_marker\": ");
            json_str(js, E.opt.window_end_marker);
            fprintf(js, ", \"end_marker_seen\": %s, \"end_marker_host_framecount\": %u",
                    E.window_end_seen ? "true" : "false", E.window_end_hfc);
        }
    }
    fprintf(js, "},\n  \"end_marker\": ");
    json_str(js, E.opt.end_marker ? E.opt.end_marker : "");
    fprintf(js, ",\n  \"extra_files\": [");
    for (int i = 0; i < E.opt.n_extra_files; i++) {
        char sh[65] = "?";
        sha256_file(E.opt.extra_path[i], sh);
        fprintf(js, "%s{\"name\": ", i ? ", " : "");
        json_str(js, E.opt.extra_name[i]);
        fprintf(js, ", \"sha256\": \"%s\"}", sh);
    }
    fprintf(js, "],\n");
    fprintf(js, "  \"wall_seconds\": %.3f,\n  \"guest_mips\": %.1f,\n", wall, (double)now.insns / 1e6 / (wall > 0 ? wall : 1));
    fprintf(js, "  \"virtual_clock\": {\"advances\": %" PRIu64 ", \"final_us\": %" PRIu64 ", \"quantum_us\": %" PRIu64 "},\n",
            E.clk_advances, E.vt_us, E.vt_quantum);
    fprintf(js, "  \"env\": {\"service_calls\": %" PRIu64 ", \"sysreg_reads\": %" PRIu64 ", \"term_fb_writes\": %" PRIu64
                ", \"unmapped_mmio\": %" PRIu64 ", \"gpuvec_captured\": %d},\n", E.svc_total, E.sysreg_reads,
            E.term_fb_writes, E.unknown_mmio, E.vec_captured);
    fprintf(js, "  \"totals\": {\"insns\": %" PRIu64 ", \"est_cycles\": %" PRIu64 ", \"icache_miss_sdram\": %" PRIu64
                ", \"icache_miss_bram\": %" PRIu64 ", \"dcache_miss\": %" PRIu64 ", \"mispredict\": %" PRIu64
                ", \"softfloat_calls\": %" PRIu64 ", \"gpu_cmd_words\": %" PRIu64 ", \"gpu_pixels\": %" PRIu64 "},\n",
            now.insns, now.cycles, now.ic_sd, now.ic_br, now.dc, now.bp, now.sf, now.words, now.gpx);
    fprintf(js, "  \"boot_until_first_frame\": {\"insns\": %" PRIu64 ", \"est_cycles\": %" PRIu64 "},\n",
            first_frame.insns, first_frame.cycles);
    window_json(js, "per_frame_all", cols, 0, nrows, profile, 0);
    /* key kept for compatibility: the timedemo frames, or the --window-on window */
    window_json(js, "per_frame_timedemo_window", cols, win_first, win_n, profile, 0);
    fprintf(js, "  \"oracle\": {\"hash_px_all\": \"%016" PRIx64 "\", \"hash_z_all\": \"%016" PRIx64
                "\", \"hash_pal_all\": \"%016" PRIx64 "\", \"hash_cmd_all\": \"%016" PRIx64 "\"}",
            oracle_px, oracle_z, oracle_pal, oracle_cmd);
    if (E.gpu) {
        const gpu_stats_t *gs = gpu_get_stats(E.gpu);
        fprintf(js, ",\n  \"gpu_sync\": {\"publishes\": %" PRIu64 ", \"published_words\": %" PRIu64
                    ", \"observations\": {\"fence_read\": %" PRIu64 ", \"rdptr_read\": %" PRIu64 ", \"kernel_wait\": %" PRIu64
                    "}, \"barriers\": {\"fence_read\": %" PRIu64 ", \"rdptr_read\": %" PRIu64 ", \"kernel_wait\": %" PRIu64
                    "}, \"barrier_px\": %" PRIu64 ", \"wait_px\": %" PRIu64 ", \"wait_words\": %" PRIu64
                    ", \"queue_max_words\": %" PRIu64 ", \"queued_words_at_end\": %u, \"ring_overflows\": %" PRIu64 "}",
                gs->publishes, gs->published_words, gs->observations[GPU_OBS_FENCE], gs->observations[GPU_OBS_RDPTR],
                gs->observations[GPU_OBS_WAIT], gs->barriers[GPU_OBS_FENCE], gs->barriers[GPU_OBS_RDPTR],
                gs->barriers[GPU_OBS_WAIT], gs->barrier_px, gs->obs_px, gs->obs_words, gs->queue_max_words,
                gpu_queued_words(E.gpu), gs->ring_overflows);
    }
    if (E.lint) {
        fprintf(js, ",\n  \"memory_coherency\": {\n    \"mode\": \"%s\",\n", E.lint->strict ? "strict" : "lint");
        lint_json(E.lint, js, "    ");
        /* hazard sites: (class, function) is layout-independent, unlike the counts */
        {
            qsim_lint *L = E.lint;
            uint32_t order[LINT_MAX_SITES];
            uint32_t n = 0;
            for (uint32_t i = 0; i < LINT_MAX_SITES; i++) if (L->site[i].used) order[n++] = i;
            for (uint32_t i = 1; i < n; i++) {           /* insertion sort by count, descending */
                uint32_t v = order[i], k = i;
                while (k && L->site[order[k - 1]].n < L->site[v].n) { order[k] = order[k - 1]; k--; }
                order[k] = v;
            }
            fprintf(js, ",\n    \"sites\": [");
            for (uint32_t i = 0; i < n; i++) {
                uint32_t k = order[i];
                const qsim_sym *sy = qsim_syms_func(&E.syms, L->site[k].pc);
                fprintf(js, "%s\n      {\"class\": \"%s\", \"func\": ", i ? "," : "",
                        lint_class_names[L->site[k].cls]);
                json_str(js, sy ? sy->name : "?");
                fprintf(js, ", \"offset\": %u, \"pc\": \"0x%08x\", \"count\": %" PRIu64 "}",
                        sy ? L->site[k].pc - sy->addr : 0u, L->site[k].pc, L->site[k].n);
            }
            fprintf(js, "%s],\n    \"sites_dropped\": %u", n ? "\n    " : "", L->sites_dropped);
        }
        fprintf(js, ",\n    \"kernel_uncached_stores\": %" PRIu64, E.env_uc_writes);
        if (E.check_invariant) fprintf(js, ",\n    \"invariant_errors\": %" PRIu64, E.invariant_errors);
        fprintf(js, "\n  }");
    }
    fprintf(js, ",\n  \"fpu_subnormal\": {\"operands\": %" PRIu64 ", \"results\": %" PRIu64 "}",
            E.cpu->fsub_in, E.cpu->fsub_out);
    if (E.cpu->model) {
        qsim_model *m = E.cpu->model;
        fprintf(js, ",\n  \"model_penalties\": {\"icache\": %" PRIu64 ", \"dcache\": %" PRIu64 ", \"store_buffer\": %" PRIu64
                    ", \"uncached_io\": %" PRIu64 ", \"mispredict\": %" PRIu64 ", \"btb_taken\": %" PRIu64
                    ", \"dependency_stalls\": %" PRIu64 ", \"div\": %" PRIu64 ", \"fdiv\": %" PRIu64 ", \"fsqrt\": %" PRIu64
                    ", \"f2i\": %" PRIu64 ", \"misaligned\": %" PRIu64 "}",
                m->pen_icache, m->pen_dcache, m->pen_sb, m->pen_uncached, m->pen_mispredict, m->pen_btb_taken,
                m->pen_dep, m->pen_div, m->pen_fdiv, m->pen_fsqrt, m->pen_f2i, m->n_misaligned);
    }
    fprintf(js, "\n}\n");
    fclose(js);

    if (profile) {
        profile_t P;
        build_profile(&E, &P);
        char tsv[4096];
        snprintf(path, sizeof path, "%s/profile.txt", out);
        snprintf(tsv, sizeof tsv, "%s/profile.tsv", out);
        write_profile(&E, &P, path, tsv, top, sha);
        free(P.rows); free(P.edges);
    }
    if (coverage) {
        snprintf(path, sizeof path, "%s/coverage.tsv", out);
        write_coverage(&E, path, sha);
    }
    if (E.lint) {
        fprintf(stderr, "qsim: memory coherency (%s): %" PRIu64 " hazards (worst case %" PRIu64 ")\n",
                E.lint->strict ? "strict" : "lint", lint_total_aware(E.lint), lint_total_wc(E.lint));
        if (!quiet || lint_total_aware(E.lint)) lint_print(E.lint, stderr);
    }
    fprintf(stderr, "qsim: %zu frames, %.3f G insns, %.1f s wall, %.0f MIPS -> %s\n", nrows,
            (double)now.insns / 1e9, wall, (double)now.insns / 1e6 / (wall > 0 ? wall : 1), out);
    int rc = E.done == ENV_DONE_ERROR ? 1 : 0;
    env_free(&E);
    free(argbuf);
    for (int mi = 0; mi < M_N; mi++) free(cols[mi]);
    free(hfc_rows);
    return rc;
}
