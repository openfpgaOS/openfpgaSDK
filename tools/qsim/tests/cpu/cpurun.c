//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * cpurun: host runner for bare-metal qsim CPU test ELFs.
 *
 *   cpurun [options] prog.elf
 *     --stubs META      check t_ops.elf records (META from build/cpu/guest)
 *     --vecs N          vectors per integer stub (F stubs get 4N)
 *     --prof            run the profiling interpreter + timing model, print report
 *     --tsv FILE        write the function profile as TSV (with --prof)
 *     --max N           instruction budget (default 20e9)
 *     --quiet           no guest console echo
 *
 * Test device at 0x4F000000:
 *   +0x00 W putchar          +0x04 W exit(code)
 *   +0x10 W select stub       +0x14 R vector count
 *   +0x18..0x28 R vector a,b,c,d,ctl (a read of +0x18 draws a new vector)
 *   +0x30 W result flags      +0x34 W result (checked)
 *   +0x40 W print hex word
 * ecall a7=93 exit(a0), a7=64 write(a0,a1,a2).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cpu.h"
#include "cache.h"
#include "prof.h"
#include "qelf.h"
#include "ref_f32.h"
#include "vecgen.h"

enum { K_NONE, K_R, K_I, K_LUI, K_AUIPC, K_BR, K_JUMP, K_MEM, K_FMEM, K_AMO, K_CSR, K_CONST, K_ILLEGAL, K_F };
enum { OP_NONE, OP_ADD, OP_SUB, OP_SLL, OP_SLT, OP_SLTU, OP_XOR, OP_SRL, OP_SRA, OP_OR, OP_AND,
       OP_MUL, OP_MULH, OP_MULHSU, OP_MULHU, OP_DIV, OP_DIVU, OP_REM, OP_REMU, OP_MV,
       OP_ADDI, OP_SLTI, OP_SLTIU, OP_XORI, OP_ORI, OP_ANDI, OP_SLLI, OP_SRLI, OP_SRAI,
       OP_BEQ, OP_BNE, OP_BLT, OP_BGE, OP_BLTU, OP_BGEU,
       OP_LB, OP_LBU, OP_LH, OP_LHU, OP_LW,
       OP_AMOSWAP, OP_AMOADD, OP_AMOXOR, OP_AMOAND, OP_AMOOR, OP_AMOMIN, OP_AMOMAX, OP_AMOMINU,
       OP_AMOMAXU, OP_LRSC,
       OP_CSRRW, OP_CSRRS, OP_CSRRC, OP_CSRRWI, OP_CSRRSI, OP_CSRRCI };
typedef struct { int kind, op, rm, imm, imm2, size, lsize; unsigned f; } stub_meta;
#define F_ZA 1
#define F_ZB 2
#define F_SAME 4
#define F_ZD 8
#define F_C 16
#define F_ALIAS 64
#define A_FROM_B 0x400
#define A_FROM_C 0x800
#define B_FROM_C 0x1000

#ifdef WITH_STUBS
#include "stubs_meta.h"
#else
#define NSTUBS 1
static const stub_meta stub_tab[1];
#endif

static const stub_meta *meta_tab;
static unsigned meta_n;

typedef struct {
    qsim_syms syms;
    int quiet;
    int exited;
    int exit_code;
    /* stub checking */
    int check;
    unsigned vecs;
    int cur;
    uint32_t va, vb, vc, vd, vctl, rflags;
    uint8_t prev_flags;
    uint64_t checks, fails;
    uint64_t kind_checks[16], kind_fails[16];
    uint32_t key_addr[4096];
    uint64_t console_bytes;
} host;

static const char *kind_name[] = {"none", "int-R", "int-I", "lui", "auipc", "branch", "jump", "load/store",
                                  "flw/fsw", "amo", "csr", "misc-nop", "illegal", "F"};

