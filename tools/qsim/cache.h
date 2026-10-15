//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * qsim cache / branch-prediction / pipeline timing model of the Pocket os25
 * VexiiRiscv.  est_cycles = instructions + sum(penalties).
 *
 * Every parameter carries its provenance in qsim_model_default_params()
 * (cache.c) and in README.md.  "GUESS" marks values not derivable from the
 * RTL/generator sources in the openfpgaOS repository.
 *
 * The model is driven by the profiling interpreter (cpu_exec.h with
 * QX_PROF=1) through the inline hooks below.  All model arithmetic is integer
 * (it must not touch the host FPU; see fpu_fast.h).
 */
#ifndef QSIM_CACHE_H
#define QSIM_CACHE_H
#include <stdint.h>
#include <stdio.h>

typedef struct qsim_model_params {
    /* ---- instruction cache (FetchL1Plugin) ---- */
    unsigned ic_sets, ic_ways, ic_line;
    unsigned ic_nl_prefetch;      /* next-line prefetcher present */
    unsigned pen_prefetch_cmd;    /* fetch halt per prefetch command */
    unsigned ic_redo;             /* refetch after the REDO trap */
    /* ---- data cache (LsuL1Plugin + LsuPlugin store buffer) ---- */
    unsigned dc_sets, dc_ways, dc_line;
    unsigned sb_slots, sb_ops;
    unsigned dc_redo;             /* execute-stage REDO re-execution cost */
    /* ---- branch prediction ---- */
    unsigned btb_sets, btb_hash_bits;
    unsigned gshare_counters, gshare_hist_bits;
    unsigned ras_depth;
    unsigned pen_btb_taken;       /* predicted-taken redirect at fetch jumpAt */
    unsigned pen_mispredict;      /* execute-stage redirect */
    /* ---- result latencies: bubbles when the very next instruction reads rd ---- */
    unsigned lat_zero, lat_load, lat_mul, lat_div, lat_csr, lat_f2i, lat_fcmp;
    unsigned lat_fadd, lat_fmul, lat_fma, lat_fdiv, lat_fmisc, lat_fload, lat_i2f;
    /* ---- multi-cycle freezes ---- */
    unsigned div_base, div_per_byte;
    unsigned fdiv_extra, fsqrt_extra, f2i_extra;
    /* ---- memory latencies (cycles) ---- */
    unsigned sdram_setup, sdram_per_word, sdram_row_miss, sdram_wr_setup;
    unsigned bram_setup, bram_per_word;
    unsigned io_access_base, io_bram_extra, io_mmio_extra, io_cram_extra, io_sdram_uc_extra;
    unsigned misaligned_trap_cycles;
    unsigned sdram_row_track;
    /* ---- memory map ---- */
    uint32_t sd_base, sd_size, bram_size, sd_alias_base;
} qsim_model_params;

/* producer classes for dependency-stall attribution (one per lat_* param) */
enum {
    QDEP_lat_zero, QDEP_lat_load, QDEP_lat_mul, QDEP_lat_div, QDEP_lat_csr, QDEP_lat_f2i, QDEP_lat_fcmp,
    QDEP_lat_fadd, QDEP_lat_fmul, QDEP_lat_fma, QDEP_lat_fdiv, QDEP_lat_fmisc, QDEP_lat_fload, QDEP_lat_i2f,
    QDEP_COUNT
};

typedef struct {
    uint32_t tag;        /* line index + 1 (0 = invalid) */
    uint8_t  dirty;
    uint64_t fill_start; /* cycle at which the refill burst's first word is requested */
} qsim_cline;

typedef struct {
    uint32_t hash;       /* pc-last-slice hash; 0xffffffff = empty */
    uint32_t slice_pc;   /* exact pc of the last slice (for alias accounting) */
    uint32_t target;
    uint8_t  is_branch, is_push, is_pop;
} qsim_btb_entry;

