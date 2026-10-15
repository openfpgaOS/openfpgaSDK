//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * Threaded interpreter template.  Included by cpu.c twice:
 *   QX_PROF 0 -> exec_fast : no instrumentation
 *   QX_PROF 1 -> exec_prof : per-PC counts, call counts, penalty attribution,
 *                            cache / predictor / pipeline timing model
 *
 * Invariants:
 *   e      current decoded entry; its pc is e->pc once decoded
 *   budget instructions still allowed; decremented at every dispatch
 *   x[32]  integer write sink for rd == 0
 */

static int QX_FN(qsim_cpu *cpu, uint64_t max, uint64_t *done)
{
    static const void *const tbl[QOP_COUNT] = {
#define QSIM_X_LBL(n, c) [QOP_##n] = &&op_##n,
        QSIM_OPS(QSIM_X_LBL)
#undef QSIM_X_LBL
    };
    uint32_t *const x = cpu->x;
    uint32_t *const f = cpu->f;
    uint8_t *const sd = cpu->sdram;
    uint8_t *const sd_uc = cpu->sdram_uc ? cpu->sdram_uc : cpu->sdram;
    const uint32_t sd_base = cpu->sdram_base, sd_size = cpu->sdram_size;
    const uint32_t br_size = cpu->bram_size;
    const uint32_t sd_alias = cpu->sdram_alias_base;
    const int trap_mis = cpu->trap_misaligned;
    qsim_dinsn *const sd_dc = cpu->sd_dc;
    qsim_dinsn *const br_dc = cpu->br_dc;
    uint32_t c_lo, c_span;
    int64_t budget = max > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)max;
    const int64_t budget0 = budget;
    qsim_dinsn *e;
    int ret = EXEC_BUDGET;
    uint32_t tcause = 0, ttval = 0, jt = 0;
#if QX_LINT
    qsim_lint *const lint = cpu->lint;
#else
    const qsim_lint *const lint = NULL;     /* hooks compile away */
#endif
#if QX_PROF
    qsim_model *const m = cpu->model;
    qsim_prof *const P = cpu->prof;
    qsim_dinsn *const arena = cpu->dc_base;
    uint32_t cur_line = m->cur_line;
#endif

#define RELOAD_CODE_RANGE() do { \
        if (cpu->code_hi > cpu->code_lo) { \
            c_lo = cpu->code_lo >= 4 ? cpu->code_lo - 4 : 0; \
            c_span = cpu->code_hi + 4 - c_lo; \
        } else { c_lo = 0; c_span = 0; } \
    } while (0)
#define ENTRY_PC(p) ((p) >= sd_dc ? sd_base + (uint32_t)(((p) - sd_dc) << 1) \
                                  : (uint32_t)(((p) - br_dc) << 1))
#define DISPATCH() do { if (unlikely(--budget < 0)) goto out_budget; goto *tbl[e->h]; } while (0)
#define NEXT() do { e += e->lf; DISPATCH(); } while (0)
#define TRAP(c, v) do { tcause = (c); ttval = (v); goto trap_here; } while (0)
#define IMM ((uint32_t)e->imm)
/* integration: MMIO callbacks see the exact pc and live instruction count */
#define SYNC_LIVE() (cpu->pc = e->pc, cpu->instret_live = cpu->instret + (uint64_t)(budget0 - budget))
#define RS1 x[e->rs1]
#define RS2 x[e->rs2]
/* D-cache coherency lint (lint.h): cached-alias (CA) and uncached-alias (UC)
 * SDRAM accesses; expressions, no-ops when the lint is off */
#if QX_LINT
#define LINT_CA(o, sz, st) (unlikely(lint != NULL) ? lint_cpu_access(lint, (o), (sz), (st), e->pc) : (void)0)
#define LINT_UC_RD(o, sz)  (unlikely(lint != NULL) ? lint_uc_rd(lint, (o), (sz), e->pc) : (void)0)
#define LINT_UC_WR(o, sz)  (unlikely(lint != NULL) ? lint_uc_wr(lint, (o), (sz)) : (void)0)
#else
#define LINT_CA(o, sz, st) ((void)(lint), (void)0)
#define LINT_UC_RD(o, sz)  ((void)0)
#define LINT_UC_WR(o, sz)  ((void)0)
#endif
/* subnormal float operand/result (exponent 0, mantissa != 0) */
#define FSUB(v) ((((v) & 0x7fffffffu) - 1u) < 0x007fffffu)

