//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * qsim CPU core -- RV32IMAFC + Zicsr + Zifencei + Zicbom (no-op), M-mode only.
 *
 * Design summary (see README.md "CPU core" for the long version):
 *   - Guest memory is two flat host arrays: SDRAM (default base 0x10000000,
 *     64 MB) and BRAM (base 0, 32 KB, covers APP_BRAM 0x4000-0x7800).  Every
 *     other address goes to the environment MMIO callbacks.
 *   - Execution uses a decoded-instruction cache laid out as a virtual arena
 *     parallel to guest memory: one 16-byte entry per 2 bytes of RAM.  The
 *     arena is mmap(MAP_NORESERVE) so only pages holding executed code are
 *     committed.  Entry index = (pc - region_base) / 2, so there is no hash
 *     and no page table on the hot path; direct branches are pointer offsets.
 *   - Two interpreter builds from one source (cpu_exec.h): a fast one and a
 *     profiling one (per-PC counts, call counts, cache/timing model hooks).
 *   - Guest writes into already-decoded code invalidate the affected entries
 *     (self-modifying code is supported, not expected).
 *   - F extension: round-to-nearest ops use host SSE (exact IEEE-754 single),
 *     flags are accrued lazily from MXCSR; all other rounding modes and every
 *     conversion use an exact integer soft-float path (fpu.c).
 */
#ifndef QSIM_CPU_H
#define QSIM_CPU_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qsim_cpu qsim_cpu;
struct qsim_model;   /* cache.h */
struct qsim_prof;    /* prof.h  */
struct qsim_lint;    /* lint.h  */

/* ---- trap causes (mcause values) ------------------------------------- */
enum {
    QSIM_CAUSE_FETCH_MISALIGNED = 0,
    QSIM_CAUSE_FETCH_FAULT      = 1,
    QSIM_CAUSE_ILLEGAL          = 2,
    QSIM_CAUSE_BREAKPOINT       = 3,
    QSIM_CAUSE_LOAD_MISALIGNED  = 4,
    QSIM_CAUSE_LOAD_FAULT       = 5,
    QSIM_CAUSE_STORE_MISALIGNED = 6,
    QSIM_CAUSE_STORE_FAULT      = 7,
    QSIM_CAUSE_ECALL_U          = 8,
    QSIM_CAUSE_ECALL_M          = 11,
};

/* Return values of qsim_env.trap */
enum {
    QSIM_TRAP_RESUME = 0,  /* env handled it; execution continues at cpu->pc
                              (for ecall the env must advance pc by 4) */
    QSIM_TRAP_GUEST  = 1,  /* deliver to the guest: mepc/mcause/mtval, pc=mtvec */
    QSIM_TRAP_STOP   = 2,  /* stop qsim_run; cpu->pc = trapping instruction */
};

/* Reasons qsim_run returns */
enum {
    QSIM_RUN_BUDGET = 0,   /* instruction budget exhausted */
    QSIM_RUN_STOP   = 1,   /* qsim_request_stop() called (env) */
    QSIM_RUN_TRAP   = 2,   /* trap callback returned QSIM_TRAP_STOP / no handler */
};

typedef struct qsim_env {
    void *ctx;
    /* MMIO: any address not in SDRAM/BRAM.  size = 1, 2 or 4.  Return 0 on
     * success, nonzero raises a load/store access fault.  NULL = fault. */
    int (*mmio_read)(void *ctx, qsim_cpu *cpu, uint32_t addr, unsigned size, uint32_t *val);
    int (*mmio_write)(void *ctx, qsim_cpu *cpu, uint32_t addr, unsigned size, uint32_t val);
    /* Synchronous trap (ecall, ebreak, illegal instruction, access faults,
     * misaligned fetch).  cpu->pc = trapping instruction (cpu->trap_len = its
     * length, 0 for fetch faults), fcsr is synced.
     * NULL => QSIM_TRAP_GUEST when mtvec != 0, else QSIM_TRAP_STOP. */
    int (*trap)(void *ctx, qsim_cpu *cpu, uint32_t cause, uint32_t tval);
    /* CSRs the core does not implement.  Return 0 if handled, nonzero =>
     * illegal instruction.  write: *val holds the new value. */
    int (*csr)(void *ctx, qsim_cpu *cpu, uint32_t csrno, int write, uint32_t *val);
    /* Zicbom cbo.clean / cbo.flush / cbo.inval executed (op 1/2/0). optional */
    void (*cbo)(void *ctx, qsim_cpu *cpu, unsigned op, uint32_t addr);
    /* Control reached an address registered with qsim_add_hook() (e.g. the
     * OS services-table trap addresses).  cpu->pc = hook address.  Set
     * cpu->pc (typically to x[1]) and return QSIM_TRAP_RESUME, or
     * QSIM_TRAP_STOP.  The hook counts as one instruction for the budget but
     * is not an instruction in the profile. */
    int (*hook)(void *ctx, qsim_cpu *cpu, uint32_t pc);
} qsim_env;

