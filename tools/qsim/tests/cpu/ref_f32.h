//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

#ifndef REF_F32_H
#define REF_F32_H
#include <stdint.h>

/* op ids (shared with the guest F harness, tests/cpu/guest/fops.S) */
enum {
    ROP_ADD = 0, ROP_SUB, ROP_MUL, ROP_DIV, ROP_SQRT, ROP_MIN, ROP_MAX,
    ROP_SGNJ, ROP_SGNJN, ROP_SGNJX, ROP_EQ, ROP_LT, ROP_LE, ROP_CLASS,
    ROP_CVT_W, ROP_CVT_WU, ROP_CVT_SW, ROP_CVT_SWU, ROP_MV_XW, ROP_MV_WX,
    ROP_MADD, ROP_MSUB, ROP_NMSUB, ROP_NMADD, ROP_COUNT
};

uint32_t ref_op(int op, uint32_t a, uint32_t b, uint32_t c, int rm, uint8_t *fl);
#endif