#if QX_PROF
#define INSN() do { \
        size_t _i = (size_t)(e - arena); \
        P->pen[P->last_i] += m->cyc - P->last_cyc; \
        P->last_i = _i; P->cnt[_i]++; \
        m->cyc++; P->last_cyc = m->cyc; \
        if (unlikely((e->pc >> 6) != cur_line)) { \
            cur_line = e->pc >> 6; \
            qsim_model_fetch_line(m, e->pc, (unsigned)e->lf << 1); \
        } \
    } while (0)
#define MR(r)       qsim_model_rd(m, (r))
#define MR2(a, b)   do { qsim_model_rd(m, (a)); qsim_model_rd(m, (b)); } while (0)
#define MRF(r)      qsim_model_rd(m, 33u + (r))
#define MRF2(a, b)  do { qsim_model_rd(m, 33u + (a)); qsim_model_rd(m, 33u + (b)); } while (0)
#define MW(r, L)    qsim_model_wr(m, (r), m->p.L, QDEP_##L)
#define MWF(r, L)   qsim_model_wr(m, 33u + (r), m->p.L, QDEP_##L)
#define MMEM(a, s)  qsim_model_mem(m, (a), (s))
#define PBR(tk, tgt) qsim_model_branch(m, e->pc, (unsigned)e->lf << 1, (tk), (tgt))
#define PJUMP(te, tpc, isr) do { \
        qsim_model_jump(m, e->pc, (unsigned)e->lf << 1, e->rd, (isr) ? e->rs1 : 0, (tpc), (isr)); \
        size_t _ti = (size_t)((te) - arena); \
        if (e->rd == 1 || e->rd == 5) { \
            size_t _si = (size_t)(e - arena); \
            P->calls[_ti]++; P->site_n[_si]++; P->site_tgt[_si] = (uint32_t)_ti; \
        } else if (!((isr) && (e->rs1 == 1 || e->rs1 == 5))) { \
            size_t _si = (size_t)(e - arena); \
            P->tails[_ti]++; P->site_n[_si]++; P->site_tgt[_si] = (uint32_t)_ti; \
        } \
    } while (0)
#define PSTMT(s)    do { s; } while (0)
#else
#define INSN()      ((void)0)
#define MR(r)       ((void)0)
#define MR2(a, b)   ((void)0)
#define MRF(r)      ((void)0)
#define MRF2(a, b)  ((void)0)
#define MW(r, L)    ((void)0)
#define MWF(r, L)   ((void)0)
#define MMEM(a, s)  ((void)0)
#define PBR(tk, tgt) ((void)0)
#define PJUMP(te, tpc, isr) ((void)0)
#define PSTMT(s)    ((void)0)
#endif

    RELOAD_CODE_RANGE();
    jt = cpu->pc;
    goto resume_at_jt;

    /* ---------------------------------------------------------------- */
op_DECODE:
    budget++;
    decode_entry(cpu, e, ENTRY_PC(e));
    RELOAD_CODE_RANGE();
    DISPATCH();

op_ILLEGAL:
    INSN();
    cpu->trap_len = (uint8_t)(e->lf << 1);
    TRAP(QSIM_CAUSE_ILLEGAL, IMM);

op_FETCH_FAULT:
    TRAP(QSIM_CAUSE_FETCH_FAULT, e->pc);

op_NOP:
    INSN();
    NEXT();

op_HOOK: {
    cpu->pc = e->pc;
    if (!cpu->env.hook) TRAP(QSIM_CAUSE_FETCH_FAULT, e->pc);
    fpu_host_fold(cpu);
    int act = cpu->env.hook(cpu->env.ctx, cpu, e->pc);
    _mm_setcsr(QSIM_MXCSR_DEFAULT);
    RELOAD_CODE_RANGE();
    if (act == QSIM_TRAP_STOP) { ret = EXEC_TRAPSTOP; goto out; }
    jt = cpu->pc;
    goto resume_at_jt;
}

op_LUI:
    INSN();
    x[e->rd] = IMM;
    MW(e->rd, lat_zero);
    NEXT();

