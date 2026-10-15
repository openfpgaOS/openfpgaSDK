//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * qsim per-PC profile: counters parallel to the decode arena (one slot per 2
 * bytes of BRAM+SDRAM, MAP_NORESERVE so only executed pages are committed).
 */
#ifndef QSIM_PROF_H
#define QSIM_PROF_H
#include <stdint.h>
#include <stdio.h>
#include "cpu.h"
#include "qelf.h"

typedef struct qsim_prof {
    size_t    n;
    uint64_t *cnt;       /* instructions executed at this pc */
    uint64_t *pen;       /* model penalty cycles attributed to this pc */
    uint64_t *calls;     /* linking jal/jalr landing here (rd = ra/t0) */
    uint64_t *tails;     /* non-linking, non-return jumps landing here */
    /* per-PC model events (integration extension): the timing model bumps
     * these at the index of the instruction that caused the event */
    uint64_t *ev_ic;     /* I-cache line misses (fetch) */
    uint64_t *ev_dc;     /* D-cache load/store misses */
    uint64_t *ev_bp;     /* branch/jump mispredicts */
    /* per call-site records: jumps (linked calls and non-return jumps)
     * leaving this pc, and the arena index of the last target */
    uint64_t *site_n;
    uint32_t *site_tgt;
    size_t    last_i;
    uint64_t  last_cyc;
} qsim_prof;

qsim_prof *qsim_prof_new(size_t entries);
void       qsim_prof_free(qsim_prof *p);
void       qsim_prof_clear(qsim_prof *p);

/* Aggregated function profile */
typedef struct {
    const char *name;
    uint32_t addr, size;
    uint64_t insns;        /* self instructions */
    uint64_t cycles;       /* self est. cycles (insns + attributed penalties) */
    uint64_t calls;        /* linked calls to the entry */
    uint64_t tailcalls;    /* non-linked jumps to the entry from elsewhere */
    uint64_t oc[QSIM_OC_COUNT];   /* per op-class instruction counts */
} qsim_func_prof;

typedef struct {
    qsim_func_prof *f;
    size_t n;
    uint64_t total_insns, total_cycles, unattributed_insns;
    uint64_t oc[QSIM_OC_COUNT];
} qsim_profile;

/* Build the function profile from cpu->prof (symbols may be NULL: one row per
 * 4 KB page).  Returns 0 on success. */
int  qsim_profile_build(qsim_cpu *cpu, const qsim_syms *syms, qsim_profile *out);
void qsim_profile_free(qsim_profile *p);
/* Human-readable report (top N by self instructions, soft-float/libm calls,
 * op-class totals). */
void qsim_profile_print(const qsim_profile *p, FILE *out, size_t top);
/* Machine-readable: one TSV row per function. */
int  qsim_profile_write_tsv(const qsim_profile *p, const char *path);

#endif
