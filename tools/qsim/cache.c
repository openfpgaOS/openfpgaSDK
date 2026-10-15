//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * qsim timing model for the Pocket os25 VexiiRiscv (see cache.h).
 *
 * Source roots used for provenance below:
 *   CFG  = openfpgaOS/src/fpga/vendor/vexriscv/configs/os25.cfg
 *   GEN  = openfpgaOS/src/fpga/vendor/vexriscv/generate_vexii.sh
 *   VX   = openfpgaOS/src/fpga/vendor/vexriscv/VexiiRiscv/src/main/scala/vexiiriscv
 *   VOS  = openfpgaOS/src/fpga/vendor/vexriscv/VexiiRiscv/VexiiRiscv_os25.v
 *   SDR  = openfpgaOS/src/fpga/targets/pocket/io_sdram.v
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "cache.h"

typedef struct { const char *name; size_t off; const char *src; } param_doc;
#define PD(n, s) { #n, offsetof(qsim_model_params, n), s }
static const param_doc param_docs[] = {
    PD(ic_sets,  "CFG:18 ICACHE_SETS=256; GEN:154 --fetch-l1-sets"),
    PD(ic_ways,  "GEN:154 --fetch-l1-ways=2"),
    PD(ic_line,  "VX/Param.scala:885 FetchL1Plugin(lineSize = 64)"),
    PD(ic_nl_prefetch, "GEN:158 --fetch-l1-hardware-prefetch=nl; VX/fetch/Prefetch.scala:27-54"),
    PD(pen_prefetch_cmd, "VX/fetch/FetchL1Plugin.scala:385 fetch(readAt).haltWhen(PREFETCH): 1 cycle per prefetch command"),
    PD(ic_redo,  "GUESS: REDO trap raised at fetch ctrlAt=3 (GEN:157) -> refetch through fetch stages 0..3"),
    PD(dc_sets,  "CFG:19 DCACHE_SETS=1024; GEN:159"),
    PD(dc_ways,  "GEN:101 LSU_L1_WAYS=2"),
    PD(dc_line,  "VX/execute/lsu/LsuL1Plugin.scala:86 lineSize = 64"),
    PD(sb_slots, "GEN:102 LSU_SB_SLOTS=2"),
    PD(sb_ops,   "GEN:103 LSU_SB_OPS=16"),
    PD(dc_redo,  "VX/execute/lsu/LsuPlugin.scala:775-781 REDO trap at ctrlAt=2 (=execute_ctrl4, VOS); 9 earlier stages re-run"),
    PD(btb_sets, "CFG:30 BTB_SETS=256; GEN:164; VX/prediction/BtbPlugin.scala:116 direct-mapped"),
    PD(btb_hash_bits, "VX/Param.scala:191 btbHashWidth=16; BtbPlugin.scala:106"),
    PD(gshare_counters, "CFG:29 GSHARE_BYTES=1024 -> 4096 2-bit counters (VX/prediction/GSharePlugin.scala:57), 2 slices/word"),
    PD(gshare_hist_bits, "VX/Param.scala:843 historyWidth=12 (hash uses 11 bits, GSharePlugin.scala:63)"),
    PD(ras_depth, "VX/Param.scala:832 rasDepth = 8"),
    PD(pen_btb_taken, "APPROXIMATION: VX/Param.scala:836 redirect at fetch stage 2; additive penalty omits fetch/execute overlap (os25 RTL tight loop: about 2 cycles for 2 instructions)"),
    PD(pen_mispredict, "VX/Param.scala:968 BranchPlugin jumpAt=relaxedBranch=1 -> execute_ctrl3 (VOS stage names); fetch0-3,decode1-2,execute1-2 = 8 stages flushed"),
    PD(lat_zero, "ALU/branch/jal results bypass from execute (GEN:166 --allow-bypass-from=0)"),
    PD(lat_load, "VX/execute/lsu/LsuPlugin.scala:75 ctrlAt=2 completion; VX/Param.scala:806 intWritebackAt=2"),
    PD(lat_mul,  "VX/execute/MulPlugin.scala:47 writebackAt=2"),
    PD(lat_div,  "VX/execute/DivPlugin.scala:36 writebackAt=1 (after the freeze)"),
    PD(lat_csr,  "GUESS (CsrAccessPlugin writeback)"),
    PD(lat_f2i,  "VX/execute/fpu/FpuF2iPlugin.scala:22 intWbAt=2"),
    PD(lat_fcmp, "GEN:168 --fpu-wb-at=1 (FPU->int writeback); GUESS for cmp/class/mv"),
    PD(lat_fadd, "GEN:169-170 --fpu-add-pack-at=5, +1 packer (GUESS)"),
    PD(lat_fmul, "VX/execute/fpu/FpuMulPlugin.scala:17 packAt=3, +1 packer (GUESS)"),
    PD(lat_fma,  "MEASURED os25: dependent FMA interval 10 cycles = lat 9 + issue 1; openfpgaOS tools/check_pocket_cpu_latency.py (2026-09-19)"),
    PD(lat_fdiv, "GUESS: packer after the div/sqrt freeze"),
    PD(lat_fmisc, "VX/Param.scala:1235 FpuMvPlugin floatWbAt=2; GUESS for sgnj/min/max"),
    PD(lat_fload, "GUESS: same as integer load"),
    PD(lat_i2f,  "GUESS: FpuUnpacker int path + packer"),
    PD(div_base, "VX/execute/DivPlugin.scala:115-127 relaxedInputs cmd delay + VX/misc/DivRadix.scala done register"),
    PD(div_per_byte, "CFG:35 DIV_RADIX=4: 2 quotient bits/cycle; VX/misc/DivRadix.scala:88-106 skips leading zero dividend bytes"),
    PD(fdiv_extra, "VX/execute/fpu/FpuDivPlugin.scala:31-32 bitsNeeded=27 -> 14 radix-4 iterations; +3 control GUESS"),
    PD(fsqrt_extra, "VX/execute/fpu/FpuSqrt.scala:33 iterations=mantissaWidth+2=25; +2 control GUESS"),
    PD(f2i_extra, "VX/execute/fpu/FpuF2iPlugin.scala:108-111 halfRate freeze"),
    PD(sdram_setup, "GUESS 4 (AXI fabric/arbiter) + SDR:571,671 request dispatch 2 + SDR:109 CAS=3 + 2 (first 32-bit word)"),
    PD(sdram_per_word, "SDR:907-934 one READ per 2 cycles (BL=2 half-words) -> 2 cycles per 32-bit word"),
    PD(sdram_row_miss, "SDR:112-115 precharge 2 + ACT + tRCD 2; open row per bank (SDR:565-571)"),
    PD(sdram_wr_setup, "GUESS (burst write path SDR:ST_BURSTWR_*)"),
    PD(bram_setup, "GUESS (local AXI BRAM)"),
    PD(bram_per_word, "GUESS"),
    PD(io_access_base, "VX/execute/lsu/LsuPlugin.scala:611-633 io path: tooEarly 1 + cmd reg 1 + rsp halfPipe 1"),
    PD(io_bram_extra, "GUESS"),
    PD(io_mmio_extra, "GUESS"),
    PD(io_cram_extra, "GUESS (CRAM0 PSRAM controller)"),
    PD(io_sdram_uc_extra, "GUESS = sdram_setup + one word: 0x5xxxxxxx uncached alias goes through LSU p_axi (GEN:59-60)"),
    PD(misaligned_trap_cycles, "GUESS: LsuPlugin.scala:571,829-832 raise mcause 4/6; kernel/misaligned.c emulates (trap entry+decode+access+mret)"),
    PD(sdram_row_track, "1 = model per-bank open row for CPU traffic only (GPU/audio contention not modeled)"),
};

