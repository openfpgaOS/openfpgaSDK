//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/* gpuvec.h -- qsim GPU equivalence test-vector format.
 *
 * Shared by the C model replayer (gputest.c) and the Verilator RTL replayer
 * (rtlsim/tb_qsim_main.cpp). A vector file is a little-endian stream of
 * records, each starting with a 4-byte tag:
 *
 *   MEM1 addr len bytes[len] pad-to-4   backdoor SDRAM byte write (GPU addr)
 *   TLUT bytes[32768]                   translucency LUT upload via MMIO 0x20/0x24
 *   PALB value                          GPU_PALOOKUP_BASE (MMIO 0x30)
 *   FLSH                                GPU_TEX_FLUSH (MMIO 0x28)
 *   CMDS n words[n]                     publish one ring batch; run to idle
 *   DUMP addr len                       append SDRAM bytes to the output
 *   FENC                                append fence_reached (u32) to output
 *   RAND addr len seed                  fill bytes from gv_rand (xorshift32)
 *   HASH                                append FNV-1a-64 of all 64 MB (u64)
 *   END.                                end of vector
 *
 * Addresses are 26-bit GPU/SDRAM offsets; both replayers model the full
 * 64 MB SDRAM (the qsim testbench copy widens tb_gpu's 4 MB model).
 */
#ifndef QSIM_GPUVEC_H
#define QSIM_GPUVEC_H

#define GV_TAG(a, b, c, d) ((unsigned)(a) | ((unsigned)(b) << 8) | \
                            ((unsigned)(c) << 16) | ((unsigned)(d) << 24))
#define GV_TAG_MEM  GV_TAG('M', 'E', 'M', '1')
#define GV_TAG_TLUT GV_TAG('T', 'L', 'U', 'T')
#define GV_TAG_PALB GV_TAG('P', 'A', 'L', 'B')
#define GV_TAG_FLSH GV_TAG('F', 'L', 'S', 'H')
#define GV_TAG_CMDS GV_TAG('C', 'M', 'D', 'S')
#define GV_TAG_DUMP GV_TAG('D', 'U', 'M', 'P')
#define GV_TAG_FENC GV_TAG('F', 'E', 'N', 'C')
#define GV_TAG_END  GV_TAG('E', 'N', 'D', '.')
#define GV_TAG_RAND GV_TAG('R', 'A', 'N', 'D')
#define GV_TAG_HASH GV_TAG('H', 'A', 'S', 'H')

#define GV_DEFAULT_PALOOKUP_BASE 0x03FC0000u
#define GV_SDRAM_BYTES           0x04000000u   /* 64 MB */
#define GV_SDRAM_MASK            0x03FFFFFFu

static inline unsigned gv_rand_next(unsigned *st)
{
    unsigned x = *st;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *st = x;
    return x;
}

#endif
