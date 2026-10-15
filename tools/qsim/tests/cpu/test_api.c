//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/* qsim CPU API / environment-hook tests (hand-encoded guest code). */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "cache.h"
#include "prof.h"

static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct {
    uint32_t last_cause, last_tval, last_pc, last_len, fflags_in_trap;
    int hook_calls;
    qsim_cpu *cpu;
} envst;

static int trap_cb(void *ctx, qsim_cpu *cpu, uint32_t cause, uint32_t tval)
{
    envst *s = ctx;
    s->last_cause = cause; s->last_tval = tval; s->last_pc = cpu->pc; s->last_len = cpu->trap_len;
    s->fflags_in_trap = qsim_fflags(cpu);
    return QSIM_TRAP_STOP;
}
static int hook_cb(void *ctx, qsim_cpu *cpu, uint32_t pc)
{
    envst *s = ctx;
    (void)pc;
    s->hook_calls++;
    cpu->x[10] = 42;
    cpu->pc = cpu->x[1];
    return QSIM_TRAP_RESUME;
}
static int mmio_w(void *ctx, qsim_cpu *cpu, uint32_t a, unsigned sz, uint32_t v)
{
    (void)ctx; (void)sz; (void)v;
    if (a == 0x4F000000u) { qsim_set_mip(cpu, 0x80, 0); return 0; }
    return -1;
}

#define ADDI_A0_1   0x00150513u
#define ADDI_A0_5   0x00550513u
#define ADDI_A1_1   0x00158593u
#define RET         0x00008067u
#define J_M4        0xffdff06fu     /* jal x0, -4 */
#define SW_A1_0_A2  0x00b62023u
#define JALR_RA_A2  0x000600e7u
#define JALR_X0_A2  0x00060067u
#define EBREAK      0x00100073u
#define LW_A0_1_A1  0x0015a503u
#define LW_A0_0_A1  0x0005a503u
#define SW_A0_4_A1  0x00a5a223u
#define MRET        0x30200073u
#define SW_ZERO_T0  0x0002a023u
#define ADDI_A0_100 0x06450513u

static void put(qsim_cpu *cpu, uint32_t addr, const uint32_t *w, int n)
{
    for (int i = 0; i < n; i++) qsim_write32(cpu, addr + 4u * i, w[i]);
}

static int call(qsim_cpu *cpu, uint32_t fn, uint64_t max)
{
    cpu->x[1] = 0x0ffffff0u;     /* return to a non-RAM address -> fetch fault -> STOP */
    cpu->x[2] = 0x13a80000u;
    cpu->pc = fn;
    return qsim_run(cpu, max);
}

