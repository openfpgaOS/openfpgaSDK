//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * qsim CPU core: decoder, CSRs, traps, memory slow paths, public API.
 * The interpreter loops live in cpu_exec.h (included three times below: fast,
 * fast + memory coherency hooks, profiling).
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <xmmintrin.h>
#include "cpu.h"
#include "cpu_ops.h"
#include "fpu.h"
#include "fpu_fast.h"
#include "cache.h"
#include "prof.h"
#include "lint.h"

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

static const uint8_t op_class_tab[QOP_COUNT] = {
#define QSIM_X_CLS(n, c) c,
    QSIM_OPS(QSIM_X_CLS)
#undef QSIM_X_CLS
};
static const char *const op_name_tab[QOP_COUNT] = {
#define QSIM_X_NAME(n, c) #n,
    QSIM_OPS(QSIM_X_NAME)
#undef QSIM_X_NAME
};

/* ------------------------------------------------------------------------ */
/* allocation                                                                */

static void *map_zero(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

#define GUARD_ENTRIES 4

qsim_cpu *qsim_cpu_new(const qsim_config *cfg)
{
    qsim_config def = { 0x10000000u, 64u << 20, 0x8000u, 0x50000000u };
    if (!cfg) cfg = &def;
    qsim_cpu *cpu = calloc(1, sizeof *cpu);
    if (!cpu) return NULL;
    cpu->sdram_base = cfg->sdram_base ? cfg->sdram_base : def.sdram_base;
    cpu->sdram_size = (cfg->sdram_size ? cfg->sdram_size : def.sdram_size) & ~1u;
    cpu->bram_size  = (cfg->bram_size ? cfg->bram_size : def.bram_size) & ~1u;
    cpu->sdram_alias_base = cfg->sdram_alias_base ? cfg->sdram_alias_base : def.sdram_alias_base;
    if (cpu->sdram_alias_base == 0xffffffffu) cpu->sdram_alias_base = cpu->sdram_base;  /* disabled */
    cpu->sdram = map_zero((size_t)cpu->sdram_size + 16);
    cpu->bram  = map_zero((size_t)cpu->bram_size + 16);
    size_t br_n = cpu->bram_size / 2 + GUARD_ENTRIES;
    size_t sd_n = cpu->sdram_size / 2 + GUARD_ENTRIES;
    cpu->dc_entries = br_n + sd_n;
    cpu->dc_base = map_zero(cpu->dc_entries * sizeof(qsim_dinsn));
    if (!cpu->sdram || !cpu->bram || !cpu->dc_base) { qsim_cpu_free(cpu); return NULL; }
    cpu->br_dc = cpu->dc_base;
    cpu->sd_dc = cpu->dc_base + br_n;
    for (int i = 0; i < GUARD_ENTRIES; i++) {
        qsim_dinsn *g = &cpu->br_dc[cpu->bram_size / 2 + i];
        g->h = QOP_FETCH_FAULT; g->pc = cpu->bram_size + 2 * i; g->lf = 1;
        g = &cpu->sd_dc[cpu->sdram_size / 2 + i];
        g->h = QOP_FETCH_FAULT; g->pc = cpu->sdram_base + cpu->sdram_size + 2 * i; g->lf = 1;
    }
    cpu->code_lo = 0xffffffffu;
    cpu->code_hi = 0;
    qsim_cpu_reset(cpu, cpu->sdram_base);
    return cpu;
}

void qsim_cpu_free(qsim_cpu *cpu)
{
    if (!cpu) return;
    if (cpu->prof) qsim_prof_free(cpu->prof);
    if (cpu->model) qsim_model_free(cpu->model);
    if (cpu->sdram) munmap(cpu->sdram, (size_t)cpu->sdram_size + 16);
    if (cpu->bram) munmap(cpu->bram, (size_t)cpu->bram_size + 16);
    if (cpu->dc_base) munmap(cpu->dc_base, cpu->dc_entries * sizeof(qsim_dinsn));
    free(cpu->hooks);
    free(cpu);
}

void qsim_cpu_reset(qsim_cpu *cpu, uint32_t pc)
{
    memset(cpu->x, 0, sizeof cpu->x);
    memset(cpu->f, 0, sizeof cpu->f);
    cpu->pc = pc;
    cpu->fflags = 0;
    cpu->frm = 0;
    cpu->resv_valid = 0;
    cpu->mstatus = 3u << 13 | 3u << 11;   /* FS = dirty, MPP = M */
    cpu->mie = cpu->mip = cpu->mtvec = cpu->mscratch = 0;
    cpu->mepc = cpu->mcause = cpu->mtval = 0;
    cpu->csr_7ff = 0;
    cpu->stop_req = 0;
    cpu->async = 0;
}

/* ------------------------------------------------------------------------ */
/* memory                                                                    */

uint8_t *qsim_mem_ptr(qsim_cpu *cpu, uint32_t addr, uint32_t len)
{
    uint32_t off = addr - cpu->sdram_base;
    if (off < cpu->sdram_size && len <= cpu->sdram_size - off) return cpu->sdram + off;
    if (addr < cpu->bram_size && len <= cpu->bram_size - addr) return cpu->bram + addr;
    off = addr - cpu->sdram_alias_base;
    if (off < cpu->sdram_size && len <= cpu->sdram_size - off) return cpu->sdram + off;
    return NULL;
}

int qsim_add_hook(qsim_cpu *cpu, uint32_t addr)
{
    uint32_t *n = realloc(cpu->hooks, (cpu->nhooks + 1) * sizeof *n);
    if (!n) return -1;
    cpu->hooks = n;
    size_t i = cpu->nhooks++;
    while (i > 0 && n[i - 1] > addr) { n[i] = n[i - 1]; i--; }
    n[i] = addr;
    qsim_invalidate(cpu, addr, 4);
    /* force re-decode even if outside the known code range */
    uint32_t off = addr - cpu->sdram_base;
    if (off < cpu->sdram_size) cpu->sd_dc[off >> 1].h = 0;
    else if (addr < cpu->bram_size) cpu->br_dc[addr >> 1].h = 0;
    return 0;
}

static int is_hook(const qsim_cpu *cpu, uint32_t pc)
{
    size_t lo = 0, hi = cpu->nhooks;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cpu->hooks[mid] < pc) lo = mid + 1; else hi = mid;
    }
    return lo < cpu->nhooks && cpu->hooks[lo] == pc;
}

