//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/mman.h>
#include "prof.h"

static void *map_zero(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

qsim_prof *qsim_prof_new(size_t entries)
{
    qsim_prof *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->n = entries;
    p->cnt = map_zero(entries * sizeof *p->cnt);
    p->pen = map_zero(entries * sizeof *p->pen);
    p->calls = map_zero(entries * sizeof *p->calls);
    p->tails = map_zero(entries * sizeof *p->tails);
    p->ev_ic = map_zero(entries * sizeof *p->ev_ic);
    p->ev_dc = map_zero(entries * sizeof *p->ev_dc);
    p->ev_bp = map_zero(entries * sizeof *p->ev_bp);
    p->site_n = map_zero(entries * sizeof *p->site_n);
    p->site_tgt = map_zero(entries * sizeof *p->site_tgt);
    if (!p->cnt || !p->pen || !p->calls || !p->tails || !p->ev_ic || !p->ev_dc || !p->ev_bp ||
        !p->site_n || !p->site_tgt) { qsim_prof_free(p); return NULL; }
    return p;
}

void qsim_prof_free(qsim_prof *p)
{
    if (!p) return;
    if (p->cnt) munmap(p->cnt, p->n * sizeof *p->cnt);
    if (p->pen) munmap(p->pen, p->n * sizeof *p->pen);
    if (p->calls) munmap(p->calls, p->n * sizeof *p->calls);
    if (p->tails) munmap(p->tails, p->n * sizeof *p->tails);
    if (p->ev_ic) munmap(p->ev_ic, p->n * sizeof *p->ev_ic);
    if (p->ev_dc) munmap(p->ev_dc, p->n * sizeof *p->ev_dc);
    if (p->ev_bp) munmap(p->ev_bp, p->n * sizeof *p->ev_bp);
    if (p->site_n) munmap(p->site_n, p->n * sizeof *p->site_n);
    if (p->site_tgt) munmap(p->site_tgt, p->n * sizeof *p->site_tgt);
    free(p);
}

void qsim_prof_clear(qsim_prof *p)
{
    madvise(p->cnt, p->n * sizeof *p->cnt, MADV_DONTNEED);
    madvise(p->pen, p->n * sizeof *p->pen, MADV_DONTNEED);
    madvise(p->calls, p->n * sizeof *p->calls, MADV_DONTNEED);
    madvise(p->tails, p->n * sizeof *p->tails, MADV_DONTNEED);
    madvise(p->ev_ic, p->n * sizeof *p->ev_ic, MADV_DONTNEED);
    madvise(p->ev_dc, p->n * sizeof *p->ev_dc, MADV_DONTNEED);
    madvise(p->ev_bp, p->n * sizeof *p->ev_bp, MADV_DONTNEED);
    madvise(p->site_n, p->n * sizeof *p->site_n, MADV_DONTNEED);
    madvise(p->site_tgt, p->n * sizeof *p->site_tgt, MADV_DONTNEED);
    p->last_i = 0;
}

static int insn_at(qsim_cpu *cpu, uint32_t pc, uint32_t *w)
{
    uint8_t *b = qsim_mem_ptr(cpu, pc, 2);
    if (!b) return -1;
    uint32_t lo = (uint32_t)b[0] | (uint32_t)b[1] << 8;
    if ((lo & 3) == 3) {
        uint8_t *c = qsim_mem_ptr(cpu, pc, 4);
        if (!c) return -1;
        lo |= (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24;
    }
    *w = lo;
    return 0;
}

typedef struct { qsim_func_prof *rows; size_t n, cap; } rowvec;

static qsim_func_prof *row_for(rowvec *v, size_t *map, size_t key)
{
    if (map[key] == 0) {
        if (v->n == v->cap) {
            v->cap = v->cap ? v->cap * 2 : 256;
            v->rows = realloc(v->rows, v->cap * sizeof *v->rows);
        }
        memset(&v->rows[v->n], 0, sizeof v->rows[0]);
        map[key] = ++v->n;
    }
    return &v->rows[map[key] - 1];
}

static void scan_region(qsim_cpu *cpu, const qsim_syms *syms, qsim_profile *out, rowvec *v,
                        size_t *map, size_t nkeys, uint32_t base, size_t first, uint32_t lo, uint32_t hi)
{
    qsim_prof *P = cpu->prof;
    for (uint32_t off = lo & ~1u; off < hi; off += 2) {
        size_t i = first + (off >> 1);
        uint64_t c = P->cnt[i], pn = P->pen[i];
        uint64_t cl = P->calls[i], tl = P->tails[i];
        if (!c && !pn && !cl && !tl) continue;
        uint32_t pc = base + off;
        const qsim_sym *s = syms ? qsim_syms_func(syms, pc) : NULL;
        size_t key;
        qsim_func_prof *r;
        if (s) {
            key = (size_t)(s - syms->s);
            r = row_for(v, map, key);
            r->name = s->name; r->addr = s->addr; r->size = s->size;
        } else {
            key = nkeys - 1;                       /* shared "unknown" bucket */
            r = row_for(v, map, key);
            r->name = "<unknown>";
            out->unattributed_insns += c;
        }
        r->insns += c;
        r->cycles += c + pn;
        if (!s || pc == s->addr) { r->calls += cl; r->tailcalls += tl; }
        uint32_t w;
        if (c && insn_at(cpu, pc, &w) == 0) {
            int oc = qsim_opclass_of(w);
            r->oc[oc] += c;
            out->oc[oc] += c;
        }
        out->total_insns += c;
        out->total_cycles += c + pn;
    }
}

int qsim_profile_build(qsim_cpu *cpu, const qsim_syms *syms, qsim_profile *out)
{
    memset(out, 0, sizeof *out);
    if (!cpu->prof) return -1;
    size_t nkeys = (syms ? syms->n : 0) + 1;
    size_t *map = calloc(nkeys, sizeof *map);
    rowvec v = {0};
    size_t br_first = 0;
    size_t sd_first = (size_t)(cpu->sd_dc - cpu->dc_base);
    scan_region(cpu, syms, out, &v, map, nkeys, 0, br_first, 0, cpu->br_code_hi);
    if (cpu->code_hi > cpu->code_lo) {
        /* jump targets can lie outside decoded code only if they faulted; scan
         * the decoded range plus a margin */
        uint32_t lo = cpu->code_lo, hi = cpu->code_hi + 64;
        if (hi > cpu->sdram_size) hi = cpu->sdram_size;
        scan_region(cpu, syms, out, &v, map, nkeys, cpu->sdram_base, sd_first, lo, hi);
    }
    free(map);
    out->f = v.rows;
    out->n = v.n;
    return 0;
}

void qsim_profile_free(qsim_profile *p)
{
    free(p->f);
    memset(p, 0, sizeof *p);
}

static int by_insns(const void *a, const void *b)
{
    const qsim_func_prof *x = a, *y = b;
    return x->insns < y->insns ? 1 : x->insns > y->insns ? -1 : 0;
}

static int is_softfloat(const char *n)
{
    static const char *pre[] = {"__add", "__sub", "__mul", "__div", "__neg", "__eq", "__ne", "__lt",
        "__le", "__gt", "__ge", "__unord", "__fix", "__float", "__extend", "__trunc", "__cmp", "__pow", 0};
    if (strncmp(n, "__", 2)) return 0;
    size_t L = strlen(n);
    if (L < 7 || L > 18) return 0;
    const char *t = n + L - 4;
    if (!strstr(t, "df") && !strstr(t, "sf") && !strstr(t, "tf") && !strstr(n + L - 5, "df") &&
        !strstr(n + L - 5, "sf") && !strstr(n + L - 5, "tf"))
        return 0;
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
    if (!strncmp(n, "__sin", 5) || !strncmp(n, "__cos", 5) || !strncmp(n, "__tan", 5) ||
        !strncmp(n, "__rem_pio2", 10) || !strncmp(n, "__math_", 7) || !strncmp(n, "__expo2", 7))
        return 1;
    return 0;
}

void qsim_profile_print(const qsim_profile *p, FILE *o, size_t top)
{
    qsim_func_prof *rows = malloc((p->n + 1) * sizeof *rows);
    memcpy(rows, p->f, p->n * sizeof *rows);
    qsort(rows, p->n, sizeof *rows, by_insns);
    fprintf(o, "profile: %" PRIu64 " instructions, %" PRIu64 " est. cycles (%.3f cyc/insn), %" PRIu64
               " unattributed\n", p->total_insns, p->total_cycles,
            p->total_insns ? (double)p->total_cycles / (double)p->total_insns : 0.0, p->unattributed_insns);
    fprintf(o, "op classes:");
    for (int i = 0; i < QSIM_OC_COUNT; i++)
        if (p->oc[i]) fprintf(o, " %s=%" PRIu64, qsim_opclass_name(i), p->oc[i]);
    fputc('\n', o);
    fprintf(o, "%-32s %14s %6s %14s %6s %11s %10s %9s %9s %8s %8s\n", "function", "self_insns", "%",
            "self_cycles", "cyc/i", "calls", "tailcalls", "mul", "div", "fdiv", "fsqrt");
    for (size_t i = 0; i < p->n && i < top; i++) {
        qsim_func_prof *r = &rows[i];
        fprintf(o, "%-32.32s %14" PRIu64 " %6.2f %14" PRIu64 " %6.2f %11" PRIu64 " %10" PRIu64
                   " %9" PRIu64 " %9" PRIu64 " %8" PRIu64 " %8" PRIu64 "\n",
                r->name, r->insns, p->total_insns ? 100.0 * (double)r->insns / (double)p->total_insns : 0.0,
                r->cycles, r->insns ? (double)r->cycles / (double)r->insns : 0.0, r->calls, r->tailcalls,
                r->oc[QSIM_OC_MUL], r->oc[QSIM_OC_DIV], r->oc[QSIM_OC_FDIV], r->oc[QSIM_OC_FSQRT]);
    }
    fprintf(o, "soft-float helper calls:\n");
    for (size_t i = 0; i < p->n; i++)
        if (is_softfloat(rows[i].name) && (rows[i].calls || rows[i].tailcalls || rows[i].insns))
            fprintf(o, "  %-24s calls %11" PRIu64 " tail %9" PRIu64 " insns %14" PRIu64 "\n",
                    rows[i].name, rows[i].calls, rows[i].tailcalls, rows[i].insns);
    fprintf(o, "libm calls:\n");
    for (size_t i = 0; i < p->n; i++)
        if (is_libm(rows[i].name) && (rows[i].calls || rows[i].tailcalls || rows[i].insns))
            fprintf(o, "  %-24s calls %11" PRIu64 " tail %9" PRIu64 " insns %14" PRIu64 "\n",
                    rows[i].name, rows[i].calls, rows[i].tailcalls, rows[i].insns);
    free(rows);
}

int qsim_profile_write_tsv(const qsim_profile *p, const char *path)
{
    FILE *o = fopen(path, "w");
    if (!o) return -1;
    fprintf(o, "function\taddr\tsize\tself_insns\tself_cycles\tcalls\ttailcalls");
    for (int i = 0; i < QSIM_OC_COUNT; i++) fprintf(o, "\t%s", qsim_opclass_name(i));
    fputc('\n', o);
    for (size_t i = 0; i < p->n; i++) {
        const qsim_func_prof *r = &p->f[i];
        fprintf(o, "%s\t0x%08x\t%u\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64, r->name, r->addr,
                r->size, r->insns, r->cycles, r->calls, r->tailcalls);
        for (int k = 0; k < QSIM_OC_COUNT; k++) fprintf(o, "\t%" PRIu64, r->oc[k]);
        fputc('\n', o);
    }
    fclose(o);
    return 0;
}