void qsim_model_default_params(qsim_model_params *p)
{
    memset(p, 0, sizeof *p);
    p->ic_sets = 256; p->ic_ways = 2; p->ic_line = 64;
    p->ic_nl_prefetch = 1; p->pen_prefetch_cmd = 1; p->ic_redo = 4;
    p->dc_sets = 1024; p->dc_ways = 2; p->dc_line = 64;
    p->sb_slots = 2; p->sb_ops = 16; p->dc_redo = 9;
    p->btb_sets = 256; p->btb_hash_bits = 16;
    p->gshare_counters = 4096; p->gshare_hist_bits = 12; p->ras_depth = 8;
    p->pen_btb_taken = 2; p->pen_mispredict = 8;
    p->lat_zero = 0;
    p->lat_load = 2; p->lat_mul = 2; p->lat_div = 1; p->lat_csr = 1;
    p->lat_f2i = 2; p->lat_fcmp = 1;
    p->lat_fadd = 6; p->lat_fmul = 4; p->lat_fma = 9; p->lat_fdiv = 1;
    p->lat_fmisc = 2; p->lat_fload = 2; p->lat_i2f = 2;
    p->div_base = 2; p->div_per_byte = 4;
    p->fdiv_extra = 17; p->fsqrt_extra = 27; p->f2i_extra = 1;
    p->sdram_setup = 11; p->sdram_per_word = 2; p->sdram_row_miss = 5; p->sdram_wr_setup = 8;
    p->bram_setup = 3; p->bram_per_word = 1;
    p->io_access_base = 3; p->io_bram_extra = 1; p->io_mmio_extra = 2; p->io_cram_extra = 10;
    p->io_sdram_uc_extra = 13; p->misaligned_trap_cycles = 400;
    p->sdram_row_track = 1;
    p->sd_base = 0x10000000u; p->sd_size = 64u << 20; p->bram_size = 0x8000; p->sd_alias_base = 0x50000000u;
}