void qsim_invalidate(qsim_cpu *cpu, uint32_t addr, uint32_t len)
{
    /* an instruction starting up to 2 bytes before addr may overlap it */
    uint64_t lo = addr >= 2 ? (uint64_t)addr - 2 : 0;
    uint64_t hi = (uint64_t)addr + len;
    uint64_t sb = cpu->sdram_base, se = sb + cpu->sdram_size;
    if (hi > sb && lo < se && cpu->code_hi > cpu->code_lo) {
        uint64_t l = (lo > sb ? lo : sb) - sb, h = (hi < se ? hi : se) - sb;
        if (l < cpu->code_lo) l = cpu->code_lo;
        if (h > cpu->code_hi) h = cpu->code_hi;
        for (uint64_t o = l & ~1ull; o < h; o += 2) cpu->sd_dc[o >> 1].h = QOP_DECODE;
    }
    if (lo < cpu->bram_size) {
        uint64_t h = hi < cpu->bram_size ? hi : cpu->bram_size;
        if (h > cpu->br_code_hi) h = cpu->br_code_hi;
        for (uint64_t o = lo & ~1ull; o < h; o += 2) cpu->br_dc[o >> 1].h = QOP_DECODE;
    }
}

static inline void fpu_host_fold(qsim_cpu *cpu)
{
    uint32_t m = _mm_getcsr();
    cpu->fflags |= qff_mxcsr_to_rv(m);
    _mm_setcsr(QSIM_MXCSR_DEFAULT);
}

uint32_t qsim_fflags(qsim_cpu *cpu)
{
    if (cpu->in_run) fpu_host_fold(cpu);
    return cpu->fflags;
}

void qsim_set_fflags(qsim_cpu *cpu, uint32_t v)
{
    if (cpu->in_run) _mm_setcsr(QSIM_MXCSR_DEFAULT);
    cpu->fflags = v & 0x1f;
}

/* slow-path data access (BRAM, MMIO); returns 0 or trap cause */
static int mem_read_slow(qsim_cpu *cpu, uint32_t a, unsigned size, uint32_t *v)
{
    if (a < cpu->bram_size && size <= cpu->bram_size - a) {
        uint32_t t = 0;
        memcpy(&t, cpu->bram + a, size);
        *v = t;
        return 0;
    }
    uint32_t off = a - cpu->sdram_base;
    int cached = off < cpu->sdram_size;
    if (!cached) off = a - cpu->sdram_alias_base;
    if (off < cpu->sdram_size) {             /* straddling the end */
        if (cpu->lint) {
            if (cached) lint_cpu_access(cpu->lint, off, size, 0, cpu->pc);
            else lint_uc_rd(cpu->lint, off, size, cpu->pc);
        }
        uint32_t t = 0;
        memcpy(&t, (cached || !cpu->sdram_uc ? cpu->sdram : cpu->sdram_uc) + off, size);
        *v = t;
        return 0;
    }
    if (cpu->env.mmio_read) {
        if (cpu->in_run) fpu_host_fold(cpu);
        int r = cpu->env.mmio_read(cpu->env.ctx, cpu, a, size, v);
        if (cpu->in_run) _mm_setcsr(QSIM_MXCSR_DEFAULT);
        if (r == 0) {
            if (size == 1) *v &= 0xff; else if (size == 2) *v &= 0xffff;
            return 0;
        }
    }
    return QSIM_CAUSE_LOAD_FAULT;
}

static int mem_write_slow(qsim_cpu *cpu, uint32_t a, unsigned size, uint32_t v)
{
    if (a < cpu->bram_size && size <= cpu->bram_size - a) {
        memcpy(cpu->bram + a, &v, size);
        if (a < cpu->br_code_hi) qsim_invalidate(cpu, a, size);
        return 0;
    }
    uint32_t off = a - cpu->sdram_base;
    int cached = off < cpu->sdram_size;
    if (!cached) off = a - cpu->sdram_alias_base;
    if (off < cpu->sdram_size) {
        if (cached) {
            if (cpu->lint) lint_cpu_access(cpu->lint, off, size, 1, cpu->pc);
            memcpy(cpu->sdram + off, &v, size);
        } else {
            memcpy((cpu->sdram_uc ? cpu->sdram_uc : cpu->sdram) + off, &v, size);
            if (cpu->lint) lint_uc_wr(cpu->lint, off, size);
        }
        qsim_invalidate(cpu, cpu->sdram_base + off, size);
        return 0;
    }
    if (cpu->env.mmio_write) {
        if (cpu->in_run) fpu_host_fold(cpu);
        int r = cpu->env.mmio_write(cpu->env.ctx, cpu, a, size, v);
        if (cpu->in_run) _mm_setcsr(QSIM_MXCSR_DEFAULT);
        if (r == 0) return 0;
    }
    return QSIM_CAUSE_STORE_FAULT;
}