op_AUIPC:
    INSN();
    x[e->rd] = e->pc + IMM;
    MW(e->rd, lat_zero);
    NEXT();

op_JAL: {
    INSN();
    qsim_dinsn *t = e + (e->imm >> 1);
    PJUMP(t, e->pc + IMM, 0);
    x[e->rd] = e->pc + ((uint32_t)e->lf << 1);
    MW(e->rd, lat_zero);
    e = t;
    DISPATCH();
}

op_JFAR:
    INSN();
    jt = e->pc + IMM;
    x[e->rd] = e->pc + ((uint32_t)e->lf << 1);
    goto jump_far;

op_JALR: {
    INSN();
    MR(e->rs1);
    jt = (RS1 + IMM) & ~1u;
    uint32_t link = e->pc + ((uint32_t)e->lf << 1);
    uint32_t o = jt - sd_base;
    qsim_dinsn *t;
    if (likely(o < sd_size)) t = sd_dc + (o >> 1);
    else if (jt < br_size) t = br_dc + (jt >> 1);
    else { x[e->rd] = link; goto jump_far; }
    PJUMP(t, jt, 1);
    x[e->rd] = link;
    MW(e->rd, lat_zero);
    e = t;
    DISPATCH();
}

#define BRANCH(cond) do { \
        INSN(); \
        MR2(e->rs1, e->rs2); \
        int _tk = (cond); \
        PBR(_tk, e->pc + IMM); \
        e += _tk ? (e->imm >> 1) : (int32_t)e->lf; \
        DISPATCH(); \
    } while (0)

op_BEQ:  BRANCH(RS1 == RS2);
op_BNE:  BRANCH(RS1 != RS2);
op_BLT:  BRANCH((int32_t)RS1 < (int32_t)RS2);
op_BGE:  BRANCH((int32_t)RS1 >= (int32_t)RS2);
op_BLTU: BRANCH(RS1 < RS2);
op_BGEU: BRANCH(RS1 >= RS2);

op_BFAR: {
    INSN();
    MR2(e->rs1, e->rs2);
    int tk = 0;
    switch (e->rd) {
    case 0: tk = RS1 == RS2; break;
    case 1: tk = RS1 != RS2; break;
    case 2: tk = (int32_t)RS1 < (int32_t)RS2; break;
    case 3: tk = (int32_t)RS1 >= (int32_t)RS2; break;
    case 4: tk = RS1 < RS2; break;
    default: tk = RS1 >= RS2; break;
    }
    PBR(tk, e->pc + IMM);
    if (!tk) NEXT();
    jt = e->pc + IMM;
    goto jump_far;
}

    /* ---- loads -------------------------------------------------------- */
#define MISALIGN_CHECK(a, sz, cause) do { \
        if (unlikely((a) & ((sz) - 1))) { \
            PSTMT(qsim_model_misaligned(m)); \
            if (trap_mis) { cpu->trap_len = (uint8_t)(e->lf << 1); TRAP(cause, (a)); } \
        } \
    } while (0)
#define LOAD_OP(T, CAST) do { \
        INSN(); \
        MR(e->rs1); \
        uint32_t _a = RS1 + IMM, _o = _a - sd_base; \
        T _t; \
        MISALIGN_CHECK(_a, sizeof _t, QSIM_CAUSE_LOAD_MISALIGNED); \
        MMEM(_a, 0); \
        if (likely(_o < sd_size)) { \
            LINT_CA(_o, sizeof _t, 0); \
            memcpy(&_t, sd + _o, sizeof _t); \
            x[e->rd] = (uint32_t)(CAST)_t; \
            MW(e->rd, lat_load); \
            NEXT(); \
        } \
        _o = _a - sd_alias; \
        if (likely(_o < sd_size)) { \
            LINT_UC_RD(_o, sizeof _t); \
            memcpy(&_t, sd_uc + _o, sizeof _t); \
            x[e->rd] = (uint32_t)(CAST)_t; \
            MW(e->rd, lat_load); \
            NEXT(); \
        } \
        uint32_t _s; \
        if (unlikely((SYNC_LIVE(), mem_read_slow(cpu, _a, sizeof _t, &_s)))) { \
            cpu->trap_len = (uint8_t)(e->lf << 1); \
            TRAP(QSIM_CAUSE_LOAD_FAULT, _a); \
        } \
        _t = (T)_s; \
        x[e->rd] = (uint32_t)(CAST)_t; \
        MW(e->rd, lat_load); \
        e += e->lf; \
        goto check_async; \
    } while (0)