typedef struct qsim_config {
    uint32_t sdram_base;       /* default 0x10000000 */
    uint32_t sdram_size;       /* default 64 MB (PMA region size) */
    uint32_t bram_size;        /* base 0; default 0x8000 */
    uint32_t sdram_alias_base; /* uncached alias of the same bytes; default
                                  0x50000000; 0xffffffff disables */
} qsim_config;

/* Decoded instruction (16 bytes).  Internal but exposed for prof/tests. */
typedef struct qsim_dinsn {
    int32_t  h;      /* op id (cpu_ops.h); 0 = not decoded yet */
    uint32_t pc;
    int32_t  imm;    /* immediate / csr number / rm / rs3 depending on op */
    uint8_t  rd;     /* integer rd 0 is remapped to 32 (write sink) */
    uint8_t  rs1, rs2;
    uint8_t  lf;     /* entry step: 1 = 16-bit instruction, 2 = 32-bit */
} qsim_dinsn;

struct qsim_cpu {
    /* architectural state */
    uint32_t x[33];          /* x[32] = sink for rd == 0 */
    uint32_t f[32];          /* raw IEEE-754 single bit patterns */
    uint32_t pc;
    uint8_t  fflags;         /* accrued; see qsim_fflags() (lazy MXCSR part) */
    uint8_t  frm;
    uint8_t  resv_valid;     /* LR/SC */
    uint8_t  fpu_host_dirty; /* host MXCSR may hold unsynced flags */
    uint32_t resv_addr;
    uint32_t mstatus, mie, mip, mtvec, mscratch, mepc, mcause, mtval;
    uint32_t csr_7ff;        /* VexiiRiscv PrefetcherNextLine disable CSR */
    uint64_t instret;        /* instructions executed (all runs) */

    /* memory */
    uint8_t *sdram;
    uint32_t sdram_base, sdram_size;
    uint8_t *bram;
    uint32_t bram_size;
    uint32_t sdram_alias_base;
    /* Data behind the uncached alias.  NULL = sdram (flat memory).  The
     * strict memory mode (lint.h) points it at the committed SDRAM image. */
    uint8_t *sdram_uc;
    /* The os25 LSU raises LOAD/STORE_MISALIGNED (mcause 4/6) for 2/4-byte
     * accesses with misaligned addresses (VexiiRiscv LsuPlugin.scala:571,
     * 829-832); the kernel emulates them.  0 (default): perform the access
     * (the model still counts/charges it), 1: trap to the environment. */
    int trap_misaligned;
    uint32_t *hooks;             /* sorted hook addresses */
    size_t nhooks;

    /* decode arena: [BRAM entries + guard][SDRAM entries + guard], one
     * mapping; entry i of a region <-> address region_base + 2*i */
    qsim_dinsn *dc_base;
    size_t dc_entries;
    qsim_dinsn *sd_dc, *br_dc;
    uint32_t code_lo, code_hi;   /* SDRAM offsets covered by decoded entries */
    uint32_t br_code_hi;         /* BRAM decoded bound */

    /* control */
    qsim_env env;
    int      stop_req;
    int      async;              /* something changed: re-check stop/irq */
    int      in_run;             /* inside qsim_run (host MXCSR owned by qsim) */
    int      run_reason;
    uint8_t  trap_len;           /* length of the trapping instruction (0 = fetch) */
    uint32_t trap_cause, trap_tval;  /* last trap that stopped the run */
    uint64_t instret_live;

    /* instrumentation (NULL = off) */
    struct qsim_prof  *prof;
    struct qsim_model *model;
    int profiling;               /* 1 = use the profiling interpreter */
    /* D-cache coherency lint (lint.h), both interpreters; NULL = off */
    struct qsim_lint  *lint;
    /* F add/sub/mul/div/sqrt/fused ops with a subnormal operand / result
     * (the os25 FPU is generated with --fpu-ignore-subnormal; qsim is exact
     * IEEE, so a nonzero count means device results may differ) */
    uint64_t fsub_in, fsub_out;
};