int qsim_read(qsim_cpu *cpu, uint32_t addr, void *buf, uint32_t len)
{
    uint8_t *p = qsim_mem_ptr(cpu, addr, len);
    if (p) { memcpy(buf, p, len); return 0; }
    uint8_t *o = buf;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t v;
        if (mem_read_slow(cpu, addr + i, 1, &v)) return -1;
        o[i] = (uint8_t)v;
    }
    return 0;
}

int qsim_write(qsim_cpu *cpu, uint32_t addr, const void *buf, uint32_t len)
{
    uint8_t *p = qsim_mem_ptr(cpu, addr, len);
    if (p) {
        memcpy(p, buf, len);
        qsim_invalidate(cpu, addr, len);
        return 0;
    }
    const uint8_t *in = buf;
    for (uint32_t i = 0; i < len; i++)
        if (mem_write_slow(cpu, addr + i, 1, in[i])) return -1;
    return 0;
}

int qsim_read32(qsim_cpu *cpu, uint32_t addr, uint32_t *v) { return qsim_read(cpu, addr, v, 4); }
int qsim_write32(qsim_cpu *cpu, uint32_t addr, uint32_t v) { return qsim_write(cpu, addr, &v, 4); }

static int amo_read(qsim_cpu *cpu, uint32_t a, uint32_t *v)
{
    if (cpu->trap_misaligned && (a & 3)) return 1;
    uint32_t off = a - cpu->sdram_base;
    if (off < cpu->sdram_size) {
        if (cpu->lint) lint_cpu_access(cpu->lint, off, 4, 0, cpu->pc);
        memcpy(v, cpu->sdram + off, 4);
        return 0;
    }
    return mem_read_slow(cpu, a, 4, v) != 0;
}

static int amo_write(qsim_cpu *cpu, uint32_t a, uint32_t v)
{
    if (cpu->trap_misaligned && (a & 3)) return 1;
    uint32_t off = a - cpu->sdram_base;
    if (off < cpu->sdram_size) {
        if (cpu->lint) lint_cpu_access(cpu->lint, off, 4, 1, cpu->pc);
        memcpy(cpu->sdram + off, &v, 4);
        qsim_invalidate(cpu, a, 4);
        return 0;
    }
    return mem_write_slow(cpu, a, 4, v) != 0;
}

/* ------------------------------------------------------------------------ */
/* traps, interrupts, CSRs                                                   */

void qsim_enter_guest_trap(qsim_cpu *cpu, uint32_t cause, uint32_t tval, uint32_t epc)
{
    cpu->mepc = epc & ~1u;
    cpu->mcause = cause;
    cpu->mtval = tval;
    uint32_t ms = cpu->mstatus;
    ms = (ms & ~(1u << 7)) | ((ms & (1u << 3)) << 4);   /* MPIE = MIE */
    ms &= ~(1u << 3);                                   /* MIE = 0 */
    ms |= 3u << 11;                                     /* MPP = M */
    cpu->mstatus = ms;
    uint32_t base = cpu->mtvec & ~3u;
    if ((cpu->mtvec & 1) && (cause & 0x80000000u)) base += 4 * (cause & 0x7fffffffu);
    cpu->pc = base;
}

/* returns QSIM_TRAP_* ; cpu->pc must be the trapping pc */
static int take_trap(qsim_cpu *cpu, uint32_t cause, uint32_t tval)
{
    int act;
    if (cpu->in_run) fpu_host_fold(cpu);
    if (cpu->env.trap) act = cpu->env.trap(cpu->env.ctx, cpu, cause, tval);
    else act = cpu->mtvec ? QSIM_TRAP_GUEST : QSIM_TRAP_STOP;
    if (cpu->in_run) _mm_setcsr(QSIM_MXCSR_DEFAULT);
    if (act == QSIM_TRAP_GUEST) qsim_enter_guest_trap(cpu, cause, tval, cpu->pc);
    else if (act == QSIM_TRAP_STOP) { cpu->trap_cause = cause; cpu->trap_tval = tval; }
    return act;
}

void qsim_request_stop(qsim_cpu *cpu) { cpu->stop_req = 1; cpu->async = 1; }

void qsim_set_mip(qsim_cpu *cpu, uint32_t mask, int level)
{
    if (level) cpu->mip |= mask; else cpu->mip &= ~mask;
    cpu->async = 1;
}

static int irq_pending(qsim_cpu *cpu)
{
    return (cpu->mstatus & (1u << 3)) && (cpu->mip & cpu->mie);
}

static void take_irq(qsim_cpu *cpu)
{
    uint32_t p = cpu->mip & cpu->mie;
    static const int prio[] = {11, 3, 7, 9, 1, 5};   /* MEI, MSI, MTI, SEI, SSI, STI */
    int bit = 31 - __builtin_clz(p);
    for (unsigned i = 0; i < sizeof prio / sizeof prio[0]; i++)
        if (p & (1u << prio[i])) { bit = prio[i]; break; }
    qsim_enter_guest_trap(cpu, 0x80000000u | (uint32_t)bit, 0, cpu->pc);
}