op_LB:  LOAD_OP(int8_t, int32_t);
op_LH:  LOAD_OP(int16_t, int32_t);
op_LW:  LOAD_OP(uint32_t, uint32_t);
op_LBU: LOAD_OP(uint8_t, uint32_t);
op_LHU: LOAD_OP(uint16_t, uint32_t);

op_FLW: {
    INSN();
    MR(e->rs1);
    uint32_t a = RS1 + IMM, o = a - sd_base, v;
    MISALIGN_CHECK(a, 4, QSIM_CAUSE_LOAD_MISALIGNED);
    MMEM(a, 0);
    if (likely(o < sd_size)) {
        LINT_CA(o, 4, 0);
        memcpy(&f[e->rd], sd + o, 4);
        MWF(e->rd, lat_fload);
        NEXT();
    }
    if ((o = a - sd_alias) < sd_size) {
        LINT_UC_RD(o, 4);
        memcpy(&f[e->rd], sd_uc + o, 4);
        MWF(e->rd, lat_fload);
        NEXT();
    }
    if (unlikely((SYNC_LIVE(), mem_read_slow(cpu, a, 4, &v)))) {
        cpu->trap_len = (uint8_t)(e->lf << 1);
        TRAP(QSIM_CAUSE_LOAD_FAULT, a);
    }
    f[e->rd] = v;
    MWF(e->rd, lat_fload);
    e += e->lf;
    goto check_async;
}

    /* ---- stores ------------------------------------------------------- */
#define STORE_OP(T, SRC, READS) do { \
        INSN(); \
        READS; \
        uint32_t _a = RS1 + IMM, _o = _a - sd_base; \
        T _t = (T)(SRC); \
        MISALIGN_CHECK(_a, sizeof _t, QSIM_CAUSE_STORE_MISALIGNED); \
        MMEM(_a, 1); \
        if (likely(_o < sd_size)) { \
            LINT_CA(_o, sizeof _t, 1); \
            memcpy(sd + _o, &_t, sizeof _t); \
            if (unlikely(_o - c_lo < c_span)) qsim_invalidate(cpu, sd_base + _o, sizeof _t); \
            NEXT(); \
        } \
        _o = _a - sd_alias; \
        if (likely(_o < sd_size)) { \
            memcpy(sd_uc + _o, &_t, sizeof _t); \
            LINT_UC_WR(_o, sizeof _t); \
            if (unlikely(_o - c_lo < c_span)) qsim_invalidate(cpu, sd_base + _o, sizeof _t); \
            NEXT(); \
        } \
        if (unlikely((SYNC_LIVE(), mem_write_slow(cpu, _a, sizeof _t, (uint32_t)_t)))) { \
            cpu->trap_len = (uint8_t)(e->lf << 1); \
            TRAP(QSIM_CAUSE_STORE_FAULT, _a); \
        } \
        RELOAD_CODE_RANGE(); \
        e += e->lf; \
        goto check_async; \
    } while (0)

op_SB: STORE_OP(uint8_t, RS2, MR2(e->rs1, e->rs2));
op_SH: STORE_OP(uint16_t, RS2, MR2(e->rs1, e->rs2));
op_SW: STORE_OP(uint32_t, RS2, MR2(e->rs1, e->rs2));
op_FSW: STORE_OP(uint32_t, f[e->rs2], MR(e->rs1); MRF(e->rs2));

    /* ---- integer ALU --------------------------------------------------- */
#define ALU_I(expr) do { INSN(); MR(e->rs1); x[e->rd] = (expr); MW(e->rd, lat_zero); NEXT(); } while (0)
#define ALU_R(expr) do { INSN(); MR2(e->rs1, e->rs2); x[e->rd] = (expr); MW(e->rd, lat_zero); NEXT(); } while (0)