void qsim_model_print_params(const qsim_model_params *p, FILE *out)
{
    for (size_t i = 0; i < sizeof param_docs / sizeof param_docs[0]; i++) {
        unsigned v = *(const unsigned *)((const char *)p + param_docs[i].off);
        fprintf(out, "  %-18s %6u  %s\n", param_docs[i].name, v, param_docs[i].src);
    }
}

qsim_model *qsim_model_new(const qsim_model_params *p)
{
    qsim_model *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->p = *p;
    m->cur_line = 0xffffffffu;
    if (m->p.ic_ways != 2 || m->p.dc_ways != 2) { free(m); return NULL; }  /* model assumes 2 ways */
    if (m->p.ras_depth > 32) m->p.ras_depth = 32;
    if (m->p.sb_slots > 8) m->p.sb_slots = 8;
    m->ic = calloc((size_t)m->p.ic_sets * 2, sizeof *m->ic);
    m->ic_mru = calloc(m->p.ic_sets, 1);
    m->dc = calloc((size_t)m->p.dc_sets * 2, sizeof *m->dc);
    m->dc_mru = calloc(m->p.dc_sets, 1);
    m->btb = calloc(m->p.btb_sets, sizeof *m->btb);
    m->gshare = calloc(m->p.gshare_counters, 1);
    if (!m->ic || !m->ic_mru || !m->dc || !m->dc_mru || !m->btb || !m->gshare) {
        qsim_model_free(m);
        return NULL;
    }
    for (unsigned i = 0; i < m->p.btb_sets; i++) m->btb[i].hash = 0xffffffffu;
    for (unsigned i = 0; i < 4; i++) m->open_row[i] = 0xffffffffu;
    m->ic_nl_line = 0xffffffffu;
    return m;
}

void qsim_model_free(qsim_model *m)
{
    if (!m) return;
    free(m->ic); free(m->ic_mru); free(m->dc); free(m->dc_mru);
    free(m->btb); free(m->gshare);
    free(m);
}

void qsim_model_reset_stats(qsim_model *m)
{
    size_t first = offsetof(qsim_model, n_insn_fetch_lines);
    memset((char *)m + first, 0, sizeof *m - first);
    m->cyc = 0;
    memset(m->ready, 0, sizeof m->ready);
    memset(m->wcls, 0, sizeof m->wcls);
    memset(m->pen_dep_by, 0, sizeof m->pen_dep_by);
}

/* ------------------------------------------------------------------------ */
/* memory transfer timing                                                    */

static uint64_t sdram_burst_start(qsim_model *m, uint32_t addr, uint64_t earliest)
{
    uint64_t t = m->ic_bus_free > earliest ? m->ic_bus_free : earliest;   /* shared SDRAM port */
    t += m->p.sdram_setup;
    if (m->p.sdram_row_track) {
        uint32_t off = addr - m->p.sd_base;
        uint32_t bank = (off >> 24) & 3, row = off >> 11;
        if (m->open_row[bank] != row) { t += m->p.sdram_row_miss; m->open_row[bank] = row; }
    }
    return t;   /* word k of the line available at t + per_word * (k + 1) */
}