static int csr_read(qsim_cpu *cpu, uint32_t csr, uint32_t *v)
{
    switch (csr) {
    case 0x001: *v = qsim_fflags(cpu); return 0;
    case 0x002: *v = cpu->frm; return 0;
    case 0x003: *v = ((uint32_t)cpu->frm << 5) | qsim_fflags(cpu); return 0;
    case 0x300: *v = cpu->mstatus; return 0;
    case 0x301: *v = 0x40001125u; return 0;     /* RV32 A C F I M */
    case 0x304: *v = cpu->mie; return 0;
    case 0x305: *v = cpu->mtvec; return 0;
    case 0x340: *v = cpu->mscratch; return 0;
    case 0x341: *v = cpu->mepc; return 0;
    case 0x342: *v = cpu->mcause; return 0;
    case 0x343: *v = cpu->mtval; return 0;
    case 0x344: *v = cpu->mip; return 0;
    case 0x7ff: *v = cpu->csr_7ff; return 0;
    case 0xf11: case 0xf12: case 0xf13: case 0xf14: *v = 0; return 0;
    /* counters: not in the os25 core (no zicntr), provided for tooling */
    case 0xb00: case 0xb02: case 0xc00: case 0xc02:
        *v = (uint32_t)cpu->instret_live; return 0;
    case 0xb80: case 0xb82: case 0xc80: case 0xc82:
        *v = (uint32_t)(cpu->instret_live >> 32); return 0;
    }
    if (cpu->env.csr) {
        if (cpu->in_run) fpu_host_fold(cpu);
        int r = cpu->env.csr(cpu->env.ctx, cpu, csr, 0, v);
        if (cpu->in_run) _mm_setcsr(QSIM_MXCSR_DEFAULT);
        return r;
    }
    return -1;
}

static int csr_write(qsim_cpu *cpu, uint32_t csr, uint32_t v)
{
    if ((csr >> 10) == 3) return -1;            /* read-only range */
    switch (csr) {
    case 0x001: qsim_set_fflags(cpu, v); return 0;
    case 0x002: cpu->frm = v & 7; return 0;
    case 0x003: qsim_set_fflags(cpu, v); cpu->frm = (v >> 5) & 7; return 0;
    case 0x300:
        cpu->mstatus = (cpu->mstatus & ~0x7888u) | (v & 0x7888u) | (3u << 11);
        cpu->async = 1;
        return 0;
    case 0x301: return 0;
    case 0x304: cpu->mie = v & 0x888u; cpu->async = 1; return 0;
    case 0x305: cpu->mtvec = v & ~2u; return 0;
    case 0x340: cpu->mscratch = v; return 0;
    case 0x341: cpu->mepc = v & ~1u; return 0;
    case 0x342: cpu->mcause = v; return 0;
    case 0x343: cpu->mtval = v; return 0;
    case 0x344: cpu->mip = (cpu->mip & ~0x888u) | (v & 0x888u); cpu->async = 1; return 0;
    case 0x7ff: cpu->csr_7ff = v & 1; return 0;
    case 0xb00: case 0xb02: case 0xb80: case 0xb82: return 0;
    }
    if (cpu->env.csr) {
        if (cpu->in_run) fpu_host_fold(cpu);
        int r = cpu->env.csr(cpu->env.ctx, cpu, csr, 1, &v);
        if (cpu->in_run) _mm_setcsr(QSIM_MXCSR_DEFAULT);
        return r;
    }
    return -1;
}

