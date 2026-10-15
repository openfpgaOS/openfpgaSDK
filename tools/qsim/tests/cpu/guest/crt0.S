/*----------------------------------------------------------------------------*/
/* SPDX-License-Identifier: Apache-2.0                                      */
/* SPDX-FileType: SOURCE                                                    */
/* SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>       */
/*----------------------------------------------------------------------------*/

/* bare-metal start for qsim CPU tests (M-mode, no OS) */
    .section .text.start, "ax"
    .globl _start
    .option norelax
_start:
    li sp, 0x13A80000
    la t0, trap_entry
    csrw mtvec, t0
    li t0, 0x6000          /* mstatus.FS = dirty */
    csrs mstatus, t0
    csrw fcsr, zero
    /* clear .bss */
    la t0, __bss_start
    la t1, __bss_end
1:  bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:  call main
    li t0, 0x4F000004      /* exit device */
    sw a0, 0(t0)
    li a7, 93
    ecall
3:  j 3b

/* Records mcause/mtval in trap_cause/trap_tval and skips the instruction.
 * Preserves every register except mepc. */
    .p2align 2
    .globl trap_entry
trap_entry:
    addi sp, sp, -16
    sw t0, 0(sp)
    sw t1, 4(sp)
    csrr t0, mcause
    la t1, trap_cause
    sw t0, 0(t1)
    csrr t0, mtval
    la t1, trap_tval
    sw t0, 0(t1)
    csrr t0, mepc
    lhu t1, 0(t0)
    andi t1, t1, 3
    addi t1, t1, -3
    beqz t1, 4f
    addi t0, t0, 2
    j 5f
4:  addi t0, t0, 4
5:  csrw mepc, t0
    lw t0, 0(sp)
    lw t1, 4(sp)
    addi sp, sp, 16
    mret

    .globl get_fflags
get_fflags:
    csrr a0, fflags
    ret
    .globl set_frm_fflags
set_frm_fflags:
    csrw frm, a0
    csrw fflags, a1
    ret
    .globl set_fcsr
set_fcsr:
    csrw fcsr, a0
    ret

    .section .bss
    .globl trap_cause
    .globl trap_tval
    .p2align 2
trap_cause: .word 0
trap_tval:  .word 0
