/*----------------------------------------------------------------------------*/
/* SPDX-License-Identifier: Apache-2.0                                      */
/* SPDX-FileType: SOURCE                                                    */
/* SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>       */
/*----------------------------------------------------------------------------*/

ENTRY(_start)
SECTIONS {
    . = 0x10400000;
    .text : { *(.text.start) *(.text .text.*) *(.rodata .rodata.* .srodata .srodata.*) }
    .data : { *(.data .data.* .sdata .sdata.*) }
    __bss_start = .;
    .bss : { *(.bss .bss.* .sbss .sbss.* COMMON) }
    . = ALIGN(4);
    __bss_end = .;
    PROVIDE(__global_pointer$ = 0x10400800);
}
