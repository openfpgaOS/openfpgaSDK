//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qelf.h"

typedef struct { unsigned char ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags;
                 uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; } Phdr;
typedef struct { uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize; } Shdr;
typedef struct { uint32_t name, value, size; uint8_t info, other; uint16_t shndx; } Sym;

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

static int check(const unsigned char *b, size_t len, Ehdr *eh)
{
    if (len < sizeof *eh) return -1;
    memcpy(eh, b, sizeof *eh);
    if (memcmp(eh->ident, "\177ELF", 4) || eh->ident[4] != 1 || eh->ident[5] != 1) return -1;
    return 0;
}

int qsim_elf_load(qsim_cpu *cpu, const char *path, uint32_t *entry)
{
    size_t len;
    unsigned char *b = slurp(path, &len);
    if (!b) return -1;
    Ehdr eh;
    int rc = -1;
    if (check(b, len, &eh)) goto out;
    for (unsigned i = 0; i < eh.phnum; i++) {
        Phdr ph;
        size_t o = eh.phoff + (size_t)i * eh.phentsize;
        if (o + sizeof ph > len) goto out;
        memcpy(&ph, b + o, sizeof ph);
        if (ph.type != 1 || ph.memsz == 0) continue;
        if ((size_t)ph.offset + ph.filesz > len) goto out;
        uint8_t *dst = qsim_mem_ptr(cpu, ph.paddr, ph.memsz);
        if (!dst) { fprintf(stderr, "qelf: segment %08x+%x outside RAM\n", ph.paddr, ph.memsz); goto out; }
        memcpy(dst, b + ph.offset, ph.filesz);
        memset(dst + ph.filesz, 0, ph.memsz - ph.filesz);
        qsim_invalidate(cpu, ph.paddr, ph.memsz);
    }
    if (entry) *entry = eh.entry;
    rc = 0;
out:
    free(b);
    return rc;
}

static int symcmp(const void *a, const void *b)
{
    const qsim_sym *x = a, *y = b;
    if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
    /* prefer global functions first at equal address */
    if (x->type != y->type) return x->type == 2 ? -1 : 1;
    if (x->bind != y->bind) return x->bind == 1 ? -1 : 1;
    return strcmp(x->name, y->name);
}

int qsim_elf_symbols(const char *path, qsim_syms *out)
{
    memset(out, 0, sizeof *out);
    size_t len;
    unsigned char *b = slurp(path, &len);
    if (!b) return -1;
    Ehdr eh;
    int rc = -1;
    if (check(b, len, &eh)) goto out;
    out->entry = eh.entry;
    for (unsigned i = 0; i < eh.shnum; i++) {
        Shdr sh;
        memcpy(&sh, b + eh.shoff + (size_t)i * eh.shentsize, sizeof sh);
        if (sh.type != 2) continue;          /* SHT_SYMTAB */
        Shdr st;
        memcpy(&st, b + eh.shoff + (size_t)sh.link * eh.shentsize, sizeof st);
        out->strings = malloc(st.size + 1);
        memcpy(out->strings, b + st.offset, st.size);
        out->strings[st.size] = 0;
        size_t n = sh.size / sizeof(Sym);
        out->s = calloc(n, sizeof *out->s);
        for (size_t k = 0; k < n; k++) {
            Sym s;
            memcpy(&s, b + sh.offset + k * sizeof s, sizeof s);
            unsigned type = s.info & 15, bind = s.info >> 4;
            if (s.shndx == 0 || s.shndx >= 0xff00) continue;
            if (type != 2 && type != 1 && type != 0) continue;   /* FUNC, OBJECT, NOTYPE */
            if (s.name >= st.size || !out->strings[s.name]) continue;
            const char *nm = out->strings + s.name;
            if (type == 0 && (nm[0] == '$' || nm[0] == '.')) continue;  /* mapping symbols */
            qsim_sym *d = &out->s[out->n++];
            d->addr = s.value; d->size = s.size; d->type = (uint8_t)type; d->bind = (uint8_t)bind;
            d->name = nm;
        }
        qsort(out->s, out->n, sizeof *out->s, symcmp);
        rc = 0;
        break;
    }
out:
    free(b);
    return rc;
}

void qsim_syms_free(qsim_syms *s)
{
    free(s->s);
    free(s->strings);
    memset(s, 0, sizeof *s);
}

const qsim_sym *qsim_syms_func(const qsim_syms *s, uint32_t addr)
{
    size_t lo = 0, hi = s->n;
    while (lo < hi) {                       /* first sym with addr > target */
        size_t mid = (lo + hi) / 2;
        if (s->s[mid].addr <= addr) lo = mid + 1; else hi = mid;
    }
    /* nearest sized function covering addr; otherwise the nearest preceding
     * unsized function or code label (assembly without .type/.size) */
    const qsim_sym *label = NULL;
    for (size_t i = lo; i-- > 0;) {
        const qsim_sym *c = &s->s[i];
        if (c->type == 2 && c->size && addr < c->addr + c->size) return c;
        if (!label && (c->type == 0 || (c->type == 2 && c->size == 0))) label = c;
        if (c->type == 2 && c->size && addr >= c->addr + c->size) break;   /* passed a function end */
        if (lo - i > 256) break;
    }
    return label;
}

const qsim_sym *qsim_syms_lookup(const qsim_syms *s, const char *name)
{
    for (size_t i = 0; i < s->n; i++)
        if (!strcmp(s->s[i].name, name)) return &s->s[i];
    return NULL;
}