/* ------------------------------------------------------------------------ */
/* instruction cache                                                         */

static unsigned ic_word_cycles(qsim_model *m, int bram, unsigned last_byte)
{
    /* FetchL1 memDataWidth = 64 (GEN:155) over a 32-bit AXI (FetchL1Bridge
     * resizer(32)): the 8-byte bank word holding last_byte needs 2*(w+1) beats */
    unsigned beats = 2 * ((last_byte >> 3) + 1);
    return beats * (bram ? m->p.bram_per_word : m->p.sdram_per_word);
}

static void ic_fill(qsim_model *m, qsim_cline *w, uint32_t line, int bram, uint32_t addr)
{
    uint64_t start;
    if (bram) {
        start = m->cyc + m->p.bram_setup;
    } else {
        start = sdram_burst_start(m, addr, m->cyc);
        m->ic_bus_free = start + 16ull * m->p.sdram_per_word;
    }
    w->tag = line + 1;
    w->fill_start = start;
    w->dirty = (uint8_t)bram;   /* reuse: remembers the refill source */
}

static void ic_prefetch(qsim_model *m, uint32_t line)
{
    m->ic_prefetch_cmds++;
    m->cyc += m->p.pen_prefetch_cmd;
    m->pen_prefetch += m->p.pen_prefetch_cmd;
    m->ic_nl_line = line;
    uint32_t addr = line << 6;
    int bram = addr < m->p.bram_size;
    if (!bram && (uint32_t)(addr - m->p.sd_base) >= m->p.sd_size) return;
    uint32_t set = line & (m->p.ic_sets - 1);
    qsim_cline *w = &m->ic[set * 2];
    if (w[0].tag == line + 1 || w[1].tag == line + 1) return;
    unsigned victim = m->ic_mru[set] ^ 1;
    ic_fill(m, &w[victim], line, bram, addr);
    m->ic_prefetch_fills++;
}

static void ic_access(qsim_model *m, uint32_t addr, unsigned last_byte)
{
    uint32_t line = addr >> 6;
    uint32_t set = line & (m->p.ic_sets - 1);
    qsim_cline *w = &m->ic[set * 2];
    int bram = addr < m->p.bram_size;
    unsigned way;
    m->n_insn_fetch_lines++;
    if (w[0].tag == line + 1) way = 0;
    else if (w[1].tag == line + 1) way = 1;
    else {
        way = m->ic_mru[set] ^ 1;
        ic_fill(m, &w[way], line, bram, addr);
        uint64_t ready = w[way].fill_start + ic_word_cycles(m, bram, last_byte);
        uint64_t pen = m->p.ic_redo + (ready > m->cyc ? ready - m->cyc : 0);
        m->cyc += pen;
        m->pen_icache += pen;
        if (bram) m->ic_miss_bram++; else m->ic_miss_sdram++;
        if (m->ev_ic) m->ev_ic[*m->ev_i]++;
        m->ic_mru[set] = (uint8_t)way;
        if (m->p.ic_nl_prefetch) ic_prefetch(m, line + 1);
        return;
    }
    m->ic_mru[set] = (uint8_t)way;
    uint64_t ready = w[way].fill_start + ic_word_cycles(m, w[way].dirty, last_byte);
    if (ready > m->cyc) {
        uint64_t pen = ready - m->cyc;
        m->cyc += pen;
        m->pen_icache += pen;
        m->ic_inflight_wait++;
    }
    if (m->p.ic_nl_prefetch && line == m->ic_nl_line) ic_prefetch(m, line + 1);
}

void qsim_model_fetch_line(qsim_model *m, uint32_t pc, unsigned bytes)
{
    unsigned o = pc & 63;
    if (o + bytes <= 64) {
        ic_access(m, pc, o + bytes - 1);
    } else {
        ic_access(m, pc, 63);
        ic_access(m, (pc | 63) + 1, o + bytes - 65);
    }
}

/* ------------------------------------------------------------------------ */
/* data cache                                                                */

static void sb_expire(qsim_model *m)
{
    for (unsigned i = 0; i < m->p.sb_slots; i++)
        if (m->sb[i].ops && m->sb[i].ready <= m->cyc) m->sb[i].ops = 0;
}