op_ADDI:  ALU_I(RS1 + IMM);
op_SLTI:  ALU_I((int32_t)RS1 < e->imm);
op_SLTIU: ALU_I(RS1 < IMM);
op_XORI:  ALU_I(RS1 ^ IMM);
op_ORI:   ALU_I(RS1 | IMM);
op_ANDI:  ALU_I(RS1 & IMM);
op_SLLI:  ALU_I(RS1 << (e->imm & 31));
op_SRLI:  ALU_I(RS1 >> (e->imm & 31));
op_SRAI:  ALU_I((uint32_t)((int32_t)RS1 >> (e->imm & 31)));
op_ADD:   ALU_R(RS1 + RS2);
op_SUB:   ALU_R(RS1 - RS2);
op_SLL:   ALU_R(RS1 << (RS2 & 31));
op_SLT:   ALU_R((int32_t)RS1 < (int32_t)RS2);
op_SLTU:  ALU_R(RS1 < RS2);
op_XOR:   ALU_R(RS1 ^ RS2);
op_SRL:   ALU_R(RS1 >> (RS2 & 31));
op_SRA:   ALU_R((uint32_t)((int32_t)RS1 >> (RS2 & 31)));
op_OR:    ALU_R(RS1 | RS2);
op_AND:   ALU_R(RS1 & RS2);

#define MUL_R(expr) do { INSN(); MR2(e->rs1, e->rs2); x[e->rd] = (expr); MW(e->rd, lat_mul); NEXT(); } while (0)
op_MUL:    MUL_R(RS1 * RS2);
op_MULH:   MUL_R((uint32_t)(((int64_t)(int32_t)RS1 * (int64_t)(int32_t)RS2) >> 32));
op_MULHSU: MUL_R((uint32_t)(((int64_t)(int32_t)RS1 * (int64_t)(uint64_t)RS2) >> 32));
op_MULHU:  MUL_R((uint32_t)(((uint64_t)RS1 * (uint64_t)RS2) >> 32));

#define DIV_R(sgn, expr) do { \
        INSN(); MR2(e->rs1, e->rs2); \
        uint32_t _ua = RS1, _ub = RS2; \
        int32_t _sa = (int32_t)_ua, _sb = (int32_t)_ub; \
        (void)_sa; (void)_sb; \
        PSTMT(qsim_model_div(m, _ua, _ub, sgn)); \
        x[e->rd] = (expr); \
        MW(e->rd, lat_div); \
        NEXT(); \
    } while (0)
op_DIV:  DIV_R(1, _sb == 0 ? 0xffffffffu : (_sa == INT32_MIN && _sb == -1) ? _ua : (uint32_t)(_sa / _sb));
op_DIVU: DIV_R(0, _ub == 0 ? 0xffffffffu : _ua / _ub);
op_REM:  DIV_R(1, _sb == 0 ? _ua : (_sa == INT32_MIN && _sb == -1) ? 0u : (uint32_t)(_sa % _sb));
op_REMU: DIV_R(0, _ub == 0 ? _ua : _ua % _ub);

    /* ---- system ------------------------------------------------------- */
op_CBO: {
    INSN();
    MR(e->rs1);
    uint32_t a = RS1;
    PSTMT(qsim_model_cbo(m, (unsigned)e->imm, a));
#if QX_LINT
    if (unlikely(lint != NULL) && a - sd_base < sd_size) lint_cbo(lint, (unsigned)e->imm, a - sd_base, e->pc);
#endif
    if (cpu->env.cbo) {
        cpu->env.cbo(cpu->env.ctx, cpu, (unsigned)e->imm, a);
        e += e->lf;
        goto check_async;
    }
    NEXT();
}

op_ECALL:
    INSN();
    cpu->trap_len = 4;
    TRAP(QSIM_CAUSE_ECALL_M, 0);

op_EBREAK:
    INSN();
    cpu->trap_len = (uint8_t)(e->lf << 1);
    TRAP(QSIM_CAUSE_BREAKPOINT, e->pc);

op_MRET: {
    INSN();
    uint32_t ms = cpu->mstatus;
    ms = (ms & ~(1u << 3)) | ((ms >> 4) & (1u << 3));   /* MIE = MPIE */
    ms |= 1u << 7;                                      /* MPIE = 1 */
    cpu->mstatus = ms;
    cpu->async = 1;
    jt = cpu->mepc;
    goto jump_far;
}

op_WFI:
    INSN();
    NEXT();