static uint32_t rotl(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static uint32_t rop(int op, uint32_t a, uint32_t b)
{
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    switch (op) {
    case OP_ADD: case OP_ADDI: return a + b;
    case OP_SUB: return a - b;
    case OP_SLL: case OP_SLLI: return a << (b & 31);
    case OP_SLT: case OP_SLTI: return sa < sb;
    case OP_SLTU: case OP_SLTIU: return a < b;
    case OP_XOR: case OP_XORI: return a ^ b;
    case OP_SRL: case OP_SRLI: return a >> (b & 31);
    case OP_SRA: case OP_SRAI: return (uint32_t)(sa >> (b & 31));
    case OP_OR: case OP_ORI: return a | b;
    case OP_AND: case OP_ANDI: return a & b;
    case OP_MUL: return a * b;
    case OP_MULH: return (uint32_t)(((int64_t)sa * sb) >> 32);
    case OP_MULHSU: return (uint32_t)(((int64_t)sa * (int64_t)(uint64_t)b) >> 32);
    case OP_MULHU: return (uint32_t)(((uint64_t)a * b) >> 32);
    case OP_DIV: return b == 0 ? 0xffffffffu : (sa == INT32_MIN && sb == -1) ? a : (uint32_t)(sa / sb);
    case OP_DIVU: return b == 0 ? 0xffffffffu : a / b;
    case OP_REM: return b == 0 ? a : (sa == INT32_MIN && sb == -1) ? 0 : (uint32_t)(sa % sb);
    case OP_REMU: return b == 0 ? a : a % b;
    case OP_MV: return b;
    case OP_BEQ: return a == b;
    case OP_BNE: return a != b;
    case OP_BLT: return sa < sb;
    case OP_BGE: return sa >= sb;
    case OP_BLTU: return a < b;
    case OP_BGEU: return a >= b;
    }
    return 0xbad0bad0u;
}

static uint32_t csr_mask(uint32_t csr)
{
    switch (csr) {
    case 0x001: return 0x1f;
    case 0x002: return 0x7;
    case 0x003: return 0xff;
    case 0x305: return 0xfffffffdu;
    case 0x341: return 0xfffffffeu;
    default: return 0xffffffffu;
    }
}

#define BUF 0x12000000u

static void new_vector(host *h)
{
    const stub_meta *m = &meta_tab[h->cur];
    h->va = vg_int(); h->vb = vg_int(); h->vc = 0; h->vd = 0; h->vctl = 1;
    switch (m->kind) {
    case K_MEM: case K_FMEM:
        h->va = BUF + 0x8000u - (uint32_t)m->imm + (vg_u32() & 0xfff);
        if (m->f & F_C) h->va &= ~3u;     /* c.lw / c.lwsp address stays in range */
        break;
    case K_AMO:
        h->va = (BUF + 0x8000u + (vg_u32() & 0xff0)) & ~3u;
        break;
    case K_CONST:
        h->va = BUF + (vg_u32() & 0xffc0);
        break;
    case K_F:
        h->va = vg_float(); h->vb = vg_float_near(h->va); h->vc = vg_float();
        if (m->op == ROP_CVT_SW || m->op == ROP_CVT_SWU || m->op == ROP_MV_WX) h->va = vg_int();
        if ((vg_u32() & 7) == 0 && m->op >= ROP_MADD) {
            uint8_t t = 0;
            extern uint32_t qf_mul(uint32_t, uint32_t, int, uint8_t *);
            h->vc = qf_mul(h->va, h->vb, 0, &t) ^ 0x80000000u ^ (vg_u32() & 3);
        }
        h->vd = (vg_u32() & 15) == 0 ? 5 + vg_u32() % 3 : vg_u32() % 5;     /* frm */
        h->vctl = (vg_u32() & 3) != 0;                                         /* clear flags */
        break;
    }
}

static void check(host *h, uint32_t got)
{
    const stub_meta *m = &meta_tab[h->cur];
    uint32_t a = h->va, b = h->vb, exp = 0;
    uint8_t efl = 0;
    int check_flags = 0;
    switch (m->kind) {
    case K_R: {
        uint32_t A = (m->f & F_ZA) ? 0 : ((m->f & F_SAME) ? b : a);
        uint32_t B = (m->f & F_ZB) ? 0 : b;
        exp = (m->f & F_ZD) ? 0 : rop(m->op, A, B);
        break;
    }
    case K_I: {
        uint32_t A = (m->f & F_ZA) ? 0 : a;
        exp = (m->f & F_ZD) ? 0 : rop(m->op, A, (uint32_t)m->imm);
        break;
    }
    case K_LUI: exp = (m->f & F_ZD) ? 0 : (uint32_t)m->imm << 12; break;
    case K_AUIPC: exp = (m->f & F_ZD) ? 0 : h->key_addr[h->cur] + ((uint32_t)m->imm << 12); break;
    case K_BR: {
        uint32_t A = (m->f & F_ZA) ? 0 : ((m->f & F_SAME) ? b : a);
        uint32_t B = (m->f & F_ZB) ? 0 : b;
        exp = rop(m->op, A, B);
        break;
    }
    case K_JUMP: exp = (m->f & F_ZD) ? 1 : h->key_addr[h->cur] + ((m->f & F_C) ? 2 : 4); break;
    case K_MEM: {
        uint64_t v = b;
        unsigned sh = (unsigned)(m->imm2 - m->imm) * 8;
        uint32_t x = (uint32_t)(v >> sh);
        switch (m->op) {
        case OP_LB: exp = (uint32_t)(int32_t)(int8_t)x; break;
        case OP_LBU: exp = x & 0xff; break;
        case OP_LH: exp = (uint32_t)(int32_t)(int16_t)x; break;
        case OP_LHU: exp = x & 0xffff; break;
        default: exp = x; break;
        }
        if (m->size < 4) {
            /* bytes above the stored width come from earlier data; the stubs never read them */
        }
        break;
    }
    case K_FMEM: exp = b; break;
    case K_AMO: {
        uint32_t old = ~b, nw = old;
        int32_t so = (int32_t)old, sb = (int32_t)b;
        switch (m->op) {
        case OP_AMOSWAP: nw = b; break;
        case OP_AMOADD: nw = old + b; break;
        case OP_AMOXOR: nw = old ^ b; break;
        case OP_AMOAND: nw = old & b; break;
        case OP_AMOOR: nw = old | b; break;
        case OP_AMOMIN: nw = so < sb ? old : b; break;
        case OP_AMOMAX: nw = so > sb ? old : b; break;
        case OP_AMOMINU: nw = old < b ? old : b; break;
        case OP_AMOMAXU: nw = old > b ? old : b; break;
        }
        if (m->op == OP_LRSC) exp = old ^ rotl(b, 7);
        else exp = old ^ rotl(nw, 7);
        break;
    }
    case K_CSR: {
        uint32_t mask = csr_mask((uint32_t)m->imm);
        uint32_t old = a & mask;
        int imm_form = m->op >= OP_CSRRWI;
        uint32_t opnd = imm_form ? (uint32_t)m->imm2 : b;
        uint32_t nw = old;
        switch (imm_form ? m->op - OP_CSRRWI + OP_CSRRW : m->op) {
        case OP_CSRRW: nw = opnd & mask; break;
        case OP_CSRRS: if (!imm_form || opnd) nw = (old | opnd) & mask; break;
        case OP_CSRRC: if (!imm_form || opnd) nw = (old & ~opnd) & mask; break;
        }
        uint32_t rd = (m->f & F_ZD) ? 0 : old;
        exp = rd ^ rotl(nw, 13);
        break;
    }
    case K_CONST: exp = (uint32_t)m->imm; break;
    case K_ILLEGAL: exp = 0xdead0002u; break;
    case K_F: {
        uint32_t A = a, B = b, C = h->vc;
        if (m->f & A_FROM_C) A = C; else if (m->f & A_FROM_B) A = B;
        if (m->f & B_FROM_C) B = C;
        int rm = m->rm == 7 ? (int)h->vd : m->rm;
        uint8_t base = h->vctl ? 0 : h->prev_flags;
        int uses_rm = m->op <= ROP_SQRT || (m->op >= ROP_CVT_W && m->op <= ROP_CVT_SWU) || m->op >= ROP_MADD;
        if (uses_rm && rm > 4) {
            exp = 0xdead0002u;
            efl = base;
        } else {
            uint8_t fl = 0;
            exp = ref_op(m->op, A, B, C, rm, &fl);
            int int_out = (m->op >= ROP_EQ && m->op <= ROP_CVT_WU) || m->op == ROP_MV_XW;
            if (int_out && (m->f & F_ZD)) exp = 0;
            efl = base | fl;
        }
        check_flags = 1;
        h->prev_flags = (uint8_t)h->rflags;
        break;
    }
    default: exp = got; break;
    }
    h->checks++;
    h->kind_checks[m->kind]++;
    if (got != exp || (check_flags && h->rflags != efl)) {
        h->fails++;
        h->kind_fails[m->kind]++;
        if (h->kind_fails[m->kind] <= 6)
            fprintf(stderr, "FAIL stub%d kind=%s op=%d rm=%d imm=%d f=0x%x a=%08x b=%08x c=%08x d=%u ctl=%u: "
                            "got %08x/%02x expected %08x/%02x\n",
                    h->cur, kind_name[m->kind], m->op, m->rm, m->imm, m->f, a, b, h->vc, h->vd, h->vctl, got,
                    h->rflags, exp, efl);
    }
}

static int dev_read(void *ctx, qsim_cpu *cpu, uint32_t addr, unsigned size, uint32_t *val)
{
    host *h = ctx;
    (void)cpu; (void)size;
    switch (addr - 0x4F000000u) {
    case 0x14: *val = meta_tab[h->cur].kind == K_F ? h->vecs * 4 : h->vecs; return 0;
    case 0x18: new_vector(h); *val = h->va; return 0;
    case 0x1c: *val = h->vb; return 0;
    case 0x20: *val = h->vc; return 0;
    case 0x24: *val = h->vd; return 0;
    case 0x28: *val = h->vctl; return 0;
    }
    return -1;
}

static int dev_write(void *ctx, qsim_cpu *cpu, uint32_t addr, unsigned size, uint32_t val)
{
    host *h = ctx;
    (void)size;
    switch (addr - 0x4F000000u) {
    case 0x00:
        h->console_bytes++;
        if (!h->quiet) fputc((int)(val & 0xff), stdout);
        return 0;
    case 0x04: h->exited = 1; h->exit_code = (int)val; qsim_request_stop(cpu); return 0;
    case 0x10:
        if (val >= meta_n) { fprintf(stderr, "bad stub index %u\n", val); return -1; }
        h->cur = (int)val; return 0;
    case 0x30: h->rflags = val; return 0;
    case 0x34: if (h->check) check(h, val); return 0;
    case 0x40: if (!h->quiet) printf("%08x\n", val); return 0;
    }
    return -1;
}

static int on_trap(void *ctx, qsim_cpu *cpu, uint32_t cause, uint32_t tval)
{
    host *h = ctx;
    if (cause == QSIM_CAUSE_ECALL_M) {
        uint32_t a7 = cpu->x[17];
        if (a7 == 93) { h->exited = 1; h->exit_code = (int)cpu->x[10]; return QSIM_TRAP_STOP; }
        if (a7 == 64) {
            uint32_t len = cpu->x[12];
            uint8_t *p = qsim_mem_ptr(cpu, cpu->x[11], len);
            if (p && !h->quiet) fwrite(p, 1, len, stdout);
            cpu->x[10] = len;
            cpu->pc += 4;
            return QSIM_TRAP_RESUME;
        }
    }
    if (cpu->mtvec) return QSIM_TRAP_GUEST;
    fprintf(stderr, "unhandled trap cause %u tval %08x pc %08x\n", cause, tval, cpu->pc);
    return QSIM_TRAP_STOP;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    host h;
    memset(&h, 0, sizeof h);
    const char *elf = NULL, *tsv = NULL;
    int prof = 0;
    uint64_t max = 20000000000ull;
    h.vecs = 200;
    meta_tab = stub_tab;
    meta_n = NSTUBS;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--stubs")) h.check = 1;
        else if (!strcmp(argv[i], "--vecs") && i + 1 < argc) h.vecs = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--prof")) prof = 1;
        else if (!strcmp(argv[i], "--tsv") && i + 1 < argc) tsv = argv[++i];
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) max = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--quiet")) h.quiet = 1;
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) vg_state = strtoull(argv[++i], NULL, 0);
        else elf = argv[i];
    }
    if (!elf) { fprintf(stderr, "usage: cpurun [--stubs] [--vecs N] [--prof] [--tsv F] [--max N] prog.elf\n"); return 2; }