static int sb_find(qsim_model *m, uint32_t line)
{
    for (unsigned i = 0; i < m->p.sb_slots; i++)
        if (m->sb[i].ops && m->sb[i].line == line) return (int)i;
    return -1;
}

static void dc_charge(qsim_model *m, uint64_t until, uint64_t *bucket)
{
    uint64_t pen = m->p.dc_redo + (until > m->cyc ? until - m->cyc : 0);
    m->cyc += pen;
    *bucket += pen;
}

/* start a line refill; returns completion cycle */
static uint64_t dc_refill(qsim_model *m, qsim_cline *w, uint32_t line, uint32_t addr)
{
    uint64_t t = m->cyc;
    if (w->tag && w->dirty) {       /* victim writeback first (GUESS: serialized) */
        m->dc_writebacks++;
        uint64_t s = m->ic_bus_free > t ? m->ic_bus_free : t;
        m->ic_bus_free = s + m->p.sdram_wr_setup + 16ull * m->p.sdram_per_word;
    }
    uint64_t start = sdram_burst_start(m, addr, t);
    uint64_t done = start + 16ull * m->p.sdram_per_word;
    m->ic_bus_free = done;
    w->tag = line + 1;
    w->dirty = 0;
    w->fill_start = done;           /* D-cache lines are usable only when complete */
    return done;
}

void qsim_model_dmiss(qsim_model *m, uint32_t addr, int flags)
{
    int is_store = flags & 1;
    uint32_t off = addr - m->p.sd_base;
    uint32_t line = off >> 6;
    uint32_t set = line & (m->p.dc_sets - 1);
    qsim_cline *w = &m->dc[set * 2];
    sb_expire(m);
    int sbi = sb_find(m, line);
    int way = w[0].tag == line + 1 ? 0 : (w[1].tag == line + 1 ? 1 : -1);

    if (way >= 0) {
        uint64_t until = w[way].fill_start;
        if (!is_store) {
            if (sbi >= 0 && m->sb[sbi].ready > until) until = m->sb[sbi].ready;
            if (until > m->cyc) {
                if (sbi >= 0) { m->dc_sb_hazard++; dc_charge(m, until, &m->pen_sb); }
                else { m->dc_inflight_wait++; dc_charge(m, until, &m->pen_dcache); }
            }
        } else {
            if (sbi >= 0) {
                if (m->sb[sbi].ops < m->p.sb_ops) m->sb[sbi].ops++;
            } else if (until > m->cyc) {
                m->dc_inflight_wait++;
                dc_charge(m, until, &m->pen_dcache);
            }
            w[way].dirty = 1;
        }
        m->dc_mru[set] = (uint8_t)way;
        return;
    }

    way = m->dc_mru[set] ^ 1;
    if (!is_store) {
        m->dc_load_miss++;
        if (m->ev_dc) m->ev_dc[*m->ev_i]++;
        uint64_t done = dc_refill(m, &w[way], line, addr);
        dc_charge(m, done, &m->pen_dcache);
        m->dc_mru[set] = (uint8_t)way;
        return;
    }
    m->dc_store_miss++;
    if (m->ev_dc) m->ev_dc[*m->ev_i]++;
    /* store miss: goes to the store buffer (no trap) if a slot is free */
    int slot = -1;
    unsigned total_ops = 0;
    for (unsigned i = 0; i < m->p.sb_slots; i++) {
        total_ops += m->sb[i].ops;
        if (!m->sb[i].ops && slot < 0) slot = (int)i;
    }
    if (slot < 0 || total_ops >= m->p.sb_ops) {
        /* store buffer full: REDO and wait for the oldest slot (LsuPlugin.scala:745,811-816) */
        uint64_t oldest = ~0ull;
        for (unsigned i = 0; i < m->p.sb_slots; i++)
            if (m->sb[i].ops && m->sb[i].ready < oldest) { oldest = m->sb[i].ready; slot = (int)i; }
        m->dc_sb_full++;
        dc_charge(m, oldest, &m->pen_sb);
        sb_expire(m);
        if (slot < 0) slot = 0;
        m->sb[slot].ops = 0;
    }
    uint64_t done = dc_refill(m, &w[way], line, addr);
    w[way].dirty = 1;
    m->sb[slot].line = line;
    m->sb[slot].ready = done + 1;
    m->sb[slot].ops = 1;
    m->dc_mru[set] = (uint8_t)way;
}