int main(void)
{
    envst st = {0};
    qsim_cpu *cpu = qsim_cpu_new(NULL);
    cpu->env.ctx = &st;
    cpu->env.trap = trap_cb;
    cpu->env.hook = hook_cb;
    cpu->env.mmio_write = mmio_w;
    st.cpu = cpu;

    /* 1. budget exactness */
    { uint32_t c[] = {ADDI_A0_1, J_M4}; put(cpu, 0x10001000, c, 2); }
    cpu->x[10] = 0; cpu->pc = 0x10001000;
    uint64_t i0 = cpu->instret;
    int r = qsim_run(cpu, 1001);
    CHECK(r == QSIM_RUN_BUDGET && cpu->x[10] == 501 && cpu->instret - i0 == 1001 && cpu->pc == 0x10001004,
          "r=%d a0=%u insns=%" PRIu64 " pc=%08x", r, cpu->x[10], cpu->instret - i0, cpu->pc);
    r = qsim_run(cpu, 1);
    CHECK(cpu->pc == 0x10001000 && cpu->x[10] == 501, "single step pc=%08x", cpu->pc);

    /* 2. SMC via qsim_write after execution */
    { uint32_t c[] = {ADDI_A0_1, RET}; put(cpu, 0x10002000, c, 2); }
    cpu->x[10] = 10; call(cpu, 0x10002000, 100);
    CHECK(cpu->x[10] == 11 && st.last_cause == QSIM_CAUSE_FETCH_FAULT && st.last_tval == 0x0ffffff0u,
          "a0=%u cause=%u tval=%08x", cpu->x[10], st.last_cause, st.last_tval);
    qsim_write32(cpu, 0x10002000, ADDI_A0_5);
    cpu->x[10] = 10; call(cpu, 0x10002000, 100);
    CHECK(cpu->x[10] == 15, "after qsim_write a0=%u", cpu->x[10]);

    /* 3. SMC via a guest store into the next, already-decoded instruction */
    { uint32_t c[] = {SW_A1_0_A2, ADDI_A0_1, RET}; put(cpu, 0x10003000, c, 3); }
    cpu->x[10] = 0; cpu->x[11] = ADDI_A0_1; cpu->x[12] = 0x10003004; call(cpu, 0x10003000, 100);
    CHECK(cpu->x[10] == 1, "first a0=%u", cpu->x[10]);
    cpu->x[10] = 0; cpu->x[11] = ADDI_A0_5; cpu->x[12] = 0x10003004; call(cpu, 0x10003000, 100);
    CHECK(cpu->x[10] == 5, "guest SMC a0=%u", cpu->x[10]);
    /* store through the uncached alias must invalidate too */
    cpu->x[10] = 0; cpu->x[11] = ADDI_A0_1; cpu->x[12] = 0x50003004; call(cpu, 0x10003000, 100);
    CHECK(cpu->x[10] == 1, "alias SMC a0=%u", cpu->x[10]);

    /* 4. hook */
    qsim_add_hook(cpu, 0x103f0010);
    { uint32_t c[] = {JALR_RA_A2, EBREAK}; put(cpu, 0x10004000, c, 2); }
    cpu->x[10] = 0; cpu->x[12] = 0x103f0010; cpu->pc = 0x10004000;
    r = qsim_run(cpu, 100);
    CHECK(st.hook_calls == 1 && cpu->x[10] == 42 && st.last_cause == QSIM_CAUSE_BREAKPOINT &&
          st.last_pc == 0x10004004 && st.last_len == 4, "hooks=%d a0=%u cause=%u pc=%08x", st.hook_calls,
          cpu->x[10], st.last_cause, st.last_pc);

    /* 5. SDRAM alias */
    qsim_write32(cpu, 0x10005000, 0xcafef00d);
    { uint32_t c[] = {LW_A0_0_A1, SW_A0_4_A1, RET}; put(cpu, 0x10005100, c, 3); }
    cpu->x[11] = 0x50005000; call(cpu, 0x10005100, 100);
    uint32_t v = 0;
    qsim_read32(cpu, 0x10005004, &v);
    CHECK(cpu->x[10] == 0xcafef00d && v == 0xcafef00d, "a0=%08x mem=%08x", cpu->x[10], v);

    /* 6. misaligned access */
    qsim_write32(cpu, 0x10005200, 0x44332211);
    qsim_write32(cpu, 0x10005204, 0x88776655);
    { uint32_t c[] = {LW_A0_1_A1, RET}; put(cpu, 0x10005300, c, 2); }
    cpu->x[11] = 0x10005200; call(cpu, 0x10005300, 100);
    CHECK(cpu->x[10] == 0x55443322 && st.last_cause == QSIM_CAUSE_FETCH_FAULT, "a0=%08x cause=%u", cpu->x[10], st.last_cause);
    cpu->trap_misaligned = 1;
    cpu->x[10] = 0; call(cpu, 0x10005300, 100);
    CHECK(st.last_cause == QSIM_CAUSE_LOAD_MISALIGNED && st.last_tval == 0x10005201 && st.last_pc == 0x10005300 &&
          cpu->x[10] == 0, "cause=%u tval=%08x pc=%08x", st.last_cause, st.last_tval, st.last_pc);
    cpu->trap_misaligned = 0;

    /* 7. interrupt: loop increments a1; handler adds 100 to a0, clears MTIP via MMIO, mret */
    { uint32_t c[] = {ADDI_A1_1, J_M4}; put(cpu, 0x10006000, c, 2); }
    { uint32_t h[] = {ADDI_A0_100, SW_ZERO_T0, MRET}; put(cpu, 0x10006100, h, 3); }
    cpu->mtvec = 0x10006100; cpu->mstatus |= 8; cpu->mie = 0x80;
    cpu->x[5] = 0x4F000000; cpu->x[10] = 0; cpu->x[11] = 0; cpu->pc = 0x10006000;
    qsim_run(cpu, 100);
    uint32_t a1_before = cpu->x[11];
    qsim_set_mip(cpu, 0x80, 1);
    qsim_run(cpu, 100);
    CHECK(cpu->x[10] == 100 && cpu->mcause == 0x80000007u && (cpu->mepc == 0x10006000 || cpu->mepc == 0x10006004) &&
          (cpu->mstatus & 8) && cpu->x[11] > a1_before, "a0=%u mcause=%08x mepc=%08x mstatus=%08x",
          cpu->x[10], cpu->mcause, cpu->mepc, cpu->mstatus);
    cpu->mstatus &= ~8u; cpu->mie = 0; cpu->mtvec = 0;

    /* 8. c.ebreak length + 9. illegal instruction tval */
    qsim_write(cpu, 0x10007000, (uint8_t[]){0x02, 0x90}, 2);
    cpu->pc = 0x10007000; qsim_run(cpu, 10);
    CHECK(st.last_cause == 3 && st.last_len == 2 && st.last_pc == 0x10007000, "c.ebreak len=%u", st.last_len);
    qsim_write32(cpu, 0x10007100, 0x0000405b);
    cpu->pc = 0x10007100; qsim_run(cpu, 10);
    CHECK(st.last_cause == 2 && st.last_tval == 0x0000405b, "illegal tval=%08x", st.last_tval);

    /* 10. lazy fflags: fdiv.s ft0, ft1, ft2 (1/0) then ebreak; fflags visible in the trap and after */
    cpu->f[1] = 0x3f800000; cpu->f[2] = 0;
    { uint32_t c[] = {(0x0c << 25) | (2 << 20) | (1 << 15) | (0 << 12) | (0 << 7) | 0x53, EBREAK}; put(cpu, 0x10008000, c, 2); }
    qsim_set_fflags(cpu, 0);
    cpu->pc = 0x10008000; qsim_run(cpu, 10);
    CHECK(cpu->f[0] == 0x7f800000 && st.fflags_in_trap == 0x08 && qsim_fflags(cpu) == 0x08,
          "f0=%08x trapflags=%x flags=%x", cpu->f[0], st.fflags_in_trap, qsim_fflags(cpu));

    /* 11. profiling: call counts + model clock */
    qsim_enable_profiling(cpu, NULL);
    { uint32_t c[] = {JALR_RA_A2, EBREAK}; put(cpu, 0x10009000, c, 2); }
    { uint32_t c[] = {ADDI_A0_1, RET}; put(cpu, 0x10009100, c, 2); }
    for (int k = 0; k < 3; k++) {
        cpu->x[12] = 0x10009100; cpu->pc = 0x10009000;
        qsim_run(cpu, 10);
    }
    size_t ti = (size_t)(cpu->sd_dc - cpu->dc_base) + (0x9100 >> 1);
    CHECK(cpu->prof->calls[ti] == 3 && cpu->prof->cnt[ti] == 3 && cpu->model->cyc > 9,
          "calls=%" PRIu64 " cnt=%" PRIu64 " cyc=%" PRIu64, cpu->prof->calls[ti], cpu->prof->cnt[ti], cpu->model->cyc);

    printf("api: %d checks, %d failures\n", checks, fails);
    qsim_cpu_free(cpu);
    return fails != 0;
}
