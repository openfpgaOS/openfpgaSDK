//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/* Minimal ELF32 little-endian RISC-V loader + symbol table (no libelf). */
#ifndef QSIM_QELF_H
#define QSIM_QELF_H
#include <stdint.h>
#include <stddef.h>
#include "cpu.h"

typedef struct {
    uint32_t addr, size;
    uint8_t  type;     /* STT_* */
    uint8_t  bind;     /* STB_* */
    const char *name;
} qsim_sym;

typedef struct {
    qsim_sym *s;       /* sorted by addr; functions and objects */
    size_t n;
    char *strings;     /* owned copy of the string table */
    uint32_t entry;
} qsim_syms;

/* Copy PT_LOAD segments into guest memory (paddr).  Zero-fills bss.
 * Returns 0 on success; *entry receives e_entry. */
int qsim_elf_load(qsim_cpu *cpu, const char *path, uint32_t *entry);
int qsim_elf_symbols(const char *path, qsim_syms *out);
void qsim_syms_free(qsim_syms *s);
/* function symbol containing addr (STT_FUNC, size-aware), or NULL */
const qsim_sym *qsim_syms_func(const qsim_syms *s, uint32_t addr);
/* exact name lookup (first match) */
const qsim_sym *qsim_syms_lookup(const qsim_syms *s, const char *name);

#endif