void qsim_model_uncached(qsim_model *m, uint32_t addr)
{
    uint64_t pen = m->p.io_access_base;
    if (addr < m->p.bram_size) { m->acc_bram++; pen += m->p.io_bram_extra; }
    else if ((uint32_t)(addr - m->p.sd_alias_base) < m->p.sd_size) { m->acc_sdram_uc++; pen += m->p.io_sdram_uc_extra; }
    else if (addr >= 0x30000000u && addr < 0x40000000u) { m->acc_cram++; pen += m->p.io_cram_extra; }
    else { m->acc_mmio++; pen += m->p.io_mmio_extra; }
    m->cyc += pen;
    m->pen_uncached += pen;
}

void qsim_model_misaligned(qsim_model *m)
{
    m->n_misaligned++;
    m->cyc += m->p.misaligned_trap_cycles;
    m->pen_misaligned += m->p.misaligned_trap_cycles;
}

void qsim_model_cbo(qsim_model *m, unsigned op, uint32_t addr)
{
    m->cbo_ops++;
    uint32_t off = addr - m->p.sd_base;
    if (off >= m->p.sd_size) return;
    uint32_t line = off >> 6, set = line & (m->p.dc_sets - 1);
    qsim_cline *w = &m->dc[set * 2];
    int way = w[0].tag == line + 1 ? 0 : (w[1].tag == line + 1 ? 1 : -1);
    if (way < 0) return;
    /* LsuPlugin.scala:236-238 decodes CBM_FLUSH exactly like CBM_CLEAN (CLEAN=1),
     * and CBM_INVALIDATE as INVALIDATE (invalIntoClean is only set below
     * M-mode, :221-226,433).  LsuL1Plugin.scala:890,951-966:
     * askCbm = WAYS_HIT && (INVALID || CLEAN && wasDirty) -> CBM_REDO;
     * tag.loaded := !INVALID (clean/flush keep the line loaded);
     * writeback only for CLEAN && wasDirty (inval discards dirty data). */
    int inval = (op == 0), clean = !inval;
    if (inval || (clean && w[way].dirty)) {
        m->cbo_redo++;
        m->cyc += m->p.dc_redo;
        m->pen_cbo += m->p.dc_redo;
        if (w[way].dirty && clean) {
            m->dc_writebacks++;
            uint64_t s = m->ic_bus_free > m->cyc ? m->ic_bus_free : m->cyc;
            m->ic_bus_free = s + m->p.sdram_wr_setup + 16ull * m->p.sdram_per_word;
        }
        if (inval) w[way].tag = 0;
        w[way].dirty = 0;
    }
}

/* ------------------------------------------------------------------------ */
/* branch prediction                                                         */

static inline uint32_t btb_idx(const qsim_model *m, uint32_t slice_pc)
{
    return (slice_pc >> 2) & (m->p.btb_sets - 1);
}
static inline uint32_t btb_hash(const qsim_model *m, uint32_t slice_pc)
{
    unsigned sb = 2 + (unsigned)__builtin_ctz(m->p.btb_sets);
    return (slice_pc >> sb) & ((1u << m->p.btb_hash_bits) - 1);
}
static inline uint32_t gshare_index(const qsim_model *m, uint32_t slice_pc)
{
    unsigned words = m->p.gshare_counters / 2;
    unsigned hb = (unsigned)__builtin_ctz(words);
    uint32_t a = (slice_pc >> 2) & ((1u << hb) - 1), r = 0;
    for (unsigned i = 0; i < hb; i++) if (a & (1u << i)) r |= 1u << (hb - 1 - i);
    uint32_t h = (r ^ m->ghist) & (words - 1);
    return h * 2 + ((slice_pc >> 1) & 1);
}