typedef struct qsim_model {
    qsim_model_params p;
    uint64_t cyc;                  /* running estimated cycle clock */
    uint64_t ready[65];            /* int regs 0..32, float regs 33..64 */
    uint8_t  wcls[65];             /* QDEP class of the last writer */
    uint64_t pen_dep_by[QDEP_COUNT];

    qsim_cline *ic;                /* ic_sets * ic_ways */
    uint8_t    *ic_mru;
    uint32_t    ic_nl_line;        /* next-line prefetcher 'address' register (line+1) */
    uint64_t    ic_bus_free;       /* fetch refill bus free at */
    qsim_cline *dc;
    uint8_t    *dc_mru;
    uint64_t    dc_bus_free;
    struct { uint32_t line; uint64_t ready; uint32_t ops; } sb[8];
    uint32_t open_row[4];          /* SDRAM open row per bank (CPU traffic only) */

    qsim_btb_entry *btb;
    uint8_t *gshare;
    uint32_t ghist;
    uint32_t ras[32];
    unsigned ras_top;

    /* per-PC event attribution (set by qsim_enable_profiling; NULL = off).
     * ev_i points at the profiler's current-instruction index. */
    uint64_t *ev_ic, *ev_dc, *ev_bp;
    const size_t *ev_i;
    /* line of the last fetched instruction, kept across qsim_run/exec
     * re-entries so run chunking (frame stops, budgets) cannot add fetches */
    uint32_t cur_line;

    /* statistics */
    uint64_t n_insn_fetch_lines, ic_miss_sdram, ic_miss_bram, ic_inflight_wait,
             ic_prefetch_cmds, ic_prefetch_fills;
    uint64_t dc_load, dc_store, dc_load_miss, dc_store_miss, dc_writebacks,
             dc_inflight_wait, dc_sb_hazard, dc_sb_full, cbo_ops, cbo_redo;
    uint64_t acc_bram, acc_mmio, acc_cram, acc_sdram_uc, n_misaligned;
    uint64_t br_cond, br_cond_taken, br_mispredict, br_btb_alias,
             jal_count, jalr_count, ret_count, jump_mispredict, ras_mispredict,
             btb_taken_redirects;
    uint64_t n_mul, n_div, n_fdiv, n_fsqrt, n_f2i;
    uint64_t pen_icache, pen_prefetch, pen_dcache, pen_sb, pen_uncached, pen_cbo, pen_misaligned,
             pen_mispredict, pen_btb_taken, pen_dep, pen_div, pen_fdiv,
             pen_fsqrt, pen_f2i;
} qsim_model;

void        qsim_model_default_params(qsim_model_params *p);
qsim_model *qsim_model_new(const qsim_model_params *p);
void        qsim_model_free(qsim_model *m);
void        qsim_model_reset_stats(qsim_model *m);
void        qsim_model_report(const qsim_model *m, uint64_t insns, FILE *out);
/* parameter table with provenance, for docs */
void        qsim_model_print_params(const qsim_model_params *p, FILE *out);

/* out-of-line parts */
void qsim_model_fetch_line(qsim_model *m, uint32_t pc, unsigned bytes);
void qsim_model_dmiss(qsim_model *m, uint32_t addr, int is_store);
void qsim_model_uncached(qsim_model *m, uint32_t addr);
void qsim_model_branch(qsim_model *m, uint32_t pc, unsigned bytes, int taken, uint32_t target);
void qsim_model_jump(qsim_model *m, uint32_t pc, unsigned bytes, unsigned rd, unsigned rs1,
                     uint32_t target, int is_jalr);
void qsim_model_div(qsim_model *m, uint32_t a, uint32_t b, int is_signed);
void qsim_model_cbo(qsim_model *m, unsigned op, uint32_t addr);
void qsim_model_misaligned(qsim_model *m);

/* ---- inline hot hooks -------------------------------------------------- */

/* D-cache access: fast hit path inline */
static inline void qsim_model_mem(qsim_model *m, uint32_t addr, int is_store)
{
    uint32_t off = addr - m->p.sd_base;
    if (__builtin_expect(off >= m->p.sd_size, 0)) { qsim_model_uncached(m, addr); return; }
    uint32_t line = off >> 6;
    uint32_t set = line & (m->p.dc_sets - 1);
    qsim_cline *w = &m->dc[set * 2];
    if (is_store) m->dc_store++; else m->dc_load++;
    unsigned way;
    if (w[0].tag == line + 1) way = 0;
    else if (w[1].tag == line + 1) way = 1;
    else { qsim_model_dmiss(m, addr, is_store); return; }
    if (__builtin_expect(w[way].fill_start > m->cyc || (m->sb[0].ops | m->sb[1].ops), 0)) {
        qsim_model_dmiss(m, addr, is_store | 2);   /* in-flight refill / store buffer check */
        return;
    }
    m->dc_mru[set] = (uint8_t)way;
    if (is_store) w[way].dirty = 1;
}

/* dependency stalls: index 0..32 int, 33..64 float */
static inline void qsim_model_rd(qsim_model *m, unsigned r)
{
    uint64_t t = m->ready[r];
    if (__builtin_expect(t > m->cyc, 0)) {
        m->pen_dep += t - m->cyc;
        m->pen_dep_by[m->wcls[r]] += t - m->cyc;
        m->cyc = t;
    }
}
static inline void qsim_model_wr(qsim_model *m, unsigned r, unsigned lat, unsigned cls)
{
    m->ready[r] = m->cyc + lat + 1;
    m->wcls[r] = (uint8_t)cls;
}

#endif