op_CSRRW: op_CSRRS: op_CSRRC: op_CSRRWI: op_CSRRSI: op_CSRRCI:
    INSN();
    if (e->h < QOP_CSRRWI) MR(e->rs1);
    cpu->pc = e->pc;
    cpu->instret_live = cpu->instret + (uint64_t)(budget0 - budget);
    if (csr_insn(cpu, e, e->h)) {
        cpu->trap_len = 4;
        TRAP(QSIM_CAUSE_ILLEGAL, 0);
    }
    MW(e->rd, lat_csr);
    e += e->lf;
    goto check_async;

    /* ---- A extension -------------------------------------------------- */
op_LR: {
    INSN(); MR(e->rs1);
    uint32_t a = RS1, v;
    MMEM(a, 0);
    if (amo_read(cpu, a, &v)) { cpu->trap_len = 4; TRAP(QSIM_CAUSE_LOAD_FAULT, a); }
    x[e->rd] = v;
    cpu->resv_addr = a;
    cpu->resv_valid = 1;
    MW(e->rd, lat_load);
    RELOAD_CODE_RANGE();
    e += e->lf;
    goto check_async;
}
op_SC: {
    INSN(); MR2(e->rs1, e->rs2);
    uint32_t a = RS1, v = RS2;
    if (cpu->resv_valid && cpu->resv_addr == a) {
        MMEM(a, 1);
        if (amo_write(cpu, a, v)) { cpu->trap_len = 4; TRAP(QSIM_CAUSE_STORE_FAULT, a); }
        x[e->rd] = 0;
    } else {
        x[e->rd] = 1;
    }
    cpu->resv_valid = 0;
    MW(e->rd, lat_load);
    RELOAD_CODE_RANGE();
    e += e->lf;
    goto check_async;
}
#define AMO(expr) do { \
        INSN(); MR2(e->rs1, e->rs2); \
        uint32_t _a = RS1, _s = RS2, _t; \
        MMEM(_a, 0); MMEM(_a, 1); \
        if (amo_read(cpu, _a, &_t)) { cpu->trap_len = 4; TRAP(QSIM_CAUSE_STORE_FAULT, _a); } \
        if (amo_write(cpu, _a, (expr))) { cpu->trap_len = 4; TRAP(QSIM_CAUSE_STORE_FAULT, _a); } \
        x[e->rd] = _t; \
        MW(e->rd, lat_load); \
        RELOAD_CODE_RANGE(); \
        e += e->lf; \
        goto check_async; \
    } while (0)
op_AMOSWAP: AMO(_s);
op_AMOADD:  AMO(_t + _s);
op_AMOXOR:  AMO(_t ^ _s);
op_AMOAND:  AMO(_t & _s);
op_AMOOR:   AMO(_t | _s);
op_AMOMIN:  AMO((int32_t)_t < (int32_t)_s ? _t : _s);
op_AMOMAX:  AMO((int32_t)_t > (int32_t)_s ? _t : _s);
op_AMOMINU: AMO(_t < _s ? _t : _s);
op_AMOMAXU: AMO(_t > _s ? _t : _s);

    /* ---- F extension -------------------------------------------------- */
#define RM_DYN(r) do { if ((r) == 7) (r) = cpu->frm; if (unlikely((r) > 4)) { cpu->trap_len = 4; TRAP(QSIM_CAUSE_ILLEGAL, 0); } } while (0)
#define FARITH(FAST, SOFT, LAT) do { \
        INSN(); MRF2(e->rs1, e->rs2); \
        int _rm = e->imm; \
        RM_DYN(_rm); \
        uint32_t _a = f[e->rs1], _b = f[e->rs2]; \
        (void)_b; \
        f[e->rd] = likely(_rm == 0) ? (FAST) : (SOFT); \
        if (unlikely(FSUB(_a) | FSUB(_b) | FSUB(f[e->rd]))) { \
            cpu->fsub_in += FSUB(_a) | FSUB(_b); cpu->fsub_out += FSUB(f[e->rd]); } \
        MWF(e->rd, LAT); \
        NEXT(); \
    } while (0)