#ifndef WITH_STUBS
    if (h.check) { fprintf(stderr, "cpurun built without stub metadata\n"); return 2; }
#endif
    qsim_cpu *cpu = qsim_cpu_new(NULL);
    if (!cpu) { fprintf(stderr, "cpu alloc failed\n"); return 2; }
    uint32_t entry;
    if (qsim_elf_load(cpu, elf, &entry)) { fprintf(stderr, "cannot load %s\n", elf); return 2; }
    qsim_elf_symbols(elf, &h.syms);
    if (h.check) {
        char nm[64];
        for (unsigned i = 0; i < meta_n && i < 4096; i++) {
            snprintf(nm, sizeof nm, "stub%u_key", i);
            const qsim_sym *s = qsim_syms_lookup(&h.syms, nm);
            h.key_addr[i] = s ? s->addr : 0;
        }
    }
    cpu->env.ctx = &h;
    cpu->env.mmio_read = dev_read;
    cpu->env.mmio_write = dev_write;
    cpu->env.trap = on_trap;
    qsim_cpu_reset(cpu, entry);
    if (prof && qsim_enable_profiling(cpu, NULL)) { fprintf(stderr, "profiling init failed\n"); return 2; }
    double t0 = now();
    int r;
    const char *trace = getenv("QSIM_TRACE");       /* "lo:hi" pc range, single-step */
    if (trace) {
        uint32_t lo = 0, hi = 0;
        sscanf(trace, "%x:%x", &lo, &hi);
        uint32_t prev[33], fprev[32];
        memcpy(prev, cpu->x, sizeof prev);
        memcpy(fprev, cpu->f, sizeof fprev);
        do {
            uint32_t pc = cpu->pc;
            r = qsim_run(cpu, 1);
            if (pc >= lo && pc < hi) {
                const char *nm = "?";
                uint32_t w = 0;
                qsim_read32(cpu, pc, &w);
                qsim_decode_name(w, &nm);
                fprintf(stderr, "%08x %08x %-8s", pc, w, nm);
                for (int i = 1; i < 32; i++)
                    if (cpu->x[i] != prev[i]) fprintf(stderr, " x%d=%08x", i, cpu->x[i]);
                for (int i = 0; i < 32; i++)
                    if (cpu->f[i] != fprev[i]) fprintf(stderr, " f%d=%08x", i, cpu->f[i]);
                fprintf(stderr, " fflags=%x\n", qsim_fflags(cpu));
            }
            memcpy(prev, cpu->x, sizeof prev);
            memcpy(fprev, cpu->f, sizeof fprev);
        } while (r == QSIM_RUN_BUDGET && cpu->instret < max);
    } else {
        r = qsim_run(cpu, max);
    }
    double dt = now() - t0;
    fflush(stdout);
    fprintf(stderr, "run: reason=%d exit=%d code=%d insns=%" PRIu64 " time=%.3fs speed=%.1f MIPS%s\n", r,
            h.exited, h.exit_code, cpu->instret, dt, (double)cpu->instret / dt / 1e6, prof ? " (profiling)" : "");
    if (prof) {
        qsim_model_report(cpu->model, cpu->instret, stderr);
        qsim_profile p;
        qsim_profile_build(cpu, &h.syms, &p);
        qsim_profile_print(&p, stderr, 25);
        if (tsv) qsim_profile_write_tsv(&p, tsv);
        qsim_profile_free(&p);
    }
    int rc = 0;
    if (h.check) {
        for (int k = 0; k < 16; k++)
            if (h.kind_checks[k])
                fprintf(stderr, "  %-10s %10" PRIu64 " checks %6" PRIu64 " failures\n", kind_name[k],
                        h.kind_checks[k], h.kind_fails[k]);
        fprintf(stderr, "stubs: %u stubs, %" PRIu64 " checks, %" PRIu64 " failures\n", meta_n, h.checks, h.fails);
        rc = (h.fails || !h.exited) ? 1 : 0;
    } else {
        rc = h.exited ? h.exit_code : 1;
    }
    qsim_syms_free(&h.syms);
    qsim_cpu_free(cpu);
    return rc;
}