void qsim_model_branch(qsim_model *m, uint32_t pc, unsigned bytes, int taken, uint32_t target)
{
    uint32_t sp = pc + bytes - 2;
    qsim_btb_entry *e = &m->btb[btb_idx(m, sp)];
    int hit = e->hash == btb_hash(m, sp) && e->slice_pc == sp;
    if (e->hash == btb_hash(m, sp) && e->slice_pc != sp) m->br_btb_alias++;
    uint32_t gi = gshare_index(m, sp);
    uint8_t *ctr = &m->gshare[gi];
    int pred_taken = 0;
    uint32_t pred_target = pc + bytes;
    if (hit) {
        pred_taken = e->is_branch ? (*ctr >= 2) : 1;
        if (pred_taken) pred_target = e->target;
    }
    uint32_t actual = taken ? target : pc + bytes;
    m->br_cond++;
    if (taken) m->br_cond_taken++;
    if (pred_target != actual) {
        m->br_mispredict++;
        if (m->ev_bp) m->ev_bp[*m->ev_i]++;
        m->cyc += m->p.pen_mispredict;
        m->pen_mispredict += m->p.pen_mispredict;
        if (taken) {   /* BtbPlugin.scala:158 learn on badPredictedTarget && taken */
            e->hash = btb_hash(m, sp); e->slice_pc = sp; e->target = target;
            e->is_branch = 1; e->is_push = 0; e->is_pop = 0;
        }
    } else if (pred_taken) {
        m->btb_taken_redirects++;
        m->cyc += m->p.pen_btb_taken;
        m->pen_btb_taken += m->p.pen_btb_taken;
    }
    if (taken) { if (*ctr < 3) (*ctr)++; } else { if (*ctr > 0) (*ctr)--; }
    m->ghist = ((m->ghist << 1) | (unsigned)taken) & ((1u << m->p.gshare_hist_bits) - 1);
}

void qsim_model_jump(qsim_model *m, uint32_t pc, unsigned bytes, unsigned rd, unsigned rs1,
                     uint32_t target, int is_jalr)
{
    int rd_link = rd == 1 || rd == 5;
    int rs1_link = is_jalr && (rs1 == 1 || rs1 == 5);
    int push = rd_link;
    int pop = rs1_link && (!rd_link || rd != rs1);
    uint32_t sp = pc + bytes - 2;
    qsim_btb_entry *e = &m->btb[btb_idx(m, sp)];
    int hit = e->hash == btb_hash(m, sp) && e->slice_pc == sp;
    uint32_t pred = pc + bytes;
    if (is_jalr) { m->jalr_count++; if (pop && !push) m->ret_count++; }
    else m->jal_count++;
    if (hit) {
        if (e->is_pop) {
            m->ras_top = (m->ras_top + m->p.ras_depth - 1) % m->p.ras_depth;
            pred = m->ras[(m->ras_top + 1) % m->p.ras_depth];
        } else {
            pred = e->target;
        }
        if (e->is_push) {
            m->ras_top = (m->ras_top + 1) % m->p.ras_depth;
            m->ras[m->ras_top] = pc + bytes;
        }
    }
    if (pred != target) {
        m->jump_mispredict++;
        if (m->ev_bp) m->ev_bp[*m->ev_i]++;
        if (hit && e->is_pop) m->ras_mispredict++;
        m->cyc += m->p.pen_mispredict;
        m->pen_mispredict += m->p.pen_mispredict;
        e->hash = btb_hash(m, sp); e->slice_pc = sp; e->target = target;
        e->is_branch = 0; e->is_push = (uint8_t)push; e->is_pop = (uint8_t)pop;
    } else {
        m->btb_taken_redirects++;
        m->cyc += m->p.pen_btb_taken;
        m->pen_btb_taken += m->p.pen_btb_taken;
    }
}

/* ------------------------------------------------------------------------ */
/* multi-cycle integer division                                              */

void qsim_model_div(qsim_model *m, uint32_t a, uint32_t b, int is_signed)
{
    (void)b;
    uint32_t ua = (is_signed && (int32_t)a < 0) ? 0 - a : a;
    unsigned bytes = ua < 0x100u ? 1 : ua < 0x10000u ? 2 : ua < 0x1000000u ? 3 : 4;
    uint64_t extra = m->p.div_base + (uint64_t)m->p.div_per_byte * bytes;
    m->n_div++;
    m->pen_div += extra;
    m->cyc += extra;
}

/* ------------------------------------------------------------------------ */

static void line(FILE *o, const char *k, uint64_t v, uint64_t insns)
{
    fprintf(o, "  %-28s %16" PRIu64, k, v);
    if (insns) fprintf(o, "   (%.4f /insn)", (double)v / (double)insns);
    fputc('\n', o);
}