op_FADD: FARITH(qff_add(_a, _b), qf_add(_a, _b, _rm, &cpu->fflags), lat_fadd);
op_FSUB: FARITH(qff_sub(_a, _b), qf_sub(_a, _b, _rm, &cpu->fflags), lat_fadd);
op_FMUL: FARITH(qff_mul(_a, _b), qf_mul(_a, _b, _rm, &cpu->fflags), lat_fmul);
op_FDIV: {
    INSN(); MRF2(e->rs1, e->rs2);
    PSTMT(m->n_fdiv++; m->pen_fdiv += m->p.fdiv_extra; m->cyc += m->p.fdiv_extra);
    int rm = e->imm;
    RM_DYN(rm);
    uint32_t a = f[e->rs1], b = f[e->rs2];
    f[e->rd] = likely(rm == 0) ? qff_div(a, b) : qf_div(a, b, rm, &cpu->fflags);
    if (unlikely(FSUB(a) | FSUB(b) | FSUB(f[e->rd]))) { cpu->fsub_in += FSUB(a) | FSUB(b); cpu->fsub_out += FSUB(f[e->rd]); }
    MWF(e->rd, lat_fdiv);
    NEXT();
}
op_FSQRT: {
    INSN(); MRF(e->rs1);
    PSTMT(m->n_fsqrt++; m->pen_fsqrt += m->p.fsqrt_extra; m->cyc += m->p.fsqrt_extra);
    int rm = e->imm;
    RM_DYN(rm);
    uint32_t a = f[e->rs1];
    f[e->rd] = likely(rm == 0) ? qff_sqrt(a) : qf_sqrt(a, rm, &cpu->fflags);
    if (unlikely(FSUB(a) | FSUB(f[e->rd]))) { cpu->fsub_in += FSUB(a); cpu->fsub_out += FSUB(f[e->rd]); }
    MWF(e->rd, lat_fdiv);
    NEXT();
}

#define FBITS(expr, LAT) do { INSN(); MRF2(e->rs1, e->rs2); uint32_t _a = f[e->rs1], _b = f[e->rs2]; \
        f[e->rd] = (expr); MWF(e->rd, LAT); NEXT(); } while (0)
op_FSGNJ:  FBITS((_a & 0x7fffffffu) | (_b & 0x80000000u), lat_fmisc);
op_FSGNJN: FBITS((_a & 0x7fffffffu) | (~_b & 0x80000000u), lat_fmisc);
op_FSGNJX: FBITS(_a ^ (_b & 0x80000000u), lat_fmisc);
op_FMIN:   FBITS(qf_min(_a, _b, &cpu->fflags), lat_fmisc);
op_FMAX:   FBITS(qf_max(_a, _b, &cpu->fflags), lat_fmisc);

op_FCVT_W: op_FCVT_WU: {
    INSN(); MRF(e->rs1);
    PSTMT(m->n_f2i++; m->pen_f2i += m->p.f2i_extra; m->cyc += m->p.f2i_extra);
    int rm = e->imm;
    RM_DYN(rm);
    uint32_t a = f[e->rs1];
    x[e->rd] = e->h == QOP_FCVT_W ? qf_to_i32(a, rm, &cpu->fflags) : qf_to_u32(a, rm, &cpu->fflags);
    MW(e->rd, lat_f2i);
    NEXT();
}
op_FCVT_SW: op_FCVT_SWU: {
    INSN(); MR(e->rs1);
    int rm = e->imm;
    RM_DYN(rm);
    uint32_t a = RS1;
    f[e->rd] = e->h == QOP_FCVT_SW ? qf_from_i32(a, rm, &cpu->fflags) : qf_from_u32(a, rm, &cpu->fflags);
    MWF(e->rd, lat_i2f);
    NEXT();
}
op_FMV_XW:
    INSN(); MRF(e->rs1);
    x[e->rd] = f[e->rs1];
    MW(e->rd, lat_fcmp);
    NEXT();
op_FMV_WX:
    INSN(); MR(e->rs1);
    f[e->rd] = RS1;
    MWF(e->rd, lat_fmisc);
    NEXT();
op_FCLASS:
    INSN(); MRF(e->rs1);
    x[e->rd] = qf_class(f[e->rs1]);
    MW(e->rd, lat_fcmp);
    NEXT();

#define FCMP(fn) do { INSN(); MRF2(e->rs1, e->rs2); \
        x[e->rd] = fn(f[e->rs1], f[e->rs2], &cpu->fflags); MW(e->rd, lat_fcmp); NEXT(); } while (0)