qsim_cpu *qsim_cpu_new(const qsim_config *cfg);  /* cfg may be NULL (defaults) */
void      qsim_cpu_free(qsim_cpu *cpu);
void      qsim_cpu_reset(qsim_cpu *cpu, uint32_t pc);

/* Host pointer to [addr, addr+len) inside SDRAM or BRAM, else NULL.  Writing
 * through it bypasses decode invalidation: call qsim_invalidate() afterwards if
 * the range may already have been executed. */
uint8_t *qsim_mem_ptr(qsim_cpu *cpu, uint32_t addr, uint32_t len);
/* Register an address whose execution calls env.hook instead of fetching. */
int      qsim_add_hook(qsim_cpu *cpu, uint32_t addr);
void     qsim_invalidate(qsim_cpu *cpu, uint32_t addr, uint32_t len);
/* Byte-level helpers honouring MMIO; return 0 on success. */
int qsim_read(qsim_cpu *cpu, uint32_t addr, void *buf, uint32_t len);
int qsim_write(qsim_cpu *cpu, uint32_t addr, const void *buf, uint32_t len);
int qsim_read32(qsim_cpu *cpu, uint32_t addr, uint32_t *v);
int qsim_write32(qsim_cpu *cpu, uint32_t addr, uint32_t v);

/* Execute up to max_insns instructions.  Returns QSIM_RUN_*. */
int  qsim_run(qsim_cpu *cpu, uint64_t max_insns);
/* From inside a callback: stop after the current instruction. */
void qsim_request_stop(qsim_cpu *cpu);
/* Interrupts: set/clear mip bits (e.g. 1<<7 MTIP, 1<<11 MEIP).  Taken at the
 * next qsim_run entry / after the current callback when mstatus.MIE & mie. */
void qsim_set_mip(qsim_cpu *cpu, uint32_t mask, int level);

/* fcsr access with lazy host-flag folding. */
uint32_t qsim_fflags(qsim_cpu *cpu);
void     qsim_set_fflags(qsim_cpu *cpu, uint32_t v);

/* Enable the profiling interpreter (per-PC counts + timing model).  mp may be
 * NULL for the os25 defaults (cache.c).  Returns 0 on success. */
struct qsim_model_params;
int qsim_enable_profiling(qsim_cpu *cpu, const struct qsim_model_params *mp);
const char *qsim_op_name(int op);

/* Environment work charged to the timing model (profiling only; no-op
 * otherwise): adds `cycles` to est_cycles and attributes them to the profile
 * slot of `pc` (e.g. an OS service hook address). */
void qsim_charge_cycles(qsim_cpu *cpu, uint32_t pc, uint64_t cycles);

/* Take a trap into the guest (sets mepc/mcause/mtval/mstatus, pc = mtvec). */
void qsim_enter_guest_trap(qsim_cpu *cpu, uint32_t cause, uint32_t tval, uint32_t epc);

/* Disassemble-lite: decode one instruction word for tests/debug.
 * Returns length (2/4) or 0 if illegal; name receives a mnemonic. */
int qsim_decode_name(uint32_t insn, const char **name);
/* Same, exposing the decoded fields (rd 0 reported as 0, not the sink). */
int qsim_decode_fields(uint32_t insn, const char **name, int *rd, int *rs1, int *rs2, int32_t *imm);

/* Op-id metadata (for post-hoc per-op-class statistics in prof.c). */
enum qsim_opclass {
    QSIM_OC_OTHER = 0, QSIM_OC_BRANCH, QSIM_OC_JAL, QSIM_OC_JALR, QSIM_OC_LOAD,
    QSIM_OC_STORE, QSIM_OC_MUL, QSIM_OC_DIV, QSIM_OC_FADD, QSIM_OC_FMUL,
    QSIM_OC_FDIV, QSIM_OC_FSQRT, QSIM_OC_FMA, QSIM_OC_FCVT, QSIM_OC_FCMP,
    QSIM_OC_FMISC, QSIM_OC_FLOAD, QSIM_OC_FSTORE, QSIM_OC_CSR, QSIM_OC_SYSTEM,
    QSIM_OC_CBO, QSIM_OC_AMO, QSIM_OC_COUNT
};
const char *qsim_opclass_name(int oc);
/* Classify an instruction word (32-bit or 16-bit form). */
int qsim_opclass_of(uint32_t insn);

#ifdef __cplusplus
}
#endif
#endif
