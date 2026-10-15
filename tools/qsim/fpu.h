//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * qsim F-extension helpers: exact IEEE-754 binary32 arithmetic.
 *
 * Every function takes the RISC-V rounding mode (0 NE, 1 TZ, 2 DN, 3 UP,
 * 4 MM) and ORs RISC-V exception bits into *fl (NX 1, UF 2, OF 4, DZ 8,
 * NV 16).  NaN results are always the canonical quiet NaN 0x7fc00000.
 * Tininess is detected after rounding (RISC-V / IEEE-754 option chosen by
 * the spec).  No host floating point is used in fpu.c, so these functions
 * never disturb the host MXCSR (the fast round-to-nearest path in cpu.c
 * relies on that).
 */
#ifndef QSIM_FPU_H
#define QSIM_FPU_H
#include <stdint.h>

#define QF_NX 0x01
#define QF_UF 0x02
#define QF_OF 0x04
#define QF_DZ 0x08
#define QF_NV 0x10

#define QF_CANON_NAN 0x7fc00000u

static inline int qf_isnan(uint32_t a)  { return (uint32_t)(a << 1) > 0xff000000u; }
static inline int qf_issnan(uint32_t a) { return ((a & 0x7fc00000u) == 0x7f800000u) && (a & 0x003fffffu); }
static inline int qf_isinf(uint32_t a)  { return (uint32_t)(a << 1) == 0xff000000u; }
static inline int qf_iszero(uint32_t a) { return (uint32_t)(a << 1) == 0; }

uint32_t qf_add(uint32_t a, uint32_t b, int rm, uint8_t *fl);
uint32_t qf_sub(uint32_t a, uint32_t b, int rm, uint8_t *fl);
uint32_t qf_mul(uint32_t a, uint32_t b, int rm, uint8_t *fl);
uint32_t qf_div(uint32_t a, uint32_t b, int rm, uint8_t *fl);
uint32_t qf_sqrt(uint32_t a, int rm, uint8_t *fl);
/* fused a*b + c (callers negate operands for fmsub/fnmsub/fnmadd) */
uint32_t qf_fma(uint32_t a, uint32_t b, uint32_t c, int rm, uint8_t *fl);

uint32_t qf_to_i32(uint32_t a, int rm, uint8_t *fl);
uint32_t qf_to_u32(uint32_t a, int rm, uint8_t *fl);
uint32_t qf_from_i32(uint32_t v, int rm, uint8_t *fl);
uint32_t qf_from_u32(uint32_t v, int rm, uint8_t *fl);

uint32_t qf_min(uint32_t a, uint32_t b, uint8_t *fl);
uint32_t qf_max(uint32_t a, uint32_t b, uint8_t *fl);
uint32_t qf_eq(uint32_t a, uint32_t b, uint8_t *fl);
uint32_t qf_lt(uint32_t a, uint32_t b, uint8_t *fl);
uint32_t qf_le(uint32_t a, uint32_t b, uint8_t *fl);
uint32_t qf_class(uint32_t a);

#endif