void qsim_model_report(const qsim_model *m, uint64_t insns, FILE *o)
{
    fprintf(o, "timing model (os25 VexiiRiscv)\n");
    line(o, "instructions", insns, 0);
    line(o, "est_cycles", m->cyc, insns);
    fprintf(o, " instruction cache\n");
    line(o, "line_changes", m->n_insn_fetch_lines, insns);
    line(o, "icache_miss_sdram", m->ic_miss_sdram, insns);
    line(o, "icache_miss_bram", m->ic_miss_bram, insns);
    line(o, "icache_inflight_waits", m->ic_inflight_wait, insns);
    line(o, "nl_prefetch_cmds", m->ic_prefetch_cmds, insns);
    line(o, "nl_prefetch_fills", m->ic_prefetch_fills, insns);
    fprintf(o, " data cache\n");
    line(o, "loads", m->dc_load, insns);
    line(o, "stores", m->dc_store, insns);
    line(o, "dcache_load_miss", m->dc_load_miss, insns);
    line(o, "dcache_store_miss", m->dc_store_miss, insns);
    line(o, "dcache_writebacks", m->dc_writebacks, insns);
    line(o, "dcache_inflight_waits", m->dc_inflight_wait, insns);
    line(o, "storebuf_load_hazards", m->dc_sb_hazard, insns);
    line(o, "storebuf_full", m->dc_sb_full, insns);
    line(o, "cbo_ops", m->cbo_ops, insns);
    line(o, "cbo_redo", m->cbo_redo, insns);
    line(o, "uncached_bram", m->acc_bram, insns);
    line(o, "uncached_mmio", m->acc_mmio, insns);
    line(o, "uncached_cram", m->acc_cram, insns);
    line(o, "uncached_sdram_alias", m->acc_sdram_uc, insns);
    line(o, "misaligned_load_store", m->n_misaligned, insns);
    fprintf(o, " control flow\n");
    line(o, "cond_branches", m->br_cond, insns);
    line(o, "cond_taken", m->br_cond_taken, insns);
    line(o, "cond_mispredicts", m->br_mispredict, insns);
    line(o, "btb_alias", m->br_btb_alias, insns);
    line(o, "jal", m->jal_count, insns);
    line(o, "jalr", m->jalr_count, insns);
    line(o, "returns", m->ret_count, insns);
    line(o, "jump_mispredicts", m->jump_mispredict, insns);
    line(o, "ras_mispredicts", m->ras_mispredict, insns);
    line(o, "btb_taken_redirects", m->btb_taken_redirects, insns);
    fprintf(o, " multi-cycle ops\n");
    line(o, "div_rem", m->n_div, insns);
    line(o, "fdiv", m->n_fdiv, insns);
    line(o, "fsqrt", m->n_fsqrt, insns);
    line(o, "fcvt_to_int", m->n_f2i, insns);
    fprintf(o, " penalty cycles\n");
    line(o, "icache", m->pen_icache, insns);
    line(o, "nl_prefetch", m->pen_prefetch, insns);
    line(o, "dcache", m->pen_dcache, insns);
    line(o, "store_buffer", m->pen_sb, insns);
    line(o, "uncached_io", m->pen_uncached, insns);
    line(o, "cbo", m->pen_cbo, insns);
    line(o, "misaligned_traps", m->pen_misaligned, insns);
    line(o, "mispredict", m->pen_mispredict, insns);
    line(o, "btb_taken", m->pen_btb_taken, insns);
    line(o, "dependency_stalls", m->pen_dep, insns);
    {
        static const char *dn[QDEP_COUNT] = {"  dep:alu", "  dep:load", "  dep:mul", "  dep:div", "  dep:csr",
            "  dep:fcvt_to_int", "  dep:fcmp/fclass/fmv", "  dep:fadd/fsub", "  dep:fmul", "  dep:fmadd*",
            "  dep:fdiv/fsqrt", "  dep:fsgnj/fmin/fmax", "  dep:flw", "  dep:fcvt_to_float"};
        for (int i = 0; i < QDEP_COUNT; i++)
            if (m->pen_dep_by[i]) line(o, dn[i], m->pen_dep_by[i], insns);
    }
    line(o, "div_extra", m->pen_div, insns);
    line(o, "fdiv_extra", m->pen_fdiv, insns);
    line(o, "fsqrt_extra", m->pen_fsqrt, insns);
    line(o, "f2i_extra", m->pen_f2i, insns);
}