/* CSR instruction; returns 0 or -1 (illegal) */
static int csr_insn(qsim_cpu *cpu, const qsim_dinsn *e, int op)
{
    uint32_t csr = (uint32_t)e->imm & 0xfff;
    uint32_t src;
    int imm_form = op >= QOP_CSRRWI;
    src = imm_form ? e->rs1 : cpu->x[e->rs1];
    int base = imm_form ? op - QOP_CSRRWI + QOP_CSRRW : op;
    uint32_t old = 0;
    if (base == QOP_CSRRW) {
        if (e->rd != 32 && csr_read(cpu, csr, &old)) return -1;
        if (e->rd == 32 && (csr >> 10) != 3) {
            /* write-only access: still must be a known CSR */
            uint32_t dummy;
            if (csr_read(cpu, csr, &dummy)) return -1;
        }
        if (csr_write(cpu, csr, src)) return -1;
    } else {
        if (csr_read(cpu, csr, &old)) return -1;
        if (e->rs1 != 0) {
            uint32_t nv = base == QOP_CSRRS ? (old | src) : (old & ~src);
            if (csr_write(cpu, csr, nv)) return -1;
        }
    }
    cpu->x[e->rd] = old;
    cpu->x[32] = 0;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* decoder                                                                   */

static inline int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

static inline uint8_t rdx(uint32_t r) { return r ? (uint8_t)r : 32; }

/* Fill e for instruction word 'insn' (16 or 32 bit per 'len').  Returns the op. */
static int decode_word(qsim_dinsn *e, uint32_t insn, unsigned len)
{
    e->rd = 32; e->rs1 = 0; e->rs2 = 0; e->imm = 0;
    e->lf = (uint8_t)(len >> 1);
    if (len == 4) {
        uint32_t opc = insn & 0x7f, rd = (insn >> 7) & 31, f3 = (insn >> 12) & 7;
        uint32_t rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31, f7 = insn >> 25;
        int32_t immi = sext(insn >> 20, 12);
        int32_t imms = sext(((insn >> 25) << 5) | ((insn >> 7) & 31), 12);
        int32_t immb = sext(((insn >> 31) << 12) | (((insn >> 7) & 1) << 11) |
                            (((insn >> 25) & 63) << 5) | (((insn >> 8) & 15) << 1), 13);
        int32_t immj = sext(((insn >> 31) << 20) | (((insn >> 12) & 255) << 12) |
                            (((insn >> 20) & 1) << 11) | (((insn >> 21) & 1023) << 1), 21);
        e->rd = rdx(rd); e->rs1 = (uint8_t)rs1; e->rs2 = (uint8_t)rs2;
        switch (opc) {
        case 0x37: e->imm = (int32_t)(insn & 0xfffff000u); return QOP_LUI;
        case 0x17: e->imm = (int32_t)(insn & 0xfffff000u); return QOP_AUIPC;
        case 0x6f: e->imm = immj; return QOP_JAL;
        case 0x67: if (f3) break; e->imm = immi; return QOP_JALR;
        case 0x63: {
            static const int8_t bops[8] = {QOP_BEQ, QOP_BNE, -1, -1, QOP_BLT, QOP_BGE, QOP_BLTU, QOP_BGEU};
            if (bops[f3] < 0) break;
            e->imm = immb; e->rd = 32;
            return bops[f3];
        }
        case 0x03: {
            static const int8_t lops[8] = {QOP_LB, QOP_LH, QOP_LW, -1, QOP_LBU, QOP_LHU, -1, -1};
            if (lops[f3] < 0) break;
            e->imm = immi;
            return lops[f3];
        }
        case 0x23: {
            static const int8_t sops[8] = {QOP_SB, QOP_SH, QOP_SW, -1, -1, -1, -1, -1};
            if (sops[f3] < 0) break;
            e->imm = imms; e->rd = 32;
            return sops[f3];
        }
        case 0x13:
            e->imm = immi;
            switch (f3) {
            case 0: return QOP_ADDI;
            case 2: return QOP_SLTI;
            case 3: return QOP_SLTIU;
            case 4: return QOP_XORI;
            case 6: return QOP_ORI;
            case 7: return QOP_ANDI;
            case 1: if (f7 != 0) break; e->imm = rs2; return QOP_SLLI;
            case 5:
                e->imm = rs2;
                if (f7 == 0) return QOP_SRLI;
                if (f7 == 0x20) return QOP_SRAI;
                break;
            }
            break;
        case 0x33:
            if (f7 == 0) {
                static const int8_t o[8] = {QOP_ADD, QOP_SLL, QOP_SLT, QOP_SLTU, QOP_XOR, QOP_SRL, QOP_OR, QOP_AND};
                return o[f3];
            }
            if (f7 == 0x20) { if (f3 == 0) return QOP_SUB; if (f3 == 5) return QOP_SRA; break; }
            if (f7 == 1) {
                static const int8_t o[8] = {QOP_MUL, QOP_MULH, QOP_MULHSU, QOP_MULHU, QOP_DIV, QOP_DIVU, QOP_REM, QOP_REMU};
                return o[f3];
            }
            break;
        case 0x0f:
            if (f3 == 0 || f3 == 1) { e->rd = 32; return QOP_NOP; }     /* fence / fence.i */
            if (f3 == 2 && rd == 0 && (immi == 0 || immi == 1 || immi == 2)) {
                e->imm = immi; e->rd = 32;                              /* cbo.inval/clean/flush */
                return QOP_CBO;
            }
            break;
        case 0x73:
            if (f3 == 0) {
                if (rd != 0 || rs1 != 0) break;
                if (insn == 0x00000073u) return QOP_ECALL;
                if (insn == 0x00100073u) return QOP_EBREAK;
                if (insn == 0x30200073u) return QOP_MRET;
                if (insn == 0x10500073u) return QOP_WFI;
                break;
            }
            if (f3 == 4) break;
            e->imm = (int32_t)(insn >> 20);
            return QOP_CSRRW + (int)f3 - 1 - (f3 > 4);
        case 0x2f:
            if (f3 != 2) break;
            switch (f7 >> 2) {
            case 0x02: if (rs2) break; return QOP_LR;
            case 0x03: return QOP_SC;
            case 0x01: return QOP_AMOSWAP;
            case 0x00: return QOP_AMOADD;
            case 0x04: return QOP_AMOXOR;
            case 0x0c: return QOP_AMOAND;
            case 0x08: return QOP_AMOOR;
            case 0x10: return QOP_AMOMIN;
            case 0x14: return QOP_AMOMAX;
            case 0x18: return QOP_AMOMINU;
            case 0x1c: return QOP_AMOMAXU;
            }
            break;
        case 0x07: if (f3 != 2) break; e->imm = immi; e->rd = (uint8_t)rd; return QOP_FLW;
        case 0x27: if (f3 != 2) break; e->imm = imms; e->rd = 32; return QOP_FSW;
        case 0x53: {
            int rm_ok = (f3 <= 4 || f3 == 7);
            e->imm = (int32_t)f3;
            e->rd = (uint8_t)rd;                /* default: float rd */
            switch (f7) {
            case 0x00: if (!rm_ok) break; return QOP_FADD;
            case 0x04: if (!rm_ok) break; return QOP_FSUB;
            case 0x08: if (!rm_ok) break; return QOP_FMUL;
            case 0x0c: if (!rm_ok) break; return QOP_FDIV;
            case 0x2c: if (!rm_ok || rs2) break; return QOP_FSQRT;
            case 0x10: if (f3 > 2) break; return QOP_FSGNJ + (int)f3;
            case 0x14: if (f3 > 1) break; return f3 ? QOP_FMAX : QOP_FMIN;
            case 0x60: if (!rm_ok || rs2 > 1) break; e->rd = rdx(rd); return rs2 ? QOP_FCVT_WU : QOP_FCVT_W;
            case 0x68: if (!rm_ok || rs2 > 1) break; return rs2 ? QOP_FCVT_SWU : QOP_FCVT_SW;
            case 0x70:
                if (rs2) break;
                e->rd = rdx(rd);
                if (f3 == 0) return QOP_FMV_XW;
                if (f3 == 1) return QOP_FCLASS;
                break;
            case 0x78: if (rs2 || f3) break; return QOP_FMV_WX;
            case 0x50:
                if (f3 > 2) break;
                e->rd = rdx(rd);
                return f3 == 2 ? QOP_FEQ : (f3 == 1 ? QOP_FLT : QOP_FLE);
            }
            break;
        }
        case 0x43: case 0x47: case 0x4b: case 0x4f:
            if (((insn >> 25) & 3) != 0) break;                  /* single only */
            if (!(f3 <= 4 || f3 == 7)) break;
            e->rd = (uint8_t)rd;
            e->imm = (int32_t)((insn >> 27) | (f3 << 8));        /* rs3 | rm << 8 */
            return opc == 0x43 ? QOP_FMADD : opc == 0x47 ? QOP_FMSUB : opc == 0x4b ? QOP_FNMSUB : QOP_FNMADD;
        }
        e->imm = (int32_t)insn;
        e->rd = 32;
        return QOP_ILLEGAL;
    }

    /* ---- 16-bit compressed ---- */
    uint32_t c = insn & 0xffff;
    uint32_t op = c & 3, f3 = (c >> 13) & 7;
    uint32_t r1 = (c >> 7) & 31, r2 = (c >> 2) & 31;          /* full reg fields */
    uint32_t r1s = ((c >> 7) & 7) + 8, r2s = ((c >> 2) & 7) + 8;  /* 3-bit fields */
    int32_t imm6 = sext(((c >> 12) & 1) << 5 | ((c >> 2) & 31), 6);
    switch (op) {
    case 0:
        switch (f3) {
        case 0: {   /* c.addi4spn */
            uint32_t imm = ((c >> 6) & 1) << 2 | ((c >> 5) & 1) << 3 | ((c >> 11) & 3) << 4 | ((c >> 7) & 15) << 6;
            if (!imm) break;
            e->rd = (uint8_t)r2s; e->rs1 = 2; e->imm = (int32_t)imm;
            return QOP_ADDI;
        }
        case 2: case 3: case 6: case 7: {   /* c.lw c.flw c.sw c.fsw */
            uint32_t imm = ((c >> 6) & 1) << 2 | ((c >> 10) & 7) << 3 | ((c >> 5) & 1) << 6;
            e->rs1 = (uint8_t)r1s; e->imm = (int32_t)imm;
            if (f3 == 2) { e->rd = (uint8_t)r2s; return QOP_LW; }
            if (f3 == 3) { e->rd = (uint8_t)r2s; return QOP_FLW; }
            e->rs2 = (uint8_t)r2s;
            return f3 == 6 ? QOP_SW : QOP_FSW;
        }
        }
        break;
    case 1:
        switch (f3) {
        case 0: case 2: /* c.addi (c.nop) / c.li */
            e->rd = rdx(r1); e->rs1 = f3 == 0 ? (uint8_t)r1 : 0; e->imm = imm6;
            return QOP_ADDI;
        case 1: case 5: { /* c.jal / c.j */
            int32_t imm = sext(((c >> 12) & 1) << 11 | ((c >> 11) & 1) << 4 | ((c >> 9) & 3) << 8 |
                               ((c >> 8) & 1) << 10 | ((c >> 7) & 1) << 6 | ((c >> 6) & 1) << 7 |
                               ((c >> 3) & 7) << 1 | ((c >> 2) & 1) << 5, 12);
            e->rd = f3 == 1 ? 1 : 32; e->imm = imm;
            return QOP_JAL;
        }
        case 3:
            if (r1 == 2) {  /* c.addi16sp */
                int32_t imm = sext(((c >> 12) & 1) << 9 | ((c >> 6) & 1) << 4 | ((c >> 5) & 1) << 6 |
                                   ((c >> 3) & 3) << 7 | ((c >> 2) & 1) << 5, 10);
                if (!imm) break;
                e->rd = 2; e->rs1 = 2; e->imm = imm;
                return QOP_ADDI;
            }
            if (imm6 == 0) break;        /* c.lui with 0 imm is reserved */
            e->rd = rdx(r1); e->imm = imm6 * 4096;   /* rd==0: HINT */
            return QOP_LUI;
        case 4: {
            uint32_t f2 = (c >> 10) & 3;
            e->rd = (uint8_t)r1s; e->rs1 = (uint8_t)r1s;
            if (f2 == 0 || f2 == 1) {       /* c.srli / c.srai */
                if ((c >> 12) & 1) break;   /* shamt[5] must be 0 on RV32 */
                e->imm = (int32_t)r2;
                return f2 ? QOP_SRAI : QOP_SRLI;
            }
            if (f2 == 2) { e->imm = imm6; return QOP_ANDI; }
            if ((c >> 12) & 1) break;       /* subw/addw: RV64 only */
            e->rs2 = (uint8_t)r2s;
            switch ((c >> 5) & 3) {
            case 0: return QOP_SUB;
            case 1: return QOP_XOR;
            case 2: return QOP_OR;
            default: return QOP_AND;
            }
        }
        case 6: case 7: {   /* c.beqz / c.bnez */
            int32_t imm = sext(((c >> 12) & 1) << 8 | ((c >> 10) & 3) << 3 | ((c >> 5) & 3) << 6 |
                               ((c >> 3) & 3) << 1 | ((c >> 2) & 1) << 5, 9);
            e->rs1 = (uint8_t)r1s; e->rs2 = 0; e->imm = imm; e->rd = 32;
            return f3 == 6 ? QOP_BEQ : QOP_BNE;
        }
        }
        break;
    case 2:
        switch (f3) {
        case 0: /* c.slli */
            if ((c >> 12) & 1) break;
            e->rd = rdx(r1); e->rs1 = (uint8_t)r1; e->imm = (int32_t)r2;
            return QOP_SLLI;
        case 2: case 3: { /* c.lwsp / c.flwsp */
            uint32_t imm = ((c >> 12) & 1) << 5 | ((c >> 4) & 7) << 2 | ((c >> 2) & 3) << 6;
            e->rs1 = 2; e->imm = (int32_t)imm;
            if (f3 == 2) { if (!r1) break; e->rd = (uint8_t)r1; return QOP_LW; }
            e->rd = (uint8_t)r1;
            return QOP_FLW;
        }
        case 4:
            if (!((c >> 12) & 1)) {
                if (r2 == 0) {        /* c.jr */
                    if (!r1) break;
                    e->rs1 = (uint8_t)r1; e->rd = 32; e->imm = 0;
                    return QOP_JALR;
                }
                e->rd = rdx(r1); e->rs1 = 0; e->rs2 = (uint8_t)r2;   /* c.mv */
                return QOP_ADD;
            }
            if (r2 == 0) {
                if (r1 == 0) return QOP_EBREAK;
                e->rs1 = (uint8_t)r1; e->rd = 1; e->imm = 0;           /* c.jalr */
                return QOP_JALR;
            }
            e->rd = rdx(r1); e->rs1 = (uint8_t)r1; e->rs2 = (uint8_t)r2;   /* c.add */
            return QOP_ADD;
        case 6: case 7: { /* c.swsp / c.fswsp */
            uint32_t imm = ((c >> 9) & 15) << 2 | ((c >> 7) & 3) << 6;
            e->rs1 = 2; e->rs2 = (uint8_t)r2; e->imm = (int32_t)imm; e->rd = 32;
            return f3 == 6 ? QOP_SW : QOP_FSW;
        }
        }
        break;
    }
    e->imm = (int32_t)c;
    e->rd = 32;
    return QOP_ILLEGAL;
}

/* decode the entry for address pc (must be inside BRAM or SDRAM) */
static void decode_entry(qsim_cpu *cpu, qsim_dinsn *e, uint32_t pc)
{
    uint8_t *base; uint32_t lim, off;
    int in_sd = e >= cpu->sd_dc;
    if (unlikely(cpu->nhooks) && is_hook(cpu, pc)) {
        memset(e, 0, sizeof *e);
        e->pc = pc;
        e->lf = 2;
        e->rd = 32;
        e->h = QOP_HOOK;
        return;
    }
    if (in_sd) { base = cpu->sdram; lim = cpu->sdram_size; off = pc - cpu->sdram_base; }
    else { base = cpu->bram; lim = cpu->bram_size; off = pc; }
    uint16_t lo;
    memcpy(&lo, base + off, 2);
    unsigned len = (lo & 3) == 3 ? 4 : 2;
    int op;
    if (len == 4 && off + 4 > lim) {
        op = QOP_FETCH_FAULT;
        memset(e, 0, sizeof *e);
        e->lf = 2;
        e->imm = (int32_t)(pc + 2);
    } else {
        uint32_t w = lo;
        if (len == 4) { uint16_t hi; memcpy(&hi, base + off + 2, 2); w |= (uint32_t)hi << 16; }
        qsim_dinsn t;
        op = decode_word(&t, w, len);
        /* direct branch/jump targets must stay inside this region */
        if (op == QOP_JAL || (op >= QOP_BEQ && op <= QOP_BGEU)) {
            uint32_t tgt = pc + (uint32_t)t.imm;
            uint32_t toff = in_sd ? tgt - cpu->sdram_base : tgt;
            if (toff >= lim) {
                if (op == QOP_JAL) op = QOP_JFAR;
                else { t.rd = (uint8_t)(op - QOP_BEQ); op = QOP_BFAR; }
            }
        }
        *e = t;
        e->imm = t.imm;
    }
    e->pc = pc;
    e->h = op;
    if (in_sd) {
        if (off < cpu->code_lo) cpu->code_lo = off;
        if (off + 4 > cpu->code_hi) cpu->code_hi = off + 4;
    } else {
        if (off + 4 > cpu->br_code_hi) cpu->br_code_hi = off + 4;
    }
}

int qsim_decode_name(uint32_t insn, const char **name)
{
    qsim_dinsn e;
    unsigned len = (insn & 3) == 3 ? 4 : 2;
    int op = decode_word(&e, insn, len);
    if (name) *name = op_name_tab[op];
    return op == QOP_ILLEGAL ? 0 : (int)len;
}

int qsim_decode_fields(uint32_t insn, const char **name, int *rd, int *rs1, int *rs2, int32_t *imm)
{
    qsim_dinsn e;
    unsigned len = (insn & 3) == 3 ? 4 : 2;
    int op = decode_word(&e, insn, len);
    if (name) *name = op_name_tab[op];
    *rd = e.rd == 32 ? 0 : e.rd;
    *rs1 = e.rs1;
    *rs2 = e.rs2;
    *imm = e.imm;
    return op == QOP_ILLEGAL ? 0 : (int)len;
}

int qsim_opclass_of(uint32_t insn)
{
    qsim_dinsn e;
    unsigned len = (insn & 3) == 3 ? 4 : 2;
    return op_class_tab[decode_word(&e, insn, len)];
}

const char *qsim_opclass_name(int oc)
{
    static const char *n[QSIM_OC_COUNT] = {
        "other", "branch", "jal", "jalr", "load", "store", "mul", "div", "fadd/fsub", "fmul",
        "fdiv", "fsqrt", "fmadd*", "fcvt", "fcmp", "fmisc", "flw", "fsw", "csr", "system",
        "cbo", "amo"};
    return (oc >= 0 && oc < QSIM_OC_COUNT) ? n[oc] : "?";
}

const char *qsim_op_name(int op) { return (op >= 0 && op < QOP_COUNT) ? op_name_tab[op] : "?"; }

/* ------------------------------------------------------------------------ */
/* interpreters                                                              */

enum { EXEC_BUDGET, EXEC_ASYNC, EXEC_TRAPSTOP };

#define QX_PROF 0
#define QX_LINT 0
#define QX_FN exec_fast
#include "cpu_exec.h"
#undef QX_PROF
#undef QX_LINT
#undef QX_FN

#define QX_PROF 0
#define QX_LINT 1
#define QX_FN exec_fast_lint
#include "cpu_exec.h"
#undef QX_PROF
#undef QX_LINT
#undef QX_FN

#define QX_PROF 1
#define QX_LINT 1
#define QX_FN exec_prof
#include "cpu_exec.h"
#undef QX_PROF
#undef QX_LINT
#undef QX_FN

int qsim_enable_profiling(qsim_cpu *cpu, const qsim_model_params *mp)
{
    if (!cpu->prof) {
        cpu->prof = qsim_prof_new(cpu->dc_entries);
        if (!cpu->prof) return -1;
    }
    if (!cpu->model) {
        qsim_model_params p;
        if (mp) p = *mp; else qsim_model_default_params(&p);
        p.sd_base = cpu->sdram_base; p.sd_size = cpu->sdram_size; p.bram_size = cpu->bram_size;
        p.sd_alias_base = cpu->sdram_alias_base;
        cpu->model = qsim_model_new(&p);
        if (!cpu->model) return -1;
    }
    cpu->model->ev_ic = cpu->prof->ev_ic;
    cpu->model->ev_dc = cpu->prof->ev_dc;
    cpu->model->ev_bp = cpu->prof->ev_bp;
    cpu->model->ev_i = &cpu->prof->last_i;
    cpu->profiling = 1;
    return 0;
}

void qsim_charge_cycles(qsim_cpu *cpu, uint32_t pc, uint64_t cycles)
{
    qsim_model *m = cpu->model;
    qsim_prof *P = cpu->prof;
    if (!cpu->profiling || !m || !P || !cycles) return;
    size_t i;
    uint32_t o = pc - cpu->sdram_base;
    if (o < cpu->sdram_size) i = (size_t)(cpu->sd_dc - cpu->dc_base) + (o >> 1);
    else if (pc < cpu->bram_size) i = (size_t)(cpu->br_dc - cpu->dc_base) + (pc >> 1);
    else return;
    P->pen[P->last_i] += m->cyc - P->last_cyc;      /* settle the caller's instruction first */
    m->cyc += cycles;
    P->pen[i] += cycles;
    P->last_cyc = m->cyc;
}

int qsim_run(qsim_cpu *cpu, uint64_t max_insns)
{
    uint32_t host_csr = _mm_getcsr();
    _mm_setcsr(QSIM_MXCSR_DEFAULT);
    cpu->in_run = 1;
    int reason = QSIM_RUN_BUDGET;
    uint64_t left = max_insns;
    cpu->instret_live = cpu->instret;
    while (left) {
        if (cpu->stop_req) { cpu->stop_req = 0; reason = QSIM_RUN_STOP; break; }
        cpu->async = 0;
        if (irq_pending(cpu)) take_irq(cpu);
        uint64_t done = 0;
        int r = cpu->profiling ? exec_prof(cpu, left, &done)
              : cpu->lint ? exec_fast_lint(cpu, left, &done) : exec_fast(cpu, left, &done);
        left -= done;
        cpu->instret += done;
        cpu->instret_live = cpu->instret;
        if (r == EXEC_TRAPSTOP) { reason = QSIM_RUN_TRAP; break; }
    }
    if (reason == QSIM_RUN_BUDGET && cpu->stop_req) { cpu->stop_req = 0; reason = QSIM_RUN_STOP; }
    fpu_host_fold(cpu);
    cpu->in_run = 0;
    _mm_setcsr(host_csr);
    cpu->run_reason = reason;
    return reason;
}