op_FEQ: FCMP(qf_eq);
op_FLT: FCMP(qf_lt);
op_FLE: FCMP(qf_le);

#define FFUSED(NA, NC) do { \
        INSN(); \
        unsigned _r3 = (unsigned)e->imm & 31; \
        MRF2(e->rs1, e->rs2); MRF(_r3); \
        int _rm = ((unsigned)e->imm >> 8) & 7; \
        RM_DYN(_rm); \
        uint32_t _a = f[e->rs1] ^ (NA), _b = f[e->rs2], _c = f[_r3] ^ (NC); \
        f[e->rd] = likely(_rm == 0) ? qff_fma(_a, _b, _c, &cpu->fflags) \
                                    : qf_fma(_a, _b, _c, _rm, &cpu->fflags); \
        if (unlikely(FSUB(_a) | FSUB(_b) | FSUB(_c) | FSUB(f[e->rd]))) { \
            cpu->fsub_in += FSUB(_a) | FSUB(_b) | FSUB(_c); cpu->fsub_out += FSUB(f[e->rd]); } \
        MWF(e->rd, lat_fma); \
        NEXT(); \
    } while (0)
op_FMADD:  FFUSED(0, 0);
op_FMSUB:  FFUSED(0, 0x80000000u);
op_FNMSUB: FFUSED(0x80000000u, 0);
op_FNMADD: FFUSED(0x80000000u, 0x80000000u);

    /* ---- shared continuations ------------------------------------------ */
jump_far:        /* control transfer to jt outside the direct-branch fast path */
resume_at_jt: {
    uint32_t o = jt - sd_base;
    if (unlikely(jt & 1)) {
        cpu->pc = jt;
        tcause = QSIM_CAUSE_FETCH_MISALIGNED;
        ttval = jt;
        cpu->trap_len = 0;
        if (--budget < 0) { budget = 0; goto out; }
        goto do_trap;
    }
    if (likely(o < sd_size)) e = sd_dc + (o >> 1);
    else if (jt < br_size) e = br_dc + (jt >> 1);
    else {
        cpu->pc = jt;
        tcause = QSIM_CAUSE_FETCH_FAULT;
        ttval = jt;
        cpu->trap_len = 0;
        if (--budget < 0) { budget = 0; goto out; }
        goto do_trap;
    }
    if (unlikely(cpu->async)) { cpu->pc = jt; ret = EXEC_ASYNC; goto out; }
    DISPATCH();
}

check_async:
    if (unlikely(cpu->async)) { cpu->pc = ENTRY_PC(e); ret = EXEC_ASYNC; goto out; }
    DISPATCH();

trap_here:
    cpu->pc = e->pc;
do_trap: {
    int act = take_trap(cpu, tcause, ttval);
    RELOAD_CODE_RANGE();
    if (act == QSIM_TRAP_STOP) { ret = EXEC_TRAPSTOP; goto out; }
    jt = cpu->pc;
    goto resume_at_jt;
}

out_budget:
    budget = 0;
    cpu->pc = ENTRY_PC(e);
out:
    *done = (uint64_t)(budget0 - budget);
#if QX_PROF
    P->pen[P->last_i] += m->cyc - P->last_cyc;
    P->last_cyc = m->cyc;
    m->cur_line = cur_line;
#endif
    return ret;

#undef RELOAD_CODE_RANGE
#undef ENTRY_PC
#undef DISPATCH
#undef NEXT
#undef TRAP
#undef IMM
#undef SYNC_LIVE
#undef RS1
#undef RS2
#undef LINT_CA
#undef LINT_UC_RD
#undef LINT_UC_WR
#undef FSUB
#undef INSN
#undef MR
#undef MR2
#undef MRF
#undef MRF2
#undef MW
#undef MWF
#undef MMEM
#undef PBR
#undef PJUMP
#undef PSTMT
#undef BRANCH
#undef LOAD_OP
#undef MISALIGN_CHECK
#undef STORE_OP
#undef ALU_I
#undef ALU_R
#undef MUL_R
#undef DIV_R
#undef AMO
#undef RM_DYN
#undef FARITH
#undef FBITS
#undef FCVT
#undef FCMP
#undef FFUSED
}
